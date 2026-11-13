#include "WupsImage.h"
#include "WupsLoader.h"

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM_ExpHeap.h"
#include "Common/StackAllocator.h"

#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>

#include <bit>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Aroma
{
static void CopyGuestPhysicalMemory(PPCInterpreter_t* cpu);

namespace
{
enum class HookType : uint32
{
	InitMalloc = 0,
	InitNewlib = 2,
	InitStdcpp = 4,
	InitDevoptab = 6,
	FiniDevoptab = 7,
	InitSockets = 8,
	FiniSockets = 9,
	InitWrapper = 10,
	InitPlugin = 15,
	ApplicationStarts = 17,
	ApplicationRequestsExit = 20,
	ApplicationEnds = 21,
	InitStorage = 22,
	InitThread = 25,
	InitReentFunctions = 26,
};

struct Hook
{
	HookType type;
	uint32 storageRoot;
	MPTR address;
};

std::vector<Hook> s_hooks;
std::vector<MPTR> s_codeAllocations;
std::vector<MEMPTR<void>> s_lowDataAllocations;
enum class ModuleHookType : uint32
{
	InitMalloc = 0,
	FiniMalloc = 1,
	InitNewlib = 2,
	FiniNewlib = 3,
	InitStdcpp = 4,
	FiniStdcpp = 5,
	InitDevoptab = 6,
	FiniDevoptab = 7,
	InitSockets = 8,
	FiniSockets = 9,
	InitWrapper = 10,
	FiniWrapper = 11,
	Initialize = 12,
	ApplicationStarts = 13,
	ApplicationEnds = 14,
	RelocationsDone = 15,
	ApplicationRequestsExit = 16,
	Deinitialize = 17,
	AllApplicationStartsDone = 18,
	AllApplicationEndsDone = 19,
	AllApplicationRequestsExitDone = 20,
	InitThread = 23,
	InitReentFunctions = 24,
};

struct ModuleHooks
{
	std::string name;
	std::vector<WumsDeclarations::Hook> hooks;
	bool skipInitFini;
	bool earlyInit;
};
std::vector<ModuleHooks> s_moduleHooks;
using ExportKey = std::tuple<std::string, std::string, bool>;
std::map<ExportKey, uint32> s_moduleExports;
constexpr uint32 kFirstWumsHandle = 0x80000000;
std::vector<std::string> s_moduleNames;
MPTR s_moduleInformation{};

struct ModuleInformationEntry
{
	uint32be relocations, relocationCount, name, exports, exportCount, hooks, hookCount;
	uint32be legacyBss[4];
	uint32be start, end, entrypoint;
	uint8 skipInitFini, earlyInit;
	uint16 reserved;
	uint32be functionSymbols, functionSymbolCount;
};
static_assert(sizeof(ModuleInformationEntry) == 68);

void CallReentInit(MPTR address);

void CallModuleHook(const ModuleHooks& module, ModuleHookType type)
{
	if (module.skipInitFini && (type == ModuleHookType::InitWrapper || type == ModuleHookType::FiniWrapper))
		return;
	for (const auto& hook : module.hooks)
	{
		if (hook.type != static_cast<uint32>(type))
			continue;
		cemuLog_log(LogType::Force, "WUMS: Calling hook {} for {}", hook.type, module.name);
		if (type == ModuleHookType::InitReentFunctions)
			CallReentInit(hook.address);
		else if (type == ModuleHookType::Initialize || type == ModuleHookType::RelocationsDone)
		{
			StackAllocator<uint32be> args;
			*args.GetPointer() = s_moduleInformation;
			PPCCoreCallback(hook.address, args.GetPointer());
		}
		else
			PPCCoreCallback(hook.address);
	}
}

void CallModuleHooks(ModuleHookType type)
{
	for (const auto& module : s_moduleHooks)
		CallModuleHook(module, type);
}

enum class Lifecycle { Unstarted, Running, Ending, Ended };
std::mutex s_lifecycleMutex;
std::condition_variable s_lifecycleChanged;
Lifecycle s_lifecycle = Lifecycle::Unstarted;
CafeStaticVar<OSThread_t> s_exitThread{coreinit::g_coreinitStaticData};
CafeStaticVar<uint8, 0x18000, 32> s_exitStack{coreinit::g_coreinitStaticData};
MPTR s_exitEntry{};

struct ReentContext
{
	MPTR value;
	MPTR cleanup;
};

std::mutex s_contextMutex;
std::unordered_map<MPTR, std::unordered_map<MPTR, ReentContext>> s_contexts;

void GetReentContext(PPCInterpreter_t* cpu)
{
	const auto plugin = cpu->gpr[3];
	const auto output = cpu->gpr[4];
	auto* thread = coreinit::OSGetCurrentThread();
	if (!output || !thread)
	{
		osLib_returnFromFunction(cpu, 0);
		return;
	}

	MPTR context = 0;
	{
		std::lock_guard lock(s_contextMutex);
		const auto threadContexts = s_contexts.find(memory_getVirtualOffsetFromPointer(thread));
		if (threadContexts != s_contexts.end())
		{
			const auto entry = threadContexts->second.find(plugin);
			if (entry != threadContexts->second.end())
				context = entry->second.value;
		}
	}
	memory_writeU32(output, context);
	osLib_returnFromFunction(cpu, 1);
}

void RegisterReentContext(PPCInterpreter_t* cpu)
{
	auto* thread = coreinit::OSGetCurrentThread();
	if (!thread || !cpu->gpr[4] || !cpu->gpr[5])
	{
		osLib_returnFromFunction(cpu, 0);
		return;
	}
	bool inserted;
	{
		std::lock_guard lock(s_contextMutex);
		auto& contexts = s_contexts[memory_getVirtualOffsetFromPointer(thread)];
		inserted = contexts.try_emplace(cpu->gpr[3], ReentContext{cpu->gpr[4], cpu->gpr[5]}).second;
	}
	osLib_returnFromFunction(cpu, inserted);
}

void CallReentInit(MPTR address)
{
	// The PowerPC ABI passes this structure by reference to a caller-owned copy.
	StackAllocator<uint32be, 3> args;
	auto* table = args.GetPointer();
	table[0] = 2;
	table[1] = RPLLoader_MakePPCCallable(GetReentContext);
	table[2] = RPLLoader_MakePPCCallable(RegisterReentContext);
	PPCCoreCallback(address, table);
}

constexpr uint32 kPluginHeapSize = 32 * 1024 * 1024;
MPTR s_pluginHeapAddress{};
coreinit::MEMHeapBase* s_pluginHeap{};

void PluginAlloc(PPCInterpreter_t* cpu, sint32 alignment)
{
	auto* allocation = coreinit::MEMAllocFromExpHeapEx(s_pluginHeap, cpu->gpr[3], alignment);
	osLib_returnFromFunction(cpu, MEMPTR<void>(allocation).GetMPTR());
}

void PluginAllocDefault(PPCInterpreter_t* cpu)
{
	PluginAlloc(cpu, 0x40);
}

void PluginAllocAligned(PPCInterpreter_t* cpu)
{
	PluginAlloc(cpu, static_cast<sint32>(cpu->gpr[4]));
}

void PluginFree(PPCInterpreter_t* cpu)
{
	if (cpu->gpr[3])
		coreinit::MEMFreeToExpHeap(s_pluginHeap, memory_getPointerFromVirtualOffset(cpu->gpr[3]));
	osLib_returnFromFunction(cpu, 0);
}

struct AllocatorImport
{
	std::string_view name;
	void (*entry)(PPCInterpreter_t*);
};
constexpr AllocatorImport kAllocatorImports[] = {
	{"MEMAllocFromDefaultHeap", PluginAllocDefault},
	{"MEMAllocFromDefaultHeapEx", PluginAllocAligned},
	{"MEMFreeToDefaultHeap", PluginFree},
};
CafeStaticVar<uint32be, std::size(kAllocatorImports)> s_allocatorSlots{coreinit::g_coreinitStaticData};

// Cemu already provides a flat guest address space. Use its Aroma heap
// instead of running the console kernel's BAT and page-table patches.
void PluginAllocMapped(PPCInterpreter_t* cpu)
{
	PluginAlloc(cpu, 4);
}

void TranslateMappedAddress(PPCInterpreter_t* cpu)
{
	const auto address = cpu->gpr[3];
	osLib_returnFromFunction(cpu, memory_isAddressRangeAccessible(address, 1) ? address : 0);
}

constexpr AllocatorImport kMappedImports[] = {
	{"MEMAllocFromMappedMemory", PluginAllocMapped},
	{"MEMAllocFromMappedMemoryEx", PluginAllocAligned},
	{"MEMAllocFromMappedMemoryForGX2Ex", PluginAllocAligned},
	{"MEMFreeToMappedMemory", PluginFree},
};
CafeStaticVar<uint32be, std::size(kMappedImports)> s_mappedAllocatorSlots{coreinit::g_coreinitStaticData};

uint32 ResolveMappedMemory(std::string_view symbol, bool isData)
{
	if (!isData)
	{
		if (symbol == "MemoryMappingEffectiveToPhysical" || symbol == "MemoryMappingPhysicalToEffective")
			return RPLLoader_MakePPCCallable(TranslateMappedAddress);
		return 0;
	}
	for (size_t i = 0; i < std::size(kMappedImports); ++i)
	{
		if (symbol == kMappedImports[i].name)
		{
			auto* slot = s_mappedAllocatorSlots.GetPtr() + i;
			*slot = RPLLoader_MakePPCCallable(kMappedImports[i].entry);
			return memory_getVirtualOffsetFromPointer(slot);
		}
	}
	return 0;
}

using StorageBooleans = std::unordered_map<std::string, bool>;
std::vector<StorageBooleans> s_pluginStorage;

void StorageGetItem(PPCInterpreter_t* cpu)
{
	const auto& args = cpu->gpr;
	sint32 result = -1;
	if (args[3] > 0 && args[3] <= s_pluginStorage.size() && !args[4] && args[5] && args[7])
	{
		const auto* key = MEMPTR<const char>(args[5]).GetPtr();
		const auto& settings = s_pluginStorage[args[3] - 1];
		const auto found = settings.find(key);
		if (found == settings.end())
			result = -0x10;
		else if (args[6] != 6) // WUPS_STORAGE_ITEM_BOOL
			result = -3;
		else if (!args[8])
			result = -4;
		else
		{
			memory_writeU8(args[7], found->second);
			if (args[9])
				memory_writeU32(args[9], 1);
			result = 0;
		}
	}
	osLib_returnFromFunction(cpu, result);
}

void CallStorageInit(MPTR address, uint32 storageRoot)
{
	// The PPC ABI passes the by-value argument structure indirectly.
	StackAllocator<uint32be, 11> args;
	auto* words = args.GetPointer();
	words[0] = 2;
	words[1] = storageRoot;
	words[9] = RPLLoader_MakePPCCallable(StorageGetItem);
	PPCCoreCallback(address, words);
}

void LoadStorage(const WupsImage& image, const fs::path& path, StorageBooleans& settings)
{
	for (const auto& section : image.Sections())
	{
		if (section.name != ".wups.meta")
			continue;
		std::string_view records(reinterpret_cast<const char*>(section.data.data()), section.data.size());
		while (!records.empty())
		{
			const auto end = records.find('\0');
			if (end == std::string_view::npos)
				break;
			const auto record = records.substr(0, end);
			records.remove_prefix(end + 1);
			if (!record.starts_with("storage_id="))
				continue;
			const auto id = record.substr(11);
			if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string_view::npos)
				return;
			std::ifstream input(path.parent_path() / "config" / (std::string(id) + ".json"));
			if (!input)
				return;
			rapidjson::IStreamWrapper stream(input);
			rapidjson::Document document;
			document.ParseStream(stream);
			if (!document.IsObject() || !document.HasMember("storageitems") || !document["storageitems"].IsObject())
				return;
			for (const auto& item : document["storageitems"].GetObject())
			{
				if (item.value.IsBool())
					settings.emplace(item.name.GetString(), item.value.GetBool());
			}
			cemuLog_log(LogType::Force, "WUPS: Loaded {} boolean settings for {}", settings.size(), id);
			return;
		}
	}
}

uint32 ResolveImport(std::string_view section, std::string_view symbol)
{
	const bool isData = section.starts_with(".dimport_");
	if (!isData && !section.starts_with(".fimport_"))
		throw std::runtime_error("Unknown WUPS import section " + std::string(section));
	const std::string library(section.substr(std::string_view(".fimport_").size()));
	// Aroma gives plugins their own allocator imports. A title may replace its
	// own function pointers with an allocator whose blocks are not ExpHeap blocks.
	if (isData && library == "coreinit")
	{
		for (size_t i = 0; i < std::size(kAllocatorImports); ++i)
		{
			const auto& allocator = kAllocatorImports[i];
			if (symbol != allocator.name)
				continue;
			auto* slot = s_allocatorSlots.GetPtr() + i;
			*slot = RPLLoader_MakePPCCallable(allocator.entry);
			return memory_getVirtualOffsetFromPointer(slot);
		}
	}
	const std::string name(symbol);
	if (auto* module = RPLLoader_GetModuleByName(library))
	{
		if (const auto address = RPLLoader_FindRPLExport(module, name.c_str(), isData))
			return address;
	}
	if (isData)
	{
		const auto address = osLib_getPointer(library.c_str(), name.c_str());
		return address == 0xffffffff ? 0 : address;
	}
	return rpl_mapHLEImport(nullptr, library.c_str(), name.c_str(), false);
}

struct Segment
{
	uint32 linkedBase;
	uint32 size{};
	uint32 alignment{4};
	uint32 guestBase{};

	void Include(const WupsSection& section)
	{
		const auto alignmentRequired = std::max(section.alignment, 1u);
		if (!std::has_single_bit(alignmentRequired) || alignmentRequired > 0x10000 ||
			section.address % alignmentRequired)
			throw std::runtime_error("Invalid WUPS section alignment");
		const auto end = uint64(section.address - linkedBase) + section.size;
		if (end > 16 * 1024 * 1024)
			throw std::runtime_error("WUPS segment exceeds guest allocation limit");
		alignment = std::max(alignment, alignmentRequired);
		size = std::max(size, static_cast<uint32>(end));
	}
};

constexpr uint32 kBranchStubCapacity = 0x4000;

struct ImagePlacement
{
	Segment code{0x02000000};
	Segment data{0x10000000};
	uint32 stubOffset{};
	std::vector<uint32> addresses;
};

WupsImage ReadImage(const fs::path& path)
{
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	const auto fileSize = stream.tellg();
	if (fileSize <= 0 || fileSize > 64 * 1024 * 1024)
		throw std::runtime_error("Cannot open WUPS image or invalid file size");
	std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
	stream.seekg(0);
	if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
		throw std::runtime_error("Could not read WUPS image");

	return WupsImage(bytes);
}

bool AddCafeDependencies(const WupsImage& image)
{
	bool added = false;
	for (const auto& section : image.Sections())
	{
		if (!section.name.starts_with(".fimport_") && !section.name.starts_with(".dimport_"))
			continue;
		const auto library = std::string_view(section.name).substr(9);
		if (RPLLoader_IsKnownCafeOSModule(library) && !RPLLoader_HasDependency(library))
		{
			RPLLoader_AddDependency(library);
			added = true;
		}
	}
	return added;
}

ImagePlacement AllocateImage(const WupsImage& image, bool needsLowData = false)
{
	ImagePlacement placement;
	auto& code = placement.code;
	auto& data = placement.data;
	std::vector<Segment*> segments;
	for (const auto& section : image.Sections())
	{
		Segment* segment = nullptr;
		if ((section.flags & 2) && (section.type == 1 || section.type == 8) && section.size)
		{
			if (section.address >= code.linkedBase && section.address < data.linkedBase)
				segment = &code;
			else if (section.address >= data.linkedBase && section.address < 0xc0000000)
				segment = &data;
		}
		if (segment)
			segment->Include(section);
		segments.push_back(segment);
	}
	if (!code.size || !data.size)
		throw std::runtime_error("WUPS executable or writable segment is missing");

	placement.stubOffset = (code.size + 3) & ~3u;
	code.guestBase = RPLLoader_AllocateCodeSpace(placement.stubOffset + kBranchStubCapacity, code.alignment);
	s_codeAllocations.push_back(code.guestBase);
	if (needsLowData)
	{
		// FunctionPatcher emits absolute branches to trampolines stored in .data.
		const auto allocation = RPLLoader_AllocateCodeCaveMem(data.alignment, data.size);
		if (!allocation)
			throw std::runtime_error("Could not allocate low memory for Aroma function trampolines");
		s_lowDataAllocations.push_back(allocation);
		data.guestBase = allocation.GetMPTR();
	}
	else
		data.guestBase = RPLLoader_AllocateBootDataSpace(data.size, data.alignment);

	auto& addresses = placement.addresses;
	addresses.resize(segments.size());
	for (size_t i = 0; i < segments.size(); ++i)
	{
		if (const auto* segment = segments[i])
			addresses[i] = segment->guestBase + (image.Sections()[i].address - segment->linkedBase);
	}

	return placement;
}

std::vector<WupsSection> LinkImage(const WupsImage& image, const ImagePlacement& placement,
	const WupsImage::ImportResolver& resolve)
{
	std::unordered_map<uint32, uint32> stubs;
	return image.Relocate(placement.addresses, resolve, [&](uint32, uint32 destination) {
		if (const auto existing = stubs.find(destination); existing != stubs.end())
			return existing->second;
		const uint32 offset = stubs.size() * 16;
		if (offset >= kBranchStubCapacity)
			throw std::runtime_error("WUPS branch stub area is full");
		const uint32 address = placement.code.guestBase + placement.stubOffset + offset;
		memory_writeU32(address, 0x3d600000 | (destination >> 16)); // lis r11, destination@h
		memory_writeU32(address + 4, 0x616b0000 | (destination & 0xffff)); // ori r11, r11, destination@l
		memory_writeU32(address + 8, 0x7d6903a6); // mtctr r11
		memory_writeU32(address + 12, 0x4e800420); // bctr
		stubs.emplace(destination, address);
		return address;
	});
}

void InstallImage(const ImagePlacement& placement, const std::vector<WupsSection>& sections)
{
	memset(memory_getPointerFromVirtualOffset(placement.data.guestBase), 0, placement.data.size);
	for (size_t i = 0; i < sections.size(); ++i)
	{
		const auto& section = sections[i];
		if (placement.addresses[i] && !section.data.empty())
			memcpy(memory_getPointerFromVirtualOffset(placement.addresses[i]), section.data.data(), section.data.size());
	}
}
}

