// SPDX-License-Identifier: AGPL-3.0-only
// Real processor + deterministic fake engine. The fake emits a 32-native-frame
// pulse for each scheduled Note On, with unequal stereo channels. No firmware.
#include "PluginProcessor.h"
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
	setenv(name, value.toRawUTF8(), 1);
#endif
}

struct Render
{
	juce::AudioBuffer<float> audio;
	int latency;
	std::uint64_t missing;
};

static Render render(double rate, bool variable, bool offline, bool mono, bool reprepare, bool lateBoot = false)
{
	if (lateBoot) environment("PROPHECY_FORCE_NO_ROM", "1");
	ProphecyAudioProcessor processor;
	processor.setNonRealtime(offline);
	processor.prepareToPlay(reprepare ? 48000 : rate, 512);
	for (int i = 0; !processor.playbackReady() && i < 1000; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	require(processor.playbackReady() || lateBoot, "asynchronous initialization did not finish");
	juce::MidiBuffer midi;
	midi.ensureSize(4096);
	juce::AudioBuffer<float> block(mono ? 1 : 2, 512);
	if (lateBoot)
	{
		for (int i = 0; i < 2000; ++i) processor.processBlock(block, midi);
#if JUCE_WINDOWS
		_putenv_s("PROPHECY_FORCE_NO_ROM", "");
#else
		unsetenv("PROPHECY_FORCE_NO_ROM");
#endif
		require(processor.maybeBootEngine(), "late ROM selection did not boot");
		for (int i = 0; !processor.playbackReady() && i < 1000; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		require(processor.playbackReady(), "late asynchronous initialization did not finish");
	}
	if (reprepare)
	{
		// End the previous epoch between native delivery boundaries, then change
		// rates. Output must begin a clean, compensated epoch at the new rate.
		block.setSize(mono ? 1 : 2, 511, false, false, true);
		processor.processBlock(block, midi);
		processor.releaseResources();
		processor.prepareToPlay(rate, 512);
	}
	Render result{juce::AudioBuffer<float>(mono ? 1 : 2, (int) (rate * 2)), processor.getLatencySamples(), 0};
	const auto initialMissing = processor.diagnosticSnapshot().audioUnderrunFrames;
	result.audio.clear();
	const int events[] = { 17, (int) (rate * 0.371), (int) (rate * 1.803) };
	int eventIndex = 0;
	const int sizes[] = { 256, 511, 17, 1, 128, 64, 170, 512 };
	for (int first = 0, iteration = 0; first < result.audio.getNumSamples(); ++iteration)
	{
		const int count = std::min(variable ? sizes[iteration % 8] : 256,
			result.audio.getNumSamples() - first);
		midi.clear();
		while (eventIndex < 3 && events[eventIndex] < first + count)
		{
			midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 64), events[eventIndex] - first);
			++eventIndex;
		}
		block.setSize(mono ? 1 : 2, count, false, false, true);
		processor.processBlock(block, midi);
		for (int channel = 0; channel < block.getNumChannels(); ++channel)
			result.audio.copyFrom(channel, first, block, channel, 0, count);
		first += count;
		// Zero-length buffers must not advance the clock or alter the waveform.
		midi.clear();
		block.setSize(mono ? 1 : 2, 0, false, false, true);
		processor.processBlock(block, midi);
		require(processor.getLatencySamples() == result.latency, "latency changed within prepare epoch");
	}
	result.missing = processor.diagnosticSnapshot().audioUnderrunFrames - initialMissing;
	return result;
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	// Compare the stateless absolute-position kernel with the pinned JUCE
	// interpolator, accounting for JUCE's two-native-frame algorithmic delay.
	for (double ratio : { 0.25, 0.5, 48000.0 / 44100.0, 2.0 })
	{
		std::array<float, 1024> source{};
		std::array<float, 400> output{};
		for (std::size_t i = 0; i < source.size(); ++i)
			source[i] = (float) (std::sin(i * 1.7) * 0.5 + std::cos(i * .113) * .3);
		juce::LagrangeInterpolator reference;
		reference.process(ratio, source.data(), output.data(), (int) output.size());
		for (std::size_t i = 20; i < output.size(); ++i)
		{
			const auto position = i * ratio - 2;
			const auto index = (std::size_t) std::floor(position);
			const auto actual = prophecy::interpolate(source[index - 2], source[index - 1],
				source[index], source[index + 1], source[index + 2], position - index);
			require(std::abs(actual - output[i]) < 2e-6f, "resampler differs from JUCE's existing kernel");
		}
	}
	const auto fixture = juce::File::getSpecialLocation(juce::File::tempDirectory)
		.getNonexistentChildFile("profligacy-timeline-test", "", false);
	require(fixture.createDirectory().wasOk(), "fixture directory");
	require(fixture.getChildFile("korgprop.zip").replaceWithText("fake engine fixture"), "fixture marker");
	environment("PROPHECY_ROMPATH", fixture.getFullPathName());
	environment("PROPHECY_NVRAM", fixture.getChildFile("nvram").getFullPathName());
	environment("PROPHECY_FAKE_TIMELINE_PROBE", "1");
#if JUCE_WINDOWS
	_putenv_s("PROPHECY_FORCE_NO_ROM", "");
#else
	unsetenv("PROPHECY_FORCE_NO_ROM");
#endif
	// Firmware readiness belongs to a background lifecycle task. prepareToPlay must
	// return promptly, while an offline render may wait for deterministic readiness.
	environment("PROPHECY_FAKE_INITIALIZATION_MS", "500");
	{
		ProphecyAudioProcessor processor;
		const auto prepareStart = std::chrono::steady_clock::now();
		processor.prepareToPlay(48000, 64);
		require(std::chrono::steady_clock::now() - prepareStart < std::chrono::milliseconds(250),
			"prepareToPlay blocked on firmware initialization");
		require(!processor.playbackReady(), "delayed initialization completed synchronously");
		juce::AudioBuffer<float> audio(2, 64);
		juce::MidiBuffer midi;
		const auto realtimeStart = std::chrono::steady_clock::now();
		processor.processBlock(audio, midi);
		require(std::chrono::steady_clock::now() - realtimeStart < std::chrono::milliseconds(100),
			"realtime callback blocked on firmware initialization");
		require(audio.getMagnitude(0, audio.getNumSamples()) == 0.0f,
			"realtime callback emitted audio before initialization");
		processor.setNonRealtime(true);
		processor.processBlock(audio, midi);
		require(processor.playbackReady(), "offline render did not wait for initialization");
	}
	environment("PROPHECY_FAKE_INITIALIZATION_MS", "5000");
	{
		const auto destructionStart = std::chrono::steady_clock::now();
		{
			ProphecyAudioProcessor processor;
			processor.prepareToPlay(48000, 64);
		}
		require(std::chrono::steady_clock::now() - destructionStart < std::chrono::milliseconds(250),
			"processor destruction did not cancel background initialization");
	}
	environment("PROPHECY_FAKE_INITIALIZATION_MS", "0");

	// A realtime callback larger than the host's declared maximum has no matching
	// latency allowance. Reject it deterministically instead of emitting a partial,
	// scheduler-dependent block, and do not deliver MIDI whose audio cannot align.
	{
		ProphecyAudioProcessor processor;
		processor.prepareToPlay(48000, 64);
		for (int i = 0; !processor.playbackReady() && i < 1000; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		require(processor.playbackReady(), "oversize-policy initialization did not finish");
		juce::AudioBuffer<float> audio(2, 65);
		for (int channel = 0; channel < audio.getNumChannels(); ++channel)
			for (int sample = 0; sample < audio.getNumSamples(); ++sample)
				audio.setSample(channel, sample, 1.0f);
		juce::MidiBuffer midi;
		midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 64), 0);
		const auto forwarded = processor.diagnosticSnapshot().hostMidiEventsForwarded;
		processor.processBlock(audio, midi);
		require(processor.oversizedAudioBlocks() == 1, "larger-than-hint realtime block was not counted");
		require(audio.getMagnitude(0, audio.getNumSamples()) == 0.0f,
			"larger-than-hint realtime block was not silent");
		require(processor.diagnosticSnapshot().hostMidiEventsForwarded == forwarded,
			"MIDI from a rejected realtime block was delivered");
	}
	for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
	{
		const auto reference = render(rate, false, false, false, false);
		const auto varied = render(rate, true, false, false, false);
		const auto offline = render(rate, true, true, false, false);
		const auto prepared = render(rate, true, true, false, true);
		const auto mono = render(rate, true, true, true, false);
		const auto late = render(rate, true, true, false, false, true);
		require(reference.missing == 0 && varied.missing == 0 && offline.missing == 0
			&& prepared.missing == 0 && mono.missing == 0 && late.missing == 0, "processor requested audio beyond known input");
		require(reference.audio.getMagnitude(0, reference.audio.getNumSamples()) > 0.4f, "probe failed to emit audio");
		for (int i = 0; i < reference.audio.getNumSamples(); ++i)
		{
			for (int channel = 0; channel < 2; ++channel)
			{
				const auto expected = reference.audio.getSample(channel, i);
				require(juce::exactlyEqual(expected, varied.audio.getSample(channel, i)), "callback partition changed PCM or MIDI timing");
				require(juce::exactlyEqual(expected, offline.audio.getSample(channel, i)), "offline PCM differs from realtime");
				require(juce::exactlyEqual(expected, prepared.audio.getSample(channel, i)), "reprepare changed the compensated timeline");
				require(juce::exactlyEqual(expected, late.audio.getSample(channel, i)), "late ROM selection changed the compensated timeline");
			}
			require(juce::exactlyEqual(mono.audio.getSample(0, i), (reference.audio.getSample(0, i) + reference.audio.getSample(1, i)) * 0.5f),
				"resampled mono output does not downmix both channels");
			if (juce::exactlyEqual(rate, 48000.0))
			{
				bool pulse = false;
				for (int note : {17, (int) (rate * .371), (int) (rate * 1.803)})
					pulse |= i >= note + reference.latency && i < note + reference.latency + 32;
				require(juce::exactlyEqual(reference.audio.getSample(0, i), pulse ? .5f : 0.0f), "reported latency does not match output delay");
			}
		}
	}
	require(fixture.deleteRecursively(), "fixture cleanup");
	std::puts("processor timeline: asynchronous boot, oversize policy, MIDI offsets, reported delay, variable/zero blocks, offline PCM, reprepare and mono passed");
}
