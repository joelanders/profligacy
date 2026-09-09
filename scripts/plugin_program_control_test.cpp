// SPDX-License-Identifier: AGPL-3.0-only
// Same state/control scenarios against the delayed fake and the native firmware.
#include "PluginProcessor.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#if PROFLIGACY_FAKE_ENGINE
#include "prophecy_engine_fake.h"
#endif

struct ProphecyProgramControlTestAccess
{
	static bool rawMidi(ProphecyAudioProcessor& processor, const std::uint8_t* bytes, std::size_t size)
	{ return processor.m_engine.pushMidi(bytes, size); }
	static std::vector<std::uint8_t> globals(ProphecyAudioProcessor& processor)
	{ return processor.m_engine.snapshotGlobals(); }
	static std::vector<std::uint8_t> storedProgram(ProphecyAudioProcessor& processor, int program)
	{ return processor.m_engine.snapshotStoredProgram(program); }
	static bool rejectsEngineEdit(ProphecyAudioProcessor& processor, prophecy::ProgramEdit edit,
		const prophecy::ProgramDocument& document)
	{
		std::lock_guard lock(processor.m_programState.mutex);
		const auto before = processor.m_engine.snapshotProgram();
		const auto frame = processor.m_engine.producedFrames();
		const auto midi = document.programMidi();
		return !processor.m_engine.initializePlayback(midi.data(), midi.size(), true, {edit})
			&& processor.m_engine.readyForPlayback() && processor.m_engine.producedFrames() == frame
			&& processor.m_engine.snapshotProgram() == before;
	}
	static void describeWriteWait(ProphecyAudioProcessor& processor)
	{
		// Read atomics only: the control thread may still own both operation
		// mutexes. A diagnostic snapshot here could itself wait for the WRITE.
		std::fprintf(stderr, "WRITE timeout: ready=%d requested=%llu produced=%llu waiting=%d error=%s\n",
			processor.m_engine.readyForPlayback(),
			(unsigned long long)processor.m_engine.requestedFrames(),
			(unsigned long long)processor.m_engine.producedFrames(),
			processor.m_engine.waitingForInput(), processor.initializationError());
	}
	static void protectedPatternWrite(ProphecyAudioProcessor& processor, const std::vector<std::uint8_t>& pattern)
	{
		std::lock_guard lock(processor.m_programState.mutex);
		processor.setParamG(0, 171, 0);
		processor.sendArpeggioPatternData(5, pattern);
		processor.setParamG(0, 171, 1);
	}
	static void midiArrivingBeforeWritePause(ProphecyAudioProcessor& processor)
	{
		// Model input arriving after the control loop's inbox drain but before
		// its due WRITE task pauses the callback. Holding the control mutex makes
		// the interleaving deterministic; the audio callback never takes it.
		std::lock_guard lock(processor.m_programState.mutex);
		if (!processor.writePatch(127)) std::abort();
		juce::MidiBuffer midi;
		juce::AudioBuffer<float> audio(2, 128);
		midi.addEvent(juce::MidiMessage::programChange(1, 9), 0);
		processor.processBlock(audio, midi);
		processor.m_writeSeq.service(std::chrono::steady_clock::now() + std::chrono::seconds(1));
	}
	static void overflowProgramInput(ProphecyAudioProcessor& processor)
	{
		std::lock_guard lock(processor.m_programState.mutex);
		juce::MidiBuffer midi;
		juce::AudioBuffer<float> audio(2, 128);
		for (int i = 0; i < 127; ++i)
			midi.addEvent(juce::MidiMessage::controllerEvent(1, 10, 0), 0);
		midi.addEvent(juce::MidiMessage::programChange(1, 9), 0);
		midi.addEvent(juce::MidiMessage::programChange(1, 8), 0);
		processor.processBlock(audio, midi);
	}
	static bool grantedWorkFinished(ProphecyAudioProcessor& processor)
	{
#if PROFLIGACY_FAKE_ENGINE
		(void)processor;
		return true;
#else
		return processor.m_engine.waitingForInput();
#endif
	}
};

static void require(bool value, const char* message)
{
	if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

static void environment(const char* key, const juce::String& value)
{
#if JUCE_WINDOWS
	_putenv_s(key, value.toRawUTF8());
#else
	if (value.isEmpty()) unsetenv(key);
	else setenv(key, value.toRawUTF8(), 1);
#endif
}

static std::vector<std::uint8_t> unpack(const juce::MemoryBlock& state)
{
	require(state.getSize() >= 6 && std::memcmp(state.getData(), "PRP2", 4) == 0, "state container");
	const auto* bytes = static_cast<const std::uint8_t*>(state.getData());
	const auto first = 6 + 2 * std::size_t(bytes[4]);
	require(state.getSize() >= first + 8 && bytes[first] == 0xf0, "state program payload");
	std::vector<std::uint8_t> raw;
	for (auto i = first + 6; i + 1 < state.getSize();)
	{
		const auto high = bytes[i++];
		for (int b = 0; b < 7 && i + 1 < state.getSize(); ++b)
			raw.push_back(std::uint8_t(bytes[i++] | (((high >> b) & 1) << 7)));
	}
	return raw;
}

static juce::MemoryBlock save(ProphecyAudioProcessor& processor)
{
	juce::MemoryBlock state;
	processor.getStateInformation(state);
	return state;
}

static void render(ProphecyAudioProcessor& processor, int blocks = 1500)
{
	juce::MidiBuffer midi;
	juce::AudioBuffer<float> audio(2, 128);
	for (int i = 0; i < blocks; ++i) processor.processBlock(audio, midi);
}

static void finishGrantedWork(ProphecyAudioProcessor& processor)
{
	// Leading silence may return before the worker finishes an existing grant.
	// Wait for that work without another callback or grant before measuring save.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!ProphecyProgramControlTestAccess::grantedWorkFinished(processor)
		&& std::chrono::steady_clock::now() < deadline) juce::Thread::sleep(1);
	require(ProphecyProgramControlTestAccess::grantedWorkFinished(processor), "prior callback grant did not finish");
}

static void pump(ProphecyAudioProcessor& processor)
{
	const auto end = juce::Time::getMillisecondCounterHiRes() + 700;
	do
	{
		juce::Timer::callPendingTimersSynchronously();
		render(processor, 1);
		juce::Thread::sleep(1);
	} while (juce::Time::getMillisecondCounterHiRes() < end);
	render(processor);
}