void LinkModules(const fs::path& directory)
{
	struct Module
	{
		WupsImage image;
		ImagePlacement placement;
		WumsMetadata metadata;
		std::vector<WumsDeclarations::Hook> hooks;
		std::vector<WupsSection> linkedSections;
	};
	std::vector<fs::path> paths;
	for (const auto& entry : fs::directory_iterator(directory))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".wms")
			paths.push_back(entry.path());
	}
	std::sort(paths.begin(), paths.end());
	if (paths.empty())
		throw std::runtime_error("No .wms files in " + directory.string());

	std::map<ExportKey, uint32> exports;
	std::unordered_set<std::string> names;
	std::vector<Module> modules;
	bool addedCafeDependencies = false;
	for (const auto& path : paths)
	{
		auto image = ReadImage(path);
		auto metadata = image.ReadModuleMetadata();
		if (!names.insert(metadata.exportName).second)
			throw std::runtime_error("Duplicate WUMS module " + metadata.exportName);
		auto placement = AllocateImage(image, metadata.exportName == "homebrew_functionpatcher");
		auto declarations = image.ReadModuleDeclarations(placement.addresses);
		addedCafeDependencies |= AddCafeDependencies(image);
		for (const auto& entry : declarations.exports)
		{
			auto address = entry.address;
			if (metadata.exportName == "homebrew_memorymapping")
			{
				address = ResolveMappedMemory(entry.name, entry.isData);
				if (!address)
					throw std::runtime_error("Unsupported Aroma memory mapping export " + entry.name);
			}
			if (!exports.emplace(ExportKey(metadata.exportName, entry.name, entry.isData), address).second)
				throw std::runtime_error("Duplicate WUMS export " + metadata.exportName + ":" + entry.name);
		}
		modules.push_back({std::move(image), std::move(placement), std::move(metadata), std::move(declarations.hooks), {}});
	}
	if (addedCafeDependencies)
	{
		RPLLoader_UpdateDependencies();
		RPLLoader_Link();
	}

	std::set<std::pair<std::string, std::string>> missing;
	for (auto& module : modules)
	{
		for (const auto& dependency : module.metadata.dependencies)
		{
			if (!names.contains(dependency))
				missing.emplace(module.metadata.exportName, "module " + dependency);
		}
		module.linkedSections = LinkImage(module.image, module.placement, [&](auto section, auto symbol, auto, auto) {
			const bool isData = section.starts_with(".dimport_");
			if (!isData && !section.starts_with(".fimport_"))
				return ResolveImport(section, symbol);
			const auto library = section.substr(std::string_view(".fimport_").size());
			const auto found = exports.find(ExportKey(library, symbol, isData));
			if (found != exports.end())
			{
				// The console export calls a kernel syscall. Cemu maps
				// guest physical addresses directly into emulated memory.
				if (!isData && library == "homebrew_kernel" && symbol == "KernelCopyData")
					return RPLLoader_MakePPCCallable(CopyGuestPhysicalMemory);
				return found->second;
			}
			if (const auto address = ResolveImport(section, symbol))
				return address;
			missing.emplace(module.metadata.exportName, std::string(library) + "::" + std::string(symbol));
			return uint32{4}; // Placeholder for diagnostics. No image is installed when an import is missing.
		});
	}

	if (!missing.empty())
	{
		for (const auto& [module, requirement] : missing)
			cemuLog_log(LogType::Force, "WUMS: {} requires {}", module, requirement);
		throw std::runtime_error("Aroma module linking stopped with " + std::to_string(missing.size()) + " missing requirements");
	}

	// Resolve the order before installing anything so a cycle cannot leave
	// a partially installed set. Keep filename order among ready modules.
	std::unordered_set<std::string> ordered;
	std::vector<const Module*> installationOrder;
	while (installationOrder.size() < modules.size())
	{
		const bool earlyPhase = std::ranges::any_of(modules, [&](const Module& module) {
			return module.metadata.initBeforeRelocationsDone && !ordered.contains(module.metadata.exportName);
		});
		const auto next = std::ranges::find_if(modules, [&](const Module& module) {
			return !ordered.contains(module.metadata.exportName) &&
				(!earlyPhase || module.metadata.initBeforeRelocationsDone) &&
				std::ranges::all_of(module.metadata.dependencies, [&](const std::string& dependency) {
					return ordered.contains(dependency);
				});
		});
		if (next == modules.end())
		{
			for (const auto& module : modules)
			{
				if (!ordered.contains(module.metadata.exportName))
					cemuLog_log(LogType::Force, "WUMS: Could not order {}", module.metadata.exportName);
			}
			throw std::runtime_error(earlyPhase ?
				"Early WUMS modules have cyclic dependencies or require a regular module" :
				"Cyclic WUMS module dependencies");
		}
		ordered.insert(next->metadata.exportName);
		installationOrder.push_back(&*next);
	}

	bool canDispatch = true;
	for (const auto& module : modules)
	{
		for (const auto& hook : module.hooks)
		{
			switch (static_cast<ModuleHookType>(hook.type))
			{
			case ModuleHookType::InitMalloc:
			case ModuleHookType::FiniMalloc:
			case ModuleHookType::InitNewlib:
			case ModuleHookType::FiniNewlib:
			case ModuleHookType::InitStdcpp:
			case ModuleHookType::FiniStdcpp:
			case ModuleHookType::InitDevoptab:
			case ModuleHookType::FiniDevoptab:
			case ModuleHookType::InitSockets:
			case ModuleHookType::FiniSockets:
			case ModuleHookType::InitWrapper:
			case ModuleHookType::FiniWrapper:
			case ModuleHookType::Initialize:
			case ModuleHookType::Deinitialize:
			case ModuleHookType::RelocationsDone:
			case ModuleHookType::InitThread:
			case ModuleHookType::InitReentFunctions:
			case ModuleHookType::ApplicationStarts:
			case ModuleHookType::ApplicationEnds:
			case ModuleHookType::ApplicationRequestsExit:
			case ModuleHookType::AllApplicationStartsDone:
			case ModuleHookType::AllApplicationEndsDone:
			case ModuleHookType::AllApplicationRequestsExitDone:
				break;
			default:
				cemuLog_log(LogType::Force, "WUMS: {} requires unsupported hook {}", module.metadata.exportName, hook.type);
				canDispatch = false;
				break;
			}
		}
	}
	ModuleInformationEntry* entries = nullptr;
	if (canDispatch)
	{
		const auto entriesAddress = RPLLoader_AllocateBootDataSpace(installationOrder.size() * sizeof(ModuleInformationEntry), 4);
		entries = reinterpret_cast<ModuleInformationEntry*>(memory_getPointerFromVirtualOffset(entriesAddress));
		s_moduleInformation = RPLLoader_AllocateBootDataSpace(5 * sizeof(uint32be), 4);
		auto* header = reinterpret_cast<uint32be*>(memory_getPointerFromVirtualOffset(s_moduleInformation));
		header[0] = 14;
		header[1] = installationOrder.size();
		header[2] = entriesAddress;
		header[3] = 0;
		header[4] = 0;
	}
	for (const auto* module : installationOrder)
	{
		InstallImage(module->placement, module->linkedSections);
		if (entries)
		{
			auto& entry = *entries++;
			entry = {};
			const auto& name = module->metadata.exportName;
			entry.name = RPLLoader_AllocateBootDataSpace(name.size() + 1, 1);
			memcpy(memory_getPointerFromVirtualOffset(entry.name), name.c_str(), name.size() + 1);
			entry.start = module->placement.code.guestBase;
			entry.end = module->placement.code.guestBase + module->placement.code.size;
			entry.skipInitFini = module->metadata.skipInitFini;
			entry.earlyInit = module->metadata.initBeforeRelocationsDone;
			for (size_t i = 0; i < module->linkedSections.size(); ++i)
			{
				const auto& section = module->linkedSections[i];
				if (section.name == ".wums.exports" && !section.data.empty())
				{
					entry.exports = module->placement.addresses[i];
					entry.exportCount = section.data.size() / 12;
				}
				if (section.name == ".wums.hooks" && !section.data.empty())
				{
					entry.hooks = module->placement.addresses[i];
					entry.hookCount = section.data.size() / 8;
				}
			}
		}
		if (canDispatch && module->metadata.exportName == "homebrew_memorymapping")
			cemuLog_log(LogType::Force, "WUMS: Using Cemu mapped-memory services for homebrew_memorymapping");
		else if (canDispatch && !module->hooks.empty())
			s_moduleHooks.push_back({module->metadata.exportName, module->hooks, module->metadata.skipInitFini,
				module->metadata.initBeforeRelocationsDone});
		if (canDispatch)
			s_moduleNames.push_back(module->metadata.exportName);
		cemuLog_log(LogType::Force, "WUMS: Linked {} at text {:08x}, data {:08x}", module->metadata.exportName,
			module->placement.code.guestBase, module->placement.data.guestBase);
	}
	if (!canDispatch)
		cemuLog_log(LogType::Force, "WUMS: Guest hooks require module runtime initialization");
	if (canDispatch && !s_pluginHeapAddress)
		s_pluginHeapAddress = RPLLoader_AllocateBootDataSpace(kPluginHeapSize, 0x1000);
	if (canDispatch)
		s_moduleExports = std::move(exports);
}

