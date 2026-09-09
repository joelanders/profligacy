// SPDX-License-Identifier: AGPL-3.0-only
// Host state/lifecycle contract with deterministic firmware readback and a held
// initialization. Native firmware/onset coverage lives in plugin_rt_safety_test.
#include "PluginProcessor.h"
#include "prophecy_engine_fake.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static void require(bool condition, const char* message)
{
	if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

static void environment(const char* name, const juce::String& value)
{
#if JUCE_WINDOWS
	_putenv_s(name, value.toRawUTF8());
#else
	if (value.isEmpty()) unsetenv(name);
	else setenv(name, value.toRawUTF8(), 1);
#endif
}

static std::vector<std::uint8_t> program(unsigned seed)
{
	std::vector<std::uint8_t> result{0xf0, 0x42, 0x30, 0x41, 0x40, 1};
	for (unsigned first = 0; first < 535; first += 7)
	{
		std::uint8_t high = 0;
		const auto count = std::min(7u, 535 - first);
		for (unsigned i = 0; i < count; ++i) high |= (((first + i + seed) >> 7) & 1) << i;
		result.push_back(high);
		for (unsigned i = 0; i < count; ++i) result.push_back((first + i + seed) & 0x7f);
	}
	result.push_back(0xf7);
	return result;
}

static bool savedProgramMatches(const juce::MemoryBlock& saved, const std::vector<std::uint8_t>& expected)
{
	if (saved.getSize() < 6) return false;
	const auto* bytes = static_cast<const std::uint8_t*>(saved.getData());
	const auto offset = 6 + 2 * std::size_t(bytes[4]);
	return std::memcmp(bytes, "PRP2", 4) == 0 && saved.getSize() == offset + expected.size()
		&& std::equal(expected.begin(), expected.end(), bytes + offset);
}

static void requireSavedProgram(ProphecyAudioProcessor& processor, const std::vector<std::uint8_t>& expected)
{
	juce::MemoryBlock saved;
	processor.getStateInformation(saved);
	require(savedProgramMatches(saved, expected), "save lost the latest restored/edited program");
}

static void requireFirstNote(ProphecyAudioProcessor& processor)
{
	juce::MidiBuffer midi;
	juce::AudioBuffer<float> audio(2, 128);
	int onset = -1;
	for (int first = 0; first < 1024; first += 128)
	{
		midi.clear();
		if (first == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 64), 17);
		processor.processBlock(audio, midi);
		for (int i = 0; i < 128; ++i)
			if (onset < 0 && !juce::exactlyEqual(audio.getSample(0, i), 0.0f)) onset = first + i;
	}
	require(onset == 17 + processor.getLatencySamples(), "restoration changed the first note's compensated position");
}

enum class Order { before, after, zero, reprepare, beforeReprepare, released, live };

static void restoreAndSave(Order order)
{
	const auto state = program(31);
	ProphecyAudioProcessor processor;
	processor.setNonRealtime(true);
	if (order == Order::before) processor.setStateInformation(state.data(), (int) state.size());
	processor.prepareToPlay(48000, 128);
	juce::MidiBuffer midi;
	juce::AudioBuffer<float> audio(2, order == Order::zero ? 0 : 128);
	if (order == Order::live) midi.addEvent(juce::MidiMessage::noteOn(1, 48, (juce::uint8) 64), 127);
	if (order != Order::before && order != Order::after) processor.processBlock(audio, midi);
	midi.clear();
	if (order == Order::reprepare || order == Order::released)
		processor.releaseResources();
	if (order == Order::reprepare) processor.prepareToPlay(48000, 128);
	if (order != Order::before) processor.setStateInformation(state.data(), (int) state.size());
	requireSavedProgram(processor, state);
	if (order == Order::beforeReprepare || order == Order::released) processor.prepareToPlay(48000, 128);
	requireSavedProgram(processor, state);
	requireFirstNote(processor);

	// A confirmed restore must not mask subsequent firmware/editor changes.
	const auto edited = program(73);
	processor.sendMidi(edited.data(), edited.size());
	// Explicit import owns the replacement immediately, without a host callback.
	requireSavedProgram(processor, edited);
	audio.setSize(2, 128);
	processor.processBlock(audio, midi);
	requireSavedProgram(processor, edited);
	processor.releaseResources();
	processor.prepareToPlay(48000, 128);
	requireSavedProgram(processor, edited);
}

