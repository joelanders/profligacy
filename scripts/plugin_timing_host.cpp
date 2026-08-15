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
	double healthInterval = 5.0;
	double healthTimeout = 3.0;
	double programInterval = 7.0;
	double programStart = 18.0;
	int block = 512;
	int phaseSamples = 0;
	int maxHealthMisses = 2;
	std::vector<int> programValues;
	std::uint32_t stressSeed = 0x50524f50; // 'PROP'
	bool stress = false, secondsSet = false;
	bool setupTraffic = true, clockTraffic = true, noteTraffic = true;
	bool controlTraffic = true, parameterTraffic = true, programTraffic = true;
	const char *output = nullptr;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
		else if (arg == "--block" && i + 1 < argc) block = std::atoi(argv[++i]);
		else if (arg == "--phase-samples" && i + 1 < argc) phaseSamples = std::atoi(argv[++i]);
		else if (arg == "--seconds" && i + 1 < argc) { durationSeconds = std::atof(argv[++i]); secondsSet = true; }
		else if (arg == "--output" && i + 1 < argc) output = argv[++i];
		else if (arg == "--stress") stress = true;
		else if (arg == "--no-setup") setupTraffic = false;
		else if (arg == "--no-clock") clockTraffic = false;
		else if (arg == "--no-notes") noteTraffic = false;
		else if (arg == "--no-controls") controlTraffic = false;
		else if (arg == "--no-params") parameterTraffic = false;
		else if (arg == "--no-programs") programTraffic = false;
		else if (arg == "--health-interval" && i + 1 < argc) healthInterval = std::atof(argv[++i]);
		else if (arg == "--health-timeout" && i + 1 < argc) healthTimeout = std::atof(argv[++i]);
		else if (arg == "--program-interval" && i + 1 < argc) programInterval = std::atof(argv[++i]);
		else if (arg == "--program-start" && i + 1 < argc) programStart = std::atof(argv[++i]);
		else if (arg == "--program-value" && i + 1 < argc) programValues.push_back(std::atoi(argv[++i]));
		else if (arg == "--max-health-misses" && i + 1 < argc) maxHealthMisses = std::atoi(argv[++i]);
		else if (arg == "--seed" && i + 1 < argc)
			stressSeed = (std::uint32_t)std::strtoul(argv[++i], nullptr, 0);
	}
	if (stress && !secondsSet) durationSeconds = 60.0;
	const std::uint32_t configuredStressSeed = stressSeed;
	if (rate < 8000.0 || block <= 0 || phaseSamples < 0 || phaseSamples >= block
			|| durationSeconds < 16.0 || output == nullptr)
	{
		std::fprintf(stderr,
			"usage: %s --rate HZ --block N [--seconds N>=16] --output capture.jsonl "
			"[--stress] [--seed N] [--health-interval S] [--health-timeout S] "
			"[--program-interval S] [--program-start S] [--program-value 0..127] "
			"[--max-health-misses N] [--phase-samples 0..BLOCK-1] "
			"[--no-setup] [--no-clock] [--no-notes] [--no-controls] "
			"[--no-params] [--no-programs]\n",
			argv[0]);
		return 2;
	}
	if (healthInterval <= 0.0 || healthTimeout <= 0.0 || programInterval <= 0.0
			|| programStart < 0.0
			|| std::any_of(programValues.begin(), programValues.end(), [](int value) {
				return value < 0 || value > 127;
			})
			|| maxHealthMisses < 0)
	{
		std::fprintf(stderr,
			"health/program intervals and timeout must be positive and max misses nonnegative\n");
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
	const double phaseSeconds = (double)phaseSamples / rate;
	auto addAt = [&events, rate, phaseSeconds](double seconds,
			std::initializer_list<std::uint8_t> bytes) {
		addEvent(events, rate, seconds + phaseSeconds, bytes);
	};
	auto addBytesAt = [&events, rate, phaseSeconds](double seconds,
			std::vector<std::uint8_t> bytes) {
		addEvent(events, rate, seconds + phaseSeconds, std::move(bytes));
	};
	if (setupTraffic)
	{
		addBytesAt(8.8, parameterChange(0, 187, 1)); // CLOCK = EXT
		addAt(9.1, {0xb0,0x63,0x00}); // NRPN select: UP pattern
		addAt(9.1, {0xb0,0x62,0x01});
		addAt(9.1, {0xb0,0x06,0x00});
		addBytesAt(10.8, parameterChange(2, 18, 5)); // sixteenth triplet = 4 F8
		addAt(11.3, {0xb0,0x63,0x00}); // NRPN: ARP ON
		addAt(11.3, {0xb0,0x62,0x02});
		addAt(11.3, {0xb0,0x06,0x7f});
	}
	constexpr std::uint8_t chord[] = {0x3c, 0x40, 0x43, 0x47};
	if (noteTraffic)
	{
		for (std::uint8_t note : chord)
			addAt(12.0, {0x90,note,0x64});
		for (std::uint8_t note : chord)
			addAt(15.0, {0x80,note,0x00});
	}
	if (setupTraffic)
	{
		addAt(15.4, {0xb0,0x63,0x00}); // NRPN: ARP OFF
		addAt(15.4, {0xb0,0x62,0x02});
		addAt(15.4, {0xb0,0x06,0x00});
	}
	constexpr double externalBpm = 120.0;
	const double clockPeriod = 60.0 / (24.0 * externalBpm);
	if (clockTraffic)
		for (double when = 11.5; when <= 15.5 + 1e-9; when += clockPeriod)
			addAt(when, {0xf8});
	if (stress)
	{
		auto random = [&stressSeed]() {
			stressSeed ^= stressSeed << 13;
			stressSeed ^= stressSeed >> 17;
			stressSeed ^= stressSeed << 5;
			return stressSeed;
		};
		for (double when = 16.0; when < durationSeconds - 1.0; when += 0.70)
		{
			const std::uint8_t note = (std::uint8_t)(36 + random() % 49);
			const std::uint8_t velocity = (std::uint8_t)(48 + random() % 80);
			if (noteTraffic)
			{
				addAt(when, {0x90, note, velocity});
				addAt(std::min(when + 0.42, durationSeconds - 0.5), {0x80, note, 0x00});
			}
		}
		for (double when = 16.15; when < durationSeconds - 1.0; when += 0.53)
		{
			const std::uint8_t controller = (std::uint8_t)(1 + random() % 31);
			const std::uint8_t controllerValue = (std::uint8_t)(random() % 128);
			const std::uint16_t bend = (std::uint16_t)(random() & 0x3fff);
			if (controlTraffic)
			{
				addAt(when, {0xb0, controller, controllerValue});
				addAt(when + 0.01,
					{0xe0, (std::uint8_t)(bend & 0x7f), (std::uint8_t)(bend >> 7)});
			}
		}
		constexpr int parameters[] = {105, 118, 131, 144, 355, 356, 364, 365, 371};
		std::size_t parameterIndex = 0;
		for (double when = 16.30; when < durationSeconds - 1.0; when += 0.85)
		{
			const int value = (int)(random() % 200);
			if (parameterTraffic)
				addBytesAt(when,
					parameterChange(1, parameters[parameterIndex % std::size(parameters)], value));
			++parameterIndex;
		}
		std::size_t programIndex = 0;
		for (double when = programStart; when < durationSeconds - 2.0; when += programInterval)
		{
			if (!programValues.empty() && programIndex >= programValues.size())
				break;
			const std::uint8_t randomProgram = (std::uint8_t)(random() % 128);
			const std::uint8_t program = !programValues.empty()
				? (std::uint8_t)programValues[programIndex] : randomProgram;
			++programIndex;
			if (programTraffic)
			{
				addAt(when, {0xb0, 0x00, 0x00});
				addAt(when, {0xb0, 0x20, (std::uint8_t)(program / 64)});
				addAt(when, {0xc0, (std::uint8_t)(program % 64)});
			}
		}
	}
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
	std::uint64_t healthAttempts = 0, healthReplies = 0, healthMisses = 0;
	std::uint64_t consecutiveHealthMisses = 0, worstConsecutiveHealthMisses = 0;
	std::uint32_t healthBaselineVersion = 0;
	double nextHealthAt = 12.0 + phaseSeconds, healthRequestedAt = -1.0, maxHealthLatency = 0.0;
	double lastEngineProgressAt = 0.0;
	std::uint64_t lastProducedFrames = 0;
	float audioPeak = 0.0f;
	bool healthPending = false, healthFailed = false;
	std::string healthFailure;
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
		const double blockStartSeconds = (double)blockStart / rate;
		const double blockEndSeconds = (double)blockEnd / rate;
		while (eventIndex < events.size() && events[eventIndex].sample < blockEnd)
		{
			const Event &event = events[eventIndex++];
			const int offset = (int)std::max<std::int64_t>(0, event.sample - blockStart);
			midi.addEvent(event.bytes.data(), (int)event.bytes.size(), offset);
		}
		if (stress && !healthFailed && !healthPending && nextHealthAt < blockEndSeconds
				&& nextHealthAt + healthTimeout < durationSeconds)
		{
			std::uint8_t previousDump[1024];
			processor.getProgramData(previousDump, sizeof(previousDump), &healthBaselineVersion);
			const std::uint8_t request[] = {0xf0, 0x42, 0x30, 0x41, 0x10, 0x00, 0xf7};
			const int requestOffset = std::clamp(
				(int)std::llround((nextHealthAt - blockStartSeconds) * rate), 0, block - 1);
			midi.addEvent(request, (int)sizeof(request), requestOffset);
			healthRequestedAt = blockStartSeconds + (double)requestOffset / rate;
			healthPending = true;
			++healthAttempts;
			std::fprintf(stderr, "[stress-health] request=%llu host_t=%.3f baseline_version=%u\n",
				(unsigned long long)healthAttempts, healthRequestedAt, healthBaselineVersion);
		}
		audio.clear();
		{
			ProcessBlockAllocationScope scope;
			processor.processBlock(audio, midi);
		}
		for (int channel = 0; channel < audio.getNumChannels(); ++channel)
			for (int sample = 0; sample < audio.getNumSamples(); ++sample)
				audioPeak = std::max(audioPeak, std::abs(audio.getSample(channel, sample)));
		const auto diagnostic = processor.diagnosticSnapshot();
		const double nowSeconds = (double)blockEnd / rate;
		if (diagnostic.producedFrames > lastProducedFrames)
		{
			lastProducedFrames = diagnostic.producedFrames;
			lastEngineProgressAt = nowSeconds;
		}
		else if (stress && !healthFailed && nowSeconds > 12.0
				&& nowSeconds - lastEngineProgressAt > healthTimeout)
		{
			healthFailed = true;
			healthFailure = "audio engine stopped producing frames";
			std::fprintf(stderr, "[stress-health] FAIL host_t=%.3f produced_frames=%llu\n",
				nowSeconds, (unsigned long long)diagnostic.producedFrames);
		}
		ProphecyEngine::MidiTxByteEvent observed[512];
		for (std::size_t n; (n = processor.popMidiTxByteEvents(observed, 512)) > 0; )
			for (std::size_t i = 0; i < n; ++i)
			{
				std::fprintf(capture, "{\"t\":%.9f,\"byte\":%u}\n",
					observed[i].emuSeconds, (unsigned)observed[i].byte);
				++captured;
			}
		if (stress && healthPending && !healthFailed)
		{
			std::uint8_t dump[1024];
			std::uint32_t version = 0;
			const std::size_t dumpBytes = processor.getProgramData(dump, sizeof(dump), &version);
			if (version > healthBaselineVersion)
			{
				const double latency = nowSeconds - healthRequestedAt;
				if (dumpBytes < 535)
				{
					healthFailed = true;
					healthFailure = "fresh current-program dump was shorter than 535 unpacked bytes";
				}
				else
				{
					++healthReplies;
					consecutiveHealthMisses = 0;
					maxHealthLatency = std::max(maxHealthLatency, latency);
					healthPending = false;
					nextHealthAt = nowSeconds + healthInterval;
					std::fprintf(stderr,
						"[stress-health] reply=%llu host_t=%.3f latency=%.3f bytes=%zu version=%u\n",
						(unsigned long long)healthReplies, nowSeconds, latency, dumpBytes, version);
				}
			}
			else if (nowSeconds - healthRequestedAt > healthTimeout)
			{
				++healthMisses;
				++consecutiveHealthMisses;
				worstConsecutiveHealthMisses = std::max(
					worstConsecutiveHealthMisses, consecutiveHealthMisses);
				if (consecutiveHealthMisses > (std::uint64_t)maxHealthMisses)
				{
					healthFailed = true;
					healthFailure = "too many consecutive current-program dump timeouts";
					std::fprintf(stderr, "[stress-health] FAIL host_t=%.3f attempt=%llu consecutive=%llu\n",
						nowSeconds, (unsigned long long)healthAttempts,
						(unsigned long long)consecutiveHealthMisses);
				}
				else
				{
					std::fprintf(stderr,
						"[stress-health] MISS attempt=%llu host_t=%.3f consecutive=%llu; retrying\n",
						(unsigned long long)healthAttempts, nowSeconds,
						(unsigned long long)consecutiveHealthMisses);
					healthPending = false;
					nextHealthAt = nowSeconds;
				}
			}
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
	const auto finalDiagnostic = processor.diagnosticSnapshot();
	if (stress && !healthFailed && (healthPending || healthReplies == 0))
	{
		healthFailed = true;
		healthFailure = healthPending
			? "host run ended with an outstanding current-program dump"
			: "host run ended before any current-program dump completed";
	}
	if (stress && !healthFailed && consecutiveHealthMisses != 0)
	{
		healthFailed = true;
		healthFailure = "host run ended before timed-out health checks recovered";
	}
	if (stress && !healthFailed && !finalDiagnostic.engineRunning)
	{
		healthFailed = true;
		healthFailure = "audio engine was no longer running at the end of the host soak";
	}
	if (stress && !healthFailed
			&& (finalDiagnostic.activeNotesLow != 0 || finalDiagnostic.activeNotesHigh != 0))
	{
		healthFailed = true;
		healthFailure = "host note tracker still contained active notes at the end of the soak";
	}
	if (stress && !healthFailed && audioPeak < 1.0e-6f)
	{
		healthFailed = true;
		healthFailure = "host soak produced no nonzero audio";
	}
	processor.releaseResources();
	std::fclose(capture);
	std::fprintf(stderr, "rate=%.1f block=%d seconds=%.1f host_events=%zu captured=%llu "
		"output_dropped=%llu input_dropped=%llu\n",
		rate, block, durationSeconds, events.size(), (unsigned long long)captured,
		(unsigned long long)processor.droppedMidiTxByteEvents(),
		(unsigned long long)processor.droppedScheduledMidiBytes());
	if (stress)
	{
		std::fprintf(stderr,
			"[stress] CONFIG seed=0x%08x rate=%.1f block=%d phase_samples=%d seconds=%.1f "
			"health_interval=%.3f health_timeout=%.3f program_start=%.3f "
			"program_interval=%.3f program_values=%zu max_health_misses=%d "
			"traffic=setup:%d,clock:%d,notes:%d,controls:%d,params:%d,programs:%d\n",
			configuredStressSeed, rate, block, phaseSamples, durationSeconds,
			healthInterval, healthTimeout, programStart, programInterval, programValues.size(),
			maxHealthMisses,
			setupTraffic, clockTraffic, noteTraffic, controlTraffic,
			parameterTraffic, programTraffic);
		std::fprintf(stderr,
			"[stress] AUDIO peak=%.9f produced_frames=%llu callbacks=%llu underrun_frames=%llu "
			"active_notes=%016llx/%016llx engine_running=%d\n",
			audioPeak, (unsigned long long)finalDiagnostic.producedFrames,
			(unsigned long long)finalDiagnostic.audioCallbacks,
			(unsigned long long)finalDiagnostic.audioUnderrunFrames,
			(unsigned long long)finalDiagnostic.activeNotesHigh,
			(unsigned long long)finalDiagnostic.activeNotesLow,
			finalDiagnostic.engineRunning ? 1 : 0);
		std::fprintf(stderr,
			"[stress-health] SUMMARY attempts=%llu replies=%llu misses=%llu "
			"worst_consecutive_misses=%llu max_latency=%.3f verdict=%s%s%s%s\n",
			(unsigned long long)healthAttempts, (unsigned long long)healthReplies,
			(unsigned long long)healthMisses,
			(unsigned long long)worstConsecutiveHealthMisses, maxHealthLatency,
			healthFailed ? "FAIL" : "PASS", healthFailed ? " reason=\"" : "",
			healthFailed ? healthFailure.c_str() : "", healthFailed ? "\"" : "");
	}
	return processor.droppedMidiTxByteEvents() == 0
		&& processor.droppedScheduledMidiBytes() == 0
		&& !healthFailed ? 0 : 1;
}