uint32 FindModuleHandle(std::string_view name)
{
	const auto found = std::ranges::find(s_moduleNames, name);
	return found == s_moduleNames.end() ? 0 : kFirstWumsHandle + static_cast<uint32>(found - s_moduleNames.begin());
}

std::string_view FindModuleName(uint32 handle)
{
	const auto index = handle - kFirstWumsHandle;
	return index < s_moduleNames.size() ? std::string_view(s_moduleNames[index]) : std::string_view{};
}

MPTR FindModuleExport(uint32 handle, bool isData, std::string_view name)
{
	const auto library = FindModuleName(handle);
	if (library.empty())
		return MPTR_NULL;
	const auto found = s_moduleExports.find(ExportKey(library, name, isData));
	return found == s_moduleExports.end() ? MPTR_NULL : found->second;
}

// Cemu uses the same addresses for guest physical and virtual memory.
static void CopyGuestPhysicalMemory(PPCInterpreter_t* cpu)
{
	const auto destination = memory_physicalToVirtual(cpu->gpr[3]);
	const auto source = memory_physicalToVirtual(cpu->gpr[4]);
	const auto size = cpu->gpr[5];
	if (size && destination <= UINT32_MAX - size &&
		memory_isAddressRangeAccessible(source, size) &&
		memory_isAddressRangeAccessible(destination, size))
	{
		memmove(memory_getPointerFromVirtualOffset(destination),
			memory_getPointerFromVirtualOffset(source), size);
		PPCRecompiler_invalidateRange(destination, destination + size);
	}
	osLib_returnFromFunction(cpu, 0);
}

