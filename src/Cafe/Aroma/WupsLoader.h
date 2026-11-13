#pragma once

namespace Aroma
{
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
