#include "GDBStub.h"
#include "Debugger.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "GDBBreakpoints.h"
#include "util/helpers/helpers.h"
#include "util/ThreadPool/ThreadPool.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/libs/coreinit/coreinit_Scheduler.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "Cafe/HW/Espresso/EspressoISA.h"
#include "Common/socket.h"

#if BOOST_OS_UNIX
#include <netinet/tcp.h>
#endif

#if BOOST_OS_LINUX
#include <sys/prctl.h>
#endif

#define GET_THREAD_ID(threadPtr) memory_getVirtualOffsetFromPointer(threadPtr)
#define GET_THREAD_BY_ID(threadId) (OSThread_t*)memory_getPointerFromPhysicalOffset(threadId)

static std::vector<MPTR> findNextInstruction(MPTR currAddress, uint32 lr, uint32 ctr)
{
	using namespace Espresso;

	uint32 nextInstr = memory_readU32(currAddress);
	if (GetPrimaryOpcode(nextInstr) == PrimaryOpcode::B)
	{
		uint32 LI;
		bool AA, LK;
		decodeOp_B(nextInstr, LI, AA, LK);
		if (!AA)
			LI += currAddress;
		return {LI};
	}
	if (GetPrimaryOpcode(nextInstr) == PrimaryOpcode::BC)
	{
		uint32 BD, BI;
		BOField BO{};
		bool AA, LK;
		decodeOp_BC(nextInstr, BD, BO, BI, AA, LK);
		if (!AA)
			BD += currAddress;
		return {currAddress + 4, BD};
	}
	if (GetPrimaryOpcode(nextInstr) == PrimaryOpcode::GROUP_19 && GetGroup19Opcode(nextInstr) == Opcode19::BCLR)
	{
		return {currAddress + 4, lr & ~3u};
	}
	if (GetPrimaryOpcode(nextInstr) == PrimaryOpcode::GROUP_19 && GetGroup19Opcode(nextInstr) == Opcode19::BCCTR)
	{
		return {currAddress + 4, ctr & ~3u};
	}
	return {currAddress + 4};
}

// Scheduler lock protects this set. Resume only suspensions owned by this debugger.
static std::set<MPTR> s_debuggerSuspendedThreads;

static void suspendDebugThread(OSThread_t* thread)
{
	if (s_debuggerSuspendedThreads.insert(GET_THREAD_ID(thread)).second)
		coreinit::__OSSuspendThreadNolock(thread);
}

static void resumeDebugThread(OSThread_t* thread)
{
	if (s_debuggerSuspendedThreads.erase(GET_THREAD_ID(thread)))
		coreinit::__OSResumeThreadInternal(thread, 1);
}

template<typename F>
static void selectThread(sint64 selectorId, F&& action_for_thread)
{
	__OSLockScheduler();
	if (activeThreadCount == 0)
	{
		__OSUnlockScheduler();
		return;
	}

	if (selectorId == -1)
	{
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			action_for_thread(GET_THREAD_BY_ID(activeThread[i]));
		}
	}
	else if (selectorId == 0)
	{
		// Use first thread if attempted to be stopped
		// todo: would this work better if it used main?
		action_for_thread(coreinit::OSGetDefaultThread(1));
	}
	else if (selectorId > 0)
	{
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			auto* thread = GET_THREAD_BY_ID(activeThread[i]);
			if (GET_THREAD_ID(thread) == selectorId)
			{
				action_for_thread(thread);
				break;
			}
		}
	}
	__OSUnlockScheduler();
}

template<typename F>
static void selectAndBreakThread(sint64 selectorId, F&& action_for_thread)
{
	__OSLockScheduler();
	if (activeThreadCount == 0)
	{
		__OSUnlockScheduler();
		return;
	}

	std::vector<OSThread_t*> pausedThreads;
	if (selectorId == -1)
	{
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			suspendDebugThread(GET_THREAD_BY_ID(activeThread[i]));
			pausedThreads.emplace_back(GET_THREAD_BY_ID(activeThread[i]));
		}
	}
	else if (selectorId == 0)
	{
		// Use first thread if attempted to be stopped
		OSThread_t* thread = GET_THREAD_BY_ID(activeThread[0]);
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			if (GET_THREAD_ID(GET_THREAD_BY_ID(activeThread[i])) < GET_THREAD_ID(thread))
			{
				thread = GET_THREAD_BY_ID(activeThread[i]);
			}
		}
		suspendDebugThread(thread);
		pausedThreads.emplace_back(thread);
	}
	else if (selectorId > 0)
	{
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			auto* thread = GET_THREAD_BY_ID(activeThread[i]);
			if (GET_THREAD_ID(thread) == selectorId)
			{
				suspendDebugThread(thread);
				pausedThreads.emplace_back(thread);
				break;
			}
		}
	}
	__OSUnlockScheduler();

	for (OSThread_t* thread : pausedThreads)
	{
		while (coreinit::OSIsThreadRunning(thread))
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		action_for_thread(thread);
	}
}

static void selectAndResumeThread(sint64 selectorId)
{
	__OSLockScheduler();
	if (activeThreadCount == 0)
	{
		__OSUnlockScheduler();
		return;
	}

	if (selectorId == -1)
	{
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			resumeDebugThread(GET_THREAD_BY_ID(activeThread[i]));
		}
	}
	else if (selectorId == 0)
	{
		// Use first thread if attempted to be stopped
		resumeDebugThread(coreinit::OSGetDefaultThread(1));
	}
	else if (selectorId > 0)
	{
		for (sint32 i = 0; i < activeThreadCount; i++)
		{
			auto* thread = GET_THREAD_BY_ID(activeThread[i]);
			if (GET_THREAD_ID(thread) == selectorId)
			{
				resumeDebugThread(thread);
				break;
			}
		}
	}
	__OSUnlockScheduler();
}