// WUPS named replacements for HLE exports. The original HLE instruction
// is callable from a separate trampoline, including from the guest hook.
static void ApplyHleReplacements(const std::vector<WupsSection>& sections, const ImagePlacement& placement)
{
	for (const auto& section : sections)
	{
		if (section.name != ".wups.load")
			continue;
		constexpr size_t entrySize = 36;
		if (section.data.size() % entrySize)
			throw std::runtime_error("Invalid WUPS replacement table size");
		for (size_t offset = 0; offset < section.data.size(); offset += entrySize)
		{
			const auto word = [&](size_t index) {
				const auto* b = section.data.data() + offset + index * 4;
				return (uint32(b[0]) << 24) | (uint32(b[1]) << 16) | (uint32(b[2]) << 8) | uint32(b[3]);
			};
			// Named nn_act replacement for a foreground game.
			if (word(0) != 1 || word(1) || word(2) || word(4) != 18 || word(8) != 16)
				continue;
			const auto name = word(3);
			const auto target = word(6);
			const auto originalSlot = word(7);
			const auto& code = placement.code;
			const auto& data = placement.data;
			if (name < data.guestBase || name - data.guestBase >= data.size ||
				(originalSlot & 3) || originalSlot < data.guestBase ||
				data.size < 4 || originalSlot - data.guestBase > data.size - 4 ||
				(target & 3) || target < code.guestBase || target - code.guestBase >= code.size)
				throw std::runtime_error("Invalid WUPS replacement address");
			const auto* symbol = MEMPTR<const char>(name).GetPtr();
			if (!memchr(symbol, 0, data.size - (name - data.guestBase)))
				throw std::runtime_error("Unterminated WUPS replacement name");
			const auto entry = rpl_mapHLEImport(nullptr, "nn_act", symbol, false);
			if (!entry)
				throw std::runtime_error("Missing WUPS replacement export");
			const auto original = memory_readU32(entry);
			if ((original >> 26) != 1)
				throw std::runtime_error("Unsupported WUPS replacement target");
			const auto trampoline = memory_getVirtualOffsetFromPointer(RPLLoader_AllocateTrampolineCodeSpace(20));
			const auto replacementStub = trampoline + 4;
			const auto displacement = sint64(replacementStub) - entry;
			if (displacement < -0x02000000 || displacement >= 0x02000000)
				throw std::runtime_error("WUPS replacement trampoline is out of branch range");
			memory_writeU32(trampoline, original);
			memory_writeU32(replacementStub, 0x3d800000 | (target >> 16)); // lis r12
			memory_writeU32(replacementStub + 4, 0x618c0000 | (target & 0xffff)); // ori r12
			memory_writeU32(replacementStub + 8, 0x7d8903a6); // mtctr r12
			memory_writeU32(replacementStub + 12, 0x4e800420); // bctr
			memory_writeU32(originalSlot, trampoline);
			memory_writeU32(entry, 0x48000000 | (uint32(displacement) & 0x03fffffc));
			PPCRecompiler_invalidateRange(entry, entry + 4);
			cemuLog_log(LogType::Force, "WUPS: Replaced nn_act::{} at {:08x} with {:08x}", symbol, entry, target);
		}
	}
}

