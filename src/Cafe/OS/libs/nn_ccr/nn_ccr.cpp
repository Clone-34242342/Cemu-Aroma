#include "Cafe/OS/common/OSCommon.h"
#include "nn_ccr.h"

namespace nn::ccr
{
	sint32 CCRSysCaffeineBootCheck()
	{
		cemuLog_logDebug(LogType::Force, "CCRSysCaffeineBootCheck()");
		return -1;
	}

	// The retail Menu calls into CCR for the DRC radio. Until a radio session
	// exists, report the timed-out state and leave the PIN output untouched.
	static sint32 CCRSysStartPairing(uint32 slot, uint32 timeout)
	{
		cemuLog_log(LogType::Force, "CCRSysStartPairing: slot {}, timeout {}s, no DRC radio session", slot, timeout);
		return -1;
	}

	static uint32 CCRSysGetPairingState()
	{
		return 2; // CCR_SYS_PAIRING_TIMED_OUT
	}

	static sint32 CCRSysGetPincode(uint32be*)
	{
		cemuLog_log(LogType::Force, "CCRSysGetPincode: no DRC radio session");
		return -1;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "nn_ccr";
		}

		void RPLMapped() override
		{
			cafeExportRegister("nn_ccr", CCRSysCaffeineBootCheck, LogType::Placeholder);
			cafeExportRegister("nn_ccr", CCRSysStartPairing, LogType::Placeholder);
			cafeExportRegister("nn_ccr", CCRSysGetPairingState, LogType::Placeholder);
			cafeExportRegister("nn_ccr", CCRSysGetPincode, LogType::Placeholder);
		};

	}s_COSnnccrModule;

	COSModule* GetModule()
	{
		return &s_COSnnccrModule;
	}
}
