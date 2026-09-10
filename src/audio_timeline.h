// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

namespace prophecy {

// One conversion of an absolute host position, never a sum of rounded block
// lengths. MIDI, render grants and output interpolation share this clock.
class SampleTimeline
{
public:
	void reset(double hostRate, std::uint64_t nativeOrigin = 0)
	{
		m_ratio = 48000.0L / hostRate;
		m_origin = nativeOrigin;
	}
	long double position(std::uint64_t hostFrame) const { return hostFrame * m_ratio; }
	std::uint64_t event(std::uint64_t hostFrame) const
	{
		return m_origin + static_cast<std::uint64_t>(std::floor(position(hostFrame) + 0.5L));
	}
	std::uint64_t horizon(std::uint64_t hostFrame) const
	{
		return m_origin + static_cast<std::uint64_t>(std::floor(position(hostFrame)));
	}
	std::uint64_t origin() const { return m_origin; }
private:
	long double m_ratio = 1;
	std::uint64_t m_origin = 0;
};

// SPSC audio storage indexed by absolute native frame, not by successful host
// reads. The consumer may skip into the future after an underrun. The producer
// then discards those expired frames while emulation catches up. It can never
// overwrite a frame in the consumer's current read window.
class TimelineAudioRing
{
public:
	explicit TimelineAudioRing(std::size_t capacity)
		: m_capacity(capacity), m_samples(capacity * 2) {}

	// Consumer only. Monotonic; retain the current window until the next call so
	// adjacent interpolation windows can overlap without resetting filter history.
	void seek(std::uint64_t frame) { m_first.store(frame, std::memory_order_release); }
	std::uint64_t end() const { return m_end.load(std::memory_order_acquire); }
	std::size_t capacity() const { return m_capacity; }
	std::size_t available() const
	{
		const auto first = m_first.load(std::memory_order_acquire);
		const auto last = end();
		return last > first ? static_cast<std::size_t>(last - first) : 0;
	}

	// Producer only. Bounded, nonblocking attempt; caller may sleep when full.
	// first is the absolute position of input[0], including any discarded frames.
	std::size_t push(std::uint64_t first, const std::int16_t* input, std::size_t frames)
	{
		const auto wanted = m_first.load(std::memory_order_acquire);
		const auto skipped = wanted > first
			? static_cast<std::size_t>(std::min<std::uint64_t>(wanted - first, frames)) : 0;
		first += skipped;
		const auto occupied = first > wanted ? first - wanted : 0;
		const auto count = std::min(frames - skipped,
			occupied < m_capacity ? m_capacity - static_cast<std::size_t>(occupied) : 0);
		for (std::size_t i = 0; i < count; ++i)
		{
			const auto slot = (first + i) % m_capacity;
			m_samples[slot * 2] = input[(skipped + i) * 2];
			m_samples[slot * 2 + 1] = input[(skipped + i) * 2 + 1];
		}
		m_end.store(first + count, std::memory_order_release);
		return skipped + count;
	}

	// Consumer only, after seek(first). Missing audio stays at its assigned time.
	std::size_t read(std::uint64_t first, float* left, float* right, std::size_t frames) const
	{
		const auto last = end();
		const auto count = last > first
			? static_cast<std::size_t>(std::min<std::uint64_t>(last - first, frames)) : 0;
		for (std::size_t i = 0; i < count; ++i)
		{
			const auto slot = (first + i) % m_capacity;
			left[i] = m_samples[slot * 2] * (1.0f / 32768.0f);
			right[i] = m_samples[slot * 2 + 1] * (1.0f / 32768.0f);
		}
		std::fill(left + count, left + frames, 0.0f);
		std::fill(right + count, right + frames, 0.0f);
		return count;
	}

private:
	const std::size_t m_capacity;
	std::vector<std::int16_t> m_samples;
	std::atomic<std::uint64_t> m_first{0}, m_end{0};
};

// Fourth-order Lagrange interpolation at x in [0, 1], with samples at -2..2.
// This preserves the polynomial order of JUCE's existing five-sample kernel.
// At integer native positions this is exactly the original PCM sample. The
// caller supplies lookahead using the explicit, integer host latency.
inline float interpolate(float a, float b, float c, float d, float e, long double x)
{
	const auto t = static_cast<float>(x);
	return a * (t * (t + 1) * (t - 1) * (t - 2) / 24)
		- b * ((t + 2) * t * (t - 1) * (t - 2) / 6)
		+ c * ((t + 2) * (t + 1) * (t - 1) * (t - 2) / 4)
		- d * ((t + 2) * (t + 1) * t * (t - 2) / 6)
		+ e * ((t + 2) * (t + 1) * t * (t - 1) / 24);
}

} // namespace prophecy