void MapPlugin(const fs::path& path)
{
	const auto image = ReadImage(path);
	if (image.GetKind() != WupsImage::Kind::Plugin)
		throw std::runtime_error("--aroma-plugin expects a WUPS plugin (.wps)");
	if (AddCafeDependencies(image))
	{
		RPLLoader_UpdateDependencies();
		RPLLoader_Link();
	}
	const auto placement = AllocateImage(image);
	std::set<std::string> missing;
	const auto sections = LinkImage(image, placement, [&](auto section, auto symbol, auto, auto) {
		const bool isData = section.starts_with(".dimport_");
		if (!isData && !section.starts_with(".fimport_"))
			return ResolveImport(section, symbol);
		const auto library = section.substr(std::string_view(".fimport_").size());
		const auto entry = s_moduleExports.find(ExportKey(library, symbol, isData));
		if (entry != s_moduleExports.end())
			return entry->second;
		if (const auto address = ResolveImport(section, symbol))
			return address;
		if (isData && library == "homebrew_memorymapping")
		{
			if (const auto address = ResolveMappedMemory(symbol, isData))
				return address;
		}
		if (!isData && library == "homebrew_kernel" && symbol == "KernelCopyData")
			return RPLLoader_MakePPCCallable(CopyGuestPhysicalMemory);
		missing.emplace(std::string(library) + "::" + std::string(symbol));
		return uint32{4}; // Only used for diagnostics. Unresolved images are never installed.
	});
	if (!missing.empty())
	{
		for (const auto& requirement : missing)
			cemuLog_log(LogType::Force, "WUPS: {} requires {}", path.filename().string(), requirement);
		throw std::runtime_error("Aroma plugin linking stopped with " + std::to_string(missing.size()) + " missing imports");
	}
	const auto& addresses = placement.addresses;
	const auto& code = placement.code;

	std::vector<Hook> hooks;
	for (size_t i = 0; i < sections.size(); ++i)
	{
		const auto& section = sections[i];
		if (section.name != ".wups.hooks" || !addresses[i])
			continue;
		if (section.data.size() % 8)
			throw std::runtime_error("Invalid WUPS hook table size");

		const auto readWord = [&](size_t offset) {
			const auto* p = section.data.data() + offset;
			return (uint32(p[0]) << 24) | (uint32(p[1]) << 16) | (uint32(p[2]) << 8) | p[3];
		};
		for (size_t offset = 0; offset < section.data.size(); offset += 8)
		{
			const auto target = readWord(offset + 4);
			if ((target & 3) || target < code.guestBase || target - code.guestBase >= code.size)
				throw std::runtime_error("WUPS hook target is outside the executable segment");
			hooks.push_back({static_cast<HookType>(readWord(offset)), static_cast<uint32>(s_pluginStorage.size() + 1), target});
		}
	}

	InstallImage(placement, sections);
	ApplyHleReplacements(sections, placement);
	if (!s_pluginHeapAddress)
		s_pluginHeapAddress = RPLLoader_AllocateBootDataSpace(kPluginHeapSize, 0x1000);
	LoadStorage(image, path, s_pluginStorage.emplace_back());
	s_hooks.insert(s_hooks.end(), hooks.begin(), hooks.end());
	cemuLog_log(LogType::Force, "WUPS: Mapped {} at text {:08x}, data {:08x}", path.string(), code.guestBase, placement.data.guestBase);
}