static void midiProgram(ProphecyAudioProcessor& processor, int program)
{
	juce::MidiBuffer midi;
	juce::AudioBuffer<float> audio(2, 128);
	midi.addEvent(juce::MidiMessage::programChange(1, program), 0);
	processor.processBlock(audio, midi);
	render(processor);
}

static std::vector<std::uint8_t> dump(ProphecyAudioProcessor& processor)
{
	const auto firstFrame = processor.diagnosticSnapshot().producedFrames;
	const auto generation = processor.requestProgramDump();
	std::vector<std::uint8_t> raw(1024);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < deadline)
	{
		render(processor, 16);
		juce::Thread::sleep(1);
		std::uint64_t completed = 0;
		const auto size = processor.getProgramData(raw.data(), raw.size(), nullptr, &completed);
		if (completed >= generation && size == 535) { raw.resize(size); return raw; }
	}
	const auto diagnostics = processor.diagnosticSnapshot();
	std::uint64_t completed = 0;
	std::uint32_t version = 0;
	processor.getProgramData(raw.data(), raw.size(), &version, &completed);
	std::fprintf(stderr, "readback timeout: request=%llu completed=%llu version=%u advanced=%llu requests=%llu sends=%llu pending=%zu error=%s\n",
		(unsigned long long)generation, (unsigned long long)completed, version,
		(unsigned long long)(diagnostics.producedFrames - firstFrame), (unsigned long long)diagnostics.editorDumpRequests,
		(unsigned long long)diagnostics.editorDumpSends, diagnostics.editorCommandsPending, processor.programStateError());
	require(false, "program readback did not complete");
	return {};
}

static std::vector<std::vector<std::uint8_t>> transmittedMessages(ProphecyAudioProcessor& processor)
{
	std::vector<std::vector<std::uint8_t>> messages;
	std::vector<std::uint8_t> partial;
	ProphecyEngine::MidiTxByteEvent events[2048];
	while (const auto count = processor.popMidiTxByteEvents(events, 2048))
		for (std::size_t i = 0; i < count; ++i)
		{
			const auto byte = events[i].byte;
			if (byte == 0xf0) partial.clear();
			else if (byte >= 0xf8 || partial.empty()) continue;
			partial.push_back(byte);
			if (byte == 0xf7) { messages.push_back(std::move(partial)); partial.clear(); }
		}
	return messages;
}

static void verifyRestoreReplies(ProphecyAudioProcessor& processor,
	const std::vector<std::vector<std::uint8_t>>& expected)
{
	if (PROFLIGACY_FAKE_ENGINE) return;
	std::size_t next = 0;
	bool awaitingIdentity = false;
	for (const auto& message : transmittedMessages(processor))
	{
		if (const auto program = prophecy::ProgramDocument::fromMidi(message.data(), message.size()))
		{
			require(!awaitingIdentity && next < expected.size(), "overlapping or unexpected restore readback");
			require(std::equal(program->base.begin(), program->base.end(), expected[next].begin(), expected[next].end()),
				"restore readback belongs to the wrong edit prefix");
			++next;
			awaitingIdentity = true;
		}
		else if (message.size() == 15 && message[1] == 0x7e && message[4] == 2)
			awaitingIdentity = false;
	}
	require(next == expected.size() && !awaitingIdentity, "restore did not finish every readback transaction");
	require(processor.droppedMidiTxByteEvents() == 0, "native readback trace overflow");
}

static void pendingSave(ProphecyAudioProcessor& processor, const juce::MemoryBlock& initial)
{
	std::vector<std::vector<prophecy::ProgramEdit>> scenarios{
		{{1, 'Z'}}, {{635, 99}}, {{154, 11}},
		{{635, 99}, {635, 31}, {1, 'Q'}},
		{{154, 0}, {std::uint16_t(4096 | 388), 0}, {154, 11}}
	};
	std::string name = "Saved intent";
	name.resize(16, ' ');
	std::vector<prophecy::ProgramEdit> rename;
	for (std::size_t i = 0; i < name.size(); ++i)
		rename.push_back({std::uint16_t(i + 1), std::int16_t(name[i])});
	scenarios.push_back(rename);
	scenarios.push_back({{4484, 0}, {4485, 0}, {4486, 99}, {4487, 0}, {4488, 0}, {4490, 0}, {4492, 0}});
	for (std::size_t scenario = 0; scenario < scenarios.size(); ++scenario)
	{
		if (PROFLIGACY_FAKE_ENGINE && scenario != 0 && scenario != 5) continue;
		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		require(processor.romOk(), "oracle restore");
		std::vector<std::vector<std::uint8_t>> expectedReadbacks{unpack(initial)};
		for (const auto edit : scenarios[scenario])
		{
			const auto message = edit.midi();
			require(ProphecyProgramControlTestAccess::rawMidi(processor, message.data(), message.size()), "oracle edit admission");
			render(processor, 375);
			expectedReadbacks.push_back(dump(processor));
		}
		const auto& expected = expectedReadbacks.back();
		require(scenario == 6 || expected != unpack(initial), "oracle edit did not apply");
		if (scenario == 4)
			require(expectedReadbacks[1] != expectedReadbacks[2], "dependent oscillator edit was a no-op");

		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		processor.releaseResources();
		const auto before = processor.diagnosticSnapshot();
		if (scenario == 5) processor.renamePatch("Saved intent");
		else if (scenario == 6) processor.sendMacro("saw");
		else for (const auto edit : scenarios[scenario]) processor.setParam(edit.parameter, edit.value);
		require(*processor.programStateError() == 0, "program edit admission");
		const auto pending = save(processor);
		require(pending.getSize() > 6 && std::memcmp(pending.getData(), "PRP3", 4) == 0,
			"immediate save omitted pending intent");
		const auto* bytes = static_cast<const std::uint8_t*>(pending.getData());
		const auto offset = 6 + std::size_t(bytes[4]) * 2;
		const auto document = prophecy::ProgramDocument::decode(bytes + offset, pending.getSize() - offset);
		require(document && document->edits.size() == scenarios[scenario].size(), "batch acceptance saved only part of the request");
		require(processor.diagnosticSnapshot().producedFrames == before.producedFrames,
			"accepting or saving a stopped edit advanced firmware");
		(void)transmittedMessages(processor);
		processor.setStateInformation(pending.getData(), int(pending.getSize()));
		require(processor.romOk() && *processor.programStateError() == 0, "stopped intent restore");
		verifyRestoreReplies(processor, expectedReadbacks);
		require(processor.diagnosticSnapshot().audioHostFrames == before.audioHostFrames,
			"restore needed a host audio callback");
		const auto actual = unpack(save(processor));
		if (actual != expected)
			for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i)
				if (actual[i] != expected[i]) std::fprintf(stderr, "scenario %zu byte %zu: restored %u, oracle %u\n",
					scenario, i, unsigned(actual[i]), unsigned(expected[i]));
		require(actual == expected, "restored pending intent differs from native oracle");
		processor.prepareToPlay(48000, 128);
		pump(processor);
		require(unpack(save(processor)) == expected, "restored edit was replayed by an obsolete timer");
		std::printf("PASS processor immediate save and stopped restore, scenario %zu, full 535-byte equality\n", scenario);
	}
}

