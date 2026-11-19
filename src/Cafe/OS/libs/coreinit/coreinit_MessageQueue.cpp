#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/RPL/rpl.h"

namespace coreinit
{
	void UpdateSystemMessageQueue();
	void HandleReceivedSystemMessage(OSMessage* msg);

	CafeStaticVar<OSMessageQueue> g_systemMessageQueue{coreinit::g_coreinitStaticData};
	CafeStaticVar<OSMessage, 16> _systemMessageQueueArray{coreinit::g_coreinitStaticData};

	void OSInitMessageQueueEx(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount, void* userData)
	{
		msgQueue->magic = 'mSgQ';
		msgQueue->userData = userData;
		msgQueue->msgArray = msgArray;
		msgQueue->msgCount = msgCount;
		msgQueue->firstIndex = 0;
		msgQueue->usedCount = 0;
		msgQueue->ukn08 = 0;
		OSInitThreadQueueEx(&msgQueue->threadQueueReceive, msgQueue);
		OSInitThreadQueueEx(&msgQueue->threadQueueSend, msgQueue);
	}

	void OSInitMessageQueue(OSMessageQueue* msgQueue, OSMessage* msgArray, uint32 msgCount)
	{
		OSInitMessageQueueEx(msgQueue, msgArray, msgCount, nullptr);
	}

