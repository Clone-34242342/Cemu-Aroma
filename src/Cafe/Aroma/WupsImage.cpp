#include "WupsImage.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <stdexcept>
#include <utility>
#include <zlib.h>

namespace Aroma
{
namespace
{
constexpr uint32_t kDeflated = 0x08000000;
constexpr uint32_t kNoBits = 8;
constexpr size_t kImageLimit = 64 * 1024 * 1024;
constexpr size_t kSectionHeaderSize = 40;

std::span<const uint8_t> ReadRange(std::span<const uint8_t> file, size_t offset, size_t size)
{
	if (offset > file.size() || size > file.size() - offset)
		throw std::runtime_error("WUPS section extends outside the file");
	return file.subspan(offset, size);
}

uint16_t Read16(std::span<const uint8_t> bytes, size_t offset)
{
	return (uint16_t(bytes[offset]) << 8) | bytes[offset + 1];
}

uint32_t Read32(std::span<const uint8_t> bytes, size_t offset)
{
	return (uint32_t(Read16(bytes, offset)) << 16) | Read16(bytes, offset + 2);
}

void ReserveBytes(size_t& remaining, size_t size)
{
	if (size > remaining)
		throw std::runtime_error("WUPS image exceeds the decoded size limit");
	remaining -= size;
}
}

WupsImage::WupsImage(std::span<const uint8_t> file)
{
	const auto header = ReadRange(file, 0, 52);
	if (Read32(header, 0) != 0x7f454c46 || header[4] != 1 || header[5] != 2 ||
		Read16(header, 16) != 0xfe01 || Read16(header, 18) != 20)
		throw std::runtime_error("Expected a Wii U PowerPC Aroma image");

	const auto count = Read16(header, 48);
	const auto namesIndex = Read16(header, 50);
	if (Read16(header, 46) != kSectionHeaderSize || !count || namesIndex >= count)
		throw std::runtime_error("Invalid WUPS section table");
	const auto table = ReadRange(file, Read32(header, 32), count * kSectionHeaderSize);

	size_t remaining = kImageLimit;
	m_sections.reserve(count);
	for (size_t index = 0; index < count; ++index)
	{
		const auto row = table.subspan(index * kSectionHeaderSize, kSectionHeaderSize);
		WupsSection section{
			{}, Read32(row, 4), Read32(row, 8), Read32(row, 12),
			Read32(row, 20), Read32(row, 24), Read32(row, 28),
			Read32(row, 32), Read32(row, 36), {}
		};

		if (section.type == kNoBits)
		{
			ReserveBytes(remaining, section.size);
		}
		else
		{
			const auto data = ReadRange(file, Read32(row, 16), section.size);
			if (section.flags & kDeflated)
			{
				section.size = Read32(ReadRange(data, 0, 4), 0);
				ReserveBytes(remaining, section.size);
				section.data.resize(std::max(section.size, 1u));
				uLongf decodedSize = section.data.size();
				if (uncompress(section.data.data(), &decodedSize, data.data() + 4, data.size() - 4) != Z_OK ||
					decodedSize != section.size)
					throw std::runtime_error("Invalid compressed WUPS section");
				section.data.resize(section.size);
				section.flags &= ~kDeflated;
			}
			else
			{
				ReserveBytes(remaining, data.size());
				section.data.assign(data.begin(), data.end());
			}
		}
		m_sections.push_back(std::move(section));
	}

	const auto& names = m_sections[namesIndex].data;
	for (size_t index = 0; index < count; ++index)
	{
		const auto offset = Read32(table, index * kSectionHeaderSize);
		if (offset >= names.size())
			throw std::runtime_error("Invalid WUPS section name offset");
		const auto start = names.begin() + offset;
		const auto end = std::find(start, names.end(), 0);
		if (end == names.end())
			throw std::runtime_error("Unterminated WUPS section name");
		ReserveBytes(remaining, end - start);
		m_sections[index].name.assign(start, end);
	}

	const auto hasMetadata = [&](std::string_view name) {
		return std::any_of(m_sections.begin(), m_sections.end(), [&](const WupsSection& section) {
			return section.name == name && !section.data.empty();
		});
	};
	const bool plugin = hasMetadata(".wups.meta");
	const bool module = hasMetadata(".wums.meta");
	if (plugin == module)
		throw std::runtime_error("Expected either WUPS plugin or WUMS module metadata");
	m_kind = plugin ? Kind::Plugin : Kind::Module;
}

WumsMetadata WupsImage::ReadModuleMetadata() const
{
	if (m_kind != Kind::Module)
		throw std::runtime_error("Expected a WUMS module");

	WumsMetadata result;
	for (const auto& section : m_sections)
	{
		const bool dependency = section.name == ".wums.dependencies";
		if (!dependency && section.name != ".wums.meta")
			continue;

		for (auto start = section.data.begin(); start != section.data.end();)
		{
			const auto end = std::find(start, section.data.end(), 0);
			if (end == section.data.end())
				throw std::runtime_error("Unterminated WUMS declaration");
			if (start != end)
			{
				const std::string_view entry(reinterpret_cast<const char*>(&*start), end - start);
				const auto split = entry.find('=');
				const auto key = entry.substr(0, split);
				if (dependency)
					result.dependencies.emplace_back(entry);
				else if (split != std::string_view::npos)
				{
					const auto value = entry.substr(split + 1);
					if (key == "export_name")
						result.exportName = value;
					else if (key == "wums" || key == "wum")
						result.version = value;
					else if (key == "skipInitFini")
						result.skipInitFini = value == "true";
					else if (key == "initBeforeRelocationDoneHook")
						result.initBeforeRelocationsDone = value == "true";
				}
			}
			start = end + 1;
		}
	}
	if (result.exportName.empty() || result.version.empty())
		throw std::runtime_error("Missing WUMS export name or ABI version");
	return result;
}

namespace
{
void Write16(std::span<uint8_t> data, uint32_t value)
{
	data[0] = value >> 8;
	data[1] = value;
}

void Write32(std::span<uint8_t> data, uint32_t value)
{
	Write16(data, value >> 16);
	Write16(data.subspan(2), value);
}

int WumsSegment(uint32_t address)
{
	if (address < 0x02000000 || address >= 0xc0000000)
		return -1;
	return address < 0x10000000 ? 0 : 1;
}

bool IsBranchReachable(uint32_t relative)
{
	const auto displacement = std::bit_cast<int32_t>(relative);
	return !(relative & 3) && displacement >= -0x02000000 && displacement <= 0x01fffffc;
}

void ApplyRelocation(std::span<uint8_t> data, uint8_t type, uint32_t value, uint32_t address)
{
	const uint32_t relative = value - address;
	switch (type)
	{
	case 1: // R_PPC_ADDR32
		Write32(data, value);
		break;
	case 4: // R_PPC_ADDR16_LO
		Write16(data, value);
		break;
	case 5: // R_PPC_ADDR16_HI
		Write16(data, value >> 16);
		break;
	case 6: // R_PPC_ADDR16_HA
		Write16(data, (value + 0x8000) >> 16);
		break;
	case 10: // R_PPC_REL24
	{
		if (!IsBranchReachable(relative))
			throw std::runtime_error("WUPS branch relocation requires a reachable target");
		Write32(data, (Read32(data, 0) & 0xfc000003) | (relative & 0x03fffffc));
		break;
	}
	case 251: // R_PPC_REL16_HA
		Write16(data, (relative + 0x8000) >> 16);
		break;
	case 252: // R_PPC_REL16_HI
		Write16(data, relative >> 16);
		break;
	case 253: // R_PPC_REL16_LO
		Write16(data, relative);
		break;
	default:
		throw std::runtime_error("Unsupported WUPS relocation " + std::to_string(type));
	}
}
}

std::vector<WupsSection> WupsImage::Relocate(std::span<const uint32_t> addresses, const ImportResolver& resolve,
	const BranchStubAllocator& allocateStub) const
{
	return RelocateSections(addresses, resolve, allocateStub, false);
}

WumsDeclarations WupsImage::ReadModuleDeclarations(std::span<const uint32_t> addresses) const
{
	if (m_kind != Kind::Module)
		throw std::runtime_error("Expected a WUMS module");
	const auto linked = RelocateSections(addresses, {}, {}, true);
	auto locate = [&](uint32_t address, bool executable) {
		for (size_t i = 0; i < linked.size(); ++i)
		{
			const auto& section = linked[i];
			if (addresses[i] && address >= addresses[i] && address - addresses[i] < section.size &&
				(!executable || (section.flags & 4)))
				return i;
		}
		throw std::runtime_error("WUMS declaration points outside its mapped sections");
	};

	WumsDeclarations result;
	for (size_t i = 0; i < linked.size(); ++i)
	{
		const auto& section = linked[i];
		const bool isExport = section.name == ".wums.exports";
		if ((!isExport && section.name != ".wums.hooks") || section.data.empty())
			continue;
		const size_t stride = isExport ? 12 : 8;
		if (!addresses[i] || section.data.size() % stride)
			throw std::runtime_error("Invalid WUMS declaration table");

		for (size_t offset = 0; offset < section.data.size(); offset += stride)
		{
			const auto type = Read32(section.data, offset);
			if (isExport)
			{
				if (type > 1)
					throw std::runtime_error("Unknown WUMS export type");
				const auto nameAddress = Read32(section.data, offset + 4);
				const auto address = Read32(section.data, offset + 8);
				const size_t nameIndex = locate(nameAddress, false);
				const auto& names = linked[nameIndex].data;
				const auto nameBytes = ReadRange(names, nameAddress - addresses[nameIndex], 1);
				const auto start = nameBytes.data();
				const auto end = std::find(start, names.data() + names.size(), 0);
				if (start == end || end == names.data() + names.size())
					throw std::runtime_error("Invalid WUMS export name");
				locate(address, type == 0);
				result.exports.push_back({std::string(start, end), address, type == 1});
			}
			else
			{
				const auto address = Read32(section.data, offset + 4);
				locate(address, true);
				result.hooks.push_back({type, address});
			}
		}
	}
	return result;
}

std::vector<WupsSection> WupsImage::RelocateSections(std::span<const uint32_t> addresses,
	const ImportResolver& resolve, const BranchStubAllocator& allocateStub, bool declarationsOnly) const
{
	if (addresses.size() != m_sections.size())
		throw std::runtime_error("WUPS guest address table has the wrong size");

	std::optional<uint32_t> moduleDisplacements[2];
	if (m_kind == Kind::Module)
	{
		for (size_t i = 0; i < m_sections.size(); ++i)
		{
			const auto segment = WumsSegment(m_sections[i].address);
			if (!addresses[i] || segment < 0)
				continue;
			const auto displacement = addresses[i] - m_sections[i].address;
			auto& expected = moduleDisplacements[segment];
			if (expected && *expected != displacement)
				throw std::runtime_error("WUMS sections must preserve their offsets within each segment");
			expected = displacement;
		}
	}

	auto linked = m_sections;
	for (const auto& relocations : m_sections)
	{
		if (relocations.type != 4 && relocations.type != 9)
			continue;
		auto& target = linked.at(relocations.info);
		if (declarationsOnly && target.name != ".wums.exports" && target.name != ".wums.hooks")
			continue;
		const auto targetAddress = addresses[relocations.info];
		if (!targetAddress)
			continue;
		const auto& symbols = m_sections.at(relocations.link);
		if (relocations.type != 4 || relocations.entrySize != 12 || relocations.size % 12 ||
			symbols.type != 2 || symbols.entrySize != 16 || symbols.size % 16)
			throw std::runtime_error("Unsupported WUPS relocation or symbol table");

		for (size_t offset = 0; offset < relocations.size; offset += 12)
		{
			const auto row = std::span<const uint8_t>(relocations.data).subspan(offset, 12);
			const auto info = Read32(row, 4);
			const uint8_t type = info & 0xff;
			if (type == 0)
				continue;
			const auto symbol = ReadRange(symbols.data, size_t(info >> 8) * 16, 16);
			const auto sectionIndex = Read16(symbol, 14);
			const auto symbolValue = Read32(symbol, 4);
			const auto addend = Read32(row, 8);
			const auto patchOffset = Read32(row, 0) - target.address;
			const size_t patchSize = type == 1 || type == 10 ? 4 : 2;
			ReadRange(target.data, patchOffset, patchSize);
			const auto patchAddress = targetAddress + patchOffset;

			uint32_t value = symbolValue;
			const bool undefinedWeak = sectionIndex == 0 && (symbol[12] >> 4) == 2;
			const bool absolute = sectionIndex == 0xfff1; // SHN_ABS
			if (!absolute && !undefinedWeak && m_sections.at(sectionIndex).type == 0x80000002)
			{
				const auto& origin = m_sections[sectionIndex];
				if (declarationsOnly)
					throw std::runtime_error("WUMS declaration references an import");
				const auto& strings = m_sections.at(symbols.link).data;
				const auto start = Read32(symbol, 0);
				if (start >= strings.size())
					throw std::runtime_error("Invalid WUPS import name offset");
				const auto name = std::span<const uint8_t>(strings).subspan(start);
				const auto end = std::find(name.begin(), name.end(), 0);
				if (end == name.end())
					throw std::runtime_error("Invalid WUPS import name");
				const std::string_view symbolName(reinterpret_cast<const char*>(name.data()), end - name.begin());
				value = resolve(origin.name, symbolName, type, patchAddress);
				if (!value)
					throw std::runtime_error("Unresolved WUPS import " + std::string(symbolName));
			}
			else if (m_kind == Kind::Module && !undefinedWeak && WumsSegment(symbolValue) >= 0)
			{
				// WUMS linker anchors such as _SDA_BASE_ can lie outside their
				// named section. They are rebased against the whole segment.
				const auto& displacement = moduleDisplacements[WumsSegment(symbolValue)];
				if (!displacement)
					throw std::runtime_error("WUMS relocation references an unmapped segment");
				value += *displacement;
			}
			else if (!absolute && !undefinedWeak)
			{
				const auto& origin = m_sections.at(sectionIndex);
				if (!addresses[sectionIndex] || symbolValue < origin.address ||
					symbolValue - origin.address > origin.size)
					throw std::runtime_error("WUPS relocation references an unmapped symbol in " + origin.name + " at " + std::to_string(symbolValue));
				value = addresses[sectionIndex] + (symbolValue - origin.address);
			}
			value += addend;
			if (type == 10 && !IsBranchReachable(value - patchAddress) && allocateStub)
				value = allocateStub(patchAddress, value);
			ApplyRelocation(std::span<uint8_t>(target.data).subspan(patchOffset, patchSize), type, value, patchAddress);
		}
	}
	return linked;
}

}