static void ExitThreadEntry(PPCInterpreter_t* cpu)
{
	EndPluginForGuestExit();
	osLib_returnFromFunction(cpu, 0);
}

void StartPlugin()
{
	if (s_hooks.empty() && s_moduleHooks.empty())
		return;
	s_pluginHeap = coreinit::MEMCreateExpHeapEx(memory_getPointerFromVirtualOffset(s_pluginHeapAddress),
		kPluginHeapSize, MEM_HEAP_OPTION_THREADSAFE);
	if (!s_pluginHeap)
	{
		cemuLog_log(LogType::Force, "Aroma: Could not initialize the runtime heap");
		return;
	}
	s_exitEntry = RPLLoader_MakePPCCallable(ExitThreadEntry);
	constexpr ModuleHookType moduleInitOrder[] = {
		ModuleHookType::InitReentFunctions, ModuleHookType::InitThread,
		ModuleHookType::InitMalloc, ModuleHookType::InitNewlib, ModuleHookType::InitStdcpp,
		ModuleHookType::InitDevoptab, ModuleHookType::InitSockets,
		ModuleHookType::InitWrapper, ModuleHookType::Initialize,
	};
	const auto initializeModule = [&](const ModuleHooks& module) {
		for (const auto type : moduleInitOrder)
			CallModuleHook(module, type);
	};
	for (const auto& module : s_moduleHooks)
		if (module.earlyInit)
			initializeModule(module);
	CallModuleHooks(ModuleHookType::RelocationsDone);
	for (const auto& module : s_moduleHooks)
		if (!module.earlyInit)
			initializeModule(module);
	// WUMS prepares application services again before notifying modules.
	CallModuleHooks(ModuleHookType::InitDevoptab);
	CallModuleHooks(ModuleHookType::InitSockets);
	CallModuleHooks(ModuleHookType::ApplicationStarts);
	CallModuleHooks(ModuleHookType::AllApplicationStartsDone);
	if (!s_hooks.empty())
	{
		// This runs on the default guest thread after coreinit sets up the heaps.
		constexpr HookType order[] = {
			HookType::InitReentFunctions, HookType::InitMalloc, HookType::InitNewlib, HookType::InitStdcpp, HookType::InitThread,
			HookType::InitDevoptab, HookType::InitSockets, HookType::InitWrapper,
			HookType::InitStorage, HookType::InitPlugin, HookType::ApplicationStarts,
		};
		for (auto type : order)
		{
			for (const auto& hook : s_hooks)
			{
				if (hook.type != type)
					continue;
				cemuLog_log(LogType::Force, "WUPS: Calling hook {} at {:08x}", static_cast<uint32>(type), hook.address);
				if (type == HookType::InitReentFunctions)
					CallReentInit(hook.address);
				else if (type == HookType::InitStorage)
					CallStorageInit(hook.address, hook.storageRoot);
				else
				{
					PPCCoreCallback(hook.address);
				}
			}
		}
	}
	std::lock_guard lock(s_lifecycleMutex);
	s_lifecycle = Lifecycle::Running;
}

