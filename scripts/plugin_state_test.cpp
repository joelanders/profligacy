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
	require(fixture.deleteRecursively(), "fixture cleanup");
	std::puts("state lifecycle: save/restore, zero callbacks, reprepare, edits and concurrent loading passed");
}
