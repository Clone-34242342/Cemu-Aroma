#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_IM.h"

// APD = Automatic Power Down

namespace coreinit
{
	#define IM_ERROR_NONE 0

	void coreinitExport_IMIsAPDEnabledBySysSettings(PPCInterpreter_t* hCPU)
	{
		debug_printf("IMIsAPDEnabledBySysSettings(0x%08x)\n", hCPU->gpr[3]);
		ppcDefineParamTypePtr(isAPDEnabled, uint32be, 0);
		*isAPDEnabled = 0;
		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	void coreinitExport_IMGetTimeBeforeAPD(PPCInterpreter_t* hCPU)
	{
		// parameters:
		// r3	uint32*		returns the remaining number of seconds until auto-shutdown
		memory_writeU32(hCPU->gpr[3], 60 * 30); // 30 minutes
		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	void coreinitExport_IMGetTimeBeforeDimming(PPCInterpreter_t* hCPU)
	{
		// parameters:
		// r3	uint32*		returns the remaining number of seconds until dimming
		memory_writeU32(hCPU->gpr[3], 60 * 30); // 30 minutes
		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	bool imDimIsEnabled = true;

	void coreinitExport_IMEnableDim(PPCInterpreter_t* hCPU)
	{
		imDimIsEnabled = true;
		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	void coreinitExport_IMIsDimEnabled(PPCInterpreter_t* hCPU)
	{
		// parameters:
		// r3	uint32*		returns the remaining number of seconds until auto-shutdown
		memory_writeU32(hCPU->gpr[3], imDimIsEnabled ? 1 : 0); // enabled
		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	void coreinitExport_IMGetAPDPeriod(PPCInterpreter_t* hCPU)
	{
		cemuLog_logDebug(LogType::Force, "IMGetAPDPeriod(0x{:08x})", hCPU->gpr[3]);
		// parameters:
		// r3	uint32*		returns the number of seconds until auto-shutdown occurs
		memory_writeU32(hCPU->gpr[3], 600);
		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	void coreinitExport_IM_GetParameter(PPCInterpreter_t* hCPU)
	{
		cemuLog_logDebug(LogType::Force, "IM_GetParameter()");

		ppcDefineParamS32(imHandle, 0); // handle from IM_Open()
		ppcDefineParamS32(uknR4, 1);
		ppcDefineParamS32(parameterId, 2);
		ppcDefineParamStructPtr(output, void, 3);
		ppcDefineParamS32(uknR7, 4);
		ppcDefineParamS32(uknR8, 5);

		if (parameterId == 0)
		{
			// inactive seconds
			*(uint32be*)output = 600;
		}
		else
		{
			cemu_assert_unimplemented();
		}

		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	void coreinitExport_IM_GetRuntimeParameter(PPCInterpreter_t* hCPU)
	{
		cemuLog_logDebug(LogType::Force, "IM_GetRuntimeParameter()");

		ppcDefineParamS32(parameterId, 0);
		ppcDefineParamStructPtr(output, void, 1);

		if (parameterId == 8)
		{
			// indicates if last session was ended due to auto-power-down
			*(uint32be*)output = 0;
		}
		else
		{
			cemu_assert_unimplemented();
		}

		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	void coreinitExport_IM_GetHomeButtonParams(PPCInterpreter_t* hCPU)
	{
		debug_printf("IM_GetHomeButtonParams(...)\n");
		ppcDefineParamS32(imObj, 0);
		ppcDefineParamMPTR(ipcBuf, 1);
		ppcDefineParamMPTR(paramOut, 2);
		ppcDefineParamS32(uknR6, 3);
		ppcDefineParamS32(uknR7, 4);

		// todo
		// note: No idea what these values mean. But they were chosen so that the Browser (surf.rpx) does not OSPanic()
		memory_writeU32(paramOut + 0x0, 0);
		memory_writeU32(paramOut + 0x4, 0);

		// for scope.rpx (Download Manager)
		//memory_writeU32(paramOut + 0x0, 1);
		//memory_writeU32(paramOut + 0x4, 2); // some sort of index (starting at 1?)


		osLib_returnFromFunction(hCPU, IM_ERROR_NONE);
	}

	// The Wii U Menu subscribes asynchronously to the console's IM events.
	constexpr uint32 IM_EVENT_SYNC = 1u << 7;
	constexpr sint32 IM_HANDLE = 1;
	struct IMNotification
	{
		MPTR event{};
		MPTR callback{};
		MPTR context{};
	};
	static std::mutex s_imMutex;
	static IMNotification s_imNotification;

	void ResetIM()
	{
		std::lock_guard lock(s_imMutex);
		s_imNotification = {};
	}

	bool PressConsoleSync()
	{
		IMNotification request;
		{
			std::lock_guard lock(s_imMutex);
			request = std::exchange(s_imNotification, {});
		}
		if (!request.callback)
		{
			cemuLog_log(LogType::Force, "Console SYNC pressed without an active IM subscription");
			return false;
		}
		cemuLog_log(LogType::Force, "Console SYNC delivered to IM callback {:08x}", request.callback);
		memory_writeU32(request.event, IM_EVENT_SYNC);
		coreinitAsyncCallback_add(request.callback, 2, IM_ERROR_NONE, request.context);
		return true;
	}

	static sint32 IM_Open()
	{
		return IM_HANDLE;
	}

	static sint32 IM_Close(sint32 handle)
	{
		if (handle != IM_HANDLE)
			return -4;
		ResetIM();
		return IM_ERROR_NONE;
	}

	static sint32 IM_GetEventNotify(sint32 handle, MPTR request, MPTR event, MPTR callback, MPTR context)
	{
		if (handle != IM_HANDLE || !request || !event || !callback)
			return -4;
		std::lock_guard lock(s_imMutex);
		if (s_imNotification.callback)
			return -8;
		s_imNotification = {event, callback, context};
		cemuLog_log(LogType::Force, "IM event subscription: output {:08x}, callback {:08x}", event, callback);
		return IM_ERROR_NONE;
	}

	static sint32 IM_CancelGetEventNotify(sint32 handle, MPTR request, MPTR callback, MPTR context)
	{
		if (handle != IM_HANDLE || !request)
			return -4;
		ResetIM();
		if (callback)
			coreinitAsyncCallback_add(callback, 2, IM_ERROR_NONE, context);
		return IM_ERROR_NONE;
	}

	void InitializeIM()
	{
		ResetIM();
		cafeExportRegister("coreinit", IM_Open, LogType::Placeholder);
		cafeExportRegister("coreinit", IM_Close, LogType::Placeholder);
		cafeExportRegister("coreinit", IM_GetEventNotify, LogType::Placeholder);
		cafeExportRegister("coreinit", IM_CancelGetEventNotify, LogType::Placeholder);
		osLib_addFunction("coreinit", "IMIsAPDEnabledBySysSettings", coreinitExport_IMIsAPDEnabledBySysSettings);
		osLib_addFunction("coreinit", "IMGetTimeBeforeAPD", coreinitExport_IMGetTimeBeforeAPD);
		osLib_addFunction("coreinit", "IMGetTimeBeforeDimming", coreinitExport_IMGetTimeBeforeDimming);
		osLib_addFunction("coreinit", "IMEnableDim", coreinitExport_IMEnableDim);
		osLib_addFunction("coreinit", "IMIsDimEnabled", coreinitExport_IMIsDimEnabled);
		osLib_addFunction("coreinit", "IMGetAPDPeriod", coreinitExport_IMGetAPDPeriod);
		osLib_addFunction("coreinit", "IM_GetHomeButtonParams", coreinitExport_IM_GetHomeButtonParams);
		osLib_addFunction("coreinit", "IM_GetParameter", coreinitExport_IM_GetParameter);
		osLib_addFunction("coreinit", "IM_GetRuntimeParameter", coreinitExport_IM_GetRuntimeParameter);
	}

};