bool EndPluginForGuestExit()
{
	{
		std::lock_guard lock(s_lifecycleMutex);
		if (s_lifecycle == Lifecycle::Unstarted)
			return true;
		if (s_lifecycle != Lifecycle::Running)
			return false;
		s_lifecycle = Lifecycle::Ending;
	}

	constexpr HookType order[] = {
		HookType::ApplicationRequestsExit, HookType::ApplicationEnds,
		HookType::FiniSockets, HookType::FiniDevoptab,
	};
	for (const auto type : order)
	{
		for (const auto& hook : s_hooks)
		{
			if (hook.type != type)
				continue;
			cemuLog_log(LogType::Force, "WUPS: Calling exit hook {} at {:08x}", static_cast<uint32>(type), hook.address);
			PPCCoreCallback(hook.address);
		}
	}
	CallModuleHooks(ModuleHookType::ApplicationRequestsExit);
	CallModuleHooks(ModuleHookType::AllApplicationRequestsExitDone);
	CallModuleHooks(ModuleHookType::ApplicationEnds);
	CallModuleHooks(ModuleHookType::AllApplicationEndsDone);
	// Cemu unloads the module images at title exit. Keep each provider
	// initialized until its consumers have finished their unload hooks.
	constexpr ModuleHookType moduleFiniOrder[] = {
		ModuleHookType::Deinitialize, ModuleHookType::FiniWrapper,
		ModuleHookType::FiniSockets, ModuleHookType::FiniDevoptab,
		ModuleHookType::FiniStdcpp, ModuleHookType::FiniNewlib, ModuleHookType::FiniMalloc,
	};
	for (const auto& module : s_moduleHooks | std::views::reverse)
	{
		for (const auto type : moduleFiniOrder)
			CallModuleHook(module, type);
	}
	{
		std::lock_guard lock(s_lifecycleMutex);
		s_lifecycle = Lifecycle::Ended;
	}
	s_lifecycleChanged.notify_all();
	return true;
}

