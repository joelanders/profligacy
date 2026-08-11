// Artifact-level VST3 smoke host. This intentionally does not link the plug-in
// target: it discovers and instantiates the packaged module like an external host.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

struct Options
{
	juce::String plugin;
	juce::String receipt;
	juce::String wav;
	juce::String expectStateMarker;
	juce::String stateOut;
	double sampleRate = 48000.0;
	double seconds = 18.0;
	double editorTimeoutSeconds = 15.0;
	int blockSize = 256;
	bool requireAudio = false;
	bool requireEditor = false;
	bool realtime = false;
};

bool parseOptions(int argc, char **argv, Options &o)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--plugin" && i + 1 < argc) o.plugin = argv[++i];
		else if (arg == "--receipt" && i + 1 < argc) o.receipt = argv[++i];
		else if (arg == "--wav" && i + 1 < argc) o.wav = argv[++i];
		else if (arg == "--expect-state-marker" && i + 1 < argc) o.expectStateMarker = argv[++i];
		else if (arg == "--state-out" && i + 1 < argc) o.stateOut = argv[++i];
		else if (arg == "--rate" && i + 1 < argc) o.sampleRate = std::atof(argv[++i]);
		else if (arg == "--block" && i + 1 < argc) o.blockSize = std::atoi(argv[++i]);
		else if (arg == "--seconds" && i + 1 < argc) o.seconds = std::atof(argv[++i]);
		else if (arg == "--editor-timeout" && i + 1 < argc) o.editorTimeoutSeconds = std::atof(argv[++i]);
		else if (arg == "--require-audio") o.requireAudio = true;
		else if (arg == "--require-editor") o.requireEditor = true;
		else if (arg == "--realtime") o.realtime = true;
		else return false;
	}
	return o.plugin.isNotEmpty() && o.receipt.isNotEmpty() && o.wav.isNotEmpty()
		&& o.sampleRate >= 8000.0 && o.blockSize > 0 && o.seconds >= 1.0
		&& o.editorTimeoutSeconds >= 1.0;
}

int fail(const juce::String &message)
{
	std::fprintf(stderr, "artifact host: %s\n", message.toRawUTF8());
	return 1;
}

juce::MemoryBlock unwrapVst3ComponentState(const juce::MemoryBlock &hostState)
{
	if (auto xml = juce::AudioProcessor::getXmlFromBinary(
			hostState.getData(), (int)hostState.getSize()))
	{
		if (const auto *component = xml->getChildByName("IComponent"))
		{
			juce::MemoryBlock result;
			if (result.fromBase64Encoding(component->getAllSubText()))
				return result;
		}
	}
	return hostState;
}

class EditorReadinessWaiter final : private juce::Timer
{
public:
	bool wait(const juce::File &markerToWatch, double timeoutSeconds)
	{
		marker = markerToWatch;
		deadline = juce::Time::getMillisecondCounterHiRes() + timeoutSeconds * 1000.0;
		startTimer(20);
		juce::MessageManager::getInstance()->runDispatchLoop();
		stopTimer();
		return ready;
	}

private:
	void timerCallback() override
	{
		const double now = juce::Time::getMillisecondCounterHiRes();
		if (!ready && marker.loadFileAsString().trim() == "profligacy-editor-v1")
		{
			ready = true;
			readyAt = now;
		}
		// Let the WebView message queue settle before the host destroys the editor
		// that owns the bridge.
		if ((ready && now - readyAt >= 500.0) || now >= deadline)
			juce::MessageManager::getInstance()->stopDispatchLoop();
	}

	juce::File marker;
	double deadline = 0.0;
	double readyAt = 0.0;
	bool ready = false;
};

} // namespace

