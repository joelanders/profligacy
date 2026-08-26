// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Lock-free handoff from MAME's worker thread to host observers. Electrical
// snapshots report only the current state. A single visual observer may instead
// consume rising-edge latches so pulses shorter than its polling interval are
// still presented for one complete poll.
class LedStore
{
public:
	static constexpr std::size_t kBankCount = 12;

	void set(std::uint8_t bank, std::uint8_t data)
	{
		if (bank >= kBankCount) return;
		const std::uint8_t previous = m_banks[bank].exchange(data, std::memory_order_acq_rel);
		const std::uint8_t rising = data & static_cast<std::uint8_t>(~previous);
		if (rising != 0)
			m_rising[bank].fetch_or(rising, std::memory_order_release);
		m_version.fetch_add(1, std::memory_order_release);
	}

	std::uint32_t snapshot(std::uint8_t out[kBankCount]) const
	{
		if (out != nullptr)
			for (std::size_t i = 0; i < kBankCount; ++i)
				out[i] = m_banks[i].load(std::memory_order_relaxed);
		return m_version.load(std::memory_order_acquire);
	}

	std::uint32_t visualSnapshot(std::uint8_t out[kBankCount])
	{
		if (out != nullptr)
		{
			for (std::size_t i = 0; i < kBankCount; ++i)
			{
				const std::uint8_t current = m_banks[i].load(std::memory_order_relaxed);
				const std::uint8_t rising = m_rising[i].exchange(0, std::memory_order_acq_rel);
				out[i] = current | rising;
			}
		}
		return m_version.load(std::memory_order_acquire);
	}

	void reset()
	{
		for (auto &bank : m_banks) bank.store(0, std::memory_order_relaxed);
		for (auto &bank : m_rising) bank.store(0, std::memory_order_relaxed);
		m_version.store(0, std::memory_order_release);
	}

private:
	std::atomic<std::uint8_t> m_banks[kBankCount] {};
	std::atomic<std::uint8_t> m_rising[kBankCount] {};
	std::atomic<std::uint32_t> m_version { 0 };
};