	bool OSReceiveMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		bool isSystemMessageQueue = (msgQueue == g_systemMessageQueue);
		if(isSystemMessageQueue)
			UpdateSystemMessageQueue();
		__OSLockScheduler(msgQueue);
		while (msgQueue->usedCount == (uint32be)0)
		{
			if ((flags & OS_MESSAGE_BLOCK))
			{
				msgQueue->threadQueueReceive.queueAndWait(OSGetCurrentThread());
			}
			else
			{
				__OSUnlockScheduler(msgQueue);
				return false;
			}
		}
		// copy message
		sint32 messageIndex = msgQueue->firstIndex;
		OSMessage* readMsg = &(msgQueue->msgArray[messageIndex]);
		memcpy(msg, readMsg, sizeof(OSMessage));
		msgQueue->firstIndex = ((uint32)msgQueue->firstIndex + 1) % (uint32)(msgQueue->msgCount);
		msgQueue->usedCount = (uint32)msgQueue->usedCount - 1;
		// wake up any thread waiting to add a message
		if (!msgQueue->threadQueueSend.isEmpty())
			msgQueue->threadQueueSend.wakeupSingleThreadWaitQueue(true);
		__OSUnlockScheduler(msgQueue);
		if(isSystemMessageQueue)
			HandleReceivedSystemMessage(msg);
		return true;
	}

	bool OSPeekMessage(OSMessageQueue* msgQueue, OSMessage* msg)
	{
		__OSLockScheduler(msgQueue);
		if ((msgQueue->usedCount == (uint32be)0))
		{
			__OSUnlockScheduler(msgQueue);
			return false;
		}
		// copy message
		sint32 messageIndex = msgQueue->firstIndex;
		if (msg)
		{
			OSMessage* readMsg = &(msgQueue->msgArray[messageIndex]);
			memcpy(msg, readMsg, sizeof(OSMessage));
		}
		__OSUnlockScheduler(msgQueue);
		return true;
	}

	sint32 OSSendMessage(OSMessageQueue* msgQueue, OSMessage* msg, uint32 flags)
	{
		__OSLockScheduler();
		while (msgQueue->usedCount >= msgQueue->msgCount)
		{
			if ((flags & OS_MESSAGE_BLOCK))
			{
				msgQueue->threadQueueSend.queueAndWait(OSGetCurrentThread());																  
			}
			else
			{
				__OSUnlockScheduler();
				return 0;
			}
		}
		// add message
		if ((flags & OS_MESSAGE_HIGH_PRIORITY))
		{
			// decrease firstIndex
			sint32 newFirstIndex = (sint32)((sint32)msgQueue->firstIndex + (sint32)msgQueue->msgCount - 1) % (sint32)msgQueue->msgCount;
			msgQueue->firstIndex = newFirstIndex;
			// insert message at new first index
			msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
			OSMessage* newMsg = &(msgQueue->msgArray[newFirstIndex]);
			memcpy(newMsg, msg, sizeof(OSMessage));
		}
		else
		{
			sint32 messageIndex = (uint32)(msgQueue->firstIndex + msgQueue->usedCount) % (uint32)msgQueue->msgCount;
			msgQueue->usedCount = (uint32)msgQueue->usedCount + 1;
			OSMessage* newMsg = &(msgQueue->msgArray[messageIndex]);
			memcpy(newMsg, msg, sizeof(OSMessage));
		}
		// wake up any thread waiting to read a message
		if (!msgQueue->threadQueueReceive.isEmpty())
			msgQueue->threadQueueReceive.wakeupSingleThreadWaitQueue(true);
		__OSUnlockScheduler();
		return 1;
	}

	OSMessageQueue* OSGetSystemMessageQueue()
	{
		return g_systemMessageQueue.GetPtr();
	}

	CafeStaticVar<OSMessageQueue> s_appIOQueue{g_coreinitStaticData};
	CafeStaticVar<OSMessage, 64> s_appIOMessages{g_coreinitStaticData};
	CafeStaticVar<OSThread_t> s_appIOThread{g_coreinitStaticData};
	CafeStaticVar<uint8, 0x10000, 32> s_appIOStack{g_coreinitStaticData};
	bool s_appIOStarted = false;

	static void AppIOThreadEntry(PPCInterpreter_t* cpu)
	{
		OSMessage message{};
		for (;;)
		{
			OSReceiveMessage(s_appIOQueue.GetPtr(), &message, OS_MESSAGE_BLOCK);
			// FSMessage places its function type in the final OSMessage word.
			if (message.data2 != 8 || message.message == 0)
			{
				cemuLog_log(LogType::Force, "AppIO: Unsupported completion type {}", uint32(message.data2));
				continue;
			}
			auto* result = MEMPTR<FSAsyncResult>(uint32(message.message)).GetPtr();
			// Copy before invoking guest code: it may immediately reuse the command block.
			const FSAsyncResult completion = *result;
			if (completion.fsAsyncParamsNew.userCallback)
				PPCCoreCallback(completion.fsAsyncParamsNew.userCallback, completion.fsClient.GetPtr(),
					completion.fsCmdBlock.GetPtr(), sint32(uint32(completion.fsStatusNew)),
					completion.fsAsyncParamsNew.userContext);
		}
	}

	OSMessageQueue* OSGetDefaultAppIOQueue()
	{
		__OSLockScheduler();
		if (!s_appIOStarted)
		{
			const auto size = s_appIOStack.GetByteSize();
			s_appIOStarted = __OSCreateThreadType(s_appIOThread.GetPtr(),
				RPLLoader_MakePPCCallable(AppIOThreadEntry), 0, nullptr,
				s_appIOStack.GetPtr() + size, size, 16, 7, OSThread_t::THREAD_TYPE::TYPE_APP);
			if (s_appIOStarted)
				OSResumeThread(s_appIOThread.GetPtr());
			else
				cemuLog_log(LogType::Force, "AppIO: Could not create completion thread");
		}
		__OSUnlockScheduler();
		return s_appIOStarted ? s_appIOQueue.GetPtr() : nullptr;
	}

	void InitializeMessageQueue()
	{
		s_appIOStarted = false;
		OSInitMessageQueue(s_appIOQueue.GetPtr(), s_appIOMessages.GetPtr(), s_appIOMessages.GetCount());
		cafeExportRegister("coreinit", OSGetDefaultAppIOQueue, LogType::CoreinitThread);
		OSInitMessageQueue(g_systemMessageQueue.GetPtr(), _systemMessageQueueArray.GetPtr(), _systemMessageQueueArray.GetCount());

		cafeExportRegister("coreinit", OSInitMessageQueueEx, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSInitMessageQueue, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSReceiveMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSPeekMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSSendMessage, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSGetSystemMessageQueue, LogType::CoreinitThread);
	}
};