static void restoreWithoutRom()
{
	environment("PROPHECY_FORCE_NO_ROM", "1");
	ProphecyAudioProcessor processor;
	processor.prepareToPlay(48000, 128);
	const auto first = program(11), latest = program(81);
	processor.setStateInformation(first.data(), (int) first.size());
	requireSavedProgram(processor, first);
	processor.setStateInformation(latest.data(), (int) latest.size());
	requireSavedProgram(processor, latest);
	environment("PROPHECY_FORCE_NO_ROM", "");
	require(processor.maybeBootEngine(), "late ROM boot failed");
	requireSavedProgram(processor, latest);
	requireFirstNote(processor);
}

struct StateListener : juce::AudioProcessorListener
{
	void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
	void audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) override
	{
		if (details.latencyChanged) processor->getStateInformation(saved);
	}
	juce::MemoryBlock saved;
};

static void latencyNotification()
{
	ProphecyAudioProcessor processor;
	StateListener listener;
	processor.addListener(&listener);
	const auto state = program(57);
	processor.setStateInformation(state.data(), (int) state.size());
	processor.prepareToPlay(48000, 128);
	require(savedProgramMatches(listener.saved, state), "latency notification could not retrieve restored state");
	processor.removeListener(&listener);
}

static void concurrentRestore()
{
	ProphecyAudioProcessor processor;
	processor.prepareToPlay(48000, 128);
	juce::MidiBuffer midi;
	juce::AudioBuffer<float> audio(2, 128);
	processor.processBlock(audio, midi);
	const auto state = program(19);
	prophecy::fake::holdInitialization(true);
	std::thread restore([&] { processor.setStateInformation(state.data(), (int) state.size()); });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!prophecy::fake::initializationWaiting() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	require(prophecy::fake::initializationWaiting(), "restore did not reach firmware initialization");
	juce::MemoryBlock saved;
	std::thread save([&] { processor.getStateInformation(saved); });
	// The callback must complete while firmware initialization is deliberately
	// held, even when a host save is also waiting. No wall-time benchmark needed.
	for (int i = 0; i < 32; ++i)
	{
		for (int channel = 0; channel < 2; ++channel)
			std::fill_n(audio.getWritePointer(channel), 128, 1.0f);
		processor.processBlock(audio, midi);
		require(juce::exactlyEqual(audio.getMagnitude(0, 128), 0.0f), "callback emitted old audio during restoration");
	}
	prophecy::fake::holdInitialization(false);
	restore.join();
	save.join();
	require(savedProgramMatches(saved, state), "concurrent save returned the program from before restoration");
	requireFirstNote(processor);
	const auto latest = program(117);
	processor.setStateInformation(latest.data(), (int) latest.size());
	requireSavedProgram(processor, latest);
	requireFirstNote(processor);
}

static void destroyDuringControlDelivery()
{
	auto processor = std::make_unique<ProphecyAudioProcessor>();
	processor->prepareToPlay(48000, 128);
	prophecy::fake::holdImmediateInput(true);
	processor->setParam(1, 'Q');
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!prophecy::fake::immediateInputWaiting() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	require(prophecy::fake::immediateInputWaiting(), "control delivery required a message-loop callback");
	std::atomic<bool> entered{false}, destroyed{false};
	std::thread destruction([&] {
		entered.store(true);
		processor.reset();
		destroyed.store(true);
	});
	while (!entered.load()) std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	require(!destroyed.load(), "destructor returned while a control callback still owned the processor");
	prophecy::fake::holdImmediateInput(false);
	destruction.join();
	require(destroyed.load(), "control shutdown did not complete");
	std::puts("PASS control delivery without a message loop and joined callback destruction");
}