static void liveSave(ProphecyAudioProcessor& processor, const juce::MemoryBlock& initial)
{
	(void)transmittedMessages(processor);
	const auto forwarded = processor.diagnosticSnapshot().hostMidiEventsForwarded;
	const std::uint8_t identity[]{0xf0, 0x7e, 0x7f, 6, 1, 0xf7};
	const std::uint8_t query[]{0xf0, 0x42, 0x30, 0x41, 0x10, 0, 0xf7};
	juce::MidiBuffer inquiries;
	juce::AudioBuffer<float> audio(2, 128);
	inquiries.addEvent(identity, sizeof(identity), 0);
	inquiries.addEvent(query, sizeof(query), 1);
	inquiries.addEvent(juce::MidiMessage::noteOn(1, 60, juce::uint8(64)), 7);
	inquiries.addEvent(juce::MidiMessage::noteOff(1, 60), 8);
	processor.processBlock(audio, inquiries);
	processor.sendMidi(identity, sizeof(identity));
	render(processor, 375);
	require(processor.diagnosticSnapshot().hostMidiEventsForwarded == forwarded + 2,
		"host inquiries entered the private firmware reply stream");
	require(transmittedMessages(processor).empty(), "unowned firmware inquiry produced a reply");
	const auto requests = processor.diagnosticSnapshot().editorDumpRequests;
	processor.sendMidi(query, sizeof(query));
	require(processor.diagnosticSnapshot().editorDumpRequests == requests + 1,
		"raw editor readback bypassed the control owner");
	(void)dump(processor);
	std::puts("PASS firmware inquiries stay in the control owner while host notes retain their MIDI path");

	const std::vector<std::vector<prophecy::ProgramEdit>> scenarios{
		{{1, 'Z'}, {2, 'Y'}}, {{635, 99}, {635, 31}, {1, 'Q'}},
		{{154, 0}, {std::uint16_t(4096 | 388), 0}, {154, 11}}
	};
	for (std::size_t scenario = 0; scenario < scenarios.size(); ++scenario)
	{
		if (PROFLIGACY_FAKE_ENGINE && scenario != 0) continue;
		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		for (const auto edit : scenarios[scenario])
		{
			const auto message = edit.midi();
			require(ProphecyProgramControlTestAccess::rawMidi(processor, message.data(), message.size()), "oracle edit admission");
			render(processor, 375);
		}
		const auto expected = dump(processor);
		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		for (const auto edit : scenarios[scenario]) processor.setParam(edit.parameter, edit.value);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		bool confirmed = false;
		while (std::chrono::steady_clock::now() < deadline)
		{
			render(processor, 1);
			juce::Thread::sleep(1);
			const auto state = save(processor);
			if (state.getSize() >= 6 && std::memcmp(state.getData(), "PRP2", 4) == 0)
			{
				require(unpack(state) == expected, "live confirmation differs from native oracle");
				confirmed = true;
				break;
			}
		}
		require(confirmed && *processor.programStateError() == 0, "live edit batch never confirmed");
		processor.releaseResources();
		const auto before = processor.diagnosticSnapshot();
		require(unpack(save(processor)) == expected, "stopped save lost confirmed live edits");
		require(processor.diagnosticSnapshot().producedFrames == before.producedFrames, "saving advanced firmware");
		std::printf("PASS live batch %zu: confirmed and retired, all 535 bytes match independent firmware\n", scenario);
	}

	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	(void)transmittedMessages(processor);
	const auto sent = processor.diagnosticSnapshot().editorDumpSends;
	processor.setParam(1, 'X');
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	std::vector<std::uint8_t> partial;
	bool started = false;
	while (!started && std::chrono::steady_clock::now() < deadline)
	{
		render(processor, 1);
		juce::Thread::sleep(1);
		if (PROFLIGACY_FAKE_ENGINE)
			started = processor.diagnosticSnapshot().editorDumpSends > sent;
		else
		{
			ProphecyEngine::MidiTxByteEvent events[256];
			while (const auto count = processor.popMidiTxByteEvents(events, 256))
				for (std::size_t i = 0; i < count; ++i)
				{
					const auto byte = events[i].byte;
					if (byte == 0xf0) partial.clear();
					else if (byte >= 0xf8 || partial.empty()) continue;
					partial.push_back(byte);
					if (byte == 0xf7) partial.clear();
				}
			started = partial.size() >= 6 && partial[4] == 0x40;
		}
	}
	require(started, "did not interrupt an in-flight readback");
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	require(processor.romOk() && save(processor) == initial, "replacement did not drain the older readback");
	pump(processor);
	require(save(processor) == initial, "obsolete readback changed the replacement program");
	std::puts("PASS replacement during an in-flight readback; old exchange drained and old result rejected");
}

