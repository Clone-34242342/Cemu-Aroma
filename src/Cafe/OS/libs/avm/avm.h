#pragma once

#include "Cafe/OS/RPL/COSModule.h"

namespace avm
{
	// Cafe OS display modes used by the virtual TV.
	inline constexpr uint32 kTVAspectRatio = 1; // 16:9
	inline constexpr uint32 kTVScanMode = 7; // 1080p

	COSModule* GetModule();
	COSModule* GetTVEModule();
}
