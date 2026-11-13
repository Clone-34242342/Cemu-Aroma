#include "Cafe/OS/common/OSCommon.h"
#include "avm.h"

namespace avm
{
	bool AVMGetTVAspectRatio(uint32be* aspectRatio)
	{
		*aspectRatio = kTVAspectRatio;
		return true;
	}

	bool AVMGetTVScanMode(uint32be* scanMode)
	{
		*scanMode = kTVScanMode;
		return true;
	}

	uint32 TVEGetCurrentPort()
	{
		return 0; // HDMI
	}

	bool AVMIsHDCPAvailable()
	{
		return true;
	}

	bool AVMIsHDCPOn()
	{
		return true;
	}

	bool AVMGetAnalogContentsProtectionEnable(uint32be* isEnable)
	{
		*isEnable = 1;
		return false;
	}

	bool AVMIsAnalogContentsProtectionOn()
	{
		return true;
	}

	bool AVMSetAnalogContentsProtectionEnable(sint32 newState)
	{
		return true;  // returns 1 (true) if new state was applied successfully?
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "avm";
		}

		void RPLMapped() override
		{
			cafeExportRegister("avm", AVMGetTVAspectRatio, LogType::Placeholder);
			cafeExportRegister("avm", AVMGetTVScanMode, LogType::Placeholder);
			cafeExportRegister("avm", AVMIsHDCPAvailable, LogType::Placeholder);
			cafeExportRegister("avm", AVMIsHDCPOn, LogType::Placeholder);
			cafeExportRegister("avm", AVMGetAnalogContentsProtectionEnable, LogType::Placeholder);
			cafeExportRegister("avm", AVMIsAnalogContentsProtectionOn, LogType::Placeholder);
			cafeExportRegister("avm", AVMSetAnalogContentsProtectionEnable, LogType::Placeholder);
		};
	}s_COSavmModule;

	class : public COSModule
	{
	public:
		std::string_view GetName() override { return "tve"; }

		void RPLMapped() override
		{
			cafeExportRegister("tve", TVEGetCurrentPort, LogType::Placeholder);
		}
	} s_COStveModule;

	COSModule* GetModule()
	{
		return &s_COSavmModule;
	}

	COSModule* GetTVEModule()
	{
		return &s_COStveModule;
	}
}
