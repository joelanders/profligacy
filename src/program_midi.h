// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace prophecy {

// Prophecy v1.7 global-record routing. Used by the program owner; the audio
// callback only records messages and retains their original UART timestamps.
class ProgramMidiRouting
{
public:
	bool reset(const std::vector<std::uint8_t>& bytes)
	{
		m_valid = bytes.size() == m_globals.size();
		if (m_valid) std::copy(bytes.begin(), bytes.end(), m_globals.begin());
		return m_valid;
	}
	bool valid() const { return m_valid; }
	std::uint8_t channelNumber() const { return m_globals[176] & 15; }
	bool channel(std::uint8_t status) const
	{ return m_valid && ((m_globals[177] & 2) || (status & 15) == m_globals[176]); }
	bool sysex(const std::uint8_t* bytes, std::size_t size) const
	{
		return m_valid && size >= 6 && bytes[0] == 0xf0 && bytes[1] == 0x42
			&& bytes[2] == (0x30 | m_globals[176]) && bytes[3] == 0x41
			&& bytes[size - 1] == 0xf7 && (m_globals[178] & 2)
			&& std::all_of(bytes + 1, bytes + size - 1, [](std::uint8_t b) { return b < 128; });
	}
	void global(int parameter, int value)
	{
		if (parameter == 184) m_globals[176] = std::uint8_t(value & 15);
		else if (parameter >= 185 && parameter <= 191)
		{
			const auto offset = std::size_t(parameter <= 187 ? 177 : parameter <= 189 ? 178 : 179);
			const auto bit = unsigned(parameter - (parameter <= 187 ? 185 : parameter <= 189 ? 188 : 190));
			m_globals[offset] = std::uint8_t((m_globals[offset] & ~(1u << bit)) | ((unsigned(value) & 1u) << bit));
		}
		else if (parameter >= 192 && parameter <= 197)
			m_globals[std::size_t(180 + ((parameter - 192) ^ 1))] = std::uint8_t(value);
		else if (parameter >= 198 && parameter <= 389)
			m_globals[std::size_t(parameter - 198 + 186)] = std::uint8_t(value & 127);
		else if (parameter >= 390 && parameter <= 781)
		{
			const auto offset = std::size_t(378 + ((parameter - 390) / 4) * 2);
			switch ((parameter - 390) % 4)
			{
				case 0: m_globals[offset] = std::uint8_t((m_globals[offset] & ~1) | (value & 1)); break;
				case 1: m_globals[offset] = std::uint8_t((m_globals[offset] & ~6) | ((value & 3) << 1)); break;
				case 2: m_globals[offset] = std::uint8_t((m_globals[offset] & ~8) | ((value & 1) << 3)); break;
				case 3: m_globals[offset + 1] = std::uint8_t(value); break;
			}
		}
	}
	// Returns an A/B/C slot for a recognized Program Change, or -1 for a
	// message that does not select a program. OFF bank bytes are wildcards.
	int receive(const std::uint8_t* bytes, std::size_t size)
	{
		if (sysex(bytes, size) && size == 11 && bytes[4] == 0x41 && bytes[5] == 0)
			global(bytes[6] | (bytes[7] << 7), bytes[8] | (bytes[9] << 7));
		if (size < 2 || !channel(bytes[0])) return -1;
		if (size == 3 && (bytes[0] & 0xf0) == 0xb0)
		{
			if (bytes[1] == 124 || bytes[1] == 125) global(186, bytes[1] == 125);
			else if (bytes[1] < 96)
			{
				const auto offset = std::size_t(382 + bytes[1] * 2);
				if (((m_globals[offset] >> 1) & 3) == 1)
				{
					if (m_globals[offset + 1] == 2) m_bankMsb = bytes[2];
					if (m_globals[offset + 1] == 34) m_bankLsb = bytes[2];
				}
			}
		}
		if (size != 2 || (bytes[0] & 0xf0) != 0xc0 || !(m_globals[179] & 2)) return -1;
		for (int bank = 0; bank < 3; ++bank)
		{
			const auto lsb = m_globals[std::size_t(180 + bank * 2)];
			const auto msb = m_globals[std::size_t(181 + bank * 2)];
			if ((msb != 255 && msb != m_bankMsb) || (lsb != 255 && lsb != m_bankLsb)) continue;
			for (int slot = 0; slot < 64; ++slot)
				if (m_globals[std::size_t(186 + bank * 64 + slot)] == bytes[1]) return bank * 64 + slot;
		}
		return -1;
	}
private:
	std::array<std::uint8_t, 574> m_globals{};
	std::uint8_t m_bankMsb = 0, m_bankLsb = 0;
	bool m_valid = false;
};

// One producer (audio callback), one consumer (program control mutex). Reserve
// before UART admission, then publish its result, so overflow cannot forward an
// unrecorded program change. Neither reservation nor publication allocates.
class ProgramMidiInbox
{
public:
	enum class Rejection : std::uint64_t { None, Capacity, Busy };
	struct Message
	{
		std::array<std::uint8_t, 663> bytes{};
		std::size_t size = 0;
		bool accepted = false;
	};
	Message* reserve()
	{
		const auto tail = m_tail.load(std::memory_order_relaxed);
		return tail - m_head.load(std::memory_order_acquire) < m_messages.size()
			? &m_messages[tail % m_messages.size()] : nullptr;
	}
	std::uint64_t nextSequence() const { return m_tail.load(std::memory_order_relaxed) + 1; }
	std::uint64_t publishedSequence() const { return m_tail.load(std::memory_order_acquire); }
	std::uint64_t consumedSequence() const { return m_head.load(std::memory_order_acquire); }
	void publish() { m_tail.fetch_add(1, std::memory_order_release); }
	void markFirmwareInput() { m_firmwareInput.store(nextSequence(), std::memory_order_release); }
	bool firmwareInputPending() const { return m_firmwareInput.load(std::memory_order_acquire) > consumedSequence(); }
	void reject(Rejection reason = Rejection::Capacity)
	{
		// Rejections cannot occupy an already full ring. Retain their position
		// separately so an older accepted replacement cannot erase the error.
		m_rejectedAfter.store((nextSequence() << 2) | std::uint64_t(reason), std::memory_order_release);
	}
	Rejection takeRejection()
	{
		auto after = m_rejectedAfter.load(std::memory_order_acquire);
		if (after == 0 || (after >> 2) - 1 > consumedSequence()) return Rejection::None;
		const auto reason = static_cast<Rejection>(after & 3);
		return m_rejectedAfter.compare_exchange_strong(after, 0, std::memory_order_acq_rel) ? reason : Rejection::None;
	}
	const Message* front() const
	{
		const auto head = m_head.load(std::memory_order_relaxed);
		return head != m_tail.load(std::memory_order_acquire) ? &m_messages[head % m_messages.size()] : nullptr;
	}
	std::uint64_t pop() { return m_head.fetch_add(1, std::memory_order_release) + 1; }
private:
	std::array<Message, 128> m_messages{};
	std::atomic<std::uint64_t> m_head{0}, m_tail{0};
	std::atomic<std::uint64_t> m_rejectedAfter{0};
	std::atomic<std::uint64_t> m_firmwareInput{0};
};

}
