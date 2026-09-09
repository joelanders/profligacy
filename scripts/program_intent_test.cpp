// SPDX-License-Identifier: AGPL-3.0-only
#include "prophecy_engine.h"
#include "program_state.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

static void require(bool condition, const char* message)
{
	if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

static void render(ProphecyEngine& engine, unsigned frames)
{
	float left[128]{}, right[128]{};
	while (frames)
	{
		const auto count = std::min(frames, 128u);
		const auto first = engine.requestedFrames();
		engine.requestThroughFrame(first + count);
		(void)engine.readAtFrame(first, left, right, count, true);
		frames -= count;
	}
}

static std::vector<std::uint8_t> readback(ProphecyEngine& engine)
{
	std::uint32_t before = 0, after = 0;
	engine.latestProgramData(nullptr, 0, &before);
	const std::uint8_t query[]{0xf0, 0x42, 0x30, 0x41, 0x10, 0, 0xf7};
	require(engine.pushMidi(query, sizeof(query)), "query admission");
	render(engine, 48000);
	std::vector<std::uint8_t> program(1024);
	program.resize(engine.latestProgramData(program.data(), program.size(), &after));
	require(after > before && program.size() == 535, "fresh firmware readback");
	return program;
}

static std::vector<std::uint8_t> readGlobals(ProphecyEngine& engine)
{
	std::uint8_t bytes[4096]{};
	while (engine.popMidiTx(bytes, sizeof(bytes)) != 0) {}
	const std::uint8_t query[]{0xf0, 0x42, 0x30, 0x41, 0x0e, 0, 0xf7};
	require(engine.pushMidi(query, sizeof(query)), "global query admission");
	render(engine, 48000);
	const auto size = engine.popMidiTx(bytes, sizeof(bytes));
	for (std::size_t start = 0; start + 663 <= size; ++start)
	{
		if (bytes[start] != 0xf0 || bytes[start + 4] != 0x51 || bytes[start + 662] != 0xf7) continue;
		std::vector<std::uint8_t> raw;
		for (auto i = start + 6; i < start + 662;)
		{
			const auto high = bytes[i++];
			for (int b = 0; b < 7 && i < start + 662; ++b)
				raw.push_back(std::uint8_t(bytes[i++] | (((high >> b) & 1) << 7)));
		}
		return raw;
	}
	require(false, "fresh global MIDI readback missing");
	return {};
}

static void globalSnapshots(ProphecyEngine& engine)
{
	const auto original = readGlobals(engine);
	require(original.size() == 574, "global fixture size");
	const auto check = [&] {
		const auto expected = readGlobals(engine);
		const auto before = engine.producedFrames();
		const auto snapshot = engine.snapshotGlobals();
		require(engine.producedFrames() == before, "global snapshot advanced firmware");
		require(snapshot == expected, "live global snapshot differs from independent MIDI dump");
	};
	check();
	// Change routing in the edit buffer without WRITE. A snapshot of the stored
	// global record would pass at boot but fail these comparisons.
	for (const auto edit : std::vector<prophecy::ProgramEdit>{{192, 5}, {193, 7}, {206, 12}, {186, 1}})
	{
		auto message = edit.midi();
		message[5] = 0;
		require(engine.pushMidi(message.data(), message.size()), "global edit admission");
		render(engine, 24000);
		check();
	}
	require(engine.snapshotGlobals() != original, "global test did not change live routing");
	std::puts("PASS live global snapshots match all 574 MIDI bytes after unsaved routing edits, without advancing firmware");
}

static void codec()
{
	for (const auto edit : std::vector<prophecy::ProgramEdit>{{154, 0}, {154, 11}, {30, -99}, {30, 99},
		{std::uint16_t(4096 | 388), 1}, {std::uint16_t(8192 | 388), 0}, {635, 127}})
		require(edit.supported(), "documented parameter rejected");
	for (const auto edit : std::vector<prophecy::ProgramEdit>{{154, 12}, {154, -1}, {30, -100}, {20, 0},
		{388, 0}, {std::uint16_t(12288 | 388), 0}, {0x3fff, 1}, {1, 8191}, {635, 128}})
		require(!edit.supported(), "unsupported parameter/address/value accepted");
	std::puts("PASS supported parameter IDs, documented limits and explicit oscillator slot addresses");
	prophecy::ProgramDocument document;
	for (std::size_t i = 0; i < document.base.size(); ++i) document.base[i] = std::uint8_t(i);
	for (int value : {-8192, -1, 0, 8191})
		require(document.append({0x3184, static_cast<std::int16_t>(value)}), "signed edit admission");
	const auto encoded = document.encode();
	const auto decoded = prophecy::ProgramDocument::decode(encoded.data(), encoded.size());
	require(decoded && decoded->encode() == encoded, "document roundtrip");
	const auto midi = document.programMidi();
	const auto fromMidi = prophecy::ProgramDocument::fromMidi(midi.data(), midi.size());
	require(fromMidi && fromMidi->base == document.base, "legacy MIDI unpacking");
	for (std::size_t size = 0; size < encoded.size(); ++size)
		require(!prophecy::ProgramDocument::decode(encoded.data(), size), "truncated document accepted");
	auto invalid = encoded;
	invalid.push_back(0);
	require(!prophecy::ProgramDocument::decode(invalid.data(), invalid.size()), "trailing data accepted");
	invalid = encoded;
	invalid.back() = 0xff;
	require(!prophecy::ProgramDocument::decode(invalid.data(), invalid.size()), "out-of-range value accepted");
	require(!document.append({0, 1}) && !document.append({1, 8192}), "invalid edit accepted");
	while (document.edits.size() < document.maxEdits) require(document.append({1, 1}), "capacity setup");
	const auto full = document.encode();
	require(!document.append({1, 2}) && document.encode() == full, "overflow changed accepted state");
	std::puts("PASS ordered document, signed values, malformed input and bounded admission");

	prophecy::ProgramState state;
	state.restore(*fromMidi);
	const std::vector<std::uint8_t> base(document.base.begin(), document.base.end());
	auto wrongProgram = base;
	wrongProgram[0] ^= 1;
	require(!state.confirm(state.token(), wrongProgram) && state.restoreRequired(),
		"replacement accepted the wrong program readback");
	require(state.confirm(state.token(), base), "initial confirmation");
	require(state.accept({635, 99}), "first accepted edit");
	const auto first = state.token();
	require(state.accept({635, 31}), "second accepted edit");
	auto confirmed = base;
	confirmed[525] = 99;
	require(state.confirm(first, confirmed), "prefix confirmation");
	require(state.document()->edits.size() == 1 && state.document()->edits[0].value == 31,
		"confirmation overwrote newer pending intent");
	require(!state.confirm(first, base), "duplicate confirmation accepted");
	state.fail(prophecy::ProgramState::Error::Restore);
	require(state.document()->edits.size() == 1, "failure discarded accepted intent");
	state.advanceRevision();
	require(!state.document() && !state.confirm(first, base), "superseded state retained ownership");
	state.restore(*fromMidi);
	require(!state.confirm(first, confirmed), "old completion changed replacement program");
	std::puts("PASS prefix confirmation, newer intent, failed operation and superseded ownership");

	require(state.confirm(state.token(), base), "replacement confirmation");
	state.fail(prophecy::ProgramState::Error::InvalidProgram);
	require(state.observe(state.revision(), base) && *state.error() != 0,
		"background observation erased a rejected restore error");
	require(state.accept(std::vector<prophecy::ProgramEdit>{}) && *state.error() != 0,
		"empty edit batch acknowledged an error");
	require(state.accept({1, 'Z'}) && *state.error() == 0, "new accepted edit did not acknowledge error");
	const auto accepted = state.token();
	require(!state.accept({0, 1}), "invalid edit accepted");
	require(state.confirm(accepted, base) && *state.error() != 0,
		"older successful operation erased a newer rejected edit error");
	state.restore(*fromMidi);
	require(*state.error() == 0, "explicit replacement did not acknowledge error");
	std::puts("PASS rejected-operation errors survive background observation and older confirmation");

	require(state.confirm(state.token(), base), "sequence regression base");
	require(state.accept({1, 'A'}), "sequence regression first edit");
	const auto oldPrefix = state.token();
	require(state.confirm(oldPrefix, base) && state.observe(state.revision(), base), "sequence regression refresh");
	require(state.accept({1, 'B'}), "sequence regression second edit");
	require(state.token().through > oldPrefix.through && !state.confirm(oldPrefix, base)
		&& state.document()->edits.size() == 1, "clean observation reused an old prefix token");
	std::puts("PASS clean observations preserve monotonically increasing edit sequence numbers");
}

int main(int argc, char** argv)
{
	codec();
	if (argc == 1) return 0;
	require(argc == 3 || argc == 4, "usage: program intent test [ROM directory NVRAM seed file [--banks|--globals]]");
	namespace fs = std::filesystem;
	const auto fixture = fs::temp_directory_path() / ("profligacy-intent-"
		+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	require(fs::create_directories(fixture / "korgprop"), "create disposable NVRAM");
	require(fs::copy_file(argv[2], fixture / "korgprop/sysram"), "clone NVRAM seed");
	{
		ProphecyEngine engine;
		require(engine.enableHostTimeline(), "enable timeline");
		require(engine.start({"prophecy", "korgprop", "-rompath", argv[1],
			"-nvram_directory", fixture.string(), "-video", "none", "-sound", "none",
			"-nothrottle", "-skip_gameinfo", "-debugger", "none", "-midiprovider", "none",
			"-networkprovider", "none", "-keyboardprovider", "none", "-mouseprovider", "none",
			"-lightgunprovider", "none", "-joystickprovider", "none", "-output", "none", "-noplugins"}), "start");
		require(engine.initializePlayback(), "firmware boot");
		const auto baseline = engine.snapshotProgram();
		require(baseline.size() == 535, "baseline size");
		if (argc == 4)
		{
			if (std::string(argv[3]) == "--globals")
			{
				globalSnapshots(engine);
				engine.stop();
				fs::remove_all(fixture);
				return 0;
			}
			require(std::string(argv[3]) == "--banks", "unknown test mode");
			for (int program : {0, 8, 9, 63, 64, 116, 127})
			{
				const auto before = engine.producedFrames();
				const auto stored = engine.snapshotStoredProgram(program);
				require(stored.size() == 535 && engine.producedFrames() == before, "stored snapshot advanced firmware");
				const std::uint8_t select[]{0xb0, 0, 0, 0xb0, 32, std::uint8_t(program / 64), 0xc0, std::uint8_t(program % 64)};
				require(engine.pushMidi(select, sizeof(select)), "bank program selection");
				render(engine, 144000);
				require(readback(engine) == stored, "stored snapshot differs from selected program MIDI dump");
				std::printf("PASS stored program %d snapshot matches native selection, all 535 bytes\n", program);
			}
			engine.stop();
			fs::remove_all(fixture);
			return 0;
		}

		const std::vector<std::vector<prophecy::ProgramEdit>> scenarios{
			{{1, 'Z'}}, {{635, 99}}, {{154, 11}},
			{{635, 99}, {635, 31}, {1, 'Q'}},
			{{154, 0}, {std::uint16_t(4096 | 388), 0}, {154, 11}}
		};
		for (std::size_t scenario = 0; scenario < scenarios.size(); ++scenario)
		{
			prophecy::ProgramDocument document;
			std::copy(baseline.begin(), baseline.end(), document.base.begin());
			const auto baseMidi = document.programMidi();
			for (const auto edit : scenarios[scenario]) require(document.append(edit), "accept intent");
			const auto beforeSave = engine.producedFrames();
			const auto saved = document.encode();
			require(engine.producedFrames() == beforeSave, "saving advanced firmware");
			const auto restored = prophecy::ProgramDocument::decode(saved.data(), saved.size());
			require(restored.has_value(), "decode saved intent");

			// Oracle: ordinary UART delivery while host audio advances, followed by
			// an independent MIDI dump. No manifest overlay or replay helper here.
			require(engine.initializePlayback(baseMidi.data(), baseMidi.size()), "oracle baseline restore");
			for (const auto edit : scenarios[scenario])
			{
				const auto midi = edit.midi();
				require(engine.pushMidi(midi.data(), midi.size()), "oracle edit admission");
				render(engine, 48000);
			}
			const auto expected = readback(engine);
			std::size_t changed = 0;
			for (std::size_t i = 0; i < baseline.size(); ++i) changed += baseline[i] != expected[i];
			require(changed >= (scenario == 0 ? 1u : 2u), "oracle did not exercise secondary effects");

			// Restore the saved document with no host audio grants or message loop.
			require(engine.initializePlayback(baseMidi.data(), baseMidi.size(), true, restored->edits), "pending-intent restore");
			const auto actual = engine.snapshotProgram();
			if (actual != expected)
				for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i)
					if (actual[i] != expected[i]) std::fprintf(stderr, "scenario %zu byte %zu: %u != %u\n",
						scenario, i, unsigned(actual[i]), unsigned(expected[i]));
			require(actual == expected, "saved intent differs from full native oracle program");
			std::printf("PASS native scenario %zu: %zu ordered edits, %zu changed bytes, full 535-byte equality\n",
				scenario, restored->edits.size(), changed);
		}
	}
	fs::remove_all(fixture);
}