static void waitForBrokenThreads(std::unique_ptr<GDBServer::CommandContext> context, std::string_view reason)
{
	// This should pause all threads except trapped thread
	// It should however wait for the trapped thread
	// The trapped thread should be paused by the trap word instruction handler (aka the running thread)
	std::vector<OSThread_t*> threadsList;
	__OSLockScheduler();
	for (sint32 i = 0; i < activeThreadCount; i++)
	{
		threadsList.emplace_back(GET_THREAD_BY_ID(activeThread[i]));
	}
	__OSUnlockScheduler();

	for (OSThread_t* thread : threadsList)
	{
		while (coreinit::OSIsThreadRunning(thread))
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	context->QueueResponse(reason);
}

static void breakThreads(sint64 trappedThread)
{
	__OSLockScheduler();
	if (activeThreadCount == 0)
	{
		__OSUnlockScheduler();
		return;
	}

	// First, break other threads
	OSThread_t* mainThread = nullptr;
	for (sint32 i = 0; i < activeThreadCount; i++)
	{
		if (GET_THREAD_ID(GET_THREAD_BY_ID(activeThread[i])) == trappedThread)
		{
			mainThread = GET_THREAD_BY_ID(activeThread[i]);
		}
		else
		{
			suspendDebugThread(GET_THREAD_BY_ID(activeThread[i]));
		}
	}

	// Second, break trapped thread itself which should also pause execution of this handler
	// This will temporarily lift the scheduler lock until it's resumed from its suspension
	suspendDebugThread(mainThread);

	__OSUnlockScheduler();
}

std::unique_ptr<GDBServer> g_gdbstub;

GDBServer::GDBServer(uint16 port)
	: m_port(port)
{
#if BOOST_OS_WINDOWS
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

GDBServer::~GDBServer()
{
	m_stopRequested = true;
#if BOOST_OS_WINDOWS
	constexpr int how = SD_BOTH;
#else
	constexpr int how = SHUT_RDWR;
#endif
	if (m_client_socket != INVALID_SOCKET)
		shutdown(m_client_socket, how);
	if (m_server_socket != INVALID_SOCKET)
		shutdown(m_server_socket, how);
	if (m_thread.joinable())
		m_thread.join();
	if (m_server_socket != INVALID_SOCKET)
		closesocket(m_server_socket);
#if BOOST_OS_WINDOWS
	WSACleanup();
#endif
}

bool GDBServer::Initialize()
{
	// Title reloads reuse this debugger and its listener. Rebinding would
	// discard the live socket and disable Python debugging during WiiLoad.
	if (m_server_socket != INVALID_SOCKET && m_thread.joinable())
		return true;
	cemuLog_createLogFile(false);

	if (m_server_socket = socket(PF_INET, SOCK_STREAM, 0); m_server_socket == SOCKET_ERROR)
		return false;

	int reuseEnabled = TRUE;
	if (setsockopt(m_server_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseEnabled, sizeof(reuseEnabled)) == SOCKET_ERROR)
	{
		closesocket(m_server_socket);
		m_server_socket = INVALID_SOCKET;
		return false;
	}

	int nodelayEnabled = TRUE;
	if (setsockopt(m_server_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&nodelayEnabled, sizeof(nodelayEnabled)) == SOCKET_ERROR)
	{
		closesocket(m_server_socket);
		m_server_socket = INVALID_SOCKET;
		return false;
	}

	memset(&m_server_addr, 0, sizeof(m_server_addr));
	m_server_addr.sin_family = AF_INET;
	m_server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	m_server_addr.sin_port = htons(m_port);

	if (bind(m_server_socket, (sockaddr*)&m_server_addr, sizeof(m_server_addr)) == SOCKET_ERROR)
	{
		closesocket(m_server_socket);
		m_server_socket = INVALID_SOCKET;
		return false;
	}

	if (listen(m_server_socket, s_maxGDBClients) == SOCKET_ERROR)
	{
		closesocket(m_server_socket);
		m_server_socket = INVALID_SOCKET;
		return false;
	}

#if BOOST_OS_LINUX
	// Scheduler workers can start before the debugger is enabled. Apply the
	// same inspection permission here instead of depending on their startup.
	if (prctl(PR_SET_DUMPABLE, 1ul, 0ul, 0ul, 0ul) != 0 ||
		prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0ul, 0ul, 0ul) != 0)
	{
		cemuLog_log(LogType::Force, "[GDBStub] Could not enable local process inspection");
	}
#endif

	m_thread = std::thread(std::bind(&GDBServer::ThreadFunc, this));

	return true;
}

void GDBServer::SendRaw(std::string_view data) const
{
	std::lock_guard lock(m_sendMutex);
	while (!data.empty())
	{
#if defined(MSG_NOSIGNAL)
		int sent = send(m_client_socket, data.data(), static_cast<int>(data.size()), MSG_NOSIGNAL);
#else
		int sent = send(m_client_socket, data.data(), static_cast<int>(data.size()), 0);
#endif
		if (sent <= 0)
			return;
		data.remove_prefix(sent);
	}
}

void GDBServer::ThreadFunc()
{
	SetThreadName("GDBServer");
	auto readExact = [&](char* buffer, size_t length) {
		while (length)
		{
			int received = recv(m_client_socket, buffer, static_cast<int>(length), 0);
			if (received <= 0)
				throw std::runtime_error("GDB client disconnected");
			buffer += received;
			length -= received;
		}
	};
	auto readChar = [&]() {
		char c;
		readExact(&c, 1);
		return c;
	};
	while (!m_stopRequested)
	{
		if (!m_client_connected)
		{
			socklen_t size = sizeof(m_client_addr);
			m_client_socket = accept(m_server_socket, reinterpret_cast<sockaddr*>(&m_client_addr), &size);
			m_client_connected = m_client_socket != INVALID_SOCKET;
			if (m_client_connected)
				selectAndBreakThread(-1, [](OSThread_t*) {});
			continue;
		}
		try
		{
			char prefix = readChar();
			if (prefix == '\x03')
			{
				std::string status;
				selectThread(0, [&status](OSThread_t* thread) {
					status = fmt::format("T05thread:{:08X};", GET_THREAD_ID(thread));
				});
				if (status.empty())
				{
					SendRaw(fmt::format("${}#{:02x}", RESPONSE_ERROR, CommandContext::CalculateChecksum(RESPONSE_ERROR)));
					continue;
				}
				selectAndBreakThread(-1, [](OSThread_t*) {});
				std::lock_guard runLock(m_runMutex);
				if (m_resumed_context)
				{
					m_resumed_context->QueueResponse(status);
					m_resumed_context.reset();
				}
				else
					SendRaw(fmt::format("${}#{:02x}", status, CommandContext::CalculateChecksum(status)));
			}
			else if (prefix == '$')
			{
				std::string message;
				char c;
				while ((c = readChar()) != '#')
				{
					if (message.size() >= s_maxPacketSize)
						throw std::runtime_error("GDB packet too large");
					message.push_back(c);
				}
				char checksum[2];
				readExact(checksum, sizeof(checksum));
				unsigned int expected = 0;
				auto result = std::from_chars(checksum, checksum + 2, expected, 16);
				if (result.ec != std::errc{} || result.ptr != checksum + 2 || expected != CommandContext::CalculateChecksum(message))
					SendRaw(RESPONSE_NACK);
				else
					HandleCommand(message);
			}
		}
		catch (const std::exception& error)
		{
			cemuLog_logDebug(LogType::Force, "[GDBStub] {}", error.what());
			{
				std::lock_guard runLock(m_runMutex);
				if (m_resumed_context)
					m_resumed_context->CancelResponse();
				m_resumed_context.reset();
			}
			if (!m_stopRequested)
			{
				selectAndBreakThread(-1, [](OSThread_t*) {});
				{
					std::lock_guard breakpointLock(m_breakpointMutex);
					m_patchedInstructions.clear();
					m_watch_point.reset();
				}
				selectAndResumeThread(-1);
			}
			closesocket(m_client_socket);
			m_client_socket = INVALID_SOCKET;
			m_client_connected = false;
		}
	}
	if (m_client_socket != INVALID_SOCKET)
	{
		closesocket(m_client_socket);
		m_client_socket = INVALID_SOCKET;
	}
}

void GDBServer::HandleCommand(const std::string& command_str)
{
	if (command_str.empty())
		return;
	auto context = std::make_unique<CommandContext>(this, command_str);
	if (!context->IsValid())
		return context->QueueResponse(RESPONSE_EMPTY);


	try
	{
		switch (context->GetType())
		{
		// Extended commands
		case CMDType::QUERY_GET:
		case CMDType::QUERY_SET:
			return HandleQuery(context);
		case CMDType::VCONT:
			return HandleVCont(context);
		// Regular commands
		case CMDType::IS_THREAD_RUNNING:
			return CMDIsThreadActive(context);
		case CMDType::SET_ACTIVE_THREAD:
			return CMDSetActiveThread(context);
		case CMDType::ACTIVE_THREAD_STATUS:
			return CMDGetThreadStatus(context);
		case CMDType::CONTINUE:
			return CMDContinue(context);
		case CMDType::DETACH:
			selectAndBreakThread(-1, [](OSThread_t*) {});
			{
				std::lock_guard breakpointLock(m_breakpointMutex);
				m_patchedInstructions.clear();
				m_watch_point.reset();
			}
			{
				std::lock_guard runLock(m_runMutex);
				if (m_resumed_context)
					m_resumed_context->CancelResponse();
				m_resumed_context.reset();
			}
			selectAndResumeThread(-1);
			return context->QueueResponse(RESPONSE_OK);
		case CMDType::ACTIVE_THREAD_STEP:
			break;
		case CMDType::REGISTER_READ:
			return CMDReadRegister(context);
		case CMDType::REGISTER_SET:
			return CMDWriteRegister(context);
		case CMDType::REGISTERS_READ:
			return CMDReadRegisters(context);
		case CMDType::REGISTERS_WRITE:
			return CMDWriteRegisters(context);
		case CMDType::MEMORY_READ:
			return CMDReadMemory(context);
		case CMDType::MEMORY_WRITE:
			return CMDWriteMemory(context);
		case CMDType::BREAKPOINT_SET:
			return CMDInsertBreakpoint(context);
		case CMDType::BREAKPOINT_REMOVE:
			return CMDDeleteBreakpoint(context);
		case CMDType::INVALID:
		default:
			return CMDNotFound(context);
		}

		CMDNotFound(context);
	}
	catch (const std::exception&)
	{
		if (context)
			context->QueueResponse(RESPONSE_ERROR);
	}
}

void GDBServer::HandleQuery(std::unique_ptr<CommandContext>& context) const
{
	if (!context->IsValid())
		return context->QueueResponse(RESPONSE_EMPTY);

	const auto& query_cmd = context->GetArgs()[0];
	const auto& query_args = context->GetArgs().begin() + 1;

	if (query_cmd == "qSupported")
	{
		context->QueueResponse(s_supportedFeatures);
	}
	else if (query_cmd == "qAttached")
	{
		context->QueueResponse("1");
	}
	else if (query_cmd == "qRcmd")
	{
	}
	else if (query_cmd == "qC")
	{
		selectThread(m_activeThreadSelector, [&context](OSThread_t* thread) {
			context->QueueResponse(fmt::format("QC{:x}", GET_THREAD_ID(thread)));
		});
	}
	else if (query_cmd == "qOffsets")
	{
		const auto module_list = RPLLoader_GetModuleList();
		for (sint32 i = 0; i < module_list.size(); i++)
		{
			const RPLModule* rpl = module_list[i];
			if (rpl->entrypoint == m_entry_point)
			{
				context->QueueResponse(fmt::format("TextSeg={:08X};DataSeg={:08X}", rpl->regionMappingBase_text.GetMPTR(), rpl->regionMappingBase_data));
			}
		}
	}
	else if (query_cmd == "qfThreadInfo")
	{
		std::vector<std::string> threadIds;
		selectThread(-1, [&threadIds](OSThread_t* thread) {
			threadIds.emplace_back(fmt::format("{:08X}", memory_getVirtualOffsetFromPointer(thread)));
		});
		context->QueueResponse(fmt::format("m{}", fmt::join(threadIds, ",")));
	}
	else if (query_cmd == "qsThreadInfo")
	{
		context->QueueResponse("l");
	}
	else if (query_cmd == "qXfer")
	{
		auto& type = query_args[0];

		if (type == "features")
		{
			auto& annex = query_args[1];
			sint64 read_offset = std::stoul(query_args[2], nullptr, 16);
			sint64 read_length = std::stoul(query_args[3], nullptr, 16);
			if (annex == "target.xml")
			{
				if (read_offset >= GDBTargetXML.size())
					context->QueueResponse("l");
				else
				{
					auto paginated_str = GDBTargetXML.substr(read_offset, read_length);
					context->QueueResponse((paginated_str.size() == read_length) ? "m" : "l");
					context->QueueResponse(paginated_str);
				}
			}
			else
				cemuLog_logDebug(LogType::Force, "[GDBStub] qXfer:features:read:{} isn't a known feature document", annex);
		}
		else if (type == "threads")
		{
			sint64 read_offset = std::stoul(query_args[1], nullptr, 16);
			sint64 read_length = std::stoul(query_args[2], nullptr, 16);

			std::string threads_res;
			threads_res += R"(<?xml version="1.0"?>)";
			threads_res += "<threads>";
			// note: clion seems to default to the first thread
			std::map<sint64, std::string> threads_list;
			selectThread(-1, [&threads_list](OSThread_t* thread) {
				std::string entry;
				entry += fmt::format(R"(<thread id="{:x}" core="{}")", GET_THREAD_ID(thread), thread->context.upir.value());
				if (!thread->threadName.IsNull())
					entry += fmt::format(R"( name="{}")", CommandContext::EscapeXMLString(thread->threadName.GetPtr()));
				// todo: could add a human-readable description of the thread here
				entry += fmt::format("></thread>");
				threads_list.emplace(GET_THREAD_ID(thread), entry);
			});
			for (auto& entry : threads_list)
			{
				threads_res += entry.second;
			}
			threads_res += "</threads>";

			if (read_offset >= threads_res.size())
				context->QueueResponse("l");
			else
			{
				auto paginated_str = threads_res.substr(read_offset, read_length);
				context->QueueResponse((paginated_str.size() == read_length) ? "m" : "l");
				context->QueueResponse(paginated_str);
			}
		}
		else if (type == "libraries")
		{
			sint64 read_offset = std::stoul(query_args[1], nullptr, 16);
			sint64 read_length = std::stoul(query_args[2], nullptr, 16);

			std::string library_list;
			library_list += R"(<?xml version="1.0"?>)";
			library_list += "<library-list>";

			const auto module_list = RPLLoader_GetModuleList();
			for (sint32 i = 0; i < module_list.size(); i++)
			{
				library_list += fmt::format(R"(<library name="{}"><segment address="{:#x}"/></library>)", CommandContext::EscapeXMLString(module_list[i]->moduleName), module_list[i]->regionMappingBase_text.GetMPTR());
			}
			library_list += "</library-list>";

			if (read_offset >= library_list.size())
				context->QueueResponse("l");
			else
			{
				auto paginated_str = library_list.substr(read_offset, read_length);
				context->QueueResponse((paginated_str.size() == read_length) ? "m" : "l");
				context->QueueResponse(paginated_str);
			}
		}
		else
		{
			context->QueueResponse(RESPONSE_EMPTY);
		}
	}
	else
	{
		context->QueueResponse(RESPONSE_EMPTY);
	}
}

void GDBServer::HandleVCont(std::unique_ptr<CommandContext>& context)
{
	if (!context->IsValid())
	{
		cemuLog_logDebug(LogType::Force, "[GDBStub] Received unsupported vCont command: {}", context->GetCommand());
		// cemu_assert_unimplemented();
		return context->QueueResponse(RESPONSE_EMPTY);
	}

	const std::string& vcont_cmd = context->GetArgs()[0];
	if (vcont_cmd == "vCont?")
		return context->QueueResponse("vCont;c;C;s;S");

	else if (vcont_cmd != "vCont;")
		return context->QueueResponse(RESPONSE_EMPTY);

	const std::string operations = context->GetArgs()[1];
	{
		std::lock_guard runLock(m_runMutex);
		if (m_resumed_context)
			return context->QueueResponse(RESPONSE_ERROR);
		m_resumed_context = std::move(context);
	}

	bool resumedNoThreads = true;
	for (const auto operation : TokenizeView(operations, ';'))
	{
		// todo: this might have issues with the signal versions (C/S)
		// todo: test whether this works with multiple vCont;c:123123;c:123123
		std::string_view operationType = operation.substr(0, operation.find(':'));
		sint64 threadSelector = operationType.size() == operation.size() ? -1 : std::stoll(std::string(operation.substr(operationType.size() + 1)), nullptr, 16);

		if (operationType == "c" || operationType.starts_with("C"))
		{
			selectAndResumeThread(threadSelector);
			resumedNoThreads = false;
		}
		else if (operationType == "s" || operationType.starts_with("S"))
		{
			selectThread(threadSelector, [this](OSThread_t* thread) {
				std::lock_guard breakpointLock(m_breakpointMutex);
				auto nextInstructions = findNextInstruction(thread->context.srr0, CPU_swapEndianU32(thread->context.lr), thread->context.ctr);
				for (MPTR nextInstr : nextInstructions)
				{
					auto bpIt = m_patchedInstructions.find(nextInstr);
					if (bpIt == m_patchedInstructions.end())
						this->m_patchedInstructions.try_emplace(nextInstr, nextInstr, BreakpointType::BP_STEP_POINT, false, "swbreak:;");
					else
						bpIt->second.PauseOnNextInterrupt();
				}
			});
		}
	}

	if (resumedNoThreads)
	{
		selectAndResumeThread(-1);
		cemuLog_logDebug(LogType::Force, "[GDBStub] Resumed all threads after skip instructions");
	}
}

void GDBServer::CMDContinue(std::unique_ptr<CommandContext>& context)
{
	{
		std::lock_guard runLock(m_runMutex);
		if (m_resumed_context)
			return context->QueueResponse(RESPONSE_ERROR);
		m_resumed_context = std::move(context);
	}
	selectAndResumeThread(m_activeThreadContinueSelector);
}

void GDBServer::CMDNotFound(std::unique_ptr<CommandContext>& context)
{
	return context->QueueResponse(RESPONSE_EMPTY);
}

void GDBServer::CMDIsThreadActive(std::unique_ptr<CommandContext>& context)
{
	sint64 threadSelector = std::stoll(context->GetArgs()[1], nullptr, 16);
	bool foundThread = false;
	selectThread(threadSelector, [&foundThread](OSThread_t* thread) {
		foundThread = true;
	});

	if (foundThread)
		return context->QueueResponse(RESPONSE_OK);
	else
		return context->QueueResponse(RESPONSE_ERROR);
}

void GDBServer::CMDSetActiveThread(std::unique_ptr<CommandContext>& context)
{
	sint64 threadSelector = std::stoll(context->GetArgs()[2], nullptr, 16);
	if (threadSelector >= 0)
	{
		bool foundThread = false;
		selectThread(threadSelector, [&foundThread](OSThread_t* thread) {
			foundThread = true;
		});
		if (!foundThread)
			return context->QueueResponse(RESPONSE_ERROR);
	}
	if (context->GetArgs()[1] == "c")
		m_activeThreadContinueSelector = threadSelector;
	else
		m_activeThreadSelector = threadSelector;
	return context->QueueResponse(RESPONSE_OK);
}

void GDBServer::CMDGetThreadStatus(std::unique_ptr<CommandContext>& context)
{
	selectThread(0, [&context](OSThread_t* thread) {
		context->QueueResponse(fmt::format("T05thread:{:08X};", memory_getVirtualOffsetFromPointer(thread)));
	});
}

void GDBServer::CMDReadRegister(std::unique_ptr<CommandContext>& context) const
{
	sint32 reg = std::stoi(context->GetArgs()[1], nullptr, 16);
	selectThread(m_activeThreadSelector, [reg, &context](OSThread_t* thread) {
		auto& cpu = thread->context;
		if (reg >= RegisterID::R0_START && reg <= RegisterID::R31_END)
		{
			return context->QueueResponse(fmt::format("{:08X}", CPU_swapEndianU32(cpu.gpr[reg])));
		}
		else if (reg >= RegisterID::F0_START && reg <= RegisterID::F31_END)
		{
			return context->QueueResponse(fmt::format("{:016X}", cpu.fp_ps0[reg - RegisterID::F0_START].value()));
		}
		else if (reg == RegisterID::FPSCR)
		{
			return context->QueueResponse(fmt::format("{:08X}", cpu.fpscr.fpscr.value()));
		}
		else
		{
			switch (reg)
			{
			case RegisterID::PC: return context->QueueResponse(fmt::format("{:08X}", cpu.srr0));
			case RegisterID::MSR: return context->QueueResponse("xxxxxxxx");
			case RegisterID::CR: return context->QueueResponse(fmt::format("{:08X}", cpu.cr));
			case RegisterID::LR: return context->QueueResponse(fmt::format("{:08X}", CPU_swapEndianU32(cpu.lr)));
			case RegisterID::CTR: return context->QueueResponse(fmt::format("{:08X}", cpu.ctr));
			case RegisterID::XER: return context->QueueResponse(fmt::format("{:08X}", cpu.xer));
			default: break;
			}
		}
	});
}

void GDBServer::CMDWriteRegister(std::unique_ptr<CommandContext>& context) const
{
	sint32 reg = std::stoi(context->GetArgs()[1], nullptr, 16);
	uint64 value = std::stoull(context->GetArgs()[2], nullptr, 16);
	selectThread(m_activeThreadSelector, [reg, value, &context](OSThread_t* thread) {
		auto& cpu = thread->context;
		if (reg >= RegisterID::R0_START && reg <= RegisterID::R31_END)
		{
			cpu.gpr[reg] = CPU_swapEndianU32(value);
			return context->QueueResponse(RESPONSE_OK);
		}
		else if (reg >= RegisterID::F0_START && reg <= RegisterID::F31_END)
		{
			// todo: figure out how to properly write to paired single registers
			cpu.fp_ps0[reg - RegisterID::F0_START] = uint64be{value};
			return context->QueueResponse(RESPONSE_OK);
		}
		else if (reg == RegisterID::FPSCR)
		{
			cpu.fpscr.fpscr = uint32be{(uint32)value};
			return context->QueueResponse(RESPONSE_OK);
		}
		else
		{
			switch (reg)
			{
			case RegisterID::PC:
				cpu.srr0 = value;
				return context->QueueResponse(RESPONSE_OK);
			case RegisterID::MSR:
				return context->QueueResponse(RESPONSE_ERROR);
			case RegisterID::CR:
				cpu.cr = value;
				return context->QueueResponse(RESPONSE_OK);
			case RegisterID::LR:
				cpu.lr = CPU_swapEndianU32(value);
				return context->QueueResponse(RESPONSE_OK);
			case RegisterID::CTR:
				cpu.ctr = value;
				return context->QueueResponse(RESPONSE_OK);
			case RegisterID::XER:
				cpu.xer = value;
				return context->QueueResponse(RESPONSE_OK);
			default:
				return context->QueueResponse(RESPONSE_ERROR);
			}
		}
	});
}

void GDBServer::CMDReadRegisters(std::unique_ptr<CommandContext>& context) const
{
	selectThread(m_activeThreadSelector, [&context](OSThread_t* thread) {
		auto& cpu = thread->context;
		for (uint32 reg : cpu.gpr)
			context->QueueResponse(fmt::format("{:08X}", CPU_swapEndianU32(reg)));
		for (auto& reg : cpu.fp_ps0)
			context->QueueResponse(fmt::format("{:016X}", reg.value()));
		context->QueueResponse(fmt::format("{:08X}xxxxxxxx{:08X}{:08X}{:08X}{:08X}{:08X}",
			cpu.srr0, cpu.cr, CPU_swapEndianU32(cpu.lr), cpu.ctr, cpu.xer, cpu.fpscr.fpscr.value()));
	});
}

void GDBServer::CMDWriteRegisters(std::unique_ptr<CommandContext>& context) const
{
	const auto& data = context->GetArgs()[1];
	// 32 GPRs, 32 doubles, then PC/MSR/CR/LR/CTR/XER/FPSCR, in register-number order.
	if (data.size() != 824)
		return context->QueueResponse(RESPONSE_ERROR);
	std::array<uint64, 71> values{};
	size_t offset = 0;
	for (size_t i = 0; i < values.size(); ++i)
	{
		size_t width = (i >= 32 && i < 64) ? 16 : 8;
		if (i != RegisterID::MSR) // MSR is unavailable and may be encoded as xxxxxxxx.
		{
			auto result = std::from_chars(data.data() + offset, data.data() + offset + width, values[i], 16);
			if (result.ec != std::errc{} || result.ptr != data.data() + offset + width)
				return context->QueueResponse(RESPONSE_ERROR);
		}
		offset += width;
	}
	selectThread(m_activeThreadSelector, [&context, &values](OSThread_t* thread) {
		auto& cpu = thread->context;
		for (size_t i = 0; i < 32; ++i)
		{
			cpu.gpr[i] = CPU_swapEndianU32(static_cast<uint32>(values[i]));
			cpu.fp_ps0[i] = values[i + 32];
		}
		cpu.srr0 = values[RegisterID::PC];
		cpu.cr = values[RegisterID::CR];
		cpu.lr = CPU_swapEndianU32(static_cast<uint32>(values[RegisterID::LR]));
		cpu.ctr = values[RegisterID::CTR];
		cpu.xer = values[RegisterID::XER];
		cpu.fpscr.fpscr = static_cast<uint32>(values[RegisterID::FPSCR]);
		context->QueueResponse(RESPONSE_OK);
	});
}

void GDBServer::CMDReadMemory(std::unique_ptr<CommandContext>& context)
{
	std::lock_guard breakpointLock(m_breakpointMutex);
	uint64 addr = std::stoull(context->GetArgs()[1], nullptr, 16);
	uint64 length = std::stoull(context->GetArgs()[2], nullptr, 16);

	// todo: handle cross-mmu-range memory requests
	if (length <= 0 || length > s_maxPacketSize / 2 || addr > UINT32_MAX || length > UINT32_MAX - addr || !memory_isAddressRangeAccessible(addr, length))
		return context->QueueResponse(RESPONSE_ERROR);

	std::string memoryRepr;
	uint8* values = memory_getPointerFromVirtualOffset(addr);
	for (uint64 i = 0; i < length; i++)
	{
		memoryRepr += fmt::format("{:02X}", values[i]);
	}

	// A read may begin inside an instruction; return all original bytes covered by breakpoints.
	for (uint64 i = 0; i < length; ++i)
	{
		auto bp = m_patchedInstructions.find(static_cast<MPTR>((addr + i) & ~3ull));
		if (bp != m_patchedInstructions.end())
		{
			uint32 shift = (3 - ((addr + i) & 3)) * 8;
			auto byte = (bp->second.GetOriginalOpCode() >> shift) & 0xff;
			memoryRepr.replace(i * 2, 2, fmt::format("{:02X}", byte));
		}
	}
	return context->QueueResponse(memoryRepr);
}

void GDBServer::CMDWriteMemory(std::unique_ptr<CommandContext>& context)
{
	std::lock_guard breakpointLock(m_breakpointMutex);
	uint64 addr = std::stoull(context->GetArgs()[1], nullptr, 16);
	uint64 length = std::stoull(context->GetArgs()[2], nullptr, 16);
	const auto& source = context->GetArgs()[3];
	if (length <= 0 || length > s_maxPacketSize / 2 || source.size() != length * 2)
		return context->QueueResponse(RESPONSE_ERROR);

	// todo: handle cross-mmu-range memory requests
	if (length <= 0 || length > s_maxPacketSize / 2 || addr > UINT32_MAX || length > UINT32_MAX - addr || !memory_isAddressRangeAccessible(addr, length))
		return context->QueueResponse(RESPONSE_ERROR);

	uint8* values = memory_getPointerFromVirtualOffset(addr);
	for (uint64 i = 0; i < length; i++)
	{
		uint8 hexValue;
		const std::from_chars_result result = std::from_chars(source.data() + (i * 2), (source.data() + (i * 2) + 2), hexValue, 16);
		if (result.ec == std::errc::invalid_argument || result.ec == std::errc::result_out_of_range)
			return context->QueueResponse(RESPONSE_ERROR);

		if (auto it = m_patchedInstructions.find(static_cast<MPTR>((addr + i) & ~3ull)); it != m_patchedInstructions.end())
		{
			uint32 newOpCode = it->second.GetOriginalOpCode();
			uint32 byteIndex = 3 - ((addr + i) % 4);			// inverted because of big endian, so address 0 is the highest byte
			newOpCode &= ~(0xFF << (byteIndex * 8));			// mask out the byte
			newOpCode |= ((uint32)hexValue << (byteIndex * 8)); // set new byte with OR
			it->second.WriteNewOpCode(newOpCode);
		}
		else
		{
			values[i] = hexValue;
		}
	}
	if (addr < PPC_REC_CODE_AREA_END)
		PPCRecompiler_invalidateRange(static_cast<uint32>(addr), static_cast<uint32>(std::min<uint64>(addr + length, PPC_REC_CODE_AREA_END)));
	return context->QueueResponse(RESPONSE_OK);
}

void GDBServer::CMDInsertBreakpoint(std::unique_ptr<CommandContext>& context)
{
	std::lock_guard breakpointLock(m_breakpointMutex);
	auto type = std::stoul(context->GetArgs()[1], nullptr, 16);
	auto address = std::stoull(context->GetArgs()[2], nullptr, 16);
	auto size = std::stoul(context->GetArgs()[3], nullptr, 16);
	if (type > 4 || size != 4 || address > UINT32_MAX - 4 || (address & 3) || !memory_isAddressRangeAccessible(address, 4))
		return context->QueueResponse(RESPONSE_ERROR);
	MPTR addr = static_cast<MPTR>(address);

	if (type == 0 || type == 1)
	{
		auto bp = this->m_patchedInstructions.find(addr);
		if (bp != this->m_patchedInstructions.end())
			this->m_patchedInstructions.erase(bp);
		this->m_patchedInstructions.try_emplace(addr, addr, BreakpointType::BP_PERSISTENT, type == 0, type == 0 ? "swbreak:;" : "hwbreak:;");
	}
	else if (type == 2 || type == 3 || type == 4)
	{
		if (this->m_watch_point)
			return context->QueueResponse(RESPONSE_ERROR);

		this->m_watch_point = std::make_unique<AccessBreakpoint>(addr, (AccessPointType)type);
	}

	return context->QueueResponse(RESPONSE_OK);
}

void GDBServer::CMDDeleteBreakpoint(std::unique_ptr<CommandContext>& context)
{
	std::lock_guard breakpointLock(m_breakpointMutex);
	auto type = std::stoul(context->GetArgs()[1], nullptr, 16);
	auto address = std::stoull(context->GetArgs()[2], nullptr, 16);
	auto size = std::stoul(context->GetArgs()[3], nullptr, 16);
	if (type > 4 || size != 4 || address > UINT32_MAX - 4 || (address & 3) || !memory_isAddressRangeAccessible(address, 4))
		return context->QueueResponse(RESPONSE_ERROR);
	MPTR addr = static_cast<MPTR>(address);

	if (type == 0 || type == 1)
	{
		auto bp = this->m_patchedInstructions.find(addr);
		if (bp == this->m_patchedInstructions.end() || !bp->second.ShouldBreakThreads())
			return context->QueueResponse(RESPONSE_ERROR);
		else
			this->m_patchedInstructions.erase(bp);
	}
	else if (type == 2 || type == 3 || type == 4)
	{
		if (!this->m_watch_point || this->m_watch_point->GetAddress() != addr)
			return context->QueueResponse(RESPONSE_ERROR);

		this->m_watch_point.reset();
	}

	return context->QueueResponse(RESPONSE_OK);
}

// Internal functions for control
void GDBServer::HandleTrapInstruction(PPCInterpreter_t* hCPU)
{
	std::unique_lock breakpointLock(m_breakpointMutex);
	// First, restore any removed breakpoints
	for (auto& bp : m_patchedInstructions)
	{
		if (bp.second.IsRemoved())
			bp.second.Restore();
	}

	auto patchedBP = m_patchedInstructions.find(hCPU->instructionPointer);
	if (patchedBP == m_patchedInstructions.end())
		return cemu_assert_suspicious();

	// Secondly, delete one-shot breakpoints but also temporarily delete patched instruction to run original instruction
	OSThread_t* currThread = coreinit::OSGetCurrentThread();
	std::string pauseReason = fmt::format("T05thread:{:08X};core:{:02X};{}", GET_THREAD_ID(currThread), PPCInterpreter_getCoreIndex(hCPU), patchedBP->second.GetReason());
	bool pauseThreads = patchedBP->second.ShouldBreakThreads() || patchedBP->second.ShouldBreakThreadsOnNextInterrupt();
	if (patchedBP->second.IsPersistent())
	{
		// Insert new restore breakpoints at next possible instructions which restores breakpoints but won't pause the CPU
		std::vector<MPTR> nextInstructions = findNextInstruction(hCPU->instructionPointer, hCPU->spr.LR, hCPU->spr.CTR);
		for (MPTR nextInstr : nextInstructions)
		{
			if (!m_patchedInstructions.contains(nextInstr))
				this->m_patchedInstructions.try_emplace(nextInstr, nextInstr, BreakpointType::BP_RESTORE_POINT, false, "");
		}
		patchedBP->second.RemoveTemporarily();
	}
	else
	{
		m_patchedInstructions.erase(patchedBP);
	}

	// Thirdly, delete any instructions that were generated by a skip instruction
	for (auto it = m_patchedInstructions.cbegin(), next_it = it; it != m_patchedInstructions.cend(); it = next_it)
	{
		++next_it;
		if (it->second.IsSkipBreakpoint())
		{
			m_patchedInstructions.erase(it);
		}
	}

	// Fourthly, the stub can insert breakpoints that are just meant to restore patched instructions, in which case we just want to continue
	if (pauseThreads)
	{
		cemuLog_logDebug(LogType::Force, "[GDBStub] Got trapped by a breakpoint!");
		std::unique_ptr<CommandContext> resumedContext;
		{
			std::lock_guard runLock(m_runMutex);
			resumedContext = std::move(m_resumed_context);
		}
		// Never hold the breakpoint map lock while suspending a guest thread.
		breakpointLock.unlock();
		if (resumedContext)
			ThreadPool::FireAndForget(&waitForBrokenThreads, std::move(resumedContext), pauseReason);

		breakThreads(GET_THREAD_ID(coreinit::OSGetCurrentThread()));
		cemuLog_logDebug(LogType::Force, "[GDBStub] Resumed from a breakpoint!");
	}
}

void GDBServer::HandleAccessException(uint64 dr6)
{
	std::lock_guard breakpointLock(m_breakpointMutex);
	if (!m_watch_point)
		return;
	bool triggeredWrite = GetBits(dr6, 2, 1);
	bool triggeredReadWrite = GetBits(dr6, 3, 1);

	std::string response;
	if (m_watch_point->GetType() == AccessPointType::BP_WRITE && triggeredWrite)
		response = fmt::format("watch:{:08X};", m_watch_point->GetAddress());
	else if (m_watch_point->GetType() == AccessPointType::BP_READ && triggeredReadWrite && !triggeredWrite)
		response = fmt::format("rwatch:{:08X};", m_watch_point->GetAddress());
	else if (m_watch_point->GetType() == AccessPointType::BP_BOTH && triggeredReadWrite)
		response = fmt::format("awatch:{:08X};", m_watch_point->GetAddress());

	if (!response.empty())
	{
		PPCInterpreter_t* hCPU = PPCInterpreter_getCurrentInstance();
		cemuLog_logDebug(LogType::Force, "Received matching breakpoint exception: {}", response);
		auto nextInstructions = findNextInstruction(hCPU->instructionPointer, hCPU->spr.LR, hCPU->spr.CTR);
		for (MPTR nextInstr : nextInstructions)
		{
			auto bpIt = m_patchedInstructions.find(nextInstr);
			if (bpIt == m_patchedInstructions.end())
				this->m_patchedInstructions.try_emplace(nextInstr, nextInstr, BreakpointType::BP_STEP_POINT, false, response);
			else
				bpIt->second.PauseOnNextInterrupt();
		}
	}
}

void GDBServer::HandleEntryStop(uint32 entryAddress)
{
	std::lock_guard breakpointLock(m_breakpointMutex);
	this->m_patchedInstructions.try_emplace(entryAddress, entryAddress, BreakpointType::BP_SINGLE, false, "");
	m_entry_point = entryAddress;
}
