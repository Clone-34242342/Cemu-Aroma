#pragma once

namespace Aroma
{
bool HasNativePluginBackend();
// Mocha RPX-only reload: retain the active title's content and save identity.
bool PrepareHomebrewRPX(std::string_view sdPath, uint32 offset, uint32 size);
void CancelPreparedHomebrewRPX();
bool HasPreparedHomebrewWrapper(uint64 titleId);
bool ArmPreparedHomebrewRPX(uint64 wrapperTitleId, uint64 currentTitleId);
std::optional<std::vector<uint8>> TakePreparedHomebrewRPX(uint64 titleId);
void LinkModules(const fs::path& directory);
void MapPlugin(const fs::path& path);
uint32 FindModuleHandle(std::string_view name);
std::string_view FindModuleName(uint32 handle);
MPTR FindModuleExport(uint32 handle, bool isData, std::string_view name);
void StartPlugin();
// Returns false when another guest thread already owns title shutdown.
bool EndPluginForGuestExit();
void EndPluginForHostStop();
void CleanupThread();
void ResetPlugin();
}