static void selectionSave(ProphecyAudioProcessor& processor, const juce::MemoryBlock& initial)
{
	for (int program : {8, 64, 127})
	{
		const std::uint8_t select[]{0xb0, 0, 0, 0xb0, 32, std::uint8_t(program / 64), 0xc0, std::uint8_t(program % 64)};
		processor.sendMidi(select, sizeof(select));
		render(processor, 1125);
		const auto expected = dump(processor);
		require(expected.size() == 535, "selection oracle");
		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		processor.releaseResources();
		const auto before = processor.diagnosticSnapshot();
		processor.setParam(2, 'X'); // old accepted work must be superseded
		processor.selectPatch(program == 8 ? 9 : 8);
		processor.selectPatch(program);
		require(unpack(save(processor)) == expected, "immediate save did not retain the newest selection");
		processor.setParam(1, 'Z');
		const auto pending = save(processor);
		require(std::memcmp(pending.getData(), "PRP3", 4) == 0, "selection edit not retained");
		require(processor.diagnosticSnapshot().producedFrames == before.producedFrames,
			"stopped selection or save advanced firmware");
		processor.setStateInformation(pending.getData(), int(pending.getSize()));
		auto edited = expected;
		edited[0] = 'Z';
		require(processor.romOk() && unpack(save(processor)) == edited,
			"stopped selection plus edit restored against the wrong base");
		processor.prepareToPlay(48000, 128);
		pump(processor);
		require(unpack(save(processor)) == edited, "superseded selection/edits changed restored state");

		// Live delivery must first confirm the selected stored program, then apply
		// the edit against it; time passing alone cannot open that barrier.
		processor.selectPatch(program);
		processor.setParam(1, 'Y');
		edited[0] = 'Y';
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
		bool complete = false;
		while (std::chrono::steady_clock::now() < deadline)
		{
			render(processor, 1);
			juce::Thread::sleep(1);
			const auto state = save(processor);
			if (state.getSize() >= 6 && std::memcmp(state.getData(), "PRP2", 4) == 0)
			{
				require(unpack(state) == edited, "live selection/edit confirmation differs from oracle");
				complete = true;
				break;
			}
			if (*processor.programStateError()) break;
		}
		require(complete && *processor.programStateError() == 0, "live selection and subsequent edit did not confirm");
		std::printf("PASS selection %d: newest intent saved immediately, stopped restore and ordered live edit\n", program);
	}
}

static void incomingParameterEdits(ProphecyAudioProcessor& processor, const juce::MemoryBlock& initial)
{
	const std::vector<std::vector<prophecy::ProgramEdit>> scenarios{
		{{1, 'E'}, {2, 'H'}, {3, 'U'}, {4, 'L'}},
		{{635, 99}, {635, 31}, {1, 'Q'}, {2, 'H'}},
		{{154, 0}, {4484, 0}, {154, 11}, {1, 'Z'}}
	};
	for (std::size_t scenario = 0; scenario < scenarios.size(); ++scenario)
	{
		if (PROFLIGACY_FAKE_ENGINE && scenario != 0) continue;
		const auto& edits = scenarios[scenario];
		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		for (const auto edit : edits)
		{
			const auto message = edit.midi();
			require(ProphecyProgramControlTestAccess::rawMidi(processor, message.data(), message.size()), "mixed-edit oracle admission");
			render(processor, 375);
		}
		const auto expected = dump(processor);
		require(expected != unpack(initial), "mixed-edit oracle was a no-op");
		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		for (std::size_t i = 0; i < edits.size(); ++i)
		{
			const auto message = edits[i].midi();
			if (i == 0) processor.setParam(edits[i].parameter, edits[i].value);
			else if (i == 2) processor.sendMidi(message.data(), message.size());
			else
			{
				juce::MidiBuffer midi;
				juce::AudioBuffer<float> audio(2, 128);
				midi.addEvent(message.data(), int(message.size()), 17);
				processor.processBlock(audio, midi);
			}
		}
		finishGrantedWork(processor);
		const auto frame = processor.diagnosticSnapshot().producedFrames;
		const auto pending = save(processor);
		require(processor.diagnosticSnapshot().producedFrames == frame, "mixed-edit save advanced firmware");
		require(pending.getSize() >= 6 && std::memcmp(pending.getData(), "PRP3", 4) == 0,
			"mixed parameter input was not saved as pending intent");
		const auto* bytes = static_cast<const std::uint8_t*>(pending.getData());
		const auto offset = 6 + std::size_t(bytes[4]) * 2;
		const auto document = prophecy::ProgramDocument::decode(bytes + offset, pending.getSize() - offset);
		require(document && document->edits.size() == edits.size(), "mixed parameter input was lost or applied twice");
		for (std::size_t i = 0; i < edits.size(); ++i)
			require(document->edits[i].parameter == edits[i].parameter && document->edits[i].value == edits[i].value,
				"editor and MIDI parameter input changed order");
		require(dump(processor) == expected, "live mixed edits differ from direct firmware oracle");
		processor.releaseResources();
		processor.setStateInformation(pending.getData(), int(pending.getSize()));
		require(unpack(save(processor)) == expected, "stopped mixed-edit restore differs from direct firmware oracle");
		processor.prepareToPlay(48000, 128);
		std::printf("PASS mixed parameter input %zu: ordered editor/host/UI MIDI, live and stopped restore, all 535 bytes\n", scenario);
	}
}

