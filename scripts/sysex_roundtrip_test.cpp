// SPDX-License-Identifier: AGPL-3.0-only
// SysEx file transfer against the real firmware: export every unit, import modified
// data, export again and compare. Drives the same prophecy::sysex::Transfer as the
// plugin. Needs PROPHECY_ROMPATH and PROPHECY_NVRAM (a sysram seed; the test works on
// a temporary copy). Runs in real time, about four minutes.
#include "prophecy_engine.h"
#include "sysex_transfer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace prophecy::sysex;
using Clock = std::chrono::steady_clock;

static int g_failures = 0;
static void check(bool ok, const std::string &what)
{
	std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) ++g_failures;
}

struct EnginePort : SysexPort
{
	explicit EnginePort(ProphecyEngine &e) : engine(e) {}
	ProphecyEngine &engine;
	bool ready() override { return true; }
	int channel() override { return engine.firmwareChannel(); }
	double emulatedMs() override { return double(engine.producedFrames()) * 1000.0 / ProphecyEngine::kSampleRate; }
	bool openStream() override { return engine.openSysexStream(); }
	std::size_t pushStream(const std::uint8_t *b, std::size_t n) override { return engine.pushSysexStream(b, n); }
	std::size_t streamPending() override { return engine.sysexStreamPending(); }
	void closeStream() override { engine.closeSysexStream(); }
	bool popReply(std::vector<std::uint8_t> &out) override { return engine.popSysexReply(out); }
	void clearReplies() override { engine.clearSysexReplies(); }
};

