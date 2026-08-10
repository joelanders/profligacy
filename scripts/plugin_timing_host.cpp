// Headless JUCE-host timing probe for the real ProphecyAudioProcessor processBlock path.
#include "PluginProcessor.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<std::uint64_t> g_allocationCount { 0 };
std::atomic<std::uint64_t> g_allocationBytes { 0 };
std::atomic<std::uint64_t> g_processBlockAllocationCount { 0 };
thread_local bool g_insideProcessBlock = false;

void *trackedAllocate(std::size_t size)
{
	g_allocationCount.fetch_add(1, std::memory_order_relaxed);
	g_allocationBytes.fetch_add(size, std::memory_order_relaxed);
	if (g_insideProcessBlock)
		g_processBlockAllocationCount.fetch_add(1, std::memory_order_relaxed);
	if (void *p = std::malloc(size == 0 ? 1 : size)) return p;
	throw std::bad_alloc();
}

struct ProcessBlockAllocationScope
{
	ProcessBlockAllocationScope() { g_insideProcessBlock = true; }
	~ProcessBlockAllocationScope() { g_insideProcessBlock = false; }
};

} // namespace

void *operator new(std::size_t size) { return trackedAllocate(size); }
void *operator new[](std::size_t size) { return trackedAllocate(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {

struct Event
{
	std::int64_t sample = 0;
	std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t> parameterChange(int group, int parameter, int value)
{
	value &= 0x3fff;
	parameter &= 0x3fff;
	return { 0xf0, 0x42, 0x30, 0x41, 0x41, (std::uint8_t)(group & 0x7f),
		(std::uint8_t)(parameter & 0x7f), (std::uint8_t)((parameter >> 7) & 0x7f),
		(std::uint8_t)(value & 0x7f), (std::uint8_t)((value >> 7) & 0x7f), 0xf7 };
}

void addEvent(std::vector<Event> &events, double rate, double seconds,
	std::initializer_list<std::uint8_t> bytes)
{
	events.push_back({ (std::int64_t)std::llround(seconds * rate), bytes });
}

void addEvent(std::vector<Event> &events, double rate, double seconds,
	std::vector<std::uint8_t> bytes)
{
	events.push_back({ (std::int64_t)std::llround(seconds * rate), std::move(bytes) });
}

} // namespace

int main(int argc, char **argv)
{
	double rate = 48000.0;
	double durationSeconds = 22.0;
	int block = 512;
	const char *output = nullptr;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
		else if (arg == "--block" && i + 1 < argc) block = std::atoi(argv[++i]);
		else if (arg == "--seconds" && i + 1 < argc) durationSeconds = std::atof(argv[++i]);
		else if (arg == "--output" && i + 1 < argc) output = argv[++i];
	}
	if (rate < 8000.0 || block <= 0 || durationSeconds < 16.0 || output == nullptr)
	{
		std::fprintf(stderr,
			"usage: %s --rate HZ --block N [--seconds N>=16] --output capture.jsonl\n",
			argv[0]);
		return 2;
	}

	std::FILE *capture = std::fopen(output, "w");
	if (!capture)
	{
		std::perror(output);
		return 2;
	}

	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	ProphecyAudioProcessor processor;
	if (!processor.enableMidiTxByteCapture())
	{
		std::fprintf(stderr, "could not enable MIDI capture before engine start\n");
		std::fclose(capture);
		return 2;
	}
	processor.prepareToPlay(rate, block);

	std::vector<Event> events;
	addEvent(events, rate, 8.8, parameterChange(0, 187, 1)); // CLOCK = EXT
	addEvent(events, rate, 9.1, {0xb0,0x63,0x00}); // NRPN select: UP pattern
	addEvent(events, rate, 9.1, {0xb0,0x62,0x01});
	addEvent(events, rate, 9.1, {0xb0,0x06,0x00});
	addEvent(events, rate, 10.8, parameterChange(2, 18, 5)); // sixteenth triplet = 4 F8
	addEvent(events, rate, 11.3, {0xb0,0x63,0x00}); // NRPN: ARP ON
	addEvent(events, rate, 11.3, {0xb0,0x62,0x02});
	addEvent(events, rate, 11.3, {0xb0,0x06,0x7f});
	constexpr std::uint8_t chord[] = {0x3c, 0x40, 0x43, 0x47};
	for (std::uint8_t note : chord)
		addEvent(events, rate, 12.0, {0x90,note,0x64});
	for (std::uint8_t note : chord)
		addEvent(events, rate, 15.0, {0x80,note,0x00});
	addEvent(events, rate, 15.4, {0xb0,0x63,0x00}); // NRPN: ARP OFF
	addEvent(events, rate, 15.4, {0xb0,0x62,0x02});
	addEvent(events, rate, 15.4, {0xb0,0x06,0x00});
	constexpr double externalBpm = 120.0;
	const double clockPeriod = 60.0 / (24.0 * externalBpm);
	for (double when = 11.5; when <= 15.5 + 1e-9; when += clockPeriod)
		addEvent(events, rate, when, {0xf8});
	std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
		return a.sample < b.sample;
	});

	juce::AudioBuffer<float> audio(2, block);
	std::size_t eventIndex = 0;
	std::uint64_t captured = 0;
	const bool allocationStats = std::getenv("PROPHECY_ALLOCATION_STATS") != nullptr;
	std::uint64_t previousAllocations = g_allocationCount.load(std::memory_order_relaxed);
	std::uint64_t previousBytes = g_allocationBytes.load(std::memory_order_relaxed);
	std::uint64_t previousProcessBlockAllocations =
		g_processBlockAllocationCount.load(std::memory_order_relaxed);
	int allocationSecond = 1;
	// Five seconds of drain tail lets all events through even when an unentitled
	// headless test process temporarily runs the emulator below real time.
	const std::int64_t totalSamples =
		(std::int64_t)std::llround(durationSeconds * rate);
	const auto period = std::chrono::duration<double>((double)block / rate);
	auto next = std::chrono::steady_clock::now();
	for (std::int64_t blockStart = 0; blockStart < totalSamples; blockStart += block)
	{
		juce::MidiBuffer midi;
		const std::int64_t blockEnd = blockStart + block;
		while (eventIndex < events.size() && events[eventIndex].sample < blockEnd)
		{
			const Event &event = events[eventIndex++];
			const int offset = (int)std::max<std::int64_t>(0, event.sample - blockStart);
			midi.addEvent(event.bytes.data(), (int)event.bytes.size(), offset);
		}
		audio.clear();
		{
			ProcessBlockAllocationScope scope;
			processor.processBlock(audio, midi);
		}
		ProphecyEngine::MidiTxByteEvent observed[512];
		for (std::size_t n; (n = processor.popMidiTxByteEvents(observed, 512)) > 0; )
			for (std::size_t i = 0; i < n; ++i)
			{
				std::fprintf(capture, "{\"t\":%.9f,\"byte\":%u}\n",
					observed[i].emuSeconds, (unsigned)observed[i].byte);
				++captured;
			}
		if (allocationStats && blockEnd >= (std::int64_t)std::llround(allocationSecond * rate))
		{
			const std::uint64_t allocations = g_allocationCount.load(std::memory_order_relaxed);
			const std::uint64_t bytes = g_allocationBytes.load(std::memory_order_relaxed);
			const std::uint64_t processBlockAllocations =
				g_processBlockAllocationCount.load(std::memory_order_relaxed);
			std::fprintf(stderr,
				"[alloc] host_second=%d allocations=%llu bytes=%llu process_block=%llu\n",
				allocationSecond,
				(unsigned long long)(allocations - previousAllocations),
				(unsigned long long)(bytes - previousBytes),
				(unsigned long long)(processBlockAllocations - previousProcessBlockAllocations));
			previousAllocations = allocations;
			previousBytes = bytes;
			previousProcessBlockAllocations = processBlockAllocations;
			++allocationSecond;
		}
		next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
		std::this_thread::sleep_until(next);
	}
	processor.releaseResources();
	std::fclose(capture);
	std::fprintf(stderr, "rate=%.1f block=%d seconds=%.1f host_events=%zu captured=%llu "
		"output_dropped=%llu input_dropped=%llu\n",
		rate, block, durationSeconds, events.size(), (unsigned long long)captured,
		(unsigned long long)processor.droppedMidiTxByteEvents(),
		(unsigned long long)processor.droppedScheduledMidiBytes());
	return processor.droppedMidiTxByteEvents() == 0
		&& processor.droppedScheduledMidiBytes() == 0 ? 0 : 1;
}