static void incomingPrograms(ProphecyAudioProcessor& processor, const juce::MemoryBlock& initial)
{
	for (int program : {9, 73, 8})
	{
		const std::uint8_t selection[]{0xb0, 0, 0, 0xb0, 32, std::uint8_t(program / 64), 0xc0, std::uint8_t(program % 64)};
		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		require(ProphecyProgramControlTestAccess::rawMidi(processor, selection, sizeof(selection)), "oracle program admission");
		render(processor);
		const auto expected = dump(processor);
		processor.setStateInformation(initial.getData(), int(initial.getSize()));
		processor.selectPatch(program == 9 ? 8 : 9);
		processor.setParam(1, 'X');
		juce::MidiBuffer midi;
		juce::AudioBuffer<float> audio(2, 128);
		midi.addEvent(selection, 3, 3);
		midi.addEvent(selection + 3, 3, 7);
		midi.addEvent(selection + 6, 2, 29);
		midi.addEvent(juce::MidiMessage::noteOn(1, 60, std::uint8_t(64)), 97);
		processor.processBlock(audio, midi);
		finishGrantedWork(processor);
		const auto frame = processor.diagnosticSnapshot().producedFrames;
		const auto accepted = save(processor);
		require(unpack(accepted) == expected, "immediate MIDI program save retained the previous program");
		require(processor.diagnosticSnapshot().producedFrames == frame, "MIDI program save advanced firmware");
		require(dump(processor) == expected, "MIDI program did not confirm against raw UART oracle");
		processor.releaseResources();
		processor.setStateInformation(accepted.getData(), int(accepted.getSize()));
		require(save(processor) == accepted, "stopped restore lost accepted MIDI program");
		processor.prepareToPlay(48000, 128);
		std::printf("PASS host MIDI program %d: immediate save, live confirmation and stopped restore match raw UART oracle\n", program);
	}
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	const auto beforeOverflow = ProphecyProgramControlTestAccess::storedProgram(processor, 9);
	ProphecyProgramControlTestAccess::overflowProgramInput(processor);
	require(unpack(save(processor)) == beforeOverflow, "input overflow lost the last accepted program");
	require(*processor.programStateError() != 0, "older accepted MIDI erased a newer input-overflow error");
	processor.setParam(1, 'R');
	require(*processor.programStateError() == 0, "new accepted edit failed to acknowledge input-overflow error");
	std::puts("PASS input exhaustion preserves accepted program and reports rejection after older queued input");
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	processor.setParam(1, 'Q');
	const auto edited = save(processor);
	juce::MidiBuffer ignored;
	juce::AudioBuffer<float> audio(2, 128);
	ignored.addEvent(juce::MidiMessage::programChange(2, 10), 0);
	processor.processBlock(audio, ignored);
	require(save(processor) == edited, "wrong-channel Program Change discarded accepted editor intent");
	std::puts("PASS ignored Program Change preserves pending editor intent");
#if PROFLIGACY_FAKE_ENGINE
	prophecy::fake::rejectSnapshots(true);
	require(save(processor) == edited, "capture failure discarded accepted pending edits");
	prophecy::fake::rejectSnapshots(false);
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	prophecy::fake::rejectSnapshots(true);
	require(save(processor).isEmpty() && *processor.programStateError() != 0, "failed capture silently saved stale program");
	processor.setParam(1, 'F');
	require(save(processor).isEmpty(), "new edit accepted on a stale baseline after capture failure");
	require(processor.writePatch(0) == 0, "WRITE accepted a stale baseline after capture failure");
	// The synthetic CI firmware has an LCD sentinel, not a Korg program record.
	// Preserve its explicit diagnostic state mode when no program can be read.
	environment("PROFLIGACY_CI_EXPOSE_LCD_STATE", "1");
	const auto sentinel = save(processor);
	require(sentinel.getSize() >= 6 && std::memcmp(sentinel.getData(), "PRP2", 4) == 0,
		"synthetic firmware diagnostic state was treated as a failed Korg save");
	environment("PROFLIGACY_CI_EXPOSE_LCD_STATE", "");
	prophecy::fake::rejectSnapshots(false);
	require(save(processor) == initial, "successful recapture changed program");
	std::puts("PASS failed clean capture returns no stale state; pending accepted edits remain savable");
#else
	// Do not query the firmware between changing its channel and restoring. A
	// query reply would refresh the engine's cached channel and hide this bug.
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	std::uint8_t channel[]{0xf0, 0x42, 0x30, 0x41, 0x41, 0, 56, 1, 3, 0, 0xf7};
	require(ProphecyProgramControlTestAccess::rawMidi(processor, channel, sizeof(channel)), "global channel edit admission");
	render(processor);
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	require(processor.romOk() && save(processor) == initial, "restore did not rediscover changed firmware channel");
	channel[2] = 0x33;
	channel[8] = 0;
	require(ProphecyProgramControlTestAccess::rawMidi(processor, channel, sizeof(channel)), "global channel reset admission");
	render(processor);
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	require(processor.romOk() && save(processor) == initial, "restore did not rediscover reset firmware channel");
	std::puts("PASS restore rediscovers firmware channel before sending saved program");
#endif
}

static void invalidProgramEdits(ProphecyAudioProcessor& processor, const juce::MemoryBlock& initial)
{
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	processor.setParam(1, 'Q');
	const auto accepted = save(processor);
	const auto* savedBytes = static_cast<const std::uint8_t*>(accepted.getData());
	const auto offset = 6 + std::size_t(savedBytes[4]) * 2;
	const auto baseline = prophecy::ProgramDocument::decode(savedBytes + offset, accepted.getSize() - offset);
	require(baseline.has_value(), "invalid-edit fixture has no pending document");
	for (const auto edit : std::vector<prophecy::ProgramEdit>{{154, 8191}, {154, 12}, {154, -1}, {0x3fff, 1},
		{388, 0}, {std::uint16_t(12288 | 388), 0}, {1, 8191}, {20, 0}, {635, 128}})
	{
		processor.setParam(edit.parameter, edit.value);
		require(save(processor) == accepted && *processor.programStateError(), "invalid editor operation changed accepted state");
		const auto midi = edit.midi();
		processor.sendMidi(midi.data(), midi.size());
		require(save(processor) == accepted && *processor.programStateError(), "invalid UI MIDI changed accepted state");
		juce::MidiBuffer input;
		juce::AudioBuffer<float> audio(2, 1);
		input.addEvent(midi.data(), int(midi.size()), 0);
		processor.processBlock(audio, input);
		require(save(processor) == accepted && *processor.programStateError(), "invalid host MIDI changed accepted state");
		auto invalid = *baseline;
		require(invalid.append(edit), "invalid semantic fixture must remain wire-encodable");
		const auto payload = invalid.encode();
		const std::uint8_t preferences[]{'P', 'R', 'P', '3', 1, 74, 1, 220};
		juce::MemoryBlock state(preferences, sizeof(preferences));
		state.append(payload.data(), payload.size());
		processor.setStateInformation(state.getData(), int(state.getSize()));
		require(save(processor) == accepted && *processor.programStateError(), "invalid pending restore changed program or preferences");
		finishGrantedWork(processor);
		require(ProphecyProgramControlTestAccess::rejectsEngineEdit(processor, edit, *baseline),
			"invalid engine restoration changed firmware readiness, program or time");
	}
	auto expected = unpack(initial);
	expected[0] = 'Q';
	require(dump(processor) == expected && processor.romOk(), "rejected input reached or disrupted firmware");
	std::puts("PASS invalid editor/host/UI edits and pending restores preserve accepted state and preferences; firmware remains responsive");
}

