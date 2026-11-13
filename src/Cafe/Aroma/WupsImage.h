#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace Aroma
{
struct WupsSection
{
	std::string name;
	uint32_t type;
	uint32_t flags;
	uint32_t address;
	uint32_t size;
	uint32_t link;
	uint32_t info;
	uint32_t alignment;
	uint32_t entrySize;
	std::vector<uint8_t> data;
};

struct WumsMetadata
{
	std::string exportName;
	std::string version;
	std::vector<std::string> dependencies;
	bool skipInitFini = false;
	bool initBeforeRelocationsDone = false;
};

struct WumsDeclarations
{
	struct Export
	{
		std::string name;
		uint32_t address;
		bool isData;
	};
	struct Hook
	{
		uint32_t type;
		uint32_t address;
	};
	std::vector<Export> exports;
	std::vector<Hook> hooks;
};

class WupsImage
{
public:
	enum class Kind { Plugin, Module };
	Kind GetKind() const { return m_kind; }

	// Section indices are kept intact for symbol and relocation references.
	explicit WupsImage(std::span<const uint8_t> file);
	const std::vector<WupsSection>& Sections() const { return m_sections; }
	WumsMetadata ReadModuleMetadata() const;

	// The caller supplies the guest address for each mapped section, or zero
	// for sections it will not load. A branch import can resolve to a stub.
	// WUMS sections must preserve their relative offsets within each segment.
	using ImportResolver = std::function<uint32_t(std::string_view section, std::string_view symbol,
		uint8_t relocationType, uint32_t patchAddress)>;
	using BranchStubAllocator = std::function<uint32_t(uint32_t patchAddress, uint32_t destination)>;
	std::vector<WupsSection> Relocate(std::span<const uint32_t> addresses, const ImportResolver& resolve,
		const BranchStubAllocator& allocateStub = {}) const;

	// Relocate module declarations before registering exports and resolving imports.
	WumsDeclarations ReadModuleDeclarations(std::span<const uint32_t> addresses) const;

private:
	std::vector<WupsSection> RelocateSections(std::span<const uint32_t> addresses, const ImportResolver& resolve,
		const BranchStubAllocator& allocateStub, bool declarationsOnly) const;
	Kind m_kind;
	std::vector<WupsSection> m_sections;
};
}
