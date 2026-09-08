// SPDX-License-Identifier: AGPL-3.0-only
#include "audio_timeline.h"
#include "pending_program.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool condition, const char* message)
{
	if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main()
{
	// Ten hours of common and awkward host partitions must produce the same
	// timestamps as a single absolute conversion, including offsets inside blocks.
	for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
	{
		prophecy::SampleTimeline clock;
		clock.reset(rate, 123456);
		for (std::uint64_t block : { 1, 64, 128, 256, 511, 512, 1024 })
		{
			std::uint64_t position = 0;
			for (int i = 0; i < 20000; ++i)
			{
				position += (i % 4 == 0 ? block / 2 : block);
				const auto rateInteger = (std::uint64_t) rate;
				const auto expected = 123456 + (position * 48000 + rateInteger / 2) / rateInteger;
				require(clock.event(position) == expected, "MIDI clock depends on block partition");
				require(clock.horizon(position) <= clock.event(position), "render grant rounds into unknown input");
			}
		}
		const auto hours = (std::uint64_t) (rate * 36000);
		require(clock.event(hours) == 123456 + 48000ULL * 36000, "long-session clock drift");
	}

	// Polynomial reproduction detects phase and group-delay errors in the
	// resampler independently of a particular impulse's peak location.
	for (int degree = 0; degree <= 4; ++degree)
		for (int step = 0; step <= 100; ++step)
		{
			const long double x = step / 100.0L;
			const auto f = [degree](long double t) { return (float) std::pow(t, degree); };
			require(std::abs(prophecy::interpolate(f(-2), f(-1), f(0), f(1), f(2), x) - f(x)) < 2e-6f,
				"interpolator changes phase or polynomial samples");
		}

	prophecy::TimelineAudioRing ring(32);
	std::array<std::int16_t, 128> pcm{};
	for (int i = 0; i < 64; ++i) { pcm[i * 2] = (std::int16_t) i; pcm[i * 2 + 1] = (std::int16_t) -i; }
	std::array<float, 32> left{}, right{};
	require(ring.push(0, pcm.data(), 32) == 32, "initial push");
	ring.seek(8);
	require(ring.read(8, left.data(), right.data(), 16) == 16, "indexed read");
	require(left[0] == 8.0f / 32768 && right[15] == -23.0f / 32768, "stereo PCM altered");
	ring.seek(20); // overlapping interpolation history
	require(ring.read(20, left.data(), right.data(), 16) == 12 && left[12] == 0, "partial underrun not zero-filled");
	ring.seek(48); // discard the missing interval; never shift late frames forward
	require(ring.read(48, left.data(), right.data(), 16) == 0, "future read used stale samples");
	require(ring.push(32, pcm.data() + 64, 32) == 32, "producer did not discard expired frames");
	require(ring.read(48, left.data(), right.data(), 16) == 16, "producer failed to recover");
	require(left[0] == 48.0f / 32768 && right[15] == -63.0f / 32768, "underrun permanently changed latency");

	// Actual concurrent SPSC transfer, with changing window sizes and wraparound.
	prophecy::TimelineAudioRing concurrent(256);
	constexpr std::uint64_t frames = 100000;
	std::thread producer([&] {
		std::uint64_t first = 0;
		std::array<std::int16_t, 128> source{};
		while (first < frames)
		{
			const auto count = (std::size_t) std::min<std::uint64_t>(64, frames - first);
			for (std::size_t i = 0; i < count; ++i)
				source[2 * i] = source[2 * i + 1] = (std::int16_t) ((first + i) % 32768);
			first += concurrent.push(first, source.data(), count);
			std::this_thread::yield();
		}
	});
	for (std::uint64_t first = 0; first < frames; )
	{
		const auto count = (std::size_t) std::min<std::uint64_t>(1 + first % 31, frames - first);
		concurrent.seek(first);
		while (concurrent.end() < first + count) std::this_thread::yield();
		require(concurrent.read(first, left.data(), right.data(), count) == count, "offline read incomplete");
		for (std::size_t i = 0; i < count; ++i)
			require(left[i] == float((first + i) % 32768) / 32768 && left[i] == right[i], "concurrent frame corruption");
		first += count;
	}
	producer.join();

	// Concurrent state publication must never expose torn bytes or acknowledge
	// a newer publication merely because an older snapshot was consumed.
	prophecy::PendingProgram pending;
	std::atomic<bool> finished{false};
	std::thread writer([&] {
		std::array<std::uint8_t, 1024> bytes{};
		for (int value = 1; value <= 10000; ++value)
		{
			bytes.fill((std::uint8_t) value);
			require(pending.publish(bytes.data(), 535 + value % 489), "state publication rejected");
		}
		finished.store(true, std::memory_order_release);
	});
	std::uint64_t applied = 0;
	do
	{
		const auto state = pending.read();
		if (state.coherent && state.size)
		{
			require(state.revision >= applied, "state revision went backwards");
			for (std::size_t i = 0; i < state.size; ++i)
				require(state.bytes[i] == (std::uint8_t) (state.revision / 2), "torn pending program");
			applied = state.revision;
		}
	} while (!finished.load(std::memory_order_acquire));
	writer.join();
	const auto latest = pending.read();
	require(latest.coherent && latest.revision == 20000, "latest state publication lost");
	require(pending.publish(nullptr, 0) && pending.read().size == 0, "state clear failed");
	std::puts("audio timeline: clocks, interpolation, underrun recovery and concurrent PCM transfer passed");
}
