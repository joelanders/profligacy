// SPDX-License-Identifier: AGPL-3.0-only
// Run against native firmware with an explicitly supplied disposable NVRAM fixture.
#include "prophecy_engine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <utility>

using Bytes = std::vector<std::uint8_t>;

static void require(bool okay, const char* description)
{
	if (!okay) throw std::runtime_error(description);
}

static void advance(ProphecyEngine& engine)
{
	engine.requestThroughFrame(engine.requestedFrames() + 256);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!engine.waitingForInput())
	{
		require(engine.running() && std::chrono::steady_clock::now() < deadline, "firmware grant stalled");
		std::this_thread::sleep_for(std::chrono::microseconds(50));
	}
	float left[64]{}, right[64]{};
	const auto end = engine.producedFrames();
	if (end >= 64) engine.readAtFrame(end - 64, left, right, 64, false);
}

static Bytes parameter(int id, int value)
{
	return {0xf0,0x42,0x30,0x41,0x41,1, std::uint8_t(id & 127),std::uint8_t(id >> 7),
		std::uint8_t(value & 127),std::uint8_t((value & 0x3fff) >> 7),0xf7};
}

static Bytes programMessage(const Bytes& raw)
{
	Bytes message{0xf0,0x42,0x30,0x41,0x40,1};
	for (std::size_t first = 0; first < raw.size(); first += 7)
	{
		const auto count = std::min<std::size_t>(7, raw.size() - first);
		std::uint8_t high = 0;
		for (std::size_t i = 0; i < count; ++i) high |= (raw[first+i] >> 7) << i;
		message.push_back(high);
		for (std::size_t i = 0; i < count; ++i) message.push_back(raw[first+i] & 127);
	}
	message.push_back(0xf7);
	return message;
}

static Bytes finish(ProphecyEngine& engine, std::uint64_t ticket,
	ProphecyEngine::ProgramExchangeStatus expected = ProphecyEngine::ProgramExchangeStatus::Complete)
{
	using Status = ProphecyEngine::ProgramExchangeStatus;
	Bytes program;
	const auto start = engine.requestedFrames();
	for (;;)
	{
		const auto status = engine.pollProgramExchange(ticket, program);
		if (status != Status::Pending)
		{
			require(status == expected, "program exchange result");
			return program;
		}
		require(engine.requestedFrames() - start < 5 * ProphecyEngine::kSampleRate, "exchange timed out");
		advance(engine);
	}
}

static Bytes runExchange(ProphecyEngine& engine, const Bytes& message = {})
{
	const auto ticket = engine.beginProgramExchange(message.data(), message.size());
	require(ticket != 0, "program exchange admission failed");
	return finish(engine, ticket);
}

