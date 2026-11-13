#pragma once

#include <optional>
#include <string>
#include <string_view>

class COSModule;
struct FSStat_t;

namespace Aroma::ContentRedirection
{
COSModule* GetModule();
// An empty returned path masks the stock file.
std::optional<std::string> RedirectMergedPath(std::string_view path);
std::optional<sint32> Open(const char* path, const char* mode, sint32be* output);
std::optional<sint32> Close(uint32 handle);
std::optional<sint32> Read(uint32 handle, void* output, uint32 size, uint32 count);
std::optional<sint32> ReadWithPosition(uint32 handle, void* output, uint32 size, uint32 count, uint32 position);
std::optional<sint32> Seek(uint32 handle, uint32 position);
std::optional<sint32> Position(uint32 handle, uint32be* output);
std::optional<sint32> Stat(const char* path, FSStat_t* output);
std::optional<sint32> StatFile(uint32 handle, FSStat_t* output);
}
