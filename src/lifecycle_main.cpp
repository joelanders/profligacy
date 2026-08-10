// SPDX-License-Identifier: AGPL-3.0-only
//
// lifecycle_main.cpp - same-process ProphecyEngine teardown/restart gate.
//
// A one-shot console run cannot prove that the embedded MAME process singleton,
// worker thread, callbacks, and audio ring are reusable after plugin unload.  This
// deliberately constructs and destroys several engines in one process.  Each cycle
// must produce audio frames, publish an LCD snapshot, and stop within the bounded
// teardown window before the next cycle is allowed to start.

#include "prophecy_engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using steady_clock = std::chrono::steady_clock;

std::uint64_t env_u64(const char *name, std::uint64_t fallback)
{
	const char *value = std::getenv(name);
	if (value == nullptr || *value == '\0')
		return fallback;
	const unsigned long long parsed = std::strtoull(value, nullptr, 10);
	return parsed > 0 ? std::uint64_t(parsed) : fallback;
}

} // anonymous namespace

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	const std::uint64_t cycles = env_u64("PROPHOST_LIFECYCLE_CYCLES", 5);
	const std::uint64_t target_frames = env_u64("PROPHOST_LIFECYCLE_FRAMES", 576000); // 12 s
	const std::uint64_t timeout_ms = env_u64("PROPHOST_LIFECYCLE_TIMEOUT_MS", 30000);
	std::vector<std::string> args(argv, argv + argc);
	std::vector<float> left(512), right(512);

	std::uint64_t passed = 0;
	for (std::uint64_t cycle = 1; cycle <= cycles; ++cycle)
	{
		const auto cycle_start = steady_clock::now();
		auto engine = std::make_unique<ProphecyEngine>();
		const bool started = engine->start(args);
		bool lcd_ready = false;
		char line1[41] = {}, line2[41] = {};

		while (started && !engine->finished())
		{
			engine->pull(left.data(), right.data(), left.size());
			lcd_ready = engine->latestLcd(line1, line2, sizeof(line1)) || lcd_ready;
			if (engine->producedFrames() >= target_frames && lcd_ready)
				break;
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				steady_clock::now() - cycle_start).count();
			if (elapsed >= static_cast<long long>(timeout_ms))
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		const std::uint64_t produced = engine->producedFrames();
		const auto stop_start = steady_clock::now();
		engine->stop();
		const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			steady_clock::now() - stop_start).count();
		const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			steady_clock::now() - cycle_start).count();
		const bool finished = engine->finished();
		const bool ok = started && produced >= target_frames && lcd_ready && finished && stop_ms < 3000;

		std::fprintf(stderr,
			"[lifecycle] cycle=%llu started=%d produced=%llu lcd=%d finished=%d "
			"stop_ms=%lld wall_ms=%lld %s\n",
			static_cast<unsigned long long>(cycle), started ? 1 : 0,
			static_cast<unsigned long long>(produced), lcd_ready ? 1 : 0,
			finished ? 1 : 0, static_cast<long long>(stop_ms),
			static_cast<long long>(wall_ms), ok ? "PASS" : "FAIL");

		engine.reset();
		if (!ok)
		{
			std::fprintf(stderr,
				"FAIL lifecycle_restart cycle=%llu/%llu target_frames=%llu timeout_ms=%llu\n",
				static_cast<unsigned long long>(cycle), static_cast<unsigned long long>(cycles),
				static_cast<unsigned long long>(target_frames),
				static_cast<unsigned long long>(timeout_ms));
			return 1;
		}
		++passed;
	}

	std::fprintf(stderr, "PASS lifecycle_restart cycles=%llu target_frames=%llu\n",
		static_cast<unsigned long long>(passed), static_cast<unsigned long long>(target_frames));
	return 0;
}