static void editorMidiChannel(ProphecyAudioProcessor& processor, const juce::MemoryBlock& initial)
{
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	auto expected = unpack(initial);
	for (const auto channel : {3, 7, 0})
	{
		processor.setParamG(0, 184, channel);
		(void)dump(processor);
		const auto globals = ProphecyProgramControlTestAccess::globals(processor);
		require(globals.size() == 574 && globals[176] == channel, "editor channel change did not reach firmware");
		processor.setParam(1, 'A' + channel);
		const auto accepted = save(processor);
		require(accepted.getSize() >= 6 && std::memcmp(accepted.getData(), "PRP3", 4) == 0,
			"nondefault channel edit was not retained as pending intent");
		expected[0] = std::uint8_t('A' + channel);
		require(dump(processor) == expected, "editor edit was lost after changing MIDI channel");
		processor.releaseResources();
		processor.setStateInformation(accepted.getData(), int(accepted.getSize()));
		require(unpack(save(processor)) == expected, "nondefault channel live and stopped restoration differ");
		processor.prepareToPlay(48000, 128);
	}
	std::puts("PASS editor global/program changes follow firmware MIDI channel; live and stopped results match all 535 bytes");
}

#if !PROFLIGACY_FAKE_ENGINE
static void firmwareControlOrder(ProphecyAudioProcessor& processor)
{
	const auto globals = ProphecyProgramControlTestAccess::globals(processor);
	require(globals.size() == 574 && (globals[163] & 2), "protected pattern fixture");
	const auto pattern = [&] {
		std::vector<std::uint8_t> bytes(128);
		std::uint32_t before = 0, version = 0;
		int slot = -1;
		processor.getArpeggioPatternData(bytes.data(), bytes.size(), &before, &slot);
		processor.requestArpeggioPatternDump(5);
		(void)dump(processor);
		const auto size = processor.getArpeggioPatternData(bytes.data(), bytes.size(), &version, &slot);
		require(size == 128 && version > before && slot == 5, "fresh pattern readback missing");
		return bytes;
	};
	const auto original = pattern();
	auto changed = original;
	changed[32] = original[32] == 7 ? 8 : 7;
	ProphecyProgramControlTestAccess::protectedPatternWrite(processor, changed);
	(void)dump(processor);
	require(pattern() == changed, "coalescing reordered protection across a pattern write");
	require(ProphecyProgramControlTestAccess::globals(processor) == globals, "pattern write changed original globals");
	ProphecyProgramControlTestAccess::protectedPatternWrite(processor, original);
	(void)dump(processor);
	require(pattern() == original && ProphecyProgramControlTestAccess::globals(processor) == globals,
		"pattern/protection cleanup did not restore all bytes");
	std::puts("PASS ordered unprotect/pattern load/protect preserves all 128 pattern and 574 global bytes");
}
#endif

static void waitForWrite(ProphecyAudioProcessor& processor)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (processor.writeInProgress() && std::chrono::steady_clock::now() < deadline)
		juce::Thread::sleep(1);
	if (processor.writeInProgress())
	{
		ProphecyProgramControlTestAccess::describeWriteWait(processor);
		for (const auto& message : transmittedMessages(processor))
			if (message.size() >= 5)
				std::fprintf(stderr, "firmware reply: bytes=%zu type=%02x\n", message.size(), message[4]);
	}
	require(!processor.writeInProgress(), "WRITE required host processing or failed to finish");
}

static std::vector<std::uint8_t> globalDump(ProphecyAudioProcessor& processor)
{
#if PROFLIGACY_FAKE_ENGINE
	(void)processor;
	std::vector<std::uint8_t> result(574);
	result[163] = std::uint8_t(prophecy::fake::memoryProtected());
	return result;
#else
	(void)transmittedMessages(processor);
	const std::uint8_t query[]{0xf0, 0x42, 0x30, 0x41, 0x0e, 0, 0xf7};
	processor.sendMidi(query, sizeof(query));
	render(processor, 750);
	for (const auto& message : transmittedMessages(processor))
		if (message.size() == 663 && message[4] == 0x51)
		{
			std::vector<std::uint8_t> raw;
			for (std::size_t i = 6; i + 1 < message.size();)
			{
				const auto high = message[i++];
				for (int b = 0; b < 7 && i + 1 < message.size(); ++b)
					raw.push_back(std::uint8_t(message[i++] | (((high >> b) & 1) << 7)));
			}
			return raw;
		}
	require(false, "independent global readback missing");
	return {};
#endif
}