void EndPluginForHostStop()
{
	std::unique_lock lock(s_lifecycleMutex);
	if (s_lifecycle == Lifecycle::Unstarted || s_lifecycle == Lifecycle::Ended || !coreinit::OSIsSchedulerActive())
		return;
	const bool needsThread = s_lifecycle == Lifecycle::Running;
	lock.unlock();
	if (needsThread)
	{
		const auto size = s_exitStack.GetByteSize();
		if (!coreinit::__OSCreateThreadType(s_exitThread.GetPtr(), s_exitEntry, 0, nullptr,
			s_exitStack.GetPtr() + size, size, 16, 7, OSThread_t::THREAD_TYPE::TYPE_APP))
		{
			cemuLog_log(LogType::Force, "WUPS: Could not create a guest cleanup thread");
			return;
		}
		coreinit::OSResumeThread(s_exitThread.GetPtr());
	}
	lock.lock();
	if (!s_lifecycleChanged.wait_for(lock, std::chrono::seconds(8), [] { return s_lifecycle == Lifecycle::Ended; }))
		cemuLog_log(LogType::Force, "WUPS: Guest cleanup did not finish before scheduler shutdown");
}

void CleanupThread()
{
	if (s_hooks.empty() && s_moduleHooks.empty())
		return;

	std::unordered_map<MPTR, ReentContext> contexts;
	{
		std::lock_guard lock(s_contextMutex);
		auto entry = s_contexts.extract(memory_getVirtualOffsetFromPointer(coreinit::OSGetCurrentThread()));
		if (entry.empty())
			return;
		contexts = std::move(entry.mapped());
	}
	for (const auto& [plugin, context] : contexts)
		PPCCoreCallback(context.cleanup, context.value);
}

void ResetPlugin()
{
	s_hooks.clear();
	s_pluginStorage.clear();
	for (const auto address : s_codeAllocations)
		RPLLoader_FreeCodeSpace(address);
	s_codeAllocations.clear();
	for (const auto address : s_lowDataAllocations)
		RPLLoader_ReleaseCodeCaveMem(address);
	s_lowDataAllocations.clear();
	s_moduleHooks.clear();
	s_moduleExports.clear();
	s_moduleNames.clear();
	s_moduleInformation = 0;
	s_pluginHeap = nullptr;
	s_pluginHeapAddress = 0;
	{
		std::lock_guard lock(s_lifecycleMutex);
		s_lifecycle = Lifecycle::Unstarted;
		s_exitEntry = 0;
	}
	std::lock_guard lock(s_contextMutex);
	s_contexts.clear();
}
}
