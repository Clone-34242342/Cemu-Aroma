#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace DRCInput
{
struct Sample
{
	std::uint32_t buttons{};
	std::array<float, 4> sticks{};
	std::uint16_t touchX{}, touchY{};
	bool touching{};
};

std::optional<Sample> Decode(std::span<const std::uint8_t> bytes);
// True when a native receiver owns VPAD channel 0. A stale connection returns neutral input.
bool Read(Sample& sample, bool* receiving = nullptr);
bool Configure(bool enabled, std::string_view local, std::string_view peer);
}
