#include "DRCInput.h"

#include <algorithm>

#ifdef __linux__
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#endif

namespace DRCInput
{
std::optional<Sample> Decode(std::span<const std::uint8_t> bytes)
{
	if (bytes.size() != 128)
		return std::nullopt;

	Sample sample;
	sample.buttons = ((bytes[2] << 8) | bytes[3]) & 0xfffe;
	if (bytes[80] & 0x40)
		sample.buttons |= 0x00020000;
	if (bytes[80] & 0x80)
		sample.buttons |= 0x00040000;
	for (std::size_t axis = 0; axis < sample.sticks.size(); ++axis)
	{
		const auto offset = 6 + axis * 2;
		const int value = bytes[offset] | (bytes[offset + 1] << 8);
		const int delta = value - 2050;
		if (delta <= -115 || delta >= 115)
			sample.sticks[axis] = std::clamp(delta / 1150.0f, -1.0f, 1.0f);
	}

	int x = 0, y = 0, pressure = 0;
	for (std::size_t point = 0; point < 10; ++point)
	{
		const auto offset = 36 + point * 4;
		x += bytes[offset] | ((bytes[offset + 1] & 15) << 8);
		y += bytes[offset + 2] | ((bytes[offset + 3] & 15) << 8);
		if (point < 2)
			pressure |= (bytes[offset + 1] | bytes[offset + 3]) & 0x70;
	}
	sample.touchX = x / 10;
	sample.touchY = y / 10;
	sample.touching = pressure != 0;
	return sample;
}

#ifdef __linux__
namespace
{
class Receiver
{
public:
	Receiver()
	{
		const char* bindAddress = std::getenv("CEMU_DRC_BIND_ADDRESS");
		const char* peerAddress = std::getenv("CEMU_DRC_PEER_ADDRESS");
		if (!bindAddress || !peerAddress)
			return;

		Configure(true, bindAddress, peerAddress);
	}

	bool Configure(bool enabled, std::string_view bindAddress, std::string_view peerAddress)
	{
		std::lock_guard lock(m_mutex);
		if (!enabled)
		{
			if (m_fd >= 0)
				close(m_fd);
			m_fd = -1;
			m_lastPacket = {};
			return true;
		}

		in_addr peer{};
		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_port = htons(50022);
		if (inet_pton(AF_INET, std::string(bindAddress).c_str(), &local.sin_addr) != 1 ||
			local.sin_addr.s_addr == INADDR_ANY ||
			inet_pton(AF_INET, std::string(peerAddress).c_str(), &peer) != 1 || peer.s_addr == INADDR_ANY)
			return false;

		if (m_fd >= 0 && m_local.s_addr == local.sin_addr.s_addr)
		{
			if (m_peer.s_addr != peer.s_addr)
				m_lastPacket = {};
			m_peer = peer;
			return true;
		}
		const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (fd < 0)
			return false;
		if (bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
		{
			close(fd);
			return false;
		}
		if (m_fd >= 0)
			close(m_fd);
		m_fd = fd;
		m_local = local.sin_addr;
		m_peer = peer;
		m_lastPacket = {};
		return true;
	}

	~Receiver()
	{
		if (m_fd >= 0)
			close(m_fd);
	}

	bool Read(Sample& sample, bool* receiving)
	{
		std::lock_guard lock(m_mutex);
		if (m_fd < 0)
			return false;
		const auto now = std::chrono::steady_clock::now();
		for (int i = 0; i < 64; ++i)
		{
			std::array<std::uint8_t, 128> packet;
			sockaddr_in sender{};
			socklen_t length = sizeof(sender);
			const auto size = recvfrom(m_fd, packet.data(), packet.size(), MSG_TRUNC,
				reinterpret_cast<sockaddr*>(&sender), &length);
			if (size < 0)
				break;
			if (static_cast<std::size_t>(size) == packet.size() && sender.sin_addr.s_addr == m_peer.s_addr)
			{
				m_sample = *Decode(packet);
				m_lastPacket = now;
			}
		}
		const bool fresh = now - m_lastPacket < std::chrono::milliseconds(150);
		if (receiving)
			*receiving = fresh;
		sample = fresh ? m_sample : Sample{};
		return true;
	}

private:
	int m_fd{-1};
	in_addr m_peer{}, m_local{};
	std::mutex m_mutex;
	Sample m_sample{};
	std::chrono::steady_clock::time_point m_lastPacket{};
};
Receiver& GetReceiver()
{
	static Receiver receiver;
	return receiver;
}
}
#endif

bool Configure(bool enabled, std::string_view local, std::string_view peer)
{
#ifdef __linux__
	return GetReceiver().Configure(enabled, local, peer);
#else
	return !enabled;
#endif
}

bool Read(Sample& sample, bool* receiving)
{
	if (receiving)
		*receiving = false;
#ifdef __linux__
	return GetReceiver().Read(sample, receiving);
#else
	return false;
#endif
}
}