int main()
{
	const auto* rom = std::getenv("PROPHECY_ROMPATH");
	const auto* nvram = std::getenv("PROPHECY_NVRAM");
	if (!rom || !nvram) return 77;
#if defined(_WIN32)
	_putenv_s("SDL_VIDEODRIVER", "dummy");
	_putenv_s("SDL_AUDIODRIVER", "dummy");
#else
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
	ProphecyEngine engine;
	try
	{
		require(engine.enableHostTimeline(), "enable timeline");
		require(engine.start({"prophecy", "korgprop", "-rompath", rom, "-nvram_directory", nvram,
			"-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo", "-debugger", "none",
			"-midiprovider", "none", "-networkprovider", "none", "-keyboardprovider", "none",
			"-mouseprovider", "none", "-joystickprovider", "none", "-lightgunprovider", "none",
			"-output", "none", "-noplugins"}), "boot");
		require(engine.initializePlayback(), "initialize playback");
		advance(engine);
		const auto frame = engine.requestedFrames();
		const auto base = engine.snapshotProgram();
		require(base.size() == 535 && engine.requestedFrames() == frame, "snapshot advanced firmware");
		const auto baseMessage = programMessage(base);
		const Bytes oversized(ProphecyEngine::kMaxProgramBatchBytes + 1, 0);
		require(!engine.beginProgramExchange(oversized.data(), oversized.size())
			&& !engine.beginProgramExchange(nullptr, 1), "invalid batch admission");
		require(engine.snapshotStoredProgram(-1).empty() && engine.snapshotStoredProgram(128).empty(),
			"stored program bounds");
		const auto edit = parameter(1, 'Q');
		const auto ticket = engine.beginProgramExchange(edit.data(), edit.size());
		require(ticket != 0 && !engine.beginProgramExchange(nullptr, 0), "one outstanding exchange");
		Bytes result;
		require(engine.pollProgramExchange(ticket + 1, result) == ProphecyEngine::ProgramExchangeStatus::Failed,
			"foreign ticket consumed active exchange");
		require(engine.pollProgramExchange(ticket, result) == ProphecyEngine::ProgramExchangeStatus::Pending,
			"stopped input completed an undelivered edit");
		require(engine.requestedFrames() == frame, "poll advanced firmware");
		require(finish(engine, ticket)[0] == 'Q', "ordinary edit result");
		require(engine.pollProgramExchange(ticket, result) == ProphecyEngine::ProgramExchangeStatus::Failed,
			"completed ticket replayed");
		require(runExchange(engine, baseMessage) == base, "replace base");
		Bytes batch;
		for (const auto& entry : {std::pair{1,int('Q')}, {635,99}, {635,32}, {154,1}, {154,0}, {1,int('R')}})
		{
			const auto message = parameter(entry.first, entry.second);
			batch.insert(batch.end(), message.begin(), message.end());
			result = runExchange(engine, message);
		}
		const auto expected = result;
		require(runExchange(engine, baseMessage) == base, "reset batch base");
		require(runExchange(engine, batch) == expected, "batch differs from individual semantic edits");
		std::puts("PASS stopped snapshots, ticket ownership and ordered semantic batch");

		require(runExchange(engine, baseMessage) == base, "physical test base");
		engine.pushPanelPulse(0, 7, 75); // PE1 page; knob1's meaning is firmware-dependent.
		runExchange(engine);
		for (const int value : {255,0})
		{
			require(engine.pushAdin(1, value), "physical knob admission");
			const auto completed = runExchange(engine);
			for (int i = 0; i < 3 * ProphecyEngine::kSampleRate / 256; ++i) advance(engine);
			require(completed == engine.snapshotProgram(), "physical result used the earlier serial dump");
		}
		std::puts("PASS physical control result captured at completion");

		const auto obsolete = engine.beginProgramExchange(edit.data(), edit.size());
		require(obsolete != 0, "obsolete exchange admission");
		require(engine.initializePlayback(baseMessage.data(), baseMessage.size()), "restore did not drain old exchange");
		require(engine.snapshotProgram() == base, "old edit survived replacement");
		require(engine.pollProgramExchange(obsolete, result) == ProphecyEngine::ProgramExchangeStatus::Failed,
			"old completion survived replacement");
		std::puts("PASS replacement joins and invalidates old exchange");

		// The first query uses the previous channel. Its identity reply still
		// drains the exchange and discovers the new channel for the next query.
		const Bytes channel{0xf0,0x42,0x30,0x41,0x41,0,56,1,5,0,0xf7};
		const auto changedChannel = engine.beginProgramExchange(channel.data(), channel.size());
		require(changedChannel != 0, "channel edit admission");
		finish(engine, changedChannel, ProphecyEngine::ProgramExchangeStatus::NoReply);
		require(runExchange(engine) == base, "query after channel change");
		require(engine.snapshotGlobals().at(176) == 5, "configured channel snapshot");
		require(engine.initializePlayback(baseMessage.data(), baseMessage.size()), "restore on configured channel");
		const Bytes disableSysex{0xf0,0x42,0x35,0x41,0x41,0,61,1,0,0,0xf7};
		const auto disabled = engine.beginProgramExchange(disableSysex.data(), disableSysex.size());
		require(disabled != 0, "disable SysEx admission");
		finish(engine, disabled, ProphecyEngine::ProgramExchangeStatus::NoReply);
		require((engine.snapshotGlobals().at(178) & 2) == 0 && engine.snapshotProgram() == base,
			"missing query reply changed or concealed actual state");
		std::puts("PASS channel discovery and explicit missing-reply result");

		const auto grant = engine.producedFrames() + ProphecyEngine::kTimelineCapacity + 256;
		engine.requestThroughFrame(grant);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (engine.available() < ProphecyEngine::kTimelineCapacity)
		{
			require(engine.running() && std::chrono::steady_clock::now() < deadline, "output did not fill");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		require(engine.snapshotProgram() == base && engine.requestedFrames() == grant,
			"snapshot stalled or changed the grant during output backpressure");
		std::puts("PASS program snapshot during output backpressure");
		engine.stop();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL program exchange: %s\n", error.what());
		engine.stop();
		return 1;
	}
}
