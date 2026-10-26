// SPDX-License-Identifier: AGPL-3.0-only
// SysEx file export/import through the real ProphecyAudioProcessor: its message-thread
// timer, file writing, import retargeting, status reporting and learned program names.
// Needs PROPHECY_ROMPATH and a PROPHECY_NVRAM sysram seed (the test uses a copy).
#include "PluginProcessor.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace prophecy::sysex;

static int g_failures = 0;
static void check(bool ok, const juce::String &what)
{
	std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
	if (!ok) ++g_failures;
}

// Run the processor's message-thread timers until `done` or the deadline (the plugin
// build has no modal dispatch loop; plugin_timing_host drives timers the same way).
template<class Predicate> static bool pumpUntil(Predicate done, double seconds)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (!done())
	{
		if (std::chrono::steady_clock::now() > deadline) return false;
		juce::Timer::callPendingTimersSynchronously();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return true;
}

static juce::String transferState(ProphecyAudioProcessor &p) { return p.sysexStatus()["state"].toString(); }

static bool finishTransfer(ProphecyAudioProcessor &p, const juce::String &what)
{
	pumpUntil([&] { return transferState(p) != "running"; }, 120.0);
	const auto st = p.sysexStatus();
	const bool ok = st["state"].toString() == "succeeded";
	check(ok, what + ": " + st["message"].toString());
	return ok;
}

int main()
{
	const char *rom = std::getenv("PROPHECY_ROMPATH");
	const char *seed = std::getenv("PROPHECY_NVRAM");
	if (!rom || !seed)
	{
		std::puts("Requires PROPHECY_ROMPATH and a PROPHECY_NVRAM sysram seed");
		return 77;
	}
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	const juce::File work = juce::File::createTempFile("profligacy-plugin-sysex");
	work.createDirectory();
	const juce::File nvram = work.getChildFile("nvram");
	juce::File(juce::String::fromUTF8(seed)).copyDirectoryTo(nvram);
	// The processor uses PROPHECY_NVRAM in place, so point it at the copy.
#if defined(_WIN32)
	_putenv_s("PROPHECY_NVRAM", nvram.getFullPathName().toRawUTF8());
#else
	setenv("PROPHECY_NVRAM", nvram.getFullPathName().toRawUTF8(), 1);
#endif

	int code = 0;
	{
		ProphecyAudioProcessor processor;
		constexpr int block = 512;
		processor.prepareToPlay(48000.0, block);
		std::atomic<bool> stop { false };
		std::thread audio([&] {
			juce::AudioBuffer<float> buffer(2, block);
			juce::MidiBuffer midi;
			auto next = std::chrono::steady_clock::now();
			while (!stop.load())
			{
				buffer.clear();
				midi.clear();
				processor.processBlock(buffer, midi);
				next += std::chrono::microseconds(block * 1000000 / 48000);
				std::this_thread::sleep_until(next);
			}
		});

		check(pumpUntil([&] { return processor.playbackReady(); }, 60.0), "processor ready");
		pumpUntil([] { return false; }, 2.0); // let the startup program read-back settle

		// Export bank A to a file.
		const juce::File bankFile = work.getChildFile("bankA.syx");
		check(processor.startSysexExport(Unit::Bank, 0, bankFile), "start bank A export");
		check(!processor.startSysexExport(Unit::Global, 0, work.getChildFile("x.syx")), "second transfer refused");
		if (finishTransfer(processor, "export bank A"))
			check(bankFile.getSize() == (juce::int64) messageBytes(Unit::Bank), "bank file size");

		// Rename A02 in the file and load it into bank B.
		juce::MemoryBlock data;
		bankFile.loadFileAsData(data);
		const auto msgs = split(static_cast<const std::uint8_t *>(data.getData()), data.getSize());
		auto bank = msgs.size() == 1 ? parse(msgs[0]) : std::nullopt;
		check(bank.has_value(), "bank file parses");
		if (bank)
		{
			const char name[] = "Plugin Test B02 ";
			std::copy(name, name + kNameLength, bank->raw.begin() + 2 * long(kProgramBytes));
			const auto msg = build(*bank, 0);
			const juce::File modified = work.getChildFile("modified.syx");
			modified.replaceWithData(msg.data(), msg.size());
			const auto summary = processor.prepareSysexImport(modified);
			check((bool) summary["ok"] && summary["items"].size() == 1
				&& summary["items"][0]["unit"].toString() == "bank", "import summary: one bank");
			check(processor.confirmSysexImport(1), "confirm import into bank B");
			finishTransfer(processor, "import into bank B");
			check(processor.patchNames()[66].trimEnd() == "Plugin Test B02", "browser name learned: "
				+ processor.patchNames()[66]);

			const juce::File bankB = work.getChildFile("bankB.syx");
			processor.startSysexExport(Unit::Bank, 1, bankB);
			if (finishTransfer(processor, "export bank B"))
			{
				juce::MemoryBlock back;
				bankB.loadFileAsData(back);
				auto parsed = parse(std::vector<std::uint8_t>(static_cast<const std::uint8_t *>(back.getData()),
					static_cast<const std::uint8_t *>(back.getData()) + back.getSize()));
				check(parsed && parsed->raw == bank->raw, "bank B holds the imported data");
			}
		}

		// One program into the edit buffer.
		const juce::File progFile = work.getChildFile("a03.syx");
		processor.startSysexExport(Unit::Program, 3, progFile);
		if (finishTransfer(processor, "export A03"))
		{
			const auto summary = processor.prepareSysexImport(progFile);
			check((bool) summary["ok"] && summary["items"][0]["unit"].toString() == "program", "import summary: program");
			check(processor.confirmSysexImport(-1), "confirm import into edit buffer");
			finishTransfer(processor, "import into edit buffer");
		}

		// Files without Prophecy data are rejected up front.
		const juce::File junk = work.getChildFile("junk.syx");
		junk.replaceWithText("not sysex");
		check(!(bool) processor.prepareSysexImport(junk)["ok"], "non-SysEx file rejected");
		check(!processor.confirmSysexImport(0), "nothing to confirm after a rejected file");

		stop = true;
		audio.join();
		processor.releaseResources();
		code = g_failures == 0 ? 0 : 1;
	}
	work.deleteRecursively();
	if (code == 0) std::puts("PASS plugin sysex");
	return code;
}