static double nowMs()
{
	static const auto start = Clock::now();
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

static bool run(Transfer &t, EnginePort &port)
{
	while (t.needsTicks())
	{
		t.tick(port, nowMs());
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (t.status().state != Transfer::State::Succeeded)
		std::fprintf(stderr, "     transfer: %s\n", t.status().message.c_str());
	return t.status().state == Transfer::State::Succeeded;
}

static std::optional<Dump> exportUnit(EnginePort &port, Unit unit, int index)
{
	Transfer t;
	t.startExport(unit, index);
	if (!run(t, port)) return std::nullopt;
	return parse(t.exported());
}

static bool importDumps(EnginePort &port, std::vector<Dump> dumps)
{
	Transfer t;
	t.startImport(std::move(dumps));
	return run(t, port);
}

// Ask for the identity reply again and wait for it; the engine takes the synth's channel
// from it. Returns false if no reply arrives.
static bool identify(ProphecyEngine &engine, int expected)
{
	const std::uint8_t identity[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		engine.pushMidi(identity, sizeof(identity));
		for (int i = 0; i < 100; ++i)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			if (engine.firmwareChannel() == expected) return true;
		}
	}
	return false;
}

static void setGlobalParameter(ProphecyEngine &engine, int param, int value)
{
	const auto msg = globalParameter(param, value, engine.firmwareChannel());
	engine.pushMidi(msg.data(), msg.size());
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

static void setName(std::vector<std::uint8_t> &raw, std::size_t offset, const char *name)
{
	for (std::size_t i = 0; i < kNameLength; ++i)
		raw[offset + i] = std::uint8_t(name[i] ? name[i] : ' ');
}

int main()
{
#if defined(_WIN32)
	_putenv_s("SDL_VIDEODRIVER", "dummy");
	_putenv_s("SDL_AUDIODRIVER", "dummy");
#else
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
	const char *rom = std::getenv("PROPHECY_ROMPATH");
	const char *seed = std::getenv("PROPHECY_NVRAM");
	if (!rom || !seed)
	{
		std::puts("Requires PROPHECY_ROMPATH and a PROPHECY_NVRAM sysram seed");
		return 77;
	}
	namespace fs = std::filesystem;
	const fs::path nvram = fs::temp_directory_path() / ("profligacy-sysex-" + std::to_string(std::rand()));
	fs::create_directories(nvram);
	fs::copy(seed, nvram, fs::copy_options::recursive);

	const std::vector<std::string> args = {
		"prophecy", "korgprop", "-rompath", rom, "-nvram_directory", nvram.string(),
		"-video", "none", "-sound", "none", "-skip_gameinfo",
		"-debugger", "none", "-midiprovider", "none", "-networkprovider", "none",
		"-keyboardprovider", "none", "-mouseprovider", "none", "-joystickprovider", "none",
		"-lightgunprovider", "none", "-output", "none", "-noplugins"
	};
	ProphecyEngine engine;
	if (!engine.start(args)) { std::fputs("FAIL engine start\n", stderr); return 1; }
	std::atomic<bool> stop{false};
	std::thread audio([&] {
		float l[256], r[256];
		while (!stop.load())
			if (engine.pull(l, r, 256) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	});
	auto finish = [&](int code) {
		stop = true;
		audio.join();
		engine.stop();
		std::error_code ec;
		fs::remove_all(nvram, ec);
		return code;
	};

	// Wait for the program page before sending anything (bytes during the power-on
	// self-test can wedge the firmware), then identify the channel.
	{
		const auto deadline = Clock::now() + std::chrono::seconds(60);
		char l1[41]{}, l2[41]{};
		while (!(engine.latestLcd(l1, l2, sizeof(l1)) && (l1[0] == 'A' || l1[0] == 'B') && l1[3] == ':'))
		{
			if (Clock::now() > deadline) { std::fputs("FAIL firmware did not boot\n", stderr); return finish(1); }
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		const std::uint8_t identity[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
		engine.pushMidi(identity, sizeof(identity));
		std::this_thread::sleep_for(std::chrono::seconds(2));
	}
	EnginePort port(engine);

	// Export every unit.
	const auto current = exportUnit(port, Unit::CurrentProgram, 0);
	const auto a05 = exportUnit(port, Unit::Program, 5);
	const auto bankB = exportUnit(port, Unit::Bank, 1);
	const auto pat3 = exportUnit(port, Unit::Pattern, 3);
	const auto pat4 = exportUnit(port, Unit::Pattern, 4);
	const auto patterns = exportUnit(port, Unit::AllPatterns, 0);
	const auto global = exportUnit(port, Unit::Global, 0);
	const auto programs = exportUnit(port, Unit::AllPrograms, 0);
	const auto all = exportUnit(port, Unit::AllData, 0);
	check(current && current->unit == Unit::CurrentProgram, "export current program");
	check(a05 && a05->index == 5, "export program A05");
	check(bankB && bankB->index == 1, "export bank B");
	check(pat3 && pat3->index == 3 && pat4 && pat4->index == 4, "export patterns 3 and 4");
	check(patterns.has_value(), "export all patterns");
	check(global.has_value(), "export global");
	check(programs.has_value(), "export all programs");
	check(all.has_value(), "export all data");
	if (g_failures) return finish(1);
	check(std::equal(a05->raw.begin(), a05->raw.end(), programs->raw.begin() + 5 * long(kProgramBytes)),
		"A05 matches all-programs dump");
	check(std::equal(bankB->raw.begin(), bankB->raw.end(), programs->raw.begin() + 64 * long(kProgramBytes)),
		"bank B matches all-programs dump");
	check(std::equal(global->raw.begin(), global->raw.end(), all->raw.begin()), "global prefixes all data");
	check(global->raw[kGlobalProtectOffset] == 0x03, "memory protected at start");

	// One program, renamed, into a different slot.
	{
		Dump p = *a05;
		p.index = 6;
		setName(p.raw, 0, "RoundTrip A06");
		check(importDumps(port, {p}), "import program into A06");
		const auto back = exportUnit(port, Unit::Program, 6);
		check(back && back->raw == p.raw, "A06 reads back as imported");
		const auto a05again = exportUnit(port, Unit::Program, 5);
		check(a05again && a05again->raw == a05->raw, "A05 untouched");
	}
	// A bank with one program renamed.
	{
		Dump b = *bankB;
		setName(b.raw, 10 * kProgramBytes, "RoundTrip B10");
		check(importDumps(port, {b}), "import bank B");
		const auto back = exportUnit(port, Unit::Bank, 1);
		check(back && back->raw == b.raw, "bank B reads back as imported");
	}
	// The edit buffer.
	{
		Dump e = *current;
		setName(e.raw, 0, "RoundTrip Edit");
		check(importDumps(port, {e}), "import edit buffer");
		const auto back = exportUnit(port, Unit::CurrentProgram, 0);
		check(back && back->raw == e.raw, "edit buffer reads back as imported");
	}
	// An arpeggio pattern copied into another slot, then all patterns restored.
	{
		Dump p = *pat4;
		p.index = 3;
		check(importDumps(port, {p}), "import pattern 4 into slot 3");
		const auto back = exportUnit(port, Unit::Pattern, 3);
		check(back && back->raw == pat4->raw, "pattern 3 reads back as pattern 4");
		check(importDumps(port, {*patterns}), "import all patterns");
		const auto restored = exportUnit(port, Unit::AllPatterns, 0);
		check(restored && restored->raw == patterns->raw, "all patterns restored");
	}
	// Global channel at offset 176, checked against the channel the identity reply
	// reports, before and after moving it with Global MIDI Channel (p184).
	const int startChannel = engine.firmwareChannel();
	const int movedChannel = (startChannel + 5) & 0x0F;
	check(global->raw[kGlobalChannelOffset] == startChannel, "global offset 176 is the identified channel");
	setGlobalParameter(engine, 184, movedChannel);
	check(identify(engine, movedChannel), "p184 moves the channel the synth identifies on");
	{
		const auto moved = exportUnit(port, Unit::Global, 0);
		check(moved && moved->raw[kGlobalChannelOffset] == movedChannel, "global offset 176 follows p184");
	}
	// Global: the name changes; the channel and the Program Change and select maps the
	// file asks for do not. Offset 178 (SysEx switches) is preserved but not checked:
	// it cannot be read back with transmit off, and receive off would end the test.
	{
		const auto before = exportUnit(port, Unit::Global, 0);
		Dump g = *global;
		setName(g.raw, 147, "RoundTrip Global");
		g.raw[kGlobalChannelOffset] = std::uint8_t((movedChannel + 3) & 0x0F);
		g.raw[kGlobalProgramChangeOffset] ^= 0x01;
		g.raw[kGlobalBankMapOffset] = std::uint8_t((g.raw[kGlobalBankMapOffset] + 1) & 0x7F);
		g.raw[kGlobalProgramMapOffset + 5] = std::uint8_t((g.raw[kGlobalProgramMapOffset + 5] + 9) & 0x7F);
		check(importDumps(port, {g}), "import global");
		check(identify(engine, movedChannel), "synth still identifies on its own channel");
		const auto back = exportUnit(port, Unit::Global, 0);
		check(before && back, "global readable before and after import");
		if (before && back)
		{
			check(programName(back->raw.data() + 147) == "RoundTrip Global", "global name imported");
			check(back->raw[kGlobalChannelOffset] == movedChannel, "global channel kept");
			check(std::equal(before->raw.begin() + long(kGlobalSysexOffset), before->raw.begin() + long(kGlobalProgramMapEnd),
				back->raw.begin() + long(kGlobalSysexOffset)), "SysEx, Program Change and select maps kept");
		}
	}
	setGlobalParameter(engine, 184, startChannel);
	check(identify(engine, startChannel), "channel moved back");
	// All data puts everything back the way it started, protect included.
	{
		check(importDumps(port, {*all}), "import all data");
		const auto back = exportUnit(port, Unit::AllData, 0);
		check(back && back->raw == all->raw, "all data restored byte for byte");
		const auto g = exportUnit(port, Unit::Global, 0);
		check(g && g->raw[kGlobalProtectOffset] == 0x03, "memory protected at end");
	}

	if (g_failures == 0) std::puts("PASS sysex round trip");
	return finish(g_failures == 0 ? 0 : 1);
}