static void stoppedCommandPacing()
{
	ProphecyAudioProcessor processor;
	processor.setNonRealtime(true);
	processor.prepareToPlay(48000, 128);
	processor.releaseResources();
	processor.renamePatch("Queued rename");
	std::this_thread::sleep_for(std::chrono::milliseconds(350));
	const auto stopped = processor.diagnosticSnapshot();
	require(stopped.editorCommandsSent == 1 && stopped.editorCommandsPending == 15,
		"wall-clock pacing filled the UART while firmware was stopped");
	juce::MemoryBlock beforeOverflow;
	processor.getStateInformation(beforeOverflow);
	// Admission of an oversized batch must preserve the previous whole request.
	for (int i = 0; i < 15; ++i) processor.renamePatch("Accepted name");
	juce::MemoryBlock full;
	processor.getStateInformation(full);
	processor.renamePatch("Rejected name");
	juce::MemoryBlock rejected;
	processor.getStateInformation(rejected);
	require(full == rejected && *processor.programStateError() != 0,
		"queue exhaustion accepted a partial rename");
	processor.setStateInformation(beforeOverflow.getData(), int(beforeOverflow.getSize()));
	require(*processor.programStateError() == 0, "restoring the accepted request did not clear the error");
	std::puts("PASS stopped firmware pacing and atomic batch overflow");
}

static void explicitProgramImport()
{
	ProphecyAudioProcessor processor;
	const auto first = program(31), second = program(79);
	processor.sendMidi(first.data(), first.size());
	requireSavedProgram(processor, first);
	processor.prepareToPlay(48000, 128);
	requireSavedProgram(processor, first);
	processor.setParam(1, 'X');
	processor.sendMidi(second.data(), second.size());
	requireSavedProgram(processor, second);
	auto invalid = first;
	invalid.pop_back();
	processor.sendMidi(invalid.data(), invalid.size());
	requireSavedProgram(processor, second);
	require(*processor.programStateError() != 0, "malformed explicit import had no error");
	std::puts("PASS explicit program import before boot, immediate save, supersession and malformed input");

	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
	auto standalone = std::make_unique<ProphecyAudioProcessor>();
	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
	require(standalone->wrapperType == juce::AudioProcessor::wrapperType_Standalone, "standalone fixture type");
	standalone->prepareToPlay(48000, 128);
	standalone->setStateInformation(first.data(), int(first.size()));
	std::vector<std::uint8_t> raw(535);
	require(standalone->getProgramData(raw.data(), raw.size(), nullptr) == raw.size()
		&& std::all_of(raw.begin(), raw.end(), [](auto byte) { return byte == 0; }),
		"standalone automatically recalled the edit buffer");
	standalone->sendMidi(second.data(), second.size());
	const auto imported = prophecy::ProgramDocument::fromMidi(second.data(), second.size());
	require(standalone->getProgramData(raw.data(), raw.size(), nullptr) == raw.size()
		&& std::equal(raw.begin(), raw.end(), imported->base.begin()), "standalone explicit import was ignored");
	juce::MemoryBlock preferences;
	standalone->getStateInformation(preferences);
	require(preferences.getSize() == 6, "standalone automatic state included the edit buffer");
	std::puts("PASS standalone imports explicitly while automatic state remains preferences only");
}

