#include "iosu_ioctl.h"
#include "iosu_mcp.h"
#include "Cafe/OS/libs/nn_common.h"

#include "config/CemuConfig.h"
#include "config/LaunchSettings.h"
#include "config/ActiveSettings.h"
#include "Cafe/Aroma/WupsLoader.h"
#include "Cafe/TitleList/GameInfo.h"
#include "util/helpers/helpers.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/IOSU/iosu_ipc_common.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Common/CafeStaticVar.h"

using namespace iosu::kernel;

namespace iosu
{
	struct
	{
		bool isInitialized;
	}iosuMcp = { 0 };

	std::recursive_mutex sTitleInfoMutex;
	constinit IOSUStaticData s_mcpStaticData;

	bool sHasAllTitlesMounted = false;

	uint32 mcpBuildTitleList(MCPTitleInfo* titleList, uint32 maxTitlesToCopy, std::function<bool(const TitleInfo&)> filterFunc)
	{
		CafeTitleList::WaitForMandatoryScan();
		std::set<TitleId> titleIdsVisited;
		auto cafeTitleList = CafeTitleList::AcquireInternalList();

		uint32 numTitlesCopied = 0;
		for (auto& it : cafeTitleList)
		{
			if (numTitlesCopied == maxTitlesToCopy)
				break;
			uint64 titleId = it->GetAppTitleId();
			if (titleIdsVisited.find(titleId) != titleIdsVisited.end())
				continue;
			if (!filterFunc(*it))
				continue;
			titleIdsVisited.emplace(titleId);
			if (titleList)
			{
				auto& titleOut = titleList[numTitlesCopied];
				titleOut.titleIdHigh = (uint32)(titleId >> 32);
				titleOut.titleIdLow = (uint32)(titleId & 0xFFFFFFFF);
				titleOut.titleVersion = it->GetAppTitleVersion();
				titleOut.appGroup = it->GetAppGroup();
				titleOut.appType = it->GetAppType();

				std::string titlePath = CafeSystem::GetMlcStoragePath(titleId);
				strcpy(titleOut.appPath, titlePath.c_str());

				strcpy((char*)titleOut.deviceName, "mlc");

				titleOut.osVersion = 0; // todo
				titleOut.sdkVersion = it->GetAppSDKVersion();
			}

			numTitlesCopied++;
		}
		CafeTitleList::ReleaseInternalList();

		if (!sHasAllTitlesMounted)
		{
			CafeSystem::MlcStorageMountAllTitles();
			sHasAllTitlesMounted = true;
		}

		return numTitlesCopied;
	}

	sint32 mcpGetTitleList(MCPTitleInfo* titleList, uint32 titleListBufferSize, uint32be* titleCount)
	{
		std::unique_lock _lock(sTitleInfoMutex);
		uint32 maxEntryCount = titleListBufferSize / sizeof(MCPTitleInfo);
		*titleCount = mcpBuildTitleList(titleList, maxEntryCount, [](const TitleInfo& titleInfo) -> bool { return true; });
		return 0;
	}

	sint32 mcpGetTitleCount()
	{
		std::unique_lock _lock(sTitleInfoMutex);
		return mcpBuildTitleList(nullptr, 0xFFFFFFFF, [](const TitleInfo& titleInfo) -> bool { return true; });
	}

	sint32 mcpGetTitleListByAppType(MCPTitleInfo* titleList, uint32 titleListBufferSize, uint32be* titleCount, uint32 appType)
	{
		std::unique_lock _lock(sTitleInfoMutex);
		uint32 maxEntryCount = titleListBufferSize / sizeof(MCPTitleInfo);
		*titleCount = mcpBuildTitleList(titleList, maxEntryCount, [appType](const TitleInfo& titleInfo) -> bool { return titleInfo.GetAppType() == appType; });
		return 0;
	}

	sint32 mcpGetTitleListByTitleId(MCPTitleInfo* titleList, uint32 titleListBufferSize, uint32be* titleCount, uint64 titleId)
	{
		std::unique_lock _lock(sTitleInfoMutex);
		uint32 maxEntryCount = titleListBufferSize / sizeof(MCPTitleInfo);
		*titleCount = mcpBuildTitleList(titleList, maxEntryCount, [titleId](const TitleInfo& titleInfo) -> bool { return titleInfo.GetAppTitleId() == titleId; });
		return 0;
	}