int main(int argc, char **argv)
{
	Options options;
	if (!parseOptions(argc, argv, options))
		return fail("usage: --plugin PATH --receipt FILE --wav FILE [--rate HZ] "
			"[--block N] [--seconds N] [--require-audio] [--require-editor] "
			"[--editor-timeout N] [--realtime] "
			"[--expect-state-marker TEXT] [--state-out FILE]");
	const juce::File editorReadyMarker(options.receipt + ".editor-ready");
	if (options.requireEditor)
	{
		editorReadyMarker.deleteFile();
	#if JUCE_WINDOWS
		_putenv_s("PROFLIGACY_EDITOR_SMOKE", "1");
		_putenv_s("PROFLIGACY_EDITOR_SMOKE_RECEIPT", editorReadyMarker.getFullPathName().toRawUTF8());
	#else
		setenv("PROFLIGACY_EDITOR_SMOKE", "1", 1);
		setenv("PROFLIGACY_EDITOR_SMOKE_RECEIPT", editorReadyMarker.getFullPathName().toRawUTF8(), 1);
	#endif
	}

	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	juce::AudioPluginFormatManager formats;
	formats.addFormat(std::make_unique<juce::VST3PluginFormat>());

	juce::OwnedArray<juce::PluginDescription> descriptions;
	for (auto *format : formats.getFormats())
		if (format != nullptr && format->getName() == "VST3")
			format->findAllTypesForFile(descriptions, options.plugin);
	if (descriptions.size() != 1)
		return fail("expected exactly one VST3 type, found " + juce::String(descriptions.size()));

	const juce::PluginDescription description(*descriptions[0]);
	juce::String loadError;
	auto instance = formats.createPluginInstance(description, options.sampleRate,
		options.blockSize, loadError);
	if (instance == nullptr)
		return fail("could not instantiate VST3: " + loadError);

	const int inputChannels = instance->getTotalNumInputChannels();
	const int outputChannels = instance->getTotalNumOutputChannels();
	const bool identityOk = description.name == "Profligacy"
		&& description.pluginFormatName == "VST3" && description.isInstrument
		&& inputChannels == 0 && outputChannels == 2;
	if (!identityOk)
		return fail("unexpected plug-in identity or bus declaration: name=" + description.name
			+ " format=" + description.pluginFormatName
			+ " instrument=" + juce::String((int)description.isInstrument)
			+ " inputs=" + juce::String(inputChannels)
			+ " outputs=" + juce::String(outputChannels));
	if (!instance->acceptsMidi())
		return fail("plug-in instance does not accept MIDI");

	bool editorReady = false;
	if (options.requireEditor)
	{
		std::unique_ptr<juce::AudioProcessorEditor> editor(instance->createEditorAndMakeActive());
		if (editor == nullptr)
			return fail("plug-in did not create an editor");

		juce::DocumentWindow window("Profligacy packaged editor smoke",
			juce::Colours::black, juce::DocumentWindow::closeButton, false);
		window.setUsingNativeTitleBar(false);
		window.setContentNonOwned(editor.get(), true);
		window.centreWithSize(editor->getWidth(), editor->getHeight());
		window.addToDesktop();
		window.setVisible(true);

		EditorReadinessWaiter waiter;
		editorReady = waiter.wait(editorReadyMarker, options.editorTimeoutSeconds);

		window.setVisible(false);
		window.clearContentComponent();
		editorReadyMarker.deleteFile();
		if (!editorReady)
			return fail("packaged editor did not load its embedded page and native bridge within "
				+ juce::String(options.editorTimeoutSeconds, 1) + " seconds");
	}

	juce::MemoryBlock state;
	instance->getStateInformation(state);
	if (state.getSize() == 0)
		return fail("plug-in returned empty state");
	instance->setStateInformation(state.getData(), (int)state.getSize());

	instance->setPlayConfigDetails(0, 2, options.sampleRate, options.blockSize);
	instance->prepareToPlay(options.sampleRate, options.blockSize);

	juce::File wavFile(options.wav);
	wavFile.deleteFile();
	std::unique_ptr<juce::OutputStream> wavStream = wavFile.createOutputStream();
	if (wavStream == nullptr)
		return fail("could not create WAV output");
	juce::WavAudioFormat wavFormat;
	const auto writerOptions = juce::AudioFormatWriterOptions{}
		.withSampleRate(options.sampleRate).withNumChannels(2).withBitsPerSample(24);
	auto writer = wavFormat.createWriterFor(wavStream, writerOptions);
	if (writer == nullptr)
		return fail("could not create WAV writer");

	const std::int64_t totalSamples = (std::int64_t)std::llround(options.seconds * options.sampleRate);
	const std::int64_t noteOnSample = (std::int64_t)std::llround(11.0 * options.sampleRate);
	const std::int64_t noteOffSample = (std::int64_t)std::llround(13.0 * options.sampleRate);
	juce::AudioBuffer<float> audio(2, options.blockSize);
	double sumSquares = 0.0;
	float peak = 0.0f;
	std::uint64_t finiteSamples = 0;
	std::uint64_t nonFiniteSamples = 0;
	std::uint64_t nonZeroSamples = 0;
	std::uint64_t blocks = 0;
	auto nextBlock = std::chrono::steady_clock::now();

	for (std::int64_t blockStart = 0; blockStart < totalSamples; blockStart += options.blockSize)
	{
		const int samples = (int)std::min<std::int64_t>(options.blockSize, totalSamples - blockStart);
		juce::MidiBuffer midi;
		if (noteOnSample >= blockStart && noteOnSample < blockStart + samples)
			midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100),
				(int)(noteOnSample - blockStart));
		if (noteOffSample >= blockStart && noteOffSample < blockStart + samples)
			midi.addEvent(juce::MidiMessage::noteOff(1, 60),
				(int)(noteOffSample - blockStart));
		audio.setSize(2, samples, false, false, true);
		audio.clear();
		instance->processBlock(audio, midi);
		writer->writeFromAudioSampleBuffer(audio, 0, samples);
		for (int channel = 0; channel < audio.getNumChannels(); ++channel)
		{
			const float *data = audio.getReadPointer(channel);
			for (int i = 0; i < samples; ++i)
			{
				const float value = data[i];
				if (!std::isfinite(value)) { ++nonFiniteSamples; continue; }
				++finiteSamples;
				if (value != 0.0f) ++nonZeroSamples;
				peak = std::max(peak, std::abs(value));
				sumSquares += (double)value * (double)value;
			}
		}
		++blocks;
		if (options.realtime)
		{
			nextBlock += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
				std::chrono::duration<double>((double)samples / options.sampleRate));
			std::this_thread::sleep_until(nextBlock);
		}
	}

	juce::MemoryBlock finalState;
	instance->getStateInformation(finalState);
	if (options.stateOut.isNotEmpty()
		&& !juce::File(options.stateOut).replaceWithData(finalState.getData(), finalState.getSize()))
		return fail("could not write final state capture");
	const auto componentState = unwrapVst3ComponentState(finalState);
	const std::string finalStateBytes(
		static_cast<const char *>(componentState.getData()), componentState.getSize());
	std::string observedStateMarker;
	if (const auto marker = finalStateBytes.find("CILC:"); marker != std::string::npos)
	{
		const auto end = finalStateBytes.find('\0', marker);
		observedStateMarker = finalStateBytes.substr(marker,
			end == std::string::npos ? std::string::npos : end - marker);
	}
	const bool stateMarkerOk = options.expectStateMarker.isEmpty()
		|| observedStateMarker == options.expectStateMarker.toStdString();
	writer->flush();
	writer.reset();
	instance->releaseResources();
	instance.reset();

	const double rms = finiteSamples == 0 ? 0.0 : std::sqrt(sumSquares / (double)finiteSamples);
	const bool audioOk = nonFiniteSamples == 0
		&& (!options.requireAudio || (peak > 1.0e-6f && nonZeroSamples > 1000));

	juce::DynamicObject::Ptr receiptObject = new juce::DynamicObject();
	receiptObject->setProperty("schema", "profligacy-artifact-host-v1");
	receiptObject->setProperty("success", identityOk && audioOk && stateMarkerOk
		&& (!options.requireEditor || editorReady));
	receiptObject->setProperty("plugin_path", options.plugin);
	receiptObject->setProperty("name", description.name);
	receiptObject->setProperty("manufacturer", description.manufacturerName);
	receiptObject->setProperty("version", description.version);
	receiptObject->setProperty("format", description.pluginFormatName);
	receiptObject->setProperty("is_instrument", description.isInstrument);
	receiptObject->setProperty("accepts_midi", true);
	receiptObject->setProperty("input_channels", inputChannels);
	receiptObject->setProperty("output_channels", outputChannels);
	receiptObject->setProperty("state_bytes", (juce::int64)state.getSize());
	receiptObject->setProperty("sample_rate", options.sampleRate);
	receiptObject->setProperty("block_size", options.blockSize);
	receiptObject->setProperty("duration_seconds", options.seconds);
	receiptObject->setProperty("blocks", (juce::int64)blocks);
	receiptObject->setProperty("finite_samples", (juce::int64)finiteSamples);
	receiptObject->setProperty("nonfinite_samples", (juce::int64)nonFiniteSamples);
	receiptObject->setProperty("nonzero_samples", (juce::int64)nonZeroSamples);
	receiptObject->setProperty("peak", (double)peak);
	receiptObject->setProperty("rms", rms);
	receiptObject->setProperty("audio_required", options.requireAudio);
	receiptObject->setProperty("editor_required", options.requireEditor);
	receiptObject->setProperty("editor_ready", editorReady);
	receiptObject->setProperty("editor_timeout_seconds", options.editorTimeoutSeconds);
	receiptObject->setProperty("final_state_bytes", (juce::int64)finalState.getSize());
	receiptObject->setProperty("component_state_bytes", (juce::int64)componentState.getSize());
	receiptObject->setProperty("expected_state_marker", options.expectStateMarker);
	receiptObject->setProperty("observed_state_marker", juce::String(observedStateMarker));
	receiptObject->setProperty("state_marker_ok", stateMarkerOk);
	receiptObject->setProperty("realtime_pacing", options.realtime);
	receiptObject->setProperty("wav_path", options.wav);

	const juce::String json = juce::JSON::toString(juce::var(receiptObject.get()), true);
	if (!juce::File(options.receipt).replaceWithText(json + "\n"))
		return fail("could not write receipt");
	std::puts(json.toRawUTF8());
	if (!stateMarkerOk)
		std::fprintf(stderr, "artifact host: expected state marker '%s' was absent\n",
			options.expectStateMarker.toRawUTF8());
	return identityOk && audioOk && stateMarkerOk
		&& (!options.requireEditor || editorReady) ? 0 : 1;
}