static std::vector<std::uint8_t> writeProgram(ProphecyAudioProcessor& processor, const juce::MemoryBlock& initial)
{
	const auto globals = globalDump(processor);
	require(globals.size() == 574 && (globals[163] & 1), "protected WRITE fixture");
	const auto beforeCancelledWrite = ProphecyProgramControlTestAccess::storedProgram(processor, 127);
	ProphecyProgramControlTestAccess::midiArrivingBeforeWritePause(processor);
	require(processor.writeStatus() == ProphecyAudioProcessor::WriteStatus::Cancelled,
		"new host program failed to cancel WRITE before persistent mutation");
	require(ProphecyProgramControlTestAccess::storedProgram(processor, 127) == beforeCancelledWrite,
		"cancelled WRITE changed its destination");
	// Do not request another dump here: a fresh UI request could repair an
	// incorrectly cancelled automatic readback and conceal the ownership bug.
	const auto confirmationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (processor.programStateStatus() != prophecy::ProgramState::Status::Confirmed
		&& std::chrono::steady_clock::now() < confirmationDeadline)
	{
		render(processor, 16);
		juce::Thread::sleep(1);
	}
	require(processor.programStateStatus() == prophecy::ProgramState::Status::Confirmed,
		"program replacing cancelled WRITE failed to confirm automatically");
	std::vector<std::uint8_t> replacement(535);
	require(processor.getProgramData(replacement.data(), replacement.size(), nullptr) == 535
		&& replacement == ProphecyProgramControlTestAccess::storedProgram(processor, 9),
		"cancelled WRITE replacement readback differs from selected program");
	std::puts("PASS MIDI before WRITE pause preserves the stored destination and confirms the replacing program");
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	processor.releaseResources();
	processor.setParam(1, 'W');
	const auto accepted = save(processor);
	require(std::memcmp(accepted.getData(), "PRP3", 4) == 0, "WRITE test has no pending intent");
	const auto callbacks = processor.diagnosticSnapshot().audioCallbacks;
	require(processor.writePatch(127) != 0, "WRITE B63 was not accepted");
	waitForWrite(processor);
	require(processor.writeStatus() == ProphecyAudioProcessor::WriteStatus::Succeeded, "WRITE B63 failed");
	require(processor.diagnosticSnapshot().audioCallbacks == callbacks, "WRITE needed an audio callback");
	auto expected = unpack(initial);
	expected[0] = 'W';
	require(unpack(save(processor)) == expected, "WRITE lost the pending edit");
	processor.selectPatch(127);
	require(unpack(save(processor)) == expected, "WRITE did not persist to its explicit destination");
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	processor.prepareToPlay(48000, 128);
	require(globalDump(processor) == globals, "WRITE changed original global/protection settings");
	std::puts("PASS stopped WRITE saves pending intent to B63 and restores every global byte");

	const std::uint8_t unprotect[]{0xf0, 0x42, 0x30, 0x41, 0x41, 0, 42, 1, 0, 0, 0xf7};
	processor.sendMidi(unprotect, sizeof(unprotect));
	render(processor, 750);
	const auto unprotected = globalDump(processor);
	require((unprotected[163] & 1) == 0, "unprotected WRITE fixture");
	processor.setParam(1, 'U');
	require(processor.writePatch(126) != 0, "WRITE B62 was not accepted");
	waitForWrite(processor);
	require(processor.writeStatus() == ProphecyAudioProcessor::WriteStatus::Succeeded, "unprotected WRITE failed");
	require(globalDump(processor) == unprotected, "WRITE enabled an originally disabled protection setting");
	std::puts("PASS WRITE preserves an originally unprotected bank");

	const std::uint8_t protect[]{0xf0, 0x42, 0x30, 0x41, 0x41, 0, 42, 1, 1, 0, 0xf7};
	processor.sendMidi(protect, sizeof(protect));
	render(processor, 750);
	require(globalDump(processor) == globals, "test protection cleanup");
#if PROFLIGACY_FAKE_ENGINE
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	processor.setParam(1, 'R');
	prophecy::fake::rejectWrites(true);
	require(processor.writePatch(127) != 0, "rejected WRITE not started");
	waitForWrite(processor);
	prophecy::fake::rejectWrites(false);
	require(processor.writeStatus() == ProphecyAudioProcessor::WriteStatus::Failed
		&& prophecy::fake::memoryProtected() && *processor.programStateError(), "rejected WRITE reported success or lost protection");
	auto rejected = unpack(initial);
	rejected[0] = 'R';
	require(unpack(save(processor)) == rejected, "rejected WRITE lost accepted program");
	processor.selectPatch(127);
	require(unpack(save(processor)) == expected, "rejected WRITE changed its destination");
	processor.setStateInformation(initial.getData(), int(initial.getSize()));
	std::puts("PASS rejected WRITE preserves storage, accepted program and original protection");

	prophecy::fake::holdWriteCleanup(true);
	require(processor.writePatch(0) != 0, "concurrent WRITE not started");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!prophecy::fake::writeCleanupWaiting() && std::chrono::steady_clock::now() < deadline) juce::Thread::sleep(1);
	require(prophecy::fake::writeCleanupWaiting() && !prophecy::fake::memoryProtected(), "WRITE cleanup hold not reached");
	std::atomic<bool> restored{false};
	std::thread restoring([&] { processor.setStateInformation(initial.getData(), int(initial.getSize())); restored.store(true); });
	juce::Thread::sleep(20);
	require(!restored.load(), "host restore bypassed active WRITE cleanup");
	render(processor, 1); // paused callback must return without the control mutex
	prophecy::fake::holdWriteCleanup(false);
	restoring.join();
	require(restored.load() && prophecy::fake::memoryProtected() && save(processor) == initial,
		"restore completed without preserving protection/program");
	std::puts("PASS host restore joins active WRITE cleanup; audio callback remains nonblocking");
#endif
	return expected;
}

#if PROFLIGACY_FAKE_ENGINE
static void writeShutdown()
{
	auto processor = std::make_unique<ProphecyAudioProcessor>();
	processor->prepareToPlay(48000, 128);
	prophecy::fake::holdWriteCleanup(true);
	require(processor->writePatch(0) != 0, "shutdown WRITE not started");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!prophecy::fake::writeCleanupWaiting() && std::chrono::steady_clock::now() < deadline) juce::Thread::sleep(1);
	require(prophecy::fake::writeCleanupWaiting(), "shutdown WRITE cleanup hold not reached");
	std::atomic<bool> stopped{false};
	std::thread destroying([&] { processor.reset(); stopped.store(true); });
	juce::Thread::sleep(20);
	require(!stopped.load(), "destruction bypassed WRITE cleanup");
	prophecy::fake::holdWriteCleanup(false);
	destroying.join();
	require(stopped.load() && prophecy::fake::memoryProtected(), "destruction left memory unprotected");
	std::puts("PASS destruction joins WRITE and restores protection before engine shutdown");
}
#endif

