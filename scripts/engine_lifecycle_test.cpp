// SPDX-License-Identifier: AGPL-3.0-only
// Exercise the actual MAME worker, using isolated real or clean-room ROM/NVRAM.
#include "prophecy_engine.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool ok, const char* message)
{
	if (!ok) { std::fprintf(stderr, "FAIL engine lifecycle: %s\n", message); std::exit(1); }
}

template<class Predicate> static void until(Predicate predicate)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate())
	{
		require(std::chrono::steady_clock::now() < deadline, "worker did not reach the requested state");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

int main()
{
	// Match the plugin's headless OSD setup. The standalone engine harness
	// does not construct a JUCE processor to set these before the worker starts.
#if defined(_WIN32)
	_putenv_s("SDL_VIDEODRIVER", "dummy");
	_putenv_s("SDL_AUDIODRIVER", "dummy");
#else
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
	const auto* rom = std::getenv("PROPHECY_ROMPATH");
	const auto* nvram = std::getenv("PROPHECY_NVRAM");
	if (!rom || !nvram)
	{
		std::puts("Requires an isolated ROM/NVRAM fixture; the clean-room CI runner supplies one");
		return 77;
	}
	const std::vector<std::string> args = {
		"prophecy", "korgprop", "-rompath", rom, "-nvram_directory", nvram,
		"-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
		"-debugger", "none", "-midiprovider", "none", "-networkprovider", "none",
		"-keyboardprovider", "none", "-mouseprovider", "none", "-joystickprovider", "none",
		"-lightgunprovider", "none", "-output", "none", "-noplugins"
	};
	// Every iteration boots a fresh real machine in the same process. Besides
	// cancellation, this verifies that teardown releases the process singleton.
	for (int scenario = 0; scenario < 5; ++scenario)
	{
		ProphecyEngine engine;
		if (scenario != 3) require(engine.enableHostTimeline(), "enable timeline");
		require(engine.start(args), "replacement machine could not claim the slot");
		std::thread reader;
		std::atomic<bool> readerFinished{false};
		if (scenario == 1 || scenario == 4)
			until([&] { return engine.waitingForInput(); });
		if (scenario == 2)
		{
			require(engine.initializePlayback(false), "initialize synthetic playback");
			engine.requestThroughFrame(1000000);
			until([&] { return engine.producedFrames() >= 1024; });
		}
		if (scenario == 3)
			until([&] { return engine.available() == engine.ringFrames(); });
		if (scenario == 4)
		{
			reader = std::thread([&] {
				float left[64]{}, right[64]{};
				const auto got = engine.readAtFrame(200000, left, right, 64, true);
				require(got == 0, "cancelled read returned unrendered PCM");
				readerFinished.store(true);
			});
			// Acquiring the diagnostic mutex proves readAtFrame reached its
			// condition-variable wait, rather than just launching the thread.
			until([&] { return engine.waitingForOutput(); });
		}
		const auto before = std::chrono::steady_clock::now();
		engine.stop();
		if (reader.joinable()) reader.join();
		require(engine.finished() && !engine.ownsMachineSlot(), "stop returned before worker exit");
		require(scenario != 4 || readerFinished.load(), "offline read was not cancelled");
		require(std::chrono::steady_clock::now() - before < std::chrono::seconds(5), "shutdown exceeded cancellation deadline");
		std::printf("PASS engine lifecycle scenario %d\n", scenario);
	}
	// Force the final worker to retain ownership just beyond the shutdown deadline.
	// stop() must return at the deadline without exposing the detached worker to
	// freed state or unloadable code, and the worker must release the singleton later.
#if defined(_WIN32)
	_putenv_s("PROFLIGACY_TEST_SHUTDOWN_STALL_MS", "5000");
#else
	setenv("PROFLIGACY_TEST_SHUTDOWN_STALL_MS", "5000", 1);
#endif
	{
		ProphecyEngine engine;
		require(engine.enableHostTimeline(), "enable timeout timeline");
		require(engine.start(args), "timeout machine could not claim the slot");
		until([&] { return engine.waitingForInput(); });
		const auto before = std::chrono::steady_clock::now();
		engine.stop();
		const auto elapsed = std::chrono::steady_clock::now() - before;
		require(elapsed >= std::chrono::milliseconds(2900), "timeout path returned before its deadline");
		require(elapsed < std::chrono::seconds(4), "timeout path exceeded its bounded deadline");
		require(!engine.finished() && engine.ownsMachineSlot(), "timeout path abandoned worker ownership");
		until([&] { return engine.finished() && !engine.ownsMachineSlot(); });
	}
#if defined(_WIN32)
	_putenv_s("PROFLIGACY_TEST_SHUTDOWN_STALL_MS", "");
#else
	unsetenv("PROFLIGACY_TEST_SHUTDOWN_STALL_MS");
#endif
	std::puts("PASS engine lifecycle bounded timeout");
}