	int iosuMcp_thread()
	{
		SetThreadName("iosuMcp_thread");
		while (true)
		{
			uint32 returnValue = 0; // Ioctl return value
			ioQueueEntry_t* ioQueueEntry = iosuIoctl_getNextWithWait(IOS_DEVICE_MCP);
			if (ioQueueEntry->request == IOSU_MCP_REQUEST_CEMU)
			{
				iosuMcpCemuRequest_t* mcpCemuRequest = (iosuMcpCemuRequest_t*)ioQueueEntry->bufferVectors[0].buffer.GetPtr();
				if (mcpCemuRequest->requestCode == IOSU_MCP_GET_TITLE_LIST)
				{
					uint32be titleCount = mcpCemuRequest->titleListRequest.titleCount;
					mcpCemuRequest->returnCode = mcpGetTitleList(mcpCemuRequest->titleListRequest.titleList.GetPtr(), mcpCemuRequest->titleListRequest.titleListBufferSize, &titleCount);
					mcpCemuRequest->titleListRequest.titleCount = titleCount;
				}
				else if (mcpCemuRequest->requestCode == IOSU_MCP_GET_TITLE_LIST_BY_APP_TYPE)
				{
					uint32be titleCount = mcpCemuRequest->titleListRequest.titleCount;
					mcpCemuRequest->returnCode = mcpGetTitleListByAppType(mcpCemuRequest->titleListRequest.titleList.GetPtr(), mcpCemuRequest->titleListRequest.titleListBufferSize, &titleCount, mcpCemuRequest->titleListRequest.appType);
					mcpCemuRequest->titleListRequest.titleCount = titleCount;
				}
				else if (mcpCemuRequest->requestCode == IOSU_MCP_GET_TITLE_INFO)
				{
					uint32be titleCount = mcpCemuRequest->titleListRequest.titleCount;
					mcpCemuRequest->returnCode = mcpGetTitleListByTitleId(mcpCemuRequest->titleListRequest.titleList.GetPtr(), mcpCemuRequest->titleListRequest.titleListBufferSize, &titleCount, mcpCemuRequest->titleListRequest.titleId);
					mcpCemuRequest->titleListRequest.titleCount = titleCount;
				}
				else if (mcpCemuRequest->requestCode == IOSU_MCP_GET_TITLE_COUNT)
				{
					uint32be titleCount = mcpGetTitleCount();
					mcpCemuRequest->returnCode = titleCount;
					mcpCemuRequest->titleListRequest.titleCount = titleCount;
				}
				else
					assert_dbg();
			}
			else
				assert_dbg();
			iosuIoctl_completeRequest(ioQueueEntry, returnValue);
		}
		return 0;
	}

    // deprecated
	void iosuMcp_init()
	{
		if (iosuMcp.isInitialized)
			return;
		std::thread t(iosuMcp_thread);
		t.detach();
		iosuMcp.isInitialized = true;
	}

	namespace mcp
	{
		IOSMsgQueueId sMCPIoMsgQueue;
		CafeStaticVar<iosu::kernel::IOSMessage, 352> _m_sMCPIoMsgQueueMsgBuffer{s_mcpStaticData};
		std::thread sMCPIoThread;

		struct MCPClient
		{
			std::string workingDirectory;
			bool isAllocated{ false };

			void AllocateAndInitialize()
			{
				isAllocated = true;
				workingDirectory = std::string("/");
			}

			void ReleaseAndCleanup()
			{
				isAllocated = false;
			}
		};

		std::array<MCPClient, 256> sMCPClientArray;

		IOS_ERROR MCPAllocateClient(sint32& indexOut)
		{
			for (size_t i = 0; i < sMCPClientArray.size(); i++)
			{
				if (sMCPClientArray[i].isAllocated)
					continue;
				sMCPClientArray[i].AllocateAndInitialize();
				indexOut = (sint32)i;
				return IOS_ERROR_OK;
			}
			return IOS_ERROR::IOS_ERROR_MAXIMUM_REACHED;
		}