int main(int argc, char** argv)
{
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	const auto fixture = juce::File::getSpecialLocation(juce::File::tempDirectory)
		.getNonexistentChildFile("profligacy-program-control", "", false);
	require(fixture.createDirectory().wasOk(), "create isolated fixture");
#if PROFLIGACY_FAKE_ENGINE
	(void)argc; (void)argv;
	require(fixture.getChildFile("korgprop.zip").replaceWithText("fake firmware"), "fake ROM marker");
	environment("PROPHECY_ROMPATH", fixture.getFullPathName());
#else
	require((argc == 2 || argc == 3) && std::getenv("PROPHECY_ROMPATH"), "usage: program control test <private sysram seed> [--pending]; set PROPHECY_ROMPATH");
	const auto ram = fixture.getChildFile("nvram/korgprop/sysram");
	require(ram.getParentDirectory().createDirectory().wasOk()
		&& juce::File(argv[1]).copyFileTo(ram), "copy isolated NVRAM seed");
#endif
	environment("PROPHECY_NVRAM", fixture.getChildFile("nvram").getFullPathName());
	std::vector<std::uint8_t> writtenProgram;
	for (const auto* key : {"PROPHECY_FORCE_NO_ROM", "PROPHECY_EDITOR_SELFTEST", "PROFLIGACY_CI_EXPOSE_LCD_STATE"})
		environment(key, "");
	{
		ProphecyAudioProcessor processor;
		processor.setNonRealtime(true);
		if (!PROFLIGACY_FAKE_ENGINE) require(processor.enableMidiTxByteCapture(), "native readback trace");
		processor.prepareToPlay(48000, 128);
		require(processor.romOk(), "firmware boot");
		const auto initial = save(processor);
		require(unpack(initial) == dump(processor), "boot snapshot differs from firmware dump");
		if (argc > 1 && std::string(argv[argc - 1]) == "--pending")
		{
			const auto* repeat = std::getenv("PROPHOST_INTENT_REPEATS");
			const auto count = repeat ? std::clamp(std::atoi(repeat), 1, 100) : 1;
			for (int iteration = 0; iteration < count; ++iteration)
			{
				std::fprintf(stderr, "pending-save iteration %d\n", iteration);
				pendingSave(processor, initial);
			}
		}
		else if (argc > 1 && std::string(argv[argc - 1]) == "--live") liveSave(processor, initial);
		else if (argc > 1 && std::string(argv[argc - 1]) == "--selection") selectionSave(processor, initial);
		else if (argc > 1 && std::string(argv[argc - 1]) == "--midi") incomingPrograms(processor, initial);
		else if (argc > 1 && std::string(argv[argc - 1]) == "--midi-edits") incomingParameterEdits(processor, initial);
		else if (argc > 1 && std::string(argv[argc - 1]) == "--channel") editorMidiChannel(processor, initial);
		else if (argc > 1 && std::string(argv[argc - 1]) == "--edit-validation") invalidProgramEdits(processor, initial);
#if !PROFLIGACY_FAKE_ENGINE
		else if (argc > 1 && std::string(argv[argc - 1]) == "--control-order") firmwareControlOrder(processor);
#endif
		else if (argc > 1 && std::string(argv[argc - 1]) == "--write") writtenProgram = writeProgram(processor, initial);
		else
		{

		midiProgram(processor, 8);
		std::vector<std::uint8_t> cached(1024);
		cached.resize(processor.getProgramData(cached.data(), cached.size(), nullptr));
		require(cached == unpack(initial), "test must retain a stale dump independently of firmware changes");
		const auto before = processor.diagnosticSnapshot();
		const auto stopped = save(processor);
		require(unpack(stopped) != unpack(initial), "program change did not change saved state");
		require(processor.diagnosticSnapshot().producedFrames == before.producedFrames,
			"saving advanced emulated time");
		processor.releaseResources();
		require(save(processor) == stopped, "release changed saved program");
		processor.prepareToPlay(48000, 128);
		require(unpack(stopped) == dump(processor), "stopped save differs from fresh firmware readback");
		std::puts("PASS stopped and released saves match firmware without advancing time");

		// Both work still in timers and commands already enqueued at the engine
		// must lose ownership when the host restores a different program.
		for (int scenario = 0; scenario < 7; ++scenario)
		{
			if (scenario == 0) processor.selectPatch(9);
			if (scenario == 1) processor.renamePatch("Obsolete rename");
			if (scenario == 2) processor.sendMacro("saw");
			if (scenario == 3) { processor.setParam(1, 'X'); processor.writePatch(0); }
			if (scenario == 4)
			{
				const std::uint8_t edit[]{0xf0, 0x42, 0x30, 0x41, 0x41, 1, 1, 0, 'X', 0, 0xf7};
				processor.sendMidi(edit, sizeof(edit));
			}
			if (scenario == 5 || scenario == 6)
			{
				const auto* bytes = static_cast<const std::uint8_t*>(stopped.getData());
				const auto offset = 6 + 2 * std::size_t(bytes[4]);
				juce::MidiBuffer midi;
				juce::AudioBuffer<float> audio(2, 128);
				if (scenario == 5)
				{
					processor.sendMidi(bytes + offset, stopped.getSize() - offset);
					require(save(processor) == stopped, "explicit program import did not complete through the owner");
				}
				else midi.addEvent(bytes + offset, int(stopped.getSize() - offset), 0);
				// The host MIDI case starts transmission before restoration; the UI
				// import case has already completed its owned replacement operation.
				processor.processBlock(audio, midi);
			}
			processor.setStateInformation(initial.getData(), (int)initial.getSize());
			require(processor.romOk(), "host restoration did not resume ready firmware");
			require(save(processor) == initial, "immediate restored snapshot");
			pump(processor);
			require(save(processor) == initial, "obsolete editor work changed restored program");
			require(!processor.writeInProgress(), "cancelled write remained active");
		}
		std::puts("PASS host restoration invalidates selections, bursts, writes and queued MIDI");

		processor.selectPatch(9);
		midiProgram(processor, 8);
		pump(processor);
		require(save(processor) == stopped, "old editor selection overrode newer host MIDI program");
		std::puts("PASS host MIDI program change invalidates pending editor selection");

		// Exercise a real parameter edit with no subsequent MIDI dump to refresh
		// the editor cache. Saving must still see the firmware's changed name.
		processor.setParam(1, 'Z');
		pump(processor);
		const auto edited = save(processor);
		const auto firmwareEdited = dump(processor);
		require(firmwareEdited.front() == 'Z', "parameter edit missing from firmware");
		processor.setStateInformation(edited.getData(), int(edited.getSize()));
		require(unpack(save(processor)) == firmwareEdited, "saved edit differs from firmware dump");
		std::puts("PASS saved parameter intent restores the independent firmware readback");

		processor.setStateInformation(initial.getData(), (int)initial.getSize());
		require(processor.romOk(), "transition test restoration did not resume ready firmware");
		juce::MidiBuffer midi;
		juce::AudioBuffer<float> audio(2, 128);
		midi.addEvent(juce::MidiMessage::programChange(1, 8), 0);
		for (int i = 0; i < 1500; ++i)
		{
			processor.processBlock(audio, midi);
			midi.clear();
			const auto state = save(processor);
			require(state == initial || state == stopped, "snapshot tore a firmware program transition");
		}
		std::puts("PASS snapshots throughout program loading contain complete old or new records");
		}
	}
	if (!writtenProgram.empty())
	{
#if PROFLIGACY_FAKE_ENGINE
		writeShutdown();
#else
		juce::MemoryBlock savedRam;
		require(ram.loadFileAsData(savedRam), "persistent WRITE NVRAM missing");
		const auto* data = static_cast<const std::uint8_t*>(savedRam.getData());
		constexpr std::size_t offset = 0x20a10 + 127 * 535;
		require(savedRam.getSize() >= offset + 535
			&& std::equal(writtenProgram.begin(), writtenProgram.end(), data + offset), "persistent B63 differs after engine shutdown");
		std::puts("PASS all 535 bytes persisted to B63 in disposable NVRAM");
#endif
	}
	require(fixture.deleteRecursively(), "isolated fixture cleanup");
	return 0;
}