static void delayedLiveConfirmation()
{
	ProphecyAudioProcessor processor;
	processor.setNonRealtime(true);
	processor.prepareToPlay(48000, 128);
	const auto initial = program(31);
	processor.setStateInformation(initial.data(), int(initial.size()));
	juce::MidiBuffer midi;
	juce::AudioBuffer<float> audio(2, 128);
	auto step = [&] { processor.processBlock(audio, midi); std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
	auto waitFor = [&](auto predicate, const char* failure) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!predicate() && std::chrono::steady_clock::now() < deadline) step();
		require(predicate(), failure);
	};
	auto saved = [&] { juce::MemoryBlock state; processor.getStateInformation(state); return state; };
	auto pending = [&]() -> std::optional<prophecy::ProgramDocument> {
		const auto state = saved();
		const auto* bytes = static_cast<const std::uint8_t*>(state.getData());
		if (state.getSize() < 6 || std::memcmp(bytes, "PRP3", 4)) return {};
		const auto offset = 6 + 2 * std::size_t(bytes[4]);
		return prophecy::ProgramDocument::decode(bytes + offset, state.getSize() - offset);
	};
	prophecy::fake::holdReadbackCompletion(true);
	processor.setParam(1, 'Z');
	waitFor([&] {
		std::uint8_t first = 0;
		return processor.getProgramData(&first, 1, nullptr) == 1 && first == 'Z';
	}, "live edit dump not published");
	require(pending() && pending()->edits.size() == 1, "dump version prematurely confirmed an edit");
	processor.setParam(2, 'Y');
	for (int i = 0; i < 250; ++i) step();
	require(processor.diagnosticSnapshot().editorCommandsSent == 1,
		"new edit was delivered during the older readback");
	require(pending() && pending()->edits.size() == 2, "newer accepted intent was not saved");
	prophecy::fake::holdReadbackCompletion(false);
	waitFor([&] {
		const auto document = pending();
		return document && document->edits.size() == 1 && document->base[0] == 'Z'
			&& document->edits[0].parameter == 2 && document->edits[0].value == 'Y';
	}, "older confirmation did not retain the newer edit");
	auto expected = prophecy::ProgramDocument::fromMidi(initial.data(), initial.size());
	expected->base[0] = 'Z'; expected->base[1] = 'Y';
	waitFor([&] { return savedProgramMatches(saved(), expected->programMidi()); },
		"completed live edits were not retired into the saved base");
	std::puts("PASS delayed live readback, retained newer intent, exact prefix retirement and plain final state");

	const auto requests = processor.diagnosticSnapshot().editorDumpSends;
	prophecy::fake::rejectReadbacks(true);
	processor.setParam(3, 'X');
	waitFor([&] { return *processor.programStateError() != 0; }, "missing firmware readback never failed");
	require(processor.diagnosticSnapshot().editorDumpSends == requests + 3, "readback retries were not bounded");
	const auto retained = pending();
	require(retained && retained->edits.size() == 1 && retained->edits[0].parameter == 3,
		"failed readback discarded accepted intent");
	const auto recover = saved();
	prophecy::fake::rejectReadbacks(false);
	processor.setStateInformation(recover.getData(), int(recover.getSize()));
	expected->base[2] = 'X';
	requireSavedProgram(processor, expected->programMidi());
	require(*processor.programStateError() == 0, "explicit recovery did not clear the failure");
	std::puts("PASS bounded readback failure preserves accepted intent for explicit recovery");
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	const auto fixture = juce::File::getSpecialLocation(juce::File::tempDirectory)
		.getNonexistentChildFile("profligacy-state-test", "", false);
	require(fixture.createDirectory().wasOk(), "fixture directory");
	require(fixture.getChildFile("korgprop.zip").replaceWithText("fake engine fixture"), "fixture marker");
	environment("PROPHECY_ROMPATH", fixture.getFullPathName());
	environment("PROPHECY_NVRAM", fixture.getChildFile("nvram").getFullPathName());
	environment("PROPHECY_FAKE_TIMELINE_PROBE", "1");
	environment("PROPHECY_FORCE_NO_ROM", "");
	environment("PROPHECY_EDITOR_SELFTEST", "");
	for (auto order : {Order::before, Order::after, Order::zero, Order::reprepare,
		Order::beforeReprepare, Order::released, Order::live}) restoreAndSave(order);
	restoreWithoutRom();
	concurrentRestore();
	latencyNotification();
	destroyDuringControlDelivery();
	stoppedCommandPacing();
	explicitProgramImport();
	delayedLiveConfirmation();
	require(fixture.deleteRecursively(), "fixture cleanup");
	std::puts("state lifecycle: save/restore, zero callbacks, reprepare, edits and concurrent loading passed");
}
