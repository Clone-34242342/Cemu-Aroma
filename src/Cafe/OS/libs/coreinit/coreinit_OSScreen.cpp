#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"

#include "Cafe/OS/libs/coreinit/coreinit_OSScreen_font.h"
#include "Cafe/OS/libs/coreinit/coreinit_OSScreen.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"

namespace coreinit
{

	struct
	{
		sint32 x;
		sint32 y;
		sint32 pitch;
	}screenSizes[2] =
	{
		{ 1280, 720, 1280}, // TV
		{ 896, 480, 896 } // DRC (values might be incorrect)
	};

	void* currentScreenBasePtr[2] = { 0 };

	namespace
	{
		constexpr uint32 kDisplayControllerPhysicalBase = 0x0c200000;
		constexpr uint32 kDisplayRegisterCount = 0x1a67;
		constexpr uint32 kDisplayRegisterStride = 0x200;
		constexpr uint32 kGraphicsEnable = 0x1840;
		constexpr uint32 kGraphicsPitch = 0x1848;
		constexpr uint32 kGraphicsWidth = 0x184d;
		constexpr uint32 kGraphicsHeight = 0x184e;
		constexpr uint32 kOverlayPitch = 0x1866;
		CafeStaticVar<uint32be, kDisplayRegisterCount> s_displayRegisters{g_coreinitStaticData};

		MPTR __OSPhysicalToEffectiveUncached(uint32 physicalAddress)
		{
			const auto offset = physicalAddress - kDisplayControllerPhysicalBase;
			if (offset >= kDisplayRegisterCount * sizeof(uint32be))
				return MPTR_NULL;
			return memory_getVirtualOffsetFromPointer(s_displayRegisters.GetPtr()) + offset;
		}
	}

	void _OSScreen_Clear(uint32 screenIndex, uint32 color)
	{
		if (!currentScreenBasePtr[screenIndex])
			return;

		uint32* output = (uint32*)currentScreenBasePtr[screenIndex];
		sint32 sizeInPixels = screenSizes[screenIndex].pitch * screenSizes[screenIndex].y;
		color = _swapEndianU32(color);
		for (sint32 i = 0; i < sizeInPixels; i++)
		{
			*output = color;
			output++;
		}
	}

	void OSScreenInit()
	{
		// todo - init VI registers?
	}

	uint32 OSScreenGetBufferSizeEx(uint32 screenIndex)
	{
		cemu_assert(screenIndex < 2);
		uint32 bufferSize = screenSizes[screenIndex].pitch * screenSizes[screenIndex].y * 4 * 2;
		return bufferSize;
	}

	void _updateCurrentDrawScreen(sint32 screenIndex)
	{
		uint32 screenDataSize = screenSizes[screenIndex].pitch * screenSizes[screenIndex].y * 4;

		if ((LatteGPUState.osScreen.screen[screenIndex].flipRequestCount & 1) != 0)
			currentScreenBasePtr[screenIndex] = memory_getPointerFromPhysicalOffset(LatteGPUState.osScreen.screen[screenIndex].physPtr + screenDataSize);
		else
			currentScreenBasePtr[screenIndex] = memory_getPointerFromPhysicalOffset(LatteGPUState.osScreen.screen[screenIndex].physPtr);
	}

	void OSScreenSetBufferEx(uint32 screenIndex, uint32 buffer)
	{
		cemu_assert(screenIndex < 2);
		LatteGPUState.osScreen.screen[screenIndex].physPtr = buffer;
		_updateCurrentDrawScreen(screenIndex);
	}

	void OSScreenEnableEx(uint32 screenIndex, uint32 isEnabled)
	{
		cemu_assert(screenIndex < 2);
		LatteGPUState.osScreen.screen[screenIndex].isEnabled = isEnabled != 0;
	}

	void OSScreenClearBufferEx(uint32 screenIndex, uint32 color)
	{
		cemu_assert(screenIndex < 2);
		_OSScreen_Clear(screenIndex, color);
	}

