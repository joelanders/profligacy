// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace prophecy {

// Host state callbacks may publish while audio is running. Keep the bytes
// atomic too: a sequence counter around an ordinary vector still has a data race.
class PendingProgram
{
public:
	static constexpr std::size_t capacity = 1024;
	struct Snapshot
	{
		std::array<std::uint8_t, capacity> bytes{};
		std::size_t size = 0;
		std::uint64_t revision = 0;
		bool coherent = false;
	};

	// Non-realtime writers only. Serialize writers, coalescing to the latest state.
	bool publish(const std::uint8_t* bytes, std::size_t size)
	{
		if (size > capacity || (size && !bytes)) return false;
		std::lock_guard lock(m_writer);
		m_revision.fetch_add(1, std::memory_order_relaxed);
		// Paired with the reader fence via any byte it observes from this write.
		std::atomic_thread_fence(std::memory_order_release);
		for (std::size_t i = 0; i < size; ++i) m_bytes[i].store(bytes[i], std::memory_order_relaxed);
		m_size.store(size, std::memory_order_relaxed);
		m_revision.fetch_add(1, std::memory_order_release);
		return true;
	}

	// One bounded attempt; the audio callback retries on its next block if a
	// writer overlapped this copy. A caller keeps its own last-applied revision.
	std::uint64_t revision() const { return m_revision.load(std::memory_order_acquire); }
	Snapshot read() const
	{
		Snapshot result;
		const auto revision = m_revision.load(std::memory_order_acquire);
		if (revision & 1) return result;
		result.size = m_size.load(std::memory_order_relaxed);
		for (std::size_t i = 0; i < result.size; ++i)
			result.bytes[i] = m_bytes[i].load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acquire);
		if (m_revision.load(std::memory_order_relaxed) != revision) return {};
		result.revision = revision;
		result.coherent = true;
		return result;
	}
private:
	std::mutex m_writer;
	std::array<std::atomic<std::uint8_t>, capacity> m_bytes{};
	std::atomic<std::size_t> m_size{0};
	std::atomic<std::uint64_t> m_revision{0};
};

} // namespace prophecy
