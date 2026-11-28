// SPDX-License-Identifier: AGPL-3.0-only
// Live-engine DSP concurrency probe. Uses the plugin's native-rate timeline and
// its unchanged latency calculation. Offline output is the PCM reference; paced
// output keeps zeros at their assigned times and counts missed frames.
#include "prophecy_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

namespace {
using clock_type = std::chrono::steady_clock;
constexpr std::uint64_t rate = ProphecyEngine::kSampleRate;
double env_number(const char *key, double fallback)
{
	const auto *value = std::getenv(key);
	return value ? std::atof(value) : fallback;
}
double cpu_seconds()
{
	rusage usage{};
	getrusage(RUSAGE_SELF, &usage);
	return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
		(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
}
struct event { std::uint64_t frame; std::vector<std::uint8_t> bytes; };
struct block_result { std::uint64_t first, produced; unsigned missing; double callback_us, late_us; };
std::int16_t s16(float v) { return std::int16_t(std::clamp(std::lrintf(v * 32768), -32768L, 32767L)); }
double percentile(std::vector<double> values, double p)
{
	if (values.empty()) return 0;
	std::sort(values.begin(), values.end());
	return values[std::min(values.size() - 1, std::size_t(std::ceil(p * values.size()) - 1))];
}
}

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	const unsigned block = unsigned(env_number("PROPHOST_BLOCK", 128));
	const auto seconds = env_number("PROPHOST_PROBE_SECONDS", 8);
	const auto speed = env_number("PROPHOST_PROBE_SPEED", 1);
	const bool paced = std::getenv("PROPHOST_PROBE_PACED") != nullptr;
	const bool dense = std::getenv("PROPHOST_PROBE_DENSE") != nullptr;
	const bool programs = std::getenv("PROPHOST_PROBE_PROGRAMS") != nullptr;
	const bool all_programs = std::getenv("PROPHOST_PROBE_ALL_PROGRAMS") != nullptr;
	if (!block || block > 16384 || seconds <= 0 || speed <= 0 || (all_programs && seconds < 130)) return 2;
	const unsigned latency = std::max(block, ProphecyEngine::kAudioQuantum) + 2 * ProphecyEngine::kAudioQuantum + 2;
	const std::uint64_t frames = std::uint64_t(seconds * rate) + latency;
	std::vector<event> events;
	auto add = [&](double t, std::initializer_list<std::uint8_t> bytes) {
		events.push_back({std::uint64_t(std::llround(t * rate)), bytes});
	};
	if (dense)
	{
		for (int i = 0; 0.5 + i * 0.125 < seconds - 0.2; ++i)
		{
			const auto note = std::uint8_t(48 + (i * 7) % 28);
			add(0.5 + i * 0.125, {0x90, note, 100});
			add(0.6 + i * 0.125, {0x80, note, 0});
		}
		for (int i = 0; 0.5 + i * 0.020 < seconds - 0.2; ++i)
		{
			const auto bend = unsigned(8192 + 4096 * std::sin(i * 0.13));
			add(0.5 + i * 0.020, {0xb0, 1, std::uint8_t((i * 3) & 127)});
			add(0.505 + i * 0.020, {0xe0, std::uint8_t(bend & 127), std::uint8_t(bend >> 7)});
			if (!(i % 2)) add(0.51 + i * 0.020, {0xd0, std::uint8_t(i & 127)});
		}
	}
	else
	{
		add(0.5, {0x90, 60, 100});
		add(std::max(0.6, seconds - 1), {0x80, 60, 0});
	}
	auto select_program = [&](double t, unsigned program) {
		// Firmware MIDI addresses each 64-program bank separately. Flat 0..127
		// indices are a plug-in host convention, not raw MIDI program numbers.
		add(t, {0xb0, 0x00, 0x00, 0xb0, 0x20, std::uint8_t(program / 64),
			0xc0, std::uint8_t(program % 64)});
	};
	if (all_programs)
	{
		for (unsigned i = 0; i < 128; ++i) select_program(1.0 + i, i);
	}
	else if (programs)
	{
		const std::uint8_t patches[] = {8, 116, 12, 0, 32, 84};
		for (unsigned i = 0; 1.0 + i * 3 < seconds - 1; ++i)
			select_program(1.0 + i * 3, patches[i % 6]);
	}
	std::stable_sort(events.begin(), events.end(), [](const event &a, const event &b) { return a.frame < b.frame; });
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
	ProphecyEngine engine;
	engine.enableHostTimeline();
	engine.enableMidiTxByteCapture();
	// A sped-up deadline test must describe the actual host period to the OS,
	// just as the plug-in does. This API only sets a scheduling hint: input
	// grants, emulated work, output windows and latency remain unchanged.
	const unsigned worker_period = paced ? unsigned(std::clamp(std::round(block / speed),
		double(ProphecyEngine::kAudioQuantum), double(UINT32_MAX))) : block;
	engine.setHostBlockFrames(worker_period);
	// PROPHOST_MULTICORE_DSP=1 enables multi-core DSP execution; the default is the
	// serial reference. PROPHOST_MULTICORE_DSP_TOGGLE_BLOCKS=N flips the preference
	// every N callbacks, to check that switching during playback keeps exact output.
	ProphecyEngine::setMultiCoreDsp(env_number("PROPHOST_MULTICORE_DSP", 0) != 0);
	const auto toggle_blocks = std::uint64_t(env_number("PROPHOST_MULTICORE_DSP_TOGGLE_BLOCKS", 0));
	unsigned long long toggles = 0, active_blocks = 0, unavailable_blocks = 0;
	const auto boot = clock_type::now();
	if (!engine.start(std::vector<std::string>(argv, argv + argc)) || !engine.initializePlayback()) return 3;
	const auto boot_seconds = std::chrono::duration<double>(clock_type::now() - boot).count();
	const auto origin = engine.playbackOrigin();
	int callback_policy_result = -1;
#if defined(__APPLE__)
	// A DAW audio callback has real-time scheduling, as does the existing engine
	// producer. QoS alone lets that producer starve this test's callback and then
	// turns late input grants into artificial producer underruns.
	if (paced)
	{
		mach_timebase_info_data_t timebase{};
		mach_timebase_info(&timebase);
		const auto ns = std::uint64_t(double(block) * 1e9 / (rate * speed));
		auto ticks = [&](std::uint64_t n) { return std::uint32_t(n * timebase.denom / timebase.numer); };
		thread_time_constraint_policy_data_t policy{};
		policy.period = ticks(ns);
		policy.computation = ticks(std::min<std::uint64_t>(100000, ns / 4));
		policy.constraint = policy.period;
		policy.preemptible = true;
		callback_policy_result = thread_policy_set(pthread_mach_thread_np(pthread_self()),
			THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy),
			THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	}
#endif
	std::vector<float> left(block), right(block);
	std::vector<std::int16_t> audio(frames * 2, 0);
	std::vector<block_result> blocks;
	blocks.reserve(frames / block + 1);
	std::vector<ProphecyEngine::MidiTxByteEvent> midi;
	midi.reserve(65536);
	unsigned long long missing = 0, missing_after_warmup = 0, bad_blocks = 0;
	unsigned programs_observed = 0;
	bool programs_match = true;
	std::size_t next_event = 0;
	const auto cpu_start = cpu_seconds();
	const auto start = clock_type::now();
	for (std::uint64_t first = 0; first < frames; first += block)
	{
		const auto count = unsigned(std::min<std::uint64_t>(block, frames - first));
		const auto deadline = start + std::chrono::duration_cast<clock_type::duration>(
			std::chrono::duration<double>(double(first) / (rate * speed)));
		if (paced) std::this_thread::sleep_until(deadline);
		const auto entered = clock_type::now();
		const auto late = std::max(0.0, std::chrono::duration<double, std::micro>(entered - deadline).count());
		if (toggle_blocks && first && (first / block) % toggle_blocks == 0)
		{
			ProphecyEngine::setMultiCoreDsp(!ProphecyEngine::multiCoreDsp());
			++toggles;
		}
		const auto dsp_status = ProphecyEngine::multiCoreDspStatus();
		active_blocks += dsp_status == ProphecyEngine::MultiCoreDspStatus::Active;
		unavailable_blocks += dsp_status == ProphecyEngine::MultiCoreDspStatus::Unavailable;
		while (next_event < events.size() && events[next_event].frame < first + count)
		{
			const auto &e = events[next_event++];
			if (!engine.pushMidiAtFrame(e.bytes.data(), e.bytes.size(), origin + e.frame)) return 4;
		}
		engine.requestThroughFrame(origin + first + count);
		const auto output_first = std::max<std::uint64_t>(first, latency);
		unsigned lost = 0;
		if (output_first < first + count)
		{
			const auto needed = std::size_t(first + count - output_first);
			const auto got = engine.readAtFrame(origin + output_first - latency, left.data(), right.data(), needed, !paced);
			lost = unsigned(needed - got);
			for (unsigned i = 0; i < needed; ++i)
			{
				audio[(output_first + i) * 2] = s16(left[i]);
				audio[(output_first + i) * 2 + 1] = s16(right[i]);
			}
		}
		const auto elapsed = std::chrono::duration<double, std::micro>(clock_type::now() - entered).count();
		blocks.push_back({first, engine.producedFrames(), lost, elapsed, paced ? late : 0});
		missing += lost;
		if (first >= rate) missing_after_warmup += lost;
		if (lost) ++bad_blocks;
		ProphecyEngine::MidiTxByteEvent replies[1024];
		for (std::size_t n; (n = engine.popMidiTxByteEvents(replies, 1024)); )
			midi.insert(midi.end(), replies, replies + n);
		if (all_programs && programs_observed < 128
				&& first >= std::uint64_t((1.75 + programs_observed) * rate))
		{
			char line1[41]{}, line2[41]{};
			int observed = -1;
			if (engine.latestLcd(line1, line2, sizeof(line1))
					&& (line1[0] == 'A' || line1[0] == 'B')
					&& line1[1] >= '0' && line1[1] <= '6'
					&& line1[2] >= '0' && line1[2] <= '9' && line1[3] == ':')
				observed = (line1[0] == 'B' ? 64 : 0) + (line1[1] - '0') * 10 + line1[2] - '0';
			programs_match &= observed == int(programs_observed);
			std::printf("[dsp-probe-program] expected=%u observed=%d frame=%llu lcd=%.40s\n",
				programs_observed, observed, (unsigned long long)first, line1);
			++programs_observed;
		}
		if (!engine.running()) return 5;
	}
	const auto elapsed = std::chrono::duration<double>(clock_type::now() - start).count();
	const auto cpu = cpu_seconds() - cpu_start;
	const auto produced = engine.producedFrames();
	const auto dropped_midi = engine.droppedScheduledMidiBytes();
	const auto dropped_tx = engine.droppedMidiTxByteEvents();
	char lcd1[41]{}, lcd2[41]{};
	if (engine.latestLcd(lcd1, lcd2, sizeof(lcd1)))
		std::printf("[dsp-probe-lcd] %.40s | %.40s\n", lcd1, lcd2);
	engine.stop();
	std::printf("[dsp-probe-scheduling] callback_time_constraint_result=%d worker_period_frames=%u\n",
		callback_policy_result, worker_period);
	std::printf("[dsp-probe-multicore] toggles=%llu active_blocks=%llu unavailable_blocks=%llu blocks=%zu\n",
		toggles, active_blocks, unavailable_blocks, blocks.size());
	std::vector<double> callback, lateness;
	for (const auto &b : blocks) { callback.push_back(b.callback_us); lateness.push_back(b.late_us); }
	const char *path = std::getenv("PROPHOST_PROBE_OUT");
	if (path)
	{
		if (auto *f = std::fopen((std::string(path) + ".pcm").c_str(), "wb"))
		{ std::fwrite(audio.data(), sizeof(audio[0]), audio.size(), f); std::fclose(f); }
		if (auto *f = std::fopen((std::string(path) + ".blocks.csv").c_str(), "w"))
		{
			std::fprintf(f, "first,produced,missing,callback_us,late_us\n");
			for (const auto &b : blocks) std::fprintf(f, "%llu,%llu,%u,%.3f,%.3f\n",
				(unsigned long long)b.first, (unsigned long long)b.produced, b.missing, b.callback_us, b.late_us);
			std::fclose(f);
		}
		if (auto *f = std::fopen((std::string(path) + ".midi.csv").c_str(), "w"))
		{
			std::fprintf(f, "emu_seconds,byte\n");
			for (const auto &e : midi) std::fprintf(f, "%.12f,%u\n", e.emuSeconds, unsigned(e.byte));
			std::fclose(f);
		}
	}
	std::printf("[dsp-probe] {\"paced\":%s,\"speed\":%.4f,\"block\":%u,\"latency\":%u,\"origin\":%llu,\"frames\":%llu,\"produced\":%llu,\"boot_seconds\":%.6f,\"render_wall_seconds\":%.6f,\"render_cpu_seconds\":%.6f,\"missing_frames\":%llu,\"post_warmup_missing\":%llu,\"bad_blocks\":%llu,\"callback_p99_us\":%.3f,\"callback_max_us\":%.3f,\"host_late_p99_us\":%.3f,\"host_late_max_us\":%.3f,\"midi_dropped\":%llu,\"tx_dropped\":%llu,\"programs_observed\":%u,\"programs_match\":%s}\n",
		paced ? "true" : "false", speed, block, latency,
		(unsigned long long)origin, (unsigned long long)frames, (unsigned long long)produced,
		boot_seconds, elapsed, cpu, missing, missing_after_warmup, bad_blocks,
		percentile(callback, .99), percentile(callback, 1), percentile(lateness, .99), percentile(lateness, 1),
		(unsigned long long)dropped_midi, (unsigned long long)dropped_tx,
		programs_observed, programs_match ? "true" : "false");
	return all_programs && (!programs_match || programs_observed != 128) ? 10 : (!paced && missing ? 6 : 0);
}