	void OSScreenFlipBuffersEx(uint32 screenIndex)
	{
		cemu_assert(screenIndex < 2);
		LatteGPUState.osScreen.screen[screenIndex].flipRequestCount++;
		_updateCurrentDrawScreen(screenIndex);
	}

	void OSScreenPutPixelEx(uint32 screenIndex, sint32 x, sint32 y, uint32 color)
	{
		if (screenIndex >= 2)
			return;
		if (x >= 0 && x < screenSizes[screenIndex].x && y >= 0 && y < screenSizes[screenIndex].y)
		{
			*(uint32*)((uint8*)currentScreenBasePtr[screenIndex] + (x + y * screenSizes[screenIndex].pitch) * 4) = _swapEndianS32(color);
		}
	}

	const char* osScreenCharset = "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";

	sint32 _getOSScreenFontCharIndex(char c)
	{
		const char* charset = osScreenCharset;
		while (*charset)
		{
			if (*charset == c)
			{
				return (sint32)(charset - osScreenCharset);
			}
			charset++;
		}
		return -1;
	}

	void OSScreenPutFontEx(uint32 screenIndex, sint32 x, sint32 y, const char* str)
	{
		// characters are:
		// 16 x 32 (including the margin)
		// with a margin of 4 x 8

		if (y < 0)
		{
			debug_printf("OSScreenPutFontEx: y has invalid value\n");
			return;
		}

		sint32 px = x * 16;
		sint32 py = y * 24;
		sint32 initialPx = px;

		while (*str)
		{
			sint32 charIndex = _getOSScreenFontCharIndex(*str);
			if (*str == (uint8)'\n')
			{
				px = initialPx;
				py += 24;
				str++;
				continue;
			}
			if (charIndex >= 0)
			{
				const uint8* charBitmap = osscreenBitmapFont + charIndex * 50;
				for (sint32 fy = 0; fy < 25; fy++)
				{
					for (sint32 fx = 0; fx < 14; fx++)
					{
						if (((charBitmap[(fx / 8) + (fy) * 2] >> (7 - (fx & 7))) & 1) == 0)
							continue;
						*(uint32*)((uint8*)currentScreenBasePtr[screenIndex] + ((px + fx) + (py + fy) * screenSizes[screenIndex].pitch) * 4) = 0xFFFFFFFF;
					}
				}
			}
			px += 16;
			str++;
		}
	}

	void InitializeOSScreen()
	{
		auto* displayRegisters = s_displayRegisters.GetPtr();
		std::fill_n(displayRegisters, kDisplayRegisterCount, uint32be{0});
		for (uint32 screen = 0; screen < std::size(screenSizes); ++screen)
		{
			auto* registers = displayRegisters + screen * kDisplayRegisterStride;
			registers[kGraphicsEnable] = 1;
			registers[kGraphicsPitch] = screenSizes[screen].pitch;
			registers[kGraphicsWidth] = screenSizes[screen].x;
			registers[kGraphicsHeight] = screenSizes[screen].y;
			registers[kOverlayPitch] = screenSizes[screen].pitch;
		}
		cafeExportRegister("coreinit", __OSPhysicalToEffectiveUncached, LogType::Placeholder);
		cafeExportRegister("coreinit", OSScreenInit, LogType::Placeholder);
		cafeExportRegister("coreinit", OSScreenGetBufferSizeEx, LogType::Placeholder);
		cafeExportRegister("coreinit", OSScreenSetBufferEx, LogType::Placeholder);
		cafeExportRegister("coreinit", OSScreenEnableEx, LogType::Placeholder);
		cafeExportRegister("coreinit", OSScreenClearBufferEx, LogType::Placeholder);
		cafeExportRegister("coreinit", OSScreenFlipBuffersEx, LogType::Placeholder);
		cafeExportRegister("coreinit", OSScreenPutPixelEx, LogType::Placeholder);
		cafeExportRegister("coreinit", OSScreenPutFontEx, LogType::Placeholder);
	}
}
