#include "ContentRedirection.h"

#include "Cafe/Filesystem/fsc.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/IOSU/fsa/fsa_types.h"
#include "Cafe/OS/RPL/COSModule.h"
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Common/StackAllocator.h"
#include "config/LaunchSettings.h"

#include <mutex>
#include <unordered_map>

namespace Aroma::ContentRedirection
{
namespace
{
constexpr sint32 kInvalidArgument = -0x10;
constexpr sint32 kUnknownLayerType = -0x12;
constexpr sint32 kLayerNotFound = -0x13;

struct GuestDevice
{
	uint32be magic, version;
	MEMPTR<const char> name;
	uint32be fileSize, dirSize;
	uint32be context, open, close, write, read, seek, fstat, stat;
};
static_assert(sizeof(GuestDevice) == 52);

struct GuestStat
{
	uint32be dev, ino, mode, nlink, uid, gid, rdev, padding;
	uint64be size, atime, mtime, ctime, blockSize, blocks;
};
static_assert(sizeof(GuestStat) == 80);

struct Layer
{
	std::string target;
	std::string replacement;
	std::optional<GuestDevice> device;
	bool active = true;
};

struct File
{
	GuestDevice device;
	MPTR state;
};

std::mutex s_mutex;
std::unordered_map<std::string, GuestDevice> s_devices;
std::map<uint32, Layer> s_layers;
std::unordered_map<uint32, File> s_files;
uint32 s_nextLayer = 1;
uint32 s_nextFile = 0x70000000;

sint32 CRGetVersion(uint32be* output)
{
	if (!output)
		return kInvalidArgument;
	*output = 3;
	return 0;
}

sint32 CRAddDeviceABI(const GuestDevice* device, sint32be* result)
{
	if (!device || !result || device->magic != 0x43524456 || device->version != 1 ||
		!device->name || !device->fileSize || device->fileSize > 0x10000 ||
		!device->open || !device->close || !device->read || !device->stat || !device->fstat)
		return kInvalidArgument;
	std::lock_guard lock(s_mutex);
	const auto [entry, inserted] = s_devices.try_emplace(device->name.GetPtr(), *device);
	*result = inserted ? 0 : -1;
	return 0;
}

sint32 CRRemoveDeviceABI(const char* name, sint32be* result)
{
	if (!name || !result)
		return kInvalidArgument;
	const std::string_view device(name);
	std::lock_guard lock(s_mutex);
	*result = s_devices.erase(std::string(device.substr(0, device.find(':')))) ? 0 : -1;
	return 0;
}

sint32 CRAddFSLayerEx(uint32be* handle, const char* name, const char* target, const char* replacement, uint32 type)
{
	if (!handle || !name || !target || !replacement)
		return kInvalidArgument;
	if (type != 0)
		return kUnknownLayerType;
	const std::string_view path(replacement);
	const auto colon = path.find(':');
	if (colon == std::string_view::npos)
		return kInvalidArgument;
	std::lock_guard lock(s_mutex);
	const auto device = s_devices.find(std::string(path.substr(0, colon)));
	if (device == s_devices.end())
		return kInvalidArgument;
	std::string prefix(target);
	while (prefix.size() > 1 && prefix.back() == '/')
		prefix.pop_back();
	if (prefix.empty())
		return kInvalidArgument;
	std::string replacementPrefix(path);
	if (!replacementPrefix.ends_with('/'))
		replacementPrefix += '/';
	const uint32 id = s_nextLayer++;
	s_layers.emplace(id, Layer{std::move(prefix), std::move(replacementPrefix), device->second});
	*handle = id;
	cemuLog_log(LogType::Force, "Aroma content: mounted {} at {}", replacement, target);
	return 0;
}

sint32 CRAddFSLayer(uint32be* handle, const char* name, const char* replacement, uint32 type)
{
	if (!handle || !name || !replacement)
		return kInvalidArgument;
	// SDCafiine uses FS_LAYER_TYPE_CONTENT_MERGE for the SD modpack.
	if (type != 1)
		return kUnknownLayerType;
	std::string_view path(replacement);
	if (path.starts_with("fs:"))
		path.remove_prefix(3);
	if (!path.starts_with("/vol/external01/"))
		return kInvalidArgument;
	coreinit::mountSDCard();
	std::string prefix(path);
	if (!prefix.ends_with('/'))
		prefix += '/';
	std::lock_guard lock(s_mutex);
	const uint32 id = s_nextLayer++;
	s_layers.emplace(id, Layer{"/vol/content", std::move(prefix), std::nullopt});
	*handle = id;
	cemuLog_log(LogType::Force, "Aroma content: merged {} over /vol/content", path);
	return 0;
}

sint32 CRRemoveFSLayer(uint32 handle)
{
	std::lock_guard lock(s_mutex);
	const auto layer = s_layers.find(handle);
	if (layer == s_layers.end())
		return kLayerNotFound;
	cemuLog_log(LogType::Force, "Aroma content: removed layer {} at {}", handle, layer->second.target);
	s_layers.erase(layer);
	return 0;
}

sint32 CRSetActive(uint32 handle, bool active)
{
	std::lock_guard lock(s_mutex);
	const auto layer = s_layers.find(handle);
	if (layer == s_layers.end())
		return kLayerNotFound;
	layer->second.active = active;
	return 0;
}

std::optional<std::pair<GuestDevice, std::string>> Resolve(const char* path)
{
	if (!path || !LaunchSettings::AromaEnabled())
		return std::nullopt;
	std::lock_guard lock(s_mutex);
	const std::string_view requested(path);
	for (auto layer = s_layers.rbegin(); layer != s_layers.rend(); ++layer)
	{
		const auto& entry = layer->second;
		if (!entry.active || !requested.starts_with(entry.target))
			continue;
		auto suffix = requested.substr(entry.target.size());
		if (!suffix.empty() && suffix.front() != '/')
			continue;
		if (!suffix.empty())
			suffix.remove_prefix(1);
		if (!entry.device)
			continue;
		return std::pair{*entry.device, entry.replacement + std::string(suffix)};
	}
	return std::nullopt;
}

std::optional<File> FindFile(uint32 handle)
{
	if (!LaunchSettings::AromaEnabled())
		return std::nullopt;
	std::lock_guard lock(s_mutex);
	const auto file = s_files.find(handle);
	if (file == s_files.end())
		return std::nullopt;
	return file->second;
}

sint32 Translate(sint32 result)
{
	switch (result)
	{
	case -2: return static_cast<sint32>(FS_RESULT::NOT_FOUND); // newlib ENOENT
	case -13: return static_cast<sint32>(FS_RESULT::PERMISSION_ERROR); // newlib EACCES
	default: return result < 0 ? static_cast<sint32>(FS_RESULT::FATAL_ERROR) : result;
	}
}

sint32 FillStat(sint32 result, const GuestStat& stat, FSStat_t& output)
{
	if (result < 0)
		return Translate(result);
	output = {};
	output.flag = (stat.mode & 0170000) == 0040000 ? FSFlag::IS_DIR : FSFlag::IS_FILE;
	output.permissions = stat.mode & 0777;
	output.size = static_cast<uint32>(stat.size.value());
	return 0;
}

class ContentModule : public COSModule
{
public:
	std::string_view GetName() override { return "homebrew_content_redirection"; }
	void RPLMapped() override
	{
		if (!LaunchSettings::AromaEnabled())
			return;
		cafeExportRegister("homebrew_content_redirection", CRGetVersion, LogType::Placeholder);
		cafeExportRegister("homebrew_content_redirection", CRAddDeviceABI, LogType::Placeholder);
		cafeExportRegister("homebrew_content_redirection", CRRemoveDeviceABI, LogType::Placeholder);
		cafeExportRegister("homebrew_content_redirection", CRAddFSLayer, LogType::Placeholder);
		cafeExportRegister("homebrew_content_redirection", CRAddFSLayerEx, LogType::Placeholder);
		cafeExportRegister("homebrew_content_redirection", CRRemoveFSLayer, LogType::Placeholder);
		cafeExportRegister("homebrew_content_redirection", CRSetActive, LogType::Placeholder);
	}
	void RPLUnmapped() override
	{
		std::lock_guard lock(s_mutex);
		s_files.clear();
		s_layers.clear();
		s_devices.clear();
		s_nextFile = 0x70000000;
		s_nextLayer = 1;
	}
};
}

COSModule* GetModule()
{
	static ContentModule module;
	return &module;
}

std::optional<std::string> RedirectMergedPath(std::string_view path)
{
	constexpr std::string_view target = "/vol/content/";
	if (!LaunchSettings::AromaEnabled() || !path.starts_with(target))
		return std::nullopt;
	const auto relative = path.substr(target.size());
	if (relative.empty() || relative.substr(relative.rfind('/') + 1).starts_with(".deleted_"))
		return std::nullopt;
	std::lock_guard lock(s_mutex);
	for (auto layer = s_layers.rbegin(); layer != s_layers.rend(); ++layer)
	{
		const auto& entry = layer->second;
		if (!entry.active)
			continue;
		if (entry.device)
		{
			if (path == entry.target ||
				(path.starts_with(entry.target) && path[entry.target.size()] == '/'))
				return std::nullopt;
			continue;
		}
		const std::string replacement = entry.replacement + std::string(relative);
		const auto slash = replacement.rfind('/');
		const std::string marker = replacement.substr(0, slash + 1) + ".deleted_" + replacement.substr(slash + 1);
		if (fsc_doesFileExist(marker.c_str()))
			return std::string{};
		if (fsc_doesFileExist(replacement.c_str()))
			return replacement;
	}
	return std::nullopt;
}

std::optional<sint32> Open(const char* path, const char* mode, sint32be* output)
{
	const auto route = Resolve(path);
	if (!route)
		return std::nullopt;
	if (!mode || !output || (strcmp(mode, "r") && strcmp(mode, "rb")))
		return static_cast<sint32>(FS_RESULT::PERMISSION_ERROR);
	const auto& [device, devicePath] = *route;
	auto* state = coreinit::_weak_MEMAllocFromDefaultHeap(device.fileSize);
	if (!state)
		return static_cast<sint32>(FS_RESULT::MAX_HANDLES);
	memset(state, 0, device.fileSize);
	StackAllocator<char> guestPath(devicePath.size() + 1);
	memcpy(guestPath.GetPointer(), devicePath.c_str(), devicePath.size() + 1);
	const auto result = static_cast<sint32>(PPCCoreCallback(device.open.value(), device.context.value(),
		state, guestPath.GetPointer(), 0u, 0u));
	if (result < 0)
	{
		coreinit::_weak_MEMFreeToDefaultHeap(state);
		return Translate(result);
	}
	std::lock_guard lock(s_mutex);
	const uint32 handle = s_nextFile++;
	s_files.emplace(handle, File{device, memory_getVirtualOffsetFromPointer(state)});
	*output = handle;
	return 0;
}

std::optional<sint32> Close(uint32 handle)
{
	if (!LaunchSettings::AromaEnabled())
		return std::nullopt;
	decltype(s_files)::node_type file;
	{
		std::lock_guard lock(s_mutex);
		file = s_files.extract(handle);
	}
	if (file.empty())
		return std::nullopt;
	const auto& entry = file.mapped();
	const auto result = static_cast<sint32>(PPCCoreCallback(entry.device.close.value(), entry.device.context.value(), entry.state));
	coreinit::_weak_MEMFreeToDefaultHeap(memory_getPointerFromVirtualOffset(entry.state));
	return Translate(result);
}

std::optional<sint32> Read(uint32 handle, void* output, uint32 size, uint32 count)
{
	const auto entry = FindFile(handle);
	if (!entry)
		return std::nullopt;
	if (!size || !count)
		return 0;
	if (!output || uint64(size) * count > INT32_MAX)
		return static_cast<sint32>(FS_RESULT::FATAL_ERROR);
	const auto result = static_cast<sint32>(PPCCoreCallback(entry->device.read.value(), entry->device.context.value(),
		entry->state, output, size * count));
	return result < 0 ? Translate(result) : result / static_cast<sint32>(size);
}

std::optional<sint32> Seek(uint32 handle, uint32 position)
{
	const auto entry = FindFile(handle);
	if (!entry)
		return std::nullopt;
	if (!entry->device.seek)
		return static_cast<sint32>(FS_RESULT::FATAL_ERROR);
	const auto high = PPCCoreCallback(entry->device.seek.value(), entry->device.context.value(),
		entry->state, uint64(position), 0u);
	const auto low = PPCInterpreter_getCurrentInstance()->gpr[4];
	return static_cast<sint32>(high) < 0 ? Translate(static_cast<sint32>(low)) : 0;
}

std::optional<sint32> Position(uint32 handle, uint32be* output)
{
	const auto entry = FindFile(handle);
	if (!entry)
		return std::nullopt;
	if (!entry->device.seek || !output)
		return static_cast<sint32>(FS_RESULT::FATAL_ERROR);
	const auto high = PPCCoreCallback(entry->device.seek.value(), entry->device.context.value(),
		entry->state, uint64(0), 1u);
	const auto low = PPCInterpreter_getCurrentInstance()->gpr[4];
	if (static_cast<sint32>(high) < 0)
		return Translate(static_cast<sint32>(low));
	*output = low;
	return 0;
}

std::optional<sint32> ReadWithPosition(uint32 handle, void* output, uint32 size, uint32 count, uint32 position)
{
	const auto seek = Seek(handle, position);
	if (!seek || *seek < 0)
		return seek;
	return Read(handle, output, size, count);
}

std::optional<sint32> Stat(const char* path, FSStat_t* output)
{
	const auto route = Resolve(path);
	if (!route)
		return std::nullopt;
	if (!output)
		return static_cast<sint32>(FS_RESULT::FATAL_ERROR);
	const auto& [device, devicePath] = *route;
	StackAllocator<char> guestPath(devicePath.size() + 1);
	memcpy(guestPath.GetPointer(), devicePath.c_str(), devicePath.size() + 1);
	StackAllocator<GuestStat> stat;
	const auto result = static_cast<sint32>(PPCCoreCallback(device.stat.value(), device.context.value(),
		guestPath.GetPointer(), stat.GetPointer()));
	return FillStat(result, *stat.GetPointer(), *output);
}

std::optional<sint32> StatFile(uint32 handle, FSStat_t* output)
{
	const auto entry = FindFile(handle);
	if (!entry)
		return std::nullopt;
	if (!output)
		return static_cast<sint32>(FS_RESULT::FATAL_ERROR);
	StackAllocator<GuestStat> stat;
	const auto result = static_cast<sint32>(PPCCoreCallback(entry->device.fstat.value(), entry->device.context.value(),
		entry->state, stat.GetPointer()));
	return FillStat(result, *stat.GetPointer(), *output);
}
}
