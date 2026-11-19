#include "Cafe/OS/common/OSCommon.h"
#include "coreinit_BSP.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"

namespace coreinit
{
	bool bspGetHardwareVersion(uint32be* version)
	{
		uint8 highVersion = 0x11; // anything below 0x11 will be considered as Hollywood
		// todo: Check version returned on console
		uint32 tempVers = highVersion << 24;
		*version = tempVers;
		return true;
	}

	sint32 bspRead(const char* entity, uint8 instance, const char* attribute, uint32 size, void* output)
	{
		if (!entity || !attribute || !output)
			return 0x4000; // BSP_ERROR_INVALID_PARAMETER
		if (strcmp(entity, "EE") != 0)
			return 1; // BSP_ERROR_UNKNOWN_ENTITY
		if (strcmp(attribute, "access") != 0)
			return 2; // BSP_ERROR_UNKNOWN_ATTRIBUTE
		if (!size || (size & 1) || size > 512u - uint32(instance) * 2)
			return 0x80; // BSP_ERROR_SPECIFIED_SIZE_INVALID
		return iosuCrypto_readSeepromBytes(output, instance, size) ? 0 : 0x1000;
	}

	void InitializeBSP()
	{
		cafeExportRegister("coreinit", bspGetHardwareVersion, LogType::Placeholder);
		cafeExportRegister("coreinit", bspRead, LogType::Placeholder);
	}
}