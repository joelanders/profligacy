// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace prophecy {

// Audio-thread host MIDI retains its JUCE sample offset as an engine-frame due time.
// It is separate from the immediate editor/UI queue so a future audio event can never
// head-of-line-block an immediate parameter SysEx message.
template <std::size_t Capacity>
class MidiQueue
{
public:
	bool push(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame, std::uint64_t revision = 0)
	{
		if (bytes == nullptr || n == 0) return true;
		const std::size_t t = m_tail.load(std::memory_order_relaxed);
		const std::size_t h = m_head.load(std::memory_order_acquire);
		if (CAP - (t - h) < n)
		{
			m_dropped.fetch_add(n, std::memory_order_relaxed);
			return false;
		}
		for (std::size_t i = 0; i < n; ++i)
			m_buf[(t + i) & (CAP - 1)] = Item{frame, revision, bytes[i], i == 0, i + 1 == n};
		m_tail.store(t + n, std::memory_order_release);
		return true;
	}

	std::size_t popDue(std::uint8_t *out, std::size_t cap, std::uint64_t frame, std::uint64_t currentRevision = 0)
	{
		const std::size_t h = m_head.load(std::memory_order_relaxed);
		const std::size_t t = m_tail.load(std::memory_order_acquire);
		std::size_t consumed = 0, n = 0;
		while (h + consumed < t && n < cap)
		{
			const Item &item = m_buf[(h + consumed) & (CAP - 1)];
			if (item.frame > frame) break;
			// Once a message has started, finish it even if a replacement arrives
			// halfway through a multi-pop SysEx. Never leave a torn UART message.
			if (item.first)
				m_acceptMessage = item.revision == 0
					|| item.revision == currentRevision;
			if (m_acceptMessage) out[n++] = item.byte;
			m_inMessage = !item.last;
			++consumed;
			if (n != 0 && item.last) break;
		}
		if (consumed > 0) m_head.store(h + consumed, std::memory_order_release);
		return n;
	}
	std::uint64_t dropped() const { return m_dropped.load(std::memory_order_acquire); }
	bool messageInProgress() const { return m_inMessage; } // consumer only
	// Initialization only, after the consumer has observed playback input disabled.
	void discard()
	{
		m_head.store(m_tail.load(std::memory_order_acquire), std::memory_order_release);
		m_inMessage = false;
	}
	void reset()
	{
		m_head.store(0, std::memory_order_relaxed);
		m_tail.store(0, std::memory_order_relaxed);
		m_dropped.store(0, std::memory_order_relaxed);
		m_inMessage = false;
	}

private:
	struct Item { std::uint64_t frame; std::uint64_t revision; std::uint8_t byte; bool first, last; };
	bool m_acceptMessage = true; // consumer only
	bool m_inMessage = false;
	static_assert(Capacity != 0 && (Capacity & (Capacity - 1)) == 0);
	static constexpr std::size_t CAP = Capacity;
	Item m_buf[CAP] {};
	std::atomic<std::size_t> m_head {0}, m_tail {0};
	std::atomic<std::uint64_t> m_dropped {0};
};

// A pending UI packet may take priority only at a message boundary. Switching
// queues halfway through a host SysEx would inject unrelated bytes into it.
template <std::size_t ImmediateCapacity, std::size_t ScheduledCapacity>
std::size_t popMidiInput(MidiQueue<ImmediateCapacity>& immediate,
	MidiQueue<ScheduledCapacity>& scheduled, std::uint8_t* out, std::size_t cap,
	std::uint64_t frame, std::uint64_t revision, bool allowImmediate = true)
{
	if (scheduled.messageInProgress()) return scheduled.popDue(out, cap, frame);
	const auto count = allowImmediate || immediate.messageInProgress()
		? immediate.popDue(out, cap, 0, revision) : 0;
	return count != 0 ? count : scheduled.popDue(out, cap, frame);
}

}