		void MCPIoThread()
		{
			SetThreadName("IOSU-MCP");
			IOSMessage msg;
			while (true)
			{
				IOS_ERROR r = IOS_ReceiveMessage(sMCPIoMsgQueue, &msg, 0);
				cemu_assert(!IOS_ResultIsError(r));
				if (msg == 0)
					return; // shutdown signaled
				IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(msg).GetPtr();
				uint32 clientHandle = (uint32)cmd->devHandle;
				if (cmd->cmdId == IPCCommandId::IOS_OPEN)
				{
					sint32 clientIndex = 0;
					r = MCPAllocateClient(clientIndex);
					if (r != IOS_ERROR_OK)
					{
						IOS_ResourceReply(cmd, r);
						continue;
					}
					IOS_ResourceReply(cmd, (IOS_ERROR)clientIndex);
					continue;
				}
				else if (cmd->cmdId == IPCCommandId::IOS_CLOSE)
				{
					cemu_assert(clientHandle < sMCPClientArray.size());
					sMCPClientArray[clientHandle].ReleaseAndCleanup();
					IOS_ResourceReply(cmd, IOS_ERROR_OK);
					continue;
				}
				else if (cmd->cmdId == IPCCommandId::IOS_IOCTL)
				{
					cemu_assert(clientHandle < sMCPClientArray.size());
					cemu_assert(sMCPClientArray[clientHandle].isAllocated);
					// Mocha's public IPC extension is ioctl 100 (decimal), not 0x100.
					if (LaunchSettings::AromaEnabled() && cmd->args[0] == 100 &&
						uint32(cmd->args[2]) >= 4 && uint32(cmd->args[4]) >= 4 &&
						memory_isAddressRangeAccessible(cmd->args[1], cmd->args[2]) &&
						memory_isAddressRangeAccessible(cmd->args[3], cmd->args[4]))
					{
						auto* input = MEMPTR<uint32be>(cmd->args[1]).GetPtr();
						if (input[0] == 0xfc && uint32(cmd->args[2]) >= 272)
						{
							bool prepared = false;
							if (input[1] == 0x42424242)
							{
								Aroma::CancelPreparedHomebrewRPX();
								prepared = true;
							}
							else if (input[1] == 0)
							{
								const auto* path = reinterpret_cast<const char*>(input + 4);
								const auto* end = static_cast<const char*>(memchr(path, 0, 256));
								prepared = end && Aroma::PrepareHomebrewRPX(std::string_view(path, end - path), input[3], input[2]);
							}
							IOS_ResourceReply(cmd, prepared ? IOS_ERROR_OK : IOS_ERROR_INVALID);
							continue;
						}
						if (input[0] == 0xf9) // IPC_CUSTOM_COPY_ENVIRONMENT_PATH
						{
							std::error_code ec;
							const auto directory = LaunchSettings::AromaModuleDirectory();
							const auto environment = directory ? directory->parent_path() : LaunchSettings::AromaEnvironmentDirectory();
							const auto relative = fs::relative(environment, ActiveSettings::GetUserDataPath("sdcard"), ec);
							const auto relativeText = relative.generic_string();
							const std::string path = "fs:/vol/external01/" + relativeText;
							if (!ec && !relativeText.empty() && relativeText != ".." && !relativeText.starts_with("../") &&
								path.size() + 1 <= uint32(cmd->args[4]))
							{
								memcpy(MEMPTR<void>(cmd->args[3]).GetPtr(), path.c_str(), path.size() + 1);
								IOS_ResourceReply(cmd, IOS_ERROR_OK);
								continue;
							}
						}
						if (input[0] == 0xf8 && uint32(cmd->args[4]) >= 8) // IPC_CUSTOM_GET_MOCHA_API_VERSION
						{
							auto* output = MEMPTR<uint32be>(cmd->args[3]).GetPtr();
							output[0] = 0xcafebabe;
							output[1] = 1;
							IOS_ResourceReply(cmd, IOS_ERROR_OK);
							continue;
						}
					}
					IOS_ResourceReply(cmd, (IOS_ERROR)0x80000000);
					continue;
				}
				else if (cmd->cmdId == IPCCommandId::IOS_IOCTLV)
				{
					cemu_assert_unimplemented();
					//uint32 requestId = cmd->args[0];
					//uint32 numIn = cmd->args[1];
					//uint32 numOut = cmd->args[2];
					//IPCIoctlVector* vec = MEMPTR<IPCIoctlVector>{ cmd->args[3] }.GetPtr();
					IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
					continue;
				}
				else
				{
					cemuLog_log(LogType::Force, "/dev/mcp: Unsupported IPC cmdId");
					cemu_assert_suspicious();
					IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
				}
			}
		}

		void Init()
		{
			s_mcpStaticData.Allocate();
			for (auto& it : sMCPClientArray)
				it.ReleaseAndCleanup();
			sMCPIoMsgQueue = (IOSMsgQueueId)IOS_CreateMessageQueue(_m_sMCPIoMsgQueueMsgBuffer.GetPtr(), _m_sMCPIoMsgQueueMsgBuffer.GetCount());
			IOS_ERROR r = IOS_RegisterResourceManager("/dev/mcp", sMCPIoMsgQueue);
			IOS_DeviceAssociateId("/dev/mcp", 11);
			if (LaunchSettings::AromaEnabled())
			{
				// libmocha opens this capability endpoint before negotiating with MCP.
				// Reuse MCP client lifetime/dispatch; unsupported hax ioctls still fail.
				IOS_RegisterResourceManager("/dev/iosuhax", sMCPIoMsgQueue);
				IOS_DeviceAssociateId("/dev/iosuhax", 11);
			}
			cemu_assert(!IOS_ResultIsError(r));
			sMCPIoThread = std::thread(MCPIoThread);
		}

		void Shutdown()
		{
			IOS_SendMessage(sMCPIoMsgQueue, 0, 0);
			sMCPIoThread.join();
			IOS_DestroyMessageQueue(sMCPIoMsgQueue);
			s_mcpStaticData.Deallocate();
		}
	};
}
