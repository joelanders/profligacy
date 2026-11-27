// SPDX-License-Identifier: AGPL-3.0-only
// SysEx file transfers through the real processor and the fake engine, which accepts the
// stream and never answers. Covers what needs no firmware: a host save while a transfer
// owns the synth, refusing a second transfer, cancelling mid-dump, and the emulator
// stopping mid-dump (the plugin bypassed). No ROM.
#include "PluginProcessor.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

std::vector<std::uint8_t> prophecyFakeTakeSysexStream(); // prophecy_engine_fake.cpp

using namespace prophecy::sysex;
using Clock = std::chrono::steady_clock;

static int g_failures = 0;
static void check(bool ok, const juce::String &what)
{
	std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
	if (!ok) ++g_failures;
}

static void environment(const char *name, const juce::String &value)
{
#if JUCE_WINDOWS
	_putenv_s(name, value.toRawUTF8());
#else
	setenv(name, value.toRawUTF8(), 1);
#endif
}

static double seconds(Clock::time_point since)
{
	return std::chrono::duration<double>(Clock::now() - since).count();
}

// The plugin build has no modal dispatch loop: drive the message-thread timers directly.
template<class Predicate> static bool pumpUntil(Predicate done, double limitSeconds)
{
	const auto start = Clock::now();
	while (!done())
	{
		if (seconds(start) > limitSeconds) return false;
		juce::Timer::callPendingTimersSynchronously();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}
static void pumpFor(double s) { pumpUntil([] { return false; }, s); }

static juce::String state(ProphecyAudioProcessor &p) { return p.sysexStatus()["state"].toString(); }
static juce::String message(ProphecyAudioProcessor &p) { return p.sysexStatus()["message"].toString(); }

static juce::File writeDump(const juce::File &dir, const char *name, const Dump &d)
{
	const auto msg = build(d, 0);
	const juce::File f = dir.getChildFile(name);
	f.replaceWithData(msg.data(), msg.size());
	return f;
}

// Complete F0..F7 messages in the stream the fake engine received.
static std::vector<std::vector<std::uint8_t>> messages(const std::vector<std::uint8_t> &stream)
{
	return scan(stream.data(), stream.size()).messages;
}
static bool isProtect(const std::vector<std::uint8_t> &m, int value)
{
	return m.size() == 11 && m[4] == 0x41 && m[5] == 0x00 && (m[6] | (m[7] << 7)) >= 170
		&& (m[6] | (m[7] << 7)) <= 171 && m[8] == value;
}
// The protect switches are turned off first and back on last.
static bool bracketed(const std::vector<std::vector<std::uint8_t>> &msgs)
{
	const std::size_t n = msgs.size();
	return n >= 4 && isProtect(msgs[0], 0) && isProtect(msgs[1], 0)
		&& isProtect(msgs[n - 2], 1) && isProtect(msgs[n - 1], 1);
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	const auto fixture = juce::File::getSpecialLocation(juce::File::tempDirectory)
		.getNonexistentChildFile("profligacy-sysex-fake-test", "", false);
	if (!fixture.createDirectory().wasOk()
		|| !fixture.getChildFile("korgprop.zip").replaceWithText("fake engine fixture"))
	{
		std::fputs("FAIL fixture\n", stderr);
		return 1;
	}
	environment("PROPHECY_ROMPATH", fixture.getFullPathName());
	environment("PROPHECY_NVRAM", fixture.getChildFile("nvram").getFullPathName());
	environment("PROPHECY_FAKE_PROGRAM_DUMP", "1");
#if JUCE_WINDOWS
	_putenv_s("PROPHECY_FORCE_NO_ROM", "");
#else
	unsetenv("PROPHECY_FORCE_NO_ROM");
#endif

	{
		ProphecyAudioProcessor processor;
		constexpr int block = 512;
		processor.prepareToPlay(48000.0, block);
		std::atomic<bool> stop{false}, paused{false};
		// A host calling processBlock in real time; `paused` is a bypassed plugin.
		std::thread audio([&] {
			juce::AudioBuffer<float> buffer(2, block);
			juce::MidiBuffer midi;
			auto next = Clock::now();
			while (!stop.load())
			{
				if (!paused.load())
				{
					buffer.clear();
					midi.clear();
					processor.processBlock(buffer, midi);
				}
				next += std::chrono::microseconds(block * 1000000 / 48000);
				std::this_thread::sleep_until(next);
			}
		});
		check(pumpUntil([&] { return processor.playbackReady(); }, 10.0), "processor ready");
		pumpFor(0.5);
		prophecyFakeTakeSysexStream();

		// 1. A program import the stub never confirms. While it runs: a second transfer is
		// refused with a reason, and a host save is immediate and keeps the edit buffer.
		{
			const Dump program{Unit::Program, 5, std::vector<std::uint8_t>(kProgramBytes, 0x21)};
			const juce::File file = writeDump(fixture, "program.syx", program);
			check((bool) processor.prepareSysexImport(file)["ok"], "program file accepted");
			check(processor.confirmSysexImport(5), "import into A05 starts");
			pumpFor(0.3);
			check(state(processor) == "running", "import running");
			check(!processor.startSysexExport(Unit::Global, 0, fixture.getChildFile("x.syx")), "second transfer refused");
			check(processor.sysexBusyReason().contains("already running"), "busy reason: " + processor.sysexBusyReason());
			check((bool) processor.prepareSysexImport(file)["ok"] && !processor.confirmSysexImport(5),
				"confirming another import refused while busy");

			juce::MemoryBlock saved;
			const auto saveStart = Clock::now();
			processor.getStateInformation(saved);
			const double saveSeconds = seconds(saveStart);
			check(saveSeconds < 0.1, "save during a transfer does not wait for the synth ("
				+ juce::String(saveSeconds * 1000.0, 1) + " ms)");
			const auto *bytes = static_cast<const std::uint8_t *>(saved.getData());
			std::optional<Dump> edit;
			for (std::size_t i = 0; i + messageBytes(Unit::CurrentProgram) <= saved.getSize() && !edit; ++i)
				if (bytes[i] == 0xF0 && bytes[i + 4] == 0x40)
					edit = parse(std::vector<std::uint8_t>(bytes + i, bytes + i + messageBytes(Unit::CurrentProgram)));
			check(edit && programName(edit->raw.data()) == "Fake Edit Buffer", "save during a transfer keeps the edit buffer");

			check(pumpUntil([&] { return state(processor) != "running"; }, 15.0), "unconfirmed import ends");
			check(state(processor) == "failed" && message(processor).contains("did not confirm Program A05"),
				"unconfirmed import reported: " + message(processor));
			check(processor.sysexStatus()["unseen"] && pumpUntil([&] { return processor.sysexBusyReason().isEmpty(); }, 2.0),
				"result unseen and transfer idle");
			processor.acknowledgeSysexResult();
			check(!(bool) processor.sysexStatus()["unseen"], "result acknowledged");
			const auto msgs = messages(prophecyFakeTakeSysexStream());
			check(bracketed(msgs) && msgs.size() == 5 && msgs[2] == build(program, 0),
				"stream: protect off, the program, protect on");
		}

		// 2. Cancel a bank (12.5 s on the wire) after a second: the dump is ended with F7
		// where it stands and memory protect goes back on.
		{
			const Dump bank{Unit::Bank, 0, std::vector<std::uint8_t>(rawBytes(Unit::Bank), 0x22)};
			check((bool) processor.prepareSysexImport(writeDump(fixture, "bank.syx", bank))["ok"], "bank file accepted");
			check(processor.confirmSysexImport(1), "import into bank B starts");
			pumpFor(1.5);
			const auto cancelAt = Clock::now();
			processor.cancelSysexTransfer();
			check(pumpUntil([&] { return state(processor) != "running"; }, 10.0), "cancel ends the transfer");
			check(seconds(cancelAt) < 4.0, "cancel ends it within the cut-off answer timeout ("
				+ juce::String(seconds(cancelAt), 1) + " s)");
			check(state(processor) == "cancelled" && message(processor).contains("incomplete Bank B"),
				"cancel reported: " + message(processor));
			const auto msgs = messages(prophecyFakeTakeSysexStream());
			check(bracketed(msgs) && msgs.size() == 5 && msgs[2].size() > 100
				&& msgs[2].size() < messageBytes(Unit::Bank) && msgs[2][4] == 0x4C,
				"stream: protect off, a cut-off bank ending in F7, protect on");
			processor.acknowledgeSysexResult();
		}

		// 3. The host stops calling processBlock mid-dump: the transfer fails after about
		// 5 s naming the cause, and finishes (F7, protect on) once audio runs again.
		{
			const Dump bank{Unit::Bank, 1, std::vector<std::uint8_t>(rawBytes(Unit::Bank), 0x23)};
			check((bool) processor.prepareSysexImport(writeDump(fixture, "bank2.syx", bank))["ok"], "second bank file accepted");
			check(processor.confirmSysexImport(1), "import starts");
			pumpFor(1.0);
			paused = true;
			const auto stalledAt = Clock::now();
			check(pumpUntil([&] { return state(processor) != "running"; }, 10.0), "stall ends the transfer");
			const double after = seconds(stalledAt);
			check(after > 4.5 && after < 6.5, "stall reported after about 5 s (" + juce::String(after, 1) + " s)");
			check(state(processor) == "failed" && message(processor).contains("bypassed")
				&& message(processor).contains("Memory protect will be turned back on"),
				"stall reported: " + message(processor));
			check(processor.sysexBusyReason().contains("not bypassed")
				&& !processor.startSysexExport(Unit::Global, 0, fixture.getChildFile("y.syx")),
				"new transfers wait for the cleanup: " + processor.sysexBusyReason());
			juce::MemoryBlock saved;
			const auto saveStart = Clock::now();
			processor.getStateInformation(saved);
			check(seconds(saveStart) < 0.1, "save while settling does not wait for the synth");
			paused = false;
			check(pumpUntil([&] { return processor.sysexBusyReason().isEmpty(); }, 10.0), "cleanup finishes once audio runs");
			const auto msgs = messages(prophecyFakeTakeSysexStream());
			check(bracketed(msgs) && msgs.size() == 5 && msgs[2].size() < messageBytes(Unit::Bank) && msgs[2][4] == 0x4C,
				"stream after the stall: protect off, a cut-off bank, protect on");
		}

		stop = true;
		audio.join();
		processor.releaseResources();
	}
	fixture.deleteRecursively();
	if (g_failures == 0) std::puts("PASS plugin sysex fake");
	return g_failures == 0 ? 0 : 1;
}
