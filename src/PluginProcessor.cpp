// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginProcessor.cpp - JUCE AudioProcessor that plays the MAME Korg Prophecy engine.
//
// Rung 3 proof: this is a real VST3/AU/Standalone that boots the emulated machine on
// the engine's worker thread and streams its 48 kHz stereo output through processBlock.
// It links the MAME static archives via the prophecy_engine static library.
//
// Host MIDI (notes/CC/bend/sysex) and audio share absolute sample positions (see
// processBlock); timeline interpolation adapts the engine's native 48 kHz to the host
// rate. Known limitation: only ONE instance produces audio per process (the MAME
// machine is a singleton); a second instance is explicitly unavailable.
//
#include "PluginProcessor.h"

#include "BinaryData.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>

namespace {
struct PerformanceParameterDefinition
{
	const char *id;
	const char *name;
	int adin;
	int defaultValue;
};

constexpr std::array<PerformanceParameterDefinition,
	ProphecyAudioProcessor::kPerformanceParameterCount> kPerformanceParameters {{
	{ "perf.speed",   "Performance: Speed",             0,  0x01 },
	{ "perf.knob1",   "Performance: Knob 1",            1,  0x00 },
	{ "perf.knob2",   "Performance: Knob 2",            2,  0x00 },
	{ "perf.knob3",   "Performance: Knob 3",            3,  0x00 },
	{ "perf.knob4",   "Performance: Knob 4",            4,  0x00 },
	{ "perf.knob5",   "Performance: Knob 5",            5,  0x00 },
	{ "perf.wheel1",  "Performance: Wheel 1 / Pitch",   8,  0x80 },
	{ "perf.wheel2",  "Performance: Wheel 2 / Mod",     9,  0x80 },
	{ "perf.ribbonX", "Performance: X / Ribbon X",     12,  0x80 },
	{ "perf.logY",    "Performance: Y / Log-Wheel 3",  13,  0x74 },
	{ "perf.ribbonZ", "Performance: Z / Ribbon Pressure", 14, 0x00 },
}};
}

#if defined(_WIN32)
static int prophecy_setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && std::getenv(name) != nullptr) return 0;
	return _putenv_s(name, value);
}
#define setenv prophecy_setenv
#endif

#include "rom_locator.h"

ProphecyAudioProcessor::ProphecyAudioProcessor()
	: AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
	// Parameter IDs and order are part of the AU/VST3 project contract. Additions may be
	// appended in later releases, but these first eleven IDs must never be renamed/reordered.
	for (int i = 0; i < kPerformanceParameterCount; ++i)
	{
		const auto &definition = kPerformanceParameters[(std::size_t)i];
		auto parameter = std::make_unique<PerformanceParameter>(*this, i,
			juce::ParameterID { definition.id, 1 }, definition.name, definition.defaultValue);
		m_performanceParameters[(std::size_t)i] = parameter.get();
		addParameter(parameter.release());
	}

	// Match the driver's physical ADIN defaults until the host observes a gesture.
	// Most inputs start at zero; these are the modeled nonzero rests/sensors.
	for (auto &value : m_controllerDisplayValues)
		value.store(0x00, std::memory_order_relaxed);
	m_controllerDisplayValues[0].store(0x01, std::memory_order_relaxed);  // SPEED minimum
	m_controllerDisplayValues[7].store(0xa6, std::memory_order_relaxed);  // battery sense
	m_controllerDisplayValues[8].store(0x80, std::memory_order_relaxed);  // Wheel 1
	m_controllerDisplayValues[9].store(0x80, std::memory_order_relaxed);  // Wheel 2
	m_controllerDisplayValues[12].store(0x80, std::memory_order_relaxed); // ribbon finger-up
	m_controllerDisplayValues[13].store(0x74, std::memory_order_relaxed); // Log/Wheel 3 rest
	// Read diagnostics outside processBlock. Function-local static initialization and
	// getenv() are both inappropriate on a host's real-time callback.
	m_skipStateRestore = std::getenv("PROPHECY_EDITOR_SELFTEST") != nullptr;
	(void) m_engine.enableHostTimeline();
}

ProphecyAudioProcessor::PerformanceParameter::PerformanceParameter(
	ProphecyAudioProcessor &owner, int controlIndex, const juce::ParameterID &id,
	const juce::String &displayName, int defaultValue)
	: juce::AudioParameterInt(id, displayName, 0, 255, defaultValue),
	  m_owner(owner), m_controlIndex(controlIndex)
{
}

void ProphecyAudioProcessor::PerformanceParameter::valueChanged(int newValue)
{
	m_owner.performanceParameterChanged(m_controlIndex, newValue);
}

int ProphecyAudioProcessor::performanceControlForAdin(int source)
{
	for (int i = 0; i < kPerformanceParameterCount; ++i)
		if (kPerformanceParameters[(std::size_t)i].adin == source) return i;
	return -1;
}

void ProphecyAudioProcessor::performanceParameterChanged(int controlIndex, int value)
{
	if (controlIndex < 0 || controlIndex >= kPerformanceParameterCount) return;
	const int source = kPerformanceParameters[(std::size_t)controlIndex].adin;
	value = std::clamp(value, 0, 255);
	publishControllerDisplayValue(source, value);
	if (source == 9) m_wheel2Pos.store((std::uint8_t)value, std::memory_order_relaxed);
	m_performanceDirtyMask.fetch_or((std::uint16_t)(1u << controlIndex), std::memory_order_release);
}

void ProphecyAudioProcessor::setPerformanceValue(int controlIndex, int value, bool notifyHost)
{
	if (controlIndex < 0 || controlIndex >= kPerformanceParameterCount) return;
	auto *parameter = m_performanceParameters[(std::size_t)controlIndex];
	const float normalized = parameter->getNormalisableRange().convertTo0to1(
		(float)std::clamp(value, 0, 255));
	if (notifyHost) parameter->setValueNotifyingHost(normalized);
	else if (parameter->get() != value)
		static_cast<juce::AudioProcessorParameter *>(parameter)->setValue(normalized);
	else
	{
		const int source = kPerformanceParameters[(std::size_t)controlIndex].adin;
		publishControllerDisplayValue(source, value);
		if (source == 9) m_wheel2Pos.store((std::uint8_t)value, std::memory_order_relaxed);
	}
}

void ProphecyAudioProcessor::applyPendingPerformanceParameters(std::uint64_t frame)
{
	const auto dirty = m_performanceDirtyMask.exchange(0, std::memory_order_acq_rel);
	for (int i = 0; i < kPerformanceParameterCount; ++i)
	{
		const auto bit = (std::uint16_t)(1u << i);
		if ((dirty & bit) == 0) continue;
		const auto &definition = kPerformanceParameters[(std::size_t)i];
		const int value = m_performanceParameters[(std::size_t)i]->get();
		if (!m_engine.pushAdinAtFrame(definition.adin, value, frame))
			m_performanceDirtyMask.fetch_or(bit, std::memory_order_release);
	}
}

ProphecyAudioProcessor::~ProphecyAudioProcessor()
{
	m_engine.requestStop();
	if (m_initializationThread.joinable()) m_initializationThread.join();
	m_engine.stop();
}

void ProphecyAudioProcessor::setCurrentProgram(int program)
{
	if (program < 0 || program >= kProgramCount) return;
	// VST3 represents MIDI Program Change as a host parameter. The controller may
	// call this from a non-message thread and supplies no sample offset, so publish
	// only the latest intent here. The audio callback resolves Live's accompanying
	// bank-select CCs and schedules the firmware transaction at a block boundary.
	const auto epoch = m_stateRestoreEpoch.load(std::memory_order_acquire);
	m_pendingProgram.store((epoch << 9) | (std::uint64_t)program,
		std::memory_order_release);
	m_hostProgramIntents.fetch_add(1, std::memory_order_relaxed);
}

const juce::String ProphecyAudioProcessor::getProgramName(int program)
{
	if (program < 0 || program >= kProgramCount) return {};
	return juce::String(program < 64 ? "A" : "B")
		+ juce::String(program % 64).paddedLeft('0', 2);
}

void ProphecyAudioProcessor::publishCurrentProgram(int program, bool notifyHost)
{
	if (program < 0 || program >= kProgramCount) return;
	const int previous = m_currentProgram.exchange(program, std::memory_order_acq_rel);
	if (previous == program) return;
	m_currentProgramVersion.fetch_add(1, std::memory_order_release);
	// Editor patch selection runs on the message thread. Raw AU MIDI and VST3
	// parameter dispatch run on the audio thread and deliberately avoid host calls.
	if (notifyHost)
		updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withProgramChanged(true));
}

bool ProphecyAudioProcessor::collectHostBankSelect(const juce::MidiBuffer &midi)
{
	bool rawProgramChange = false;
	for (const auto meta : midi)
	{
		const auto *data = meta.data;
		const int bytes = meta.numBytes;
		if (bytes == 2 && (data[0] & 0xf0) == 0xc0)
			rawProgramChange = true;
		if (bytes != 3 || (data[0] & 0xf0) != 0xb0) continue;
		const int cc = data[1] & 0x7f;
		if ((cc != 0 && cc != 32)
				|| (CcTarget)m_ccMap[(std::size_t)cc].load(std::memory_order_relaxed)
					!= CcTarget::Off)
			continue;
		const std::size_t channel = (std::size_t)(data[0] & 0x0f);
		if (cc == 0) m_hostBankMsb[channel] = data[2] & 0x7f;
		else m_hostBankLsb[channel] = data[2] & 0x7f;
		++m_hostBankGeneration[channel];
	}
	return rawProgramChange;
}

void ProphecyAudioProcessor::dispatchPendingProgram(std::uint64_t eventFrame,
	std::uint64_t hostFrame, bool rawProgramChangeInBlock)
{
	if (m_cancelDeferredProgram.exchange(false, std::memory_order_acq_rel))
	{
		m_deferredProgram = -1;
		m_deferredProgramPending.store(false, std::memory_order_release);
	}

	const auto request = m_pendingProgram.exchange(kNoPendingProgram,
		std::memory_order_acq_rel);
	if (request != kNoPendingProgram
			&& (request >> 9) == m_stateRestoreEpoch.load(std::memory_order_acquire))
	{
		int program = (int)(request & 0xff);
		const bool fromEditor = (request & kEditorProgramBit) != 0;
		// Ableton emits Bank/Sub-Bank as CC0/CC32 and the VST3 Program value as
		// a separate kIsProgramChange parameter. A fresh channel-1 bank pair makes
		// Program 1..64 address A00..B63; otherwise the VST3 factory-program list
		// remains a direct flat A00..B63 selector.
		constexpr std::size_t channel = 0;
		const bool freshBank = m_hostBankGeneration[channel]
			!= m_consumedHostBankGeneration[channel];
		if (!fromEditor && program < 64 && freshBank
				&& m_hostBankMsb[channel] == 0 && m_hostBankLsb[channel] < 2)
			program += (int)m_hostBankLsb[channel] * 64;
		m_consumedHostBankGeneration[channel] = m_hostBankGeneration[channel];
		// VST3 may replay its matching Program parameter after component state.
		// Without fresh Bank/Sub-Bank input that is an identity update, not a request
		// to replace the authoritative restored edit buffer with a factory patch.
		if (!fromEditor && !freshBank
				&& program == m_currentProgram.load(std::memory_order_acquire))
		{
			m_deferredProgram = -1;
			m_deferredProgramPending.store(false, std::memory_order_release);
		}
		else
		{
			m_deferredProgram = program;
			m_deferredProgramFromEditor = fromEditor;
			m_deferredProgramEarliestFrame = hostFrame + (fromEditor
				? (std::uint64_t)std::ceil(m_hostSampleRate * 0.5) : 0);
			m_deferredProgramPending.store(true, std::memory_order_release);
		}
	}

	// Some hosts deliver a real legacy MIDI Program Change as well as updating
	// their program parameter. The raw event below is already sample-positioned;
	// never duplicate it with the offset-less VST3 compatibility path.
	if (rawProgramChangeInBlock)
	{
		m_deferredProgram = -1;
		m_deferredProgramPending.store(false, std::memory_order_release);
		return;
	}
	const auto nextGenerated = m_nextGeneratedProgramFrame.load(std::memory_order_acquire);
	if (m_deferredProgram < 0 || hostFrame < nextGenerated
			|| hostFrame < m_deferredProgramEarliestFrame) return;
	const int program = m_deferredProgram;
	const std::uint8_t message[8] = {
		0xB0, 0x00, 0x00,
		0xB0, 0x20, (std::uint8_t)(program / 64),
		0xC0, (std::uint8_t)(program % 64) };
	if (!m_engine.pushMidiAtFrame(message, sizeof(message), eventFrame)) return;

	m_deferredProgram = -1;
	m_nextGeneratedProgramFrame.store(hostFrame + (std::uint64_t)std::ceil(
		m_hostSampleRate * (kPatchSelectMinIntervalMs / 1000.0)), std::memory_order_release);
	publishCurrentProgram(program, false);
	if (m_deferredProgramFromEditor)
		m_editorPatchSends.fetch_add(1, std::memory_order_relaxed);
	else
		m_hostProgramSends.fetch_add(1, std::memory_order_relaxed);
	m_deferredProgramPending.store(false, std::memory_order_release);
	const std::uint64_t wanted = hostFrame + (std::uint64_t)std::ceil(
		m_hostSampleRate * kPatchLoadSettleSeconds);
	std::uint64_t previous = m_patchLoadBarrierUntilFrame.load(std::memory_order_relaxed);
	while (previous < wanted && !m_patchLoadBarrierUntilFrame.compare_exchange_weak(
		previous, wanted, std::memory_order_release, std::memory_order_relaxed)) {}
}

bool ProphecyAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
	const auto &out = layouts.getMainOutputChannelSet();
	return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void ProphecyAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
	const double previousSampleRate = m_hostSampleRate;
	const double nextSampleRate = std::isfinite(sampleRate) && sampleRate > 0.0
		? sampleRate : (double) ProphecyEngine::kSampleRate;
	// Generated Program Change pacing uses the monotonic callback-frame counter so
	// transport seeks cannot bypass it. If a host reprepares at another rate, retain
	// the remaining wall-clock duration by rescaling deadlines into the new frame unit.
	const auto currentHostFrame = m_audioHostFrames.load(std::memory_order_acquire);
	const auto rescaleDeadline = [currentHostFrame, previousSampleRate, nextSampleRate]
		(std::uint64_t deadline)
	{
		if (deadline <= currentHostFrame) return std::uint64_t{0};
		return currentHostFrame + (std::uint64_t)std::ceil(
			(double)(deadline - currentHostFrame) * nextSampleRate / previousSampleRate);
	};
	m_nextGeneratedProgramFrame.store(rescaleDeadline(
		m_nextGeneratedProgramFrame.load(std::memory_order_acquire)), std::memory_order_release);
	m_patchLoadBarrierUntilFrame.store(rescaleDeadline(
		m_patchLoadBarrierUntilFrame.load(std::memory_order_acquire)), std::memory_order_release);
	if (m_deferredProgramPending.load(std::memory_order_acquire))
		m_deferredProgramEarliestFrame = rescaleDeadline(m_deferredProgramEarliestFrame);
	m_hostSampleRate = nextSampleRate;
	const auto quantum = ProphecyEngine::kAudioQuantum;
	m_timelineHostFrame = 0;
	m_oversizedAudioBlocks.store(0, std::memory_order_relaxed);
	m_editorPatchIntents.store(0, std::memory_order_relaxed);
	m_editorPatchSends.store(0, std::memory_order_relaxed);
	m_hostProgramIntents.store(0, std::memory_order_relaxed);
	m_hostProgramSends.store(0, std::memory_order_relaxed);
	m_editorDumpRequests.store(0, std::memory_order_relaxed);
	m_editorDumpSends.store(0, std::memory_order_relaxed);
	m_hostMidiEventsForwarded.store(0, std::memory_order_relaxed);
	// samplesPerBlock is only a host hint in JUCE. Reserve a generous fixed floor so
	// ordinary offline/host block-size changes stay allocation-free; a still-larger block
	// is explicitly silenced and counted in processBlock rather than resizing there.
	m_preparedMaxBlock = std::max(samplesPerBlock, 16384);
	m_maxExpectedBlock = std::max(samplesPerBlock, 1);

	// Prepare the 48 kHz -> host-rate resampler (bypassed when the host runs at 48 kHz).
	const double ratio = (double) ProphecyEngine::kSampleRate / m_hostSampleRate;
	const int    cap   = (int) std::ceil(m_preparedMaxBlock * ratio) + 64;
	m_rsIn[0].assign((size_t) cap, 0.0f);
	m_rsIn[1].assign((size_t) cap, 0.0f);

	// Reserve the requested output block, one quantum for grant alignment,
	// one quantum for worker execution, and interpolation lookahead. A short
	// preceding callback must not remove the worker execution allowance. Output is
	// explicitly delayed by this exact integer number of host samples; storage
	// capacity is unrelated.
	const auto nativeLead = std::max(std::ceil(std::max(samplesPerBlock, 1) * ratio),
		(double) quantum) + 2 * quantum + 2;
	setLatencySamples((int) std::ceil(nativeLead / ratio));
	m_engine.setHostBlockFrames((std::uint32_t) std::ceil(std::max(samplesPerBlock, 1) * ratio));

	maybeBootEngine();
	// Install a provisional origin immediately. Asynchronous first boot replaces it
	// with the completed firmware origin; reprepare after readiness starts beyond the
	// previous input grant, independent of worker speed.
	const auto origin = ((m_engine.requestedFrames() + quantum - 1) / quantum) * quantum;
	m_timeline.reset(m_hostSampleRate, origin);
	m_timelineAttached = m_engine.readyForPlayback();
}

// Boot the engine once, iff a valid ROM set can be located (env / persisted picker
// choice / drop dir — see rom_locator.h). Without one the plugin stays silent and the
// editor shows the first-run ROM picker; the pick then boots via setRomDirFromUser().
bool ProphecyAudioProcessor::maybeBootEngine()
{
	if (m_bootClaimed.load())
		return m_engine.instanceStatus() == ProphecyEngine::InstanceStatus::Active;
	const juce::File romDir = romloc::locateRomDir();
	if (romDir == juce::File())
		return false;
	if (m_bootClaimed.exchange(true))
		return m_engine.instanceStatus() == ProphecyEngine::InstanceStatus::Active;
	// A ROM chosen after callbacks have already begun starts a fresh audible epoch.
	// Ordinary startup instead retains project frames elapsed during initialization:
	// the completed firmware state represents project frame zero, so realtime and
	// offline initialization reach the same post-boot synth time at a later host frame.
	m_lateBootStartsFreshEpoch.store(m_audioCallbacks.load(std::memory_order_acquire) != 0,
		std::memory_order_release);
	m_romPath   = romDir.getFullPathName();
	m_nvramPath = romloc::nvramDirFor(romDir).getFullPathName();

	// Headless, focus-safe MAME (see korgprophecy build/run notes).
	setenv("SDL_VIDEODRIVER", "dummy", 1);
	setenv("SDL_AUDIODRIVER", "dummy", 1);
	setenv("KPROP_LIE_BATTERY_OK", "1", 0);

	// No -seconds_to_run: the machine runs until the plugin is destroyed (stop()).
	std::vector<std::string> args = {
		"prophecy", "korgprop",
		"-rompath", m_romPath.toStdString(),
		"-nvram_directory", m_nvramPath.toStdString(),
		"-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo",
		"-debugger", "none", "-midiprovider", "none", "-networkprovider", "none",
		"-keyboardprovider", "none", "-mouseprovider", "none",
		"-lightgunprovider", "none", "-joystickprovider", "none", "-output", "none",
		"-noplugins",
	};
	// Debug hook: PROPHECY_MAME_ARGS="-log -verbose" appends extra MAME options
	// (whitespace-split). driver logerror diagnostics (KPROP_*) need -log; run the
	// Standalone from a terminal to keep stderr.
	if (const char *extra = std::getenv("PROPHECY_MAME_ARGS"))
	{
		std::istringstream ss(extra);
		for (std::string tok; ss >> tok; )
			args.push_back(tok);
	}
	const bool started = m_engine.start(args);
	if (started)
	{
		// Clean-room CI firmware has its own sentinel protocol, not Korg's boot
		// screen or SysEx parser. Ordinary firmware must complete the handshake.
		const bool firmware = std::getenv("PROFLIGACY_CI_EXPOSE_LCD_STATE") == nullptr;
		m_initializationState.store(InitializationState::Running, std::memory_order_release);
		m_initializationThread = std::thread([this, firmware] {
			const bool ready = m_engine.initializePlayback(firmware);
			if (ready)
			{
				// Host state can restore Wheel 2 before prepareToPlay. Pre-start engine writes are
				// rejected; publish the retained value once this engine accepts playback input.
				(void) m_engine.pushAdin(9, m_wheel2Pos.load(std::memory_order_relaxed));
			}
			{
				// Pair the state transition with the wait mutex so an offline callback
				// cannot miss the notification between checking and sleeping.
				std::lock_guard lock(m_initializationMutex);
				m_initializationState.store(ready ? InitializationState::Ready
					: InitializationState::Failed, std::memory_order_release);
			}
			m_initializationChanged.notify_all();
		});
	}
	return started;
}

bool ProphecyAudioProcessor::setRomDirFromUser(const juce::File &dir)
{
	if (!romloc::isValidRomDir(dir))
		return false;
	romloc::persistRomDir(dir);
	return maybeBootEngine();
}

void ProphecyAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midi)
{
	juce::ScopedNoDenormals noDenormals;
	const int numSamples  = buffer.getNumSamples();
	const int numChannels = buffer.getNumChannels();
	bool engineActive = m_engine.readyForPlayback();
	if (!engineActive && isNonRealtime()
			&& m_initializationState.load(std::memory_order_acquire) == InitializationState::Running)
	{
		// Offline and VST3-prefetch callbacks may wait for deterministic output;
		// realtime callbacks must never wait for firmware or the emulator.
		std::unique_lock lock(m_initializationMutex);
		m_initializationChanged.wait_for(lock, std::chrono::seconds(30), [this] {
			return m_initializationState.load(std::memory_order_acquire)
				!= InitializationState::Running;
		});
		engineActive = m_engine.readyForPlayback();
	}
	if (engineActive && (!m_timelineAttached || m_timeline.origin() < m_engine.playbackOrigin()))
	{
		// A ROM may be selected long after the DAW started calling processBlock.
		// Adopt its completed initialization epoch, with the same fixed delay.
		m_timeline.reset(m_hostSampleRate, m_engine.playbackOrigin());
		if (m_lateBootStartsFreshEpoch.exchange(false, std::memory_order_acq_rel))
			m_timelineHostFrame = 0;
		m_timelineAttached = true;
	}
	m_audioCallbacks.fetch_add(1, std::memory_order_relaxed);
	m_audioHostFrames.fetch_add((std::uint64_t) std::max(numSamples, 0), std::memory_order_relaxed);
	if (numSamples > m_preparedMaxBlock
			|| (!isNonRealtime() && numSamples > m_maxExpectedBlock))
	{
		// The host exceeded its realtime prepare hint, or even the larger offline
		// reserve. Do bounded work only: output silence, count the contract violation,
		// and ignore MIDI rather than touch undersized realtime storage.
		buffer.clear();
		m_oversizedAudioBlocks.fetch_add(1, std::memory_order_relaxed);
		m_audioUnderrunFrames.fetch_add((std::uint64_t) numSamples, std::memory_order_relaxed);
		m_timelineHostFrame += (std::uint64_t) numSamples;
		if (engineActive) m_engine.requestThroughFrame(m_timeline.horizon(m_timelineHostFrame));
		return;
	}

	// Forward every host MIDI event (note/CC/bend/sysex) into the emulated 31250-baud
	// UART at its absolute sample position, before granting this input range to
	// the worker. Hardware serial and scan/voice-allocation latency stays intact.
	const auto timelineBlockStart = m_timelineHostFrame;
	const auto midiFrameBase = m_timeline.event(timelineBlockStart);
	const std::uint64_t hostBlockEnd = m_audioHostFrames.load(std::memory_order_relaxed);
	const std::uint64_t hostBlockStart = hostBlockEnd - (std::uint64_t)numSamples;
	const bool rawProgramChangeInBlock = collectHostBankSelect(midi);
	if (engineActive)
		dispatchPendingProgram(midiFrameBase, hostBlockStart, rawProgramChangeInBlock);
	// Parameter callbacks only publish a fixed atomic bitset. Deliver the latest value for
	// each changed control at this block's exact engine-frame boundary, on the same bounded
	// audio-thread queue used by sample-positioned mapped CC input.
	if (engineActive) applyPendingPerformanceParameters(midiFrameBase);
	for (const auto meta : midi)
	{
		const auto eventFrame = m_timeline.event(timelineBlockStart
			+ (std::uint64_t) std::clamp(meta.samplePosition, 0, std::max(numSamples - 1, 0)));
		// Work from MidiBuffer's borrowed metadata bytes. Constructing a MidiMessage here
		// can allocate for SysEx payloads, which is forbidden on the audio callback.
		const std::uint8_t *data = meta.data;
		const int numBytes = meta.numBytes;
		m_hostMidiEvents.fetch_add(1, std::memory_order_relaxed);
		const std::uint32_t status = numBytes > 0 ? data[0] : 0;
		const std::uint32_t data1  = numBytes > 1 ? data[1] : 0;
		const std::uint32_t data2  = numBytes > 2 ? data[2] : 0;
		m_lastHostMidi.store(status | (data1 << 8) | (data2 << 16)
			| ((std::uint32_t) std::min(numBytes, 255) << 24), std::memory_order_relaxed);
		const std::uint64_t hostEventFrame = hostBlockStart
			+ (std::uint64_t)std::max(meta.samplePosition, 0);
		if (numBytes == 2 && (data[0] & 0xf0) == 0xc0)
		{
			const std::size_t channel = (std::size_t)(data[0] & 0x0f);
			int program = data[1] & 0x7f;
			if (program < 64 && m_hostBankMsb[channel] == 0 && m_hostBankLsb[channel] < 2)
				program += (int)m_hostBankLsb[channel] * 64;
			m_consumedHostBankGeneration[channel] = m_hostBankGeneration[channel];
			publishCurrentProgram(program, false);
			m_nextGeneratedProgramFrame.store(hostBlockStart
				+ (std::uint64_t)std::max(meta.samplePosition, 0)
				+ (std::uint64_t)std::ceil(m_hostSampleRate
					* (kPatchSelectMinIntervalMs / 1000.0)), std::memory_order_release);
			const std::uint64_t settleFrames = (std::uint64_t)std::ceil(
				m_hostSampleRate * kPatchLoadSettleSeconds);
			const std::uint64_t wanted = hostEventFrame + settleFrames;
			std::uint64_t previous = m_patchLoadBarrierUntilFrame.load(std::memory_order_relaxed);
			while (previous < wanted && !m_patchLoadBarrierUntilFrame.compare_exchange_weak(
				previous, wanted, std::memory_order_release, std::memory_order_relaxed)) {}
		}
		if (numBytes >= 2)
		{
			const int command = data[0] & 0xf0;
			const int note = data[1] & 0x7f;
			const std::uint64_t bit = std::uint64_t(1) << (note & 63);
			if (command == 0x90 && numBytes >= 3 && data[2] != 0)
				m_activeHostNotes[(std::size_t) (note >> 6)].fetch_or(bit, std::memory_order_relaxed);
			else if (command == 0x80 || (command == 0x90 && numBytes >= 3 && data[2] == 0))
				m_activeHostNotes[(std::size_t) (note >> 6)].fetch_and(~bit, std::memory_order_relaxed);
			else if (command == 0xb0 && numBytes >= 3 && (data[1] == 120 || data[1] == 123))
			{
				m_activeHostNotes[0].store(0, std::memory_order_relaxed);
				m_activeHostNotes[1].store(0, std::memory_order_relaxed);
			}
		}
		// CC->ADIN remap: a mapped control-change is translated to a front-panel analog
		// write (the composite X-Y control, ribbon Z, or wheel 1/2) and NOT forwarded raw
		// (double-apply). Every
		// other message — including unmapped CCs — passes through unchanged.
		if (numBytes == 3 && (data[0] & 0xf0) == 0xb0)
		{
			const int cc = data[1] & 0x7f;
			const auto tgt = (CcTarget) m_ccMap[(std::size_t) cc].load(std::memory_order_relaxed);
			if (tgt != CcTarget::Off)
			{
				handleMappedCc(cc, data[2] & 0x7f, tgt, eventFrame);
				continue;
			}
			// An ordinary MIDI mod-wheel message remains ordinary MIDI. Mirror it onto
			// the onscreen MOD slider, but do not also write ADIN9 (that would apply it
			// twice inside the synth). Explicit CC mappings above own their target display.
			if (cc == 1)
				publishControllerDisplayValue(9, ((data[2] & 0x7f) * 255 + 63) / 127);
		}
		else if (numBytes == 3 && (data[0] & 0xf0) == 0xe0)
		{
			// Pitch bend is a 14-bit UART message, not an ADIN8 write. Display the same
			// normalized position while forwarding the original bytes unchanged below.
			const int bend = (data[1] & 0x7f) | ((data[2] & 0x7f) << 7);
			publishControllerDisplayValue(8, (bend * 255 + 8191) / 16383);
		}
		if (m_engine.pushMidiAtFrame(data, (std::size_t) numBytes, eventFrame))
			m_hostMidiEventsForwarded.fetch_add(1, std::memory_order_relaxed);
	}

	// Preserve the existing state policy: restore once after firmware boot has
	// had its historical safety margin. The timeline only changes the absolute
	// frame used for the same one-shot SysEx injection.
	if (!m_skipStateRestore && engineActive && m_pendingReady.load() && !m_pendingInjected.load()
			&& m_engine.producedFrames() > (std::uint64_t) ProphecyEngine::kSampleRate * 11)
	{
		if (m_engine.pushMidiAtFrame(m_pendingState.data(), m_pendingState.size(), midiFrameBase))
			m_pendingInjected.store(true);
	}
	m_timelineHostFrame += (std::uint64_t) numSamples;
	if (engineActive) m_engine.requestThroughFrame(m_timeline.horizon(m_timelineHostFrame));

	buffer.clear();
	if (numSamples == 0 || numChannels == 0) return;
	if (!engineActive)
	{
		m_audioUnderrunFrames.fetch_add((std::uint64_t) numSamples, std::memory_order_relaxed);
		return;
	}

	const auto latency = (std::uint64_t) getLatencySamples();
	// The leading delay is intentional silence, including after a reprepare.
	const auto firstHost = std::max(timelineBlockStart, latency);
	if (firstHost >= m_timelineHostFrame) return;
	const auto outputOffset = (int) (firstHost - timelineBlockStart);
	const auto outputCount = numSamples - outputOffset;
	const auto firstPosition = m_timeline.position(firstHost - latency);
	const auto lastPosition = m_timeline.position(m_timelineHostFrame - latency - 1);
	const auto firstIndex = (std::uint64_t) std::floor(firstPosition);
	const auto lastIndex = (std::uint64_t) std::floor(lastPosition);
	const bool nativeRate = juce::exactlyEqual(m_hostSampleRate, (double) ProphecyEngine::kSampleRate);
	const auto windowStart = nativeRate ? firstIndex : (firstIndex > 1 ? firstIndex - 2 : 0);
	const auto windowEnd = lastIndex + (nativeRate ? 1 : 3);
	const auto needed = (std::size_t) (windowEnd - windowStart);
	if (needed > m_rsIn[0].size() || needed > ProphecyEngine::kTimelineCapacity)
	{
		m_audioUnderrunFrames.fetch_add((std::uint64_t) outputCount, std::memory_order_relaxed);
		return;
	}
	const auto got = m_engine.readAtFrame(m_timeline.origin() + windowStart,
		m_rsIn[0].data(), m_rsIn[1].data(), needed, isNonRealtime());
	m_audioEngineFrames.fetch_add(got, std::memory_order_relaxed);
	m_audioUnderrunFrames.fetch_add(needed - got, std::memory_order_relaxed);

	for (int i = 0; i < outputCount; ++i)
	{
		const auto position = m_timeline.position(firstHost - latency + (std::uint64_t) i);
		const auto index = (std::uint64_t) std::floor(position);
		const auto fraction = position - index;
		const auto relative = (std::size_t) (index - windowStart);
		float stereo[2];
		for (int channel = 0; channel < 2; ++channel)
		{
			const auto* input = m_rsIn[channel].data();
			stereo[channel] = nativeRate ? input[relative]
				: prophecy::interpolate(index > 1 ? input[relative - 2] : 0.0f,
					index > 0 ? input[relative - 1] : 0.0f,
					input[relative], input[relative + 1], input[relative + 2], fraction);
		}
		if (numChannels == 1)
			buffer.setSample(0, outputOffset + i, (stereo[0] + stereo[1]) * 0.5f);
		else
		{
			buffer.setSample(0, outputOffset + i, stereo[0]);
			buffer.setSample(1, outputOffset + i, stereo[1]);
		}
	}

}

ProphecyAudioProcessor::DiagnosticSnapshot ProphecyAudioProcessor::diagnosticSnapshot() const
{
	DiagnosticSnapshot s;
	s.producedFrames = m_engine.producedFrames();
	s.bufferedFrames = m_engine.available();
	s.engineRunning = m_engine.running();
	s.audioCallbacks = m_audioCallbacks.load(std::memory_order_relaxed);
	s.audioHostFrames = m_audioHostFrames.load(std::memory_order_relaxed);
	s.audioEngineFrames = m_audioEngineFrames.load(std::memory_order_relaxed);
	s.audioUnderrunFrames = m_audioUnderrunFrames.load(std::memory_order_relaxed);
	s.hostMidiEvents = m_hostMidiEvents.load(std::memory_order_relaxed);
	s.hostMidiEventsForwarded = m_hostMidiEventsForwarded.load(std::memory_order_relaxed);
	s.lastHostMidi = m_lastHostMidi.load(std::memory_order_relaxed);
	s.activeNotesLow = m_activeHostNotes[0].load(std::memory_order_relaxed);
	s.activeNotesHigh = m_activeHostNotes[1].load(std::memory_order_relaxed);
	s.droppedImmediateMidiBytes = droppedImmediateMidiBytes();
	s.droppedScheduledMidiBytes = droppedScheduledMidiBytes();
	s.droppedUiAdinEvents = droppedUiAdinEvents();
	s.droppedAudioAdinEvents = droppedAudioAdinEvents();
	s.oversizedBlocks = oversizedAudioBlocks();
	s.editorPatchIntents = m_editorPatchIntents.load(std::memory_order_relaxed);
	s.editorPatchSends = m_editorPatchSends.load(std::memory_order_relaxed);
	s.hostProgramIntents = m_hostProgramIntents.load(std::memory_order_relaxed);
	s.hostProgramSends = m_hostProgramSends.load(std::memory_order_relaxed);
	s.currentProgram = m_currentProgram.load(std::memory_order_acquire);
	s.editorDumpRequests = m_editorDumpRequests.load(std::memory_order_relaxed);
	s.editorDumpSends = m_editorDumpSends.load(std::memory_order_relaxed);
	s.editorCommandsSent = m_editorCommandPacer.sent();
	s.editorCommandsCoalesced = m_editorCommandPacer.coalesced();
	s.editorCommandsCancelled = m_editorCommandPacer.cancelled();
	s.editorCommandsDropped = m_editorCommandPacer.dropped();
	s.editorCommandsPending = m_editorCommandPacer.pending();
	return s;
}

//============================================================
//  State: DAW projects persist the current edit buffer as the hardware's own SysEx dump.
//  The standalone deliberately persists preferences only: quitting it is not an implicit
//  patch-save operation, and the emulated synth should boot from its explicitly written NVRAM.
//============================================================
// State container (backward compatible): a legacy state is the bare SysEx program dump.
// PRP1 adds the CC->ADIN map; PRP2 adds Wheel 2; PRP3 stores the stable performance
// parameters; PRP4 also identifies the selected factory program so the VST3 program
// parameter cannot overwrite an edited-buffer dump during project restore. Layout:
// "PRP4" | u8 mapCount | map pairs | u8 parameterCount | parameter bytes
//        | u8 selectedProgram | dump...
void ProphecyAudioProcessor::getStateInformation(juce::MemoryBlock &dest)
{
	dest.reset();

	// Exact edit-buffer recall is part of a DAW project save. The standalone has the
	// hardware-like policy instead: only an explicit front-panel WRITE persists a patch.
	std::vector<std::uint8_t> dump;
	if (wrapperType != juce::AudioProcessor::wrapperType_Standalone
			&& m_engine.producedFrames() >= (std::uint64_t) ProphecyEngine::kSampleRate * 11)
	{
		std::uint8_t tmp[512];
		while (m_engine.popMidiTx(tmp, sizeof(tmp)) > 0) {}                    // drop stale TX
		const std::uint8_t req[7] = {0xF0, 0x42, 0x30, 0x41, 0x10, 0x00, 0xF7}; // current-program dump request
		(void) pushImmediateMidi(req, sizeof(req));

		std::vector<std::uint8_t> tx;
		const double deadline = juce::Time::getMillisecondCounterHiRes() + 400.0;
		bool got = false;
		while (!got && juce::Time::getMillisecondCounterHiRes() < deadline)
		{
			std::size_t g;
			while ((g = m_engine.popMidiTx(tmp, sizeof(tmp))) > 0) tx.insert(tx.end(), tmp, tmp + g);
			for (std::size_t i = 0; i + 4 < tx.size(); ++i)
			{
				if (tx[i] != 0xF0 || tx[i + 4] != 0x40) continue;             // CURRENT_PROGRAM_DATA_DUMP
				std::size_t j = i + 1;
				while (j < tx.size() && tx[j] != 0xF7) ++j;
				if (j < tx.size()) { dump.assign(tx.data() + i, tx.data() + j + 1); got = true; break; }
			}
			if (!got) juce::Thread::sleep(5);
		}
	}

	// Header + mapping (non-Off CC entries) + the stable performance-parameter bank.
	dest.append("PRP4", 4);
	std::vector<std::uint8_t> map;
	for (int cc = 0; cc < 128; ++cc)
	{
		const int t = ccMapTarget(cc);
		if (t != 0) { map.push_back((std::uint8_t) cc); map.push_back((std::uint8_t) t); }
	}
	const std::uint8_t n = (std::uint8_t) (map.size() / 2);
	dest.append(&n, 1);
	if (!map.empty()) dest.append(map.data(), map.size());
	const std::uint8_t parameterCount = kPerformanceParameterCount;
	dest.append(&parameterCount, 1);
	std::array<std::uint8_t, kPerformanceParameterCount> parameterValues {};
	for (int i = 0; i < kPerformanceParameterCount; ++i)
		parameterValues[(std::size_t)i] = (std::uint8_t)m_performanceParameters[(std::size_t)i]->get();
	dest.append(parameterValues.data(), parameterValues.size());
	const std::uint8_t selectedProgram = (std::uint8_t)getCurrentProgram();
	dest.append(&selectedProgram, 1);
	if (!dump.empty()) dest.append(dump.data(), dump.size());

	// Opt-in packaged-product CI marker carried through the standard VST3 state
	// API. It is appended only after a live LCD snapshot exists and is never
	// enabled in ordinary hosts, so the shipping state format remains unchanged.
	if (std::getenv("PROFLIGACY_CI_EXPOSE_LCD_STATE") != nullptr)
	{
		char line1[41] = {};
		char line2[41] = {};
		if (m_engine.latestLcd(line1, line2, sizeof(line1)))
		{
			static constexpr char marker[] = "CILC:";
			const auto lcdLine = juce::String::fromUTF8(line1).trimEnd();
			dest.append(marker, sizeof(marker) - 1);
			dest.append(lcdLine.toRawUTF8(), lcdLine.getNumBytesAsUTF8());
			const char terminator = '\0';
			dest.append(&terminator, 1);
		}
	}
}

void ProphecyAudioProcessor::setStateInformation(const void *data, int size)
{
	if (data == nullptr || size <= 0) return;
	const auto *p = static_cast<const std::uint8_t *>(data);
	// A state load is authoritative over any host-program callback that raced ahead
	// of it. Tag future callbacks with the new epoch and ask the audio thread to
	// discard an already-resolved but not-yet-delivered request.
	m_stateRestoreEpoch.fetch_add(1, std::memory_order_acq_rel);
	m_pendingProgram.store(kNoPendingProgram, std::memory_order_release);
	m_cancelDeferredProgram.store(true, std::memory_order_release);

	const std::uint8_t *dump = p;
	int dumpLen = size;
	const bool prp4 = (size >= 5 && std::memcmp(p, "PRP4", 4) == 0);
	const bool prp3 = (size >= 5 && std::memcmp(p, "PRP3", 4) == 0);
	const bool prp2 = (size >= 5 && std::memcmp(p, "PRP2", 4) == 0);
	const bool prp1 = (size >= 5 && std::memcmp(p, "PRP1", 4) == 0);
	if (prp1 || prp2 || prp3 || prp4)
	{
		const int n = p[4];
		int mapEnd = 5 + n * 2;
		// A blob whose declared map (+ its controller payload) overruns the buffer is corrupt —
		// bail entirely rather than feeding the magic bytes to the firmware as a dump.
		if (mapEnd + (prp2 || prp3 || prp4 ? 1 : 0) > size) return;
		const int parameterCount = (prp3 || prp4) ? p[mapEnd] : 0;
		if ((prp3 || prp4)
				&& mapEnd + 1 + parameterCount + (prp4 ? 1 : 0) > size) return;
		for (int cc = 0; cc < 128; ++cc) setCcMap(cc, 0);
		for (int i = 0; i < n; ++i) setCcMap(p[5 + i * 2], p[5 + i * 2 + 1]);
		// Reset parameters absent from legacy states to the exact modeled hardware defaults.
		for (int i = 0; i < kPerformanceParameterCount; ++i)
			setPerformanceValue(i, kPerformanceParameters[(std::size_t)i].defaultValue, false);
		if (prp3 || prp4)
		{
			++mapEnd;
			if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
			{
				// Preserve the standalone's hardware-like persistence policy: Wheel 2 is
				// friction-held state, while sprung/touch controls and PE knobs restart at
				// their modeled defaults. AU/VST3 projects restore the complete bank below.
				constexpr int wheel2Index = 7;
				if (parameterCount > wheel2Index)
					setPerformanceValue(wheel2Index, p[mapEnd + wheel2Index], false);
			}
			else
			{
				for (int i = 0; i < std::min(parameterCount, kPerformanceParameterCount); ++i)
					setPerformanceValue(i, p[mapEnd + i], false);
			}
			mapEnd += parameterCount;
			if (prp4)
			{
				const int restoredProgram = p[mapEnd++];
				if (wrapperType != juce::AudioProcessor::wrapperType_Standalone)
					publishCurrentProgram(restoredProgram, false);
			}
		}
		else if (prp2)
		{
			setWheel2(p[mapEnd++]);
		}
		dump = p + mapEnd;
		dumpLen = size - mapEnd;
	}
	else
	{
		// Bare-SysEx projects predate all controller state.
		for (int i = 0; i < kPerformanceParameterCount; ++i)
			setPerformanceValue(i, kPerformanceParameters[(std::size_t)i].defaultValue, false);
	}

	// JUCE's standalone wrapper automatically reloads its last state blob. Keep the
	// controller preferences decoded above, but never turn that automatic reload into
	// an implicit edit-buffer restore. This also accepts old standalone blobs safely:
	// their trailing program dump is ignored on the first launch after this policy change.
	if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		m_pendingReady.store(false);
		m_pendingInjected.store(false);
		m_pendingState.clear();
		return;
	}

	if (dumpLen > 0)
	{
		m_pendingState.assign(dump, dump + dumpLen);
		m_pendingInjected.store(false);
		m_pendingReady.store(true); // release: m_pendingState fully written before processBlock reads it
	}
}

void ProphecyAudioProcessor::selectPatch(int program)
{
	if (program < 0 || program > 127) return;
	publishCurrentProgram(program, true);
	m_editorPatchIntents.fetch_add(1, std::memory_order_relaxed);
	// A patch change discards the current edit buffer, so cancel work belonging to the old
	// buffer and any obsolete read-back. Program loading costs roughly half a second inside
	// the firmware; forwarding every arrow click can therefore create far more work than a
	// user can consume. Always debounce to the latest requested program. This also gives a
	// possibly in-flight rename/macro SysEx time to finish before the bank/program message.
	// The production WebView and headless editor-stress host both use this exact method.
	(void)m_renameBurst.cancel();
	(void)m_macroBurst.cancel();
	m_programDumpSync.cancel();
	m_editorCommandPacer.holdForPatchLoad(2500);
	// Start the editor-command barrier at intent time. An accepted send refreshes
	// it to cover the complete firmware load; host and editor-play MIDI still pass.
	holdEditorCommandsForPatchLoad(2.5);
	const auto epoch = m_stateRestoreEpoch.load(std::memory_order_acquire);
	m_pendingProgram.store((epoch << 9) | kEditorProgramBit | (std::uint64_t)program,
		std::memory_order_release);
}

void ProphecyAudioProcessor::holdEditorCommandsForPatchLoad(double seconds)
{
	const std::uint64_t current = m_audioHostFrames.load(std::memory_order_relaxed);
	const std::uint64_t wanted = current
		+ (std::uint64_t)std::ceil(std::max(seconds, 0.0) * m_hostSampleRate);
	std::uint64_t previous = m_patchLoadBarrierUntilFrame.load(std::memory_order_relaxed);
	while (previous < wanted && !m_patchLoadBarrierUntilFrame.compare_exchange_weak(
		previous, wanted, std::memory_order_release, std::memory_order_relaxed)) {}
}

void ProphecyAudioProcessor::sendMidi(const std::uint8_t *bytes, std::size_t size)
{
	(void)pushImmediateMidi(bytes, size);
}

juce::StringArray ProphecyAudioProcessor::patchNames() const
{
	juce::StringArray out;
	if (m_nvramPath.isEmpty())
		return out;
	const juce::File f = juce::File(m_nvramPath).getChildFile("korgprop").getChildFile("sysram");
	juce::MemoryBlock mb;
	if (!f.existsAsFile() || !f.loadFileAsData(mb))
		return out;
	// Firmware battery-backed-RAM layout (v1.7, verified against the live bank): 128
	// contiguous 535-byte program records (A00..B63) at 0x20A10; a record starts with
	// its 16-char name. The file mirrors the machine's boot-time RAM (MAME rewrites it
	// on exit), so names are as fresh as the last session — fine for a browser.
	constexpr std::size_t base = 0x20A10, rec = 535, nameLen = 16, count = 128;
	if (mb.getSize() < base + count * rec)
		return out;
	const auto *d = static_cast<const unsigned char *>(mb.getData());
	int clean = 0;
	for (std::size_t i = 0; i < count; ++i)
	{
		const unsigned char *p = d + base + i * rec;
		juce::String name;
		bool printable = true;
		for (std::size_t j = 0; j < nameLen; ++j)
		{
			const unsigned char c = p[j];
			if (c >= 32 && c < 127) name += (juce::juce_wchar) c;
			else { name += ' '; if (c != 0) printable = false; }
		}
		if (printable) ++clean;
		out.add(name.trimEnd());
	}
	// If the region doesn't look like a patch bank (fresh/blank sysram, other firmware
	// layout), return nothing rather than 128 rows of garbage.
	if (clean < 100)
		out.clear();
	return out;
}

std::uint64_t ProphecyAudioProcessor::requestProgramDump()
{
	m_editorDumpRequests.fetch_add(1, std::memory_order_relaxed);
	// Program Change and current-program dump assembly share the firmware MIDI task. A dump
	// sent during the patch-load transaction is silently discarded, so wait out any pending/recent
	// selection and let ProgramDumpSync retry the one in-flight editor transaction if needed.
	int delayMs = 0;
	if (m_pendingProgram.load(std::memory_order_acquire) != kNoPendingProgram
			|| m_deferredProgramPending.load(std::memory_order_acquire))
		delayMs = 800; // debounce checkpoint; the shared pacer remains held through the load
	else
	{
		const auto now = m_audioHostFrames.load(std::memory_order_acquire);
		const auto barrier = m_patchLoadBarrierUntilFrame.load(std::memory_order_acquire);
		if (barrier > now && m_hostSampleRate > 0.0)
			delayMs = std::max(0, (int)std::ceil((double)(barrier - now)
				* 1000.0 / m_hostSampleRate));
	}
	return m_programDumpSync.request(delayMs);
}

std::size_t ProphecyAudioProcessor::getProgramData(std::uint8_t *out, std::size_t cap,
	std::uint32_t *version, std::uint64_t *completedRequestGeneration) const
{
	std::uint32_t observedVersion = 0;
	const std::size_t bytes = m_engine.latestProgramData(out, cap, &observedVersion);
	if (version != nullptr) *version = observedVersion;
	const std::uint64_t completed = m_programDumpSync.observe(observedVersion);
	if (completedRequestGeneration != nullptr) *completedRequestGeneration = completed;
	return bytes;
}

bool ProphecyAudioProcessor::sendProgramDumpNow()
{
	// Korg current-program dump request. The 0x40 reply is captured + unpacked by the engine;
	// the editor polls getProgramData() for it. F0 42 30 41 10 00 F7.
	const std::uint8_t req[7] = {0xF0, 0x42, 0x30, 0x41, 0x10, 0x00, 0xF7};
	const bool accepted = pushImmediateMidi(req, sizeof(req));
	if (accepted) m_editorDumpSends.fetch_add(1, std::memory_order_relaxed);
	return accepted;
}

void ProphecyAudioProcessor::selectArpeggioPattern(int pattern)
{
	if (pattern < 0 || pattern > 9) return;
	// NRPN MSB=0, LSB=1 (Arpeggio Pattern Select), Data Entry MSB=INT pattern 0..9.
	const std::uint8_t msg[9] = {0xB0, 0x63, 0x00, 0xB0, 0x62, 0x01,
		0xB0, 0x06, (std::uint8_t) pattern};
	m_editorCommandPacer.enqueueMidi(0x20000, msg, sizeof(msg));
}

void ProphecyAudioProcessor::setArpeggiatorControl(int control, int value)
{
	// Documented NRPNs: 2=On/Off, 3=Octaves, 4=Latch, 5=Key Sync.
	if (control < 2 || control > 5) return;
	value = std::clamp(value, 0, control == 3 ? 3 : 127);
	const std::uint8_t msg[9] = {0xB0, 0x63, 0x00, 0xB0, 0x62, (std::uint8_t) control,
		0xB0, 0x06, (std::uint8_t) value};
	m_editorCommandPacer.enqueueMidi(0x20010 + control, msg, sizeof(msg));
}

void ProphecyAudioProcessor::requestArpeggioPatternDump(int pattern)
{
	if (pattern < 0 || pattern > 9) return;
	const std::uint8_t req[8] = {0xF0, 0x42, 0x30, 0x41, 0x34,
		(std::uint8_t) pattern, 0x00, 0xF7};
	// Only the newest read-back matters to the editor. Coalesce across pattern
	// numbers as well as duplicate clicks so an older queued request cannot hold
	// up the pattern currently visible in the UI.
	m_editorCommandPacer.enqueueMidi(0x20020, req, sizeof(req));
}

void ProphecyAudioProcessor::sendArpeggioPatternData(int pattern, const std::vector<std::uint8_t> &raw)
{
	if (pattern < 0 || pattern > 9 || raw.size() != 128) return;
	// Korg 7-in-8 packing: a high-bit bitmap followed by up to seven low-7-bit bytes.
	std::vector<std::uint8_t> msg;
	msg.reserve(7 + 147 + 1);
	msg.insert(msg.end(), {0xF0, 0x42, 0x30, 0x41, 0x69, (std::uint8_t) pattern, 0x00});
	for (std::size_t pos = 0; pos < raw.size(); pos += 7)
	{
		const std::size_t count = std::min<std::size_t>(7, raw.size() - pos);
		std::uint8_t high = 0;
		for (std::size_t i = 0; i < count; ++i)
			high |= ((raw[pos + i] >> 7) & 1) << i;
		msg.push_back(high);
		for (std::size_t i = 0; i < count; ++i)
			msg.push_back(raw[pos + i] & 0x7f);
	}
	msg.push_back(0xF7);
	m_editorCommandPacer.enqueueMidi(0x20040 + pattern, msg.data(), msg.size());
}

void ProphecyAudioProcessor::setParam(int paramId, int value)
{
	setParamG(1, paramId, value); // program group
}

void ProphecyAudioProcessor::setParamG(int group, int paramId, int value)
{
	// Korg PARAMETER_CHANGE (0x41). 14-bit param id + 14-bit value (two's complement
	// for bipolar params). F0 42 30 41 41 <group> pLSB pMSB vLSB vMSB F7.
	const int p = paramId & 0x3FFF;
	const int v = value   & 0x3FFF;
	const std::uint8_t msg[11] = {
		0xF0, 0x42, 0x30, 0x41, 0x41, (std::uint8_t)(group & 0x7F),
		(std::uint8_t)(p & 0x7F), (std::uint8_t)((p >> 7) & 0x7F),
		(std::uint8_t)(v & 0x7F), (std::uint8_t)((v >> 7) & 0x7F),
		0xF7 };
	m_editorCommandPacer.enqueueMidi(0x10000 + ((group & 0x7f) << 14) + p,
		msg, sizeof(msg));
}

void ProphecyAudioProcessor::panelPulse(int row, int bit)
{
	if (row < 0 || bit < 0) return;
	m_editorCommandPacer.enqueuePanel(row, bit);
}

void ProphecyAudioProcessor::renamePatch(const juce::String &name)
{
	// Program Name Char 1..16 = group-1 params 1..16 (ascii_char, manifest-verified).
	// Paced (one param per timer tick): 16 sysexes sent back-to-back at line rate get
	// partially dropped by the firmware when the edit buffer holds a busy patch.
	const juce::String padded = name.paddedRight(' ', 16).substring(0, 16);
	std::vector<std::pair<int, int>> burst;
	for (int i = 0; i < 16; i++)
	{
		int c = (int) (juce::juce_wchar) padded[i];
		if (c < 32 || c > 126) c = ' ';
		burst.emplace_back(1 + i, c);
	}
	// Name characters are unusually easy to lose while a motion-heavy patch is running.
	// 100 ms remains quick enough for a 16-character name edit, while giving the busy
	// firmware a full hardware-scale interval to consume each parameter message.
	m_renameBurst.start(std::move(burst), 100);
}

// Quick-init patch-shaping macros. Recipes ported EXACTLY from the MAME-tree GUI
// (src/tools/korgprophecy_gui/korgprophecy_gui.mm sendPatchMacro + kInit/kSaw/
// kFilterThru/kBypassFx) and scripts/korgprophecy_macros.py (dump-verified 2026-06-29).
// All params are program group 1. OSC1-specific params are ExID-packed as (1<<12)|param;
// setParamG splits the 14-bit id, so passing 4096|388 emits the correct ExID addressing.
void ProphecyAudioProcessor::sendMacro(const juce::String &name)
{
	constexpr int E1 = 1 << 12; // OSC1 ExID: paramId = (1<<12)|param

	// {paramId, value} pairs (paramId already ExID-packed where needed).
	using PV = std::pair<int, int>;
	static const std::vector<PV> kSaw = {
		{E1 | 388, 0}, {E1 | 389, 0}, {E1 | 390, 99},
		{E1 | 391, 0}, {E1 | 392, 0}, {E1 | 394, 0}, {E1 | 396, 0},
	};
	static const std::vector<PV> kFilterThru = { {269, 0}, {285, 0} };
	static const std::vector<PV> kBypassFx = {
		{342, 0}, {351, 0}, {361, 0}, {367, 0}, {373, 0}, {378, 0}, {381, 0},
	};
	static const std::vector<PV> kInit = {
		{E1 | 388, 0}, {E1 | 389, 0}, {E1 | 390, 99}, {E1 | 391, 0},
		{E1 | 392, 0}, {E1 | 394, 0}, {E1 | 396, 0},
		{177, 0}, {178, 0}, {185, 0}, {186, 0}, {187, 0},
		// OSC1 -> Output1 only; mute BOTH buses (OSC2 on OUT2 cancels even harmonics -> square).
		{238, 99}, {241, 0}, {244, 0}, {247, 0}, {250, 0}, {253, 0},
		{256, 0}, {259, 0}, {262, 0}, {265, 0},
		{222, 0}, {216, 0}, {217, 0}, {236, 0}, {230, 0}, {231, 0},
		{269, 0}, {285, 0},
		{301, 99}, {310, 99}, {309, 0}, {318, 0},
		{319, 99}, {320, 0}, {321, 99}, {322, 0}, {323, 99}, {324, 0}, {325, 99}, {326, 30},
		{342, 0}, {351, 0}, {361, 0}, {367, 0}, {373, 0}, {378, 0}, {381, 0},
	};

	const std::vector<PV> *recipe = nullptr;
	if      (name == "init")        recipe = &kInit;
	else if (name == "saw")         recipe = &kSaw;
	else if (name == "filter_thru") recipe = &kFilterThru;
	else if (name == "bypass_fx")   recipe = &kBypassFx;
	if (recipe == nullptr)
		return;

	std::vector<PV> burst = *recipe;
	// Re-assert OSC Set (p154) LAST if the recipe reconfigures the OSC engine: a std-osc
	// write right after a reconfig gets clobbered by the completing reconfig (GUI kSwitchOsc).
	for (const PV &pv : *recipe)
		if (pv.first == 154) { burst.emplace_back(154, pv.second); break; }

	m_macroBurst.start(std::move(burst));
}

void ProphecyAudioProcessor::setCcMap(int cc, int target)
{
	if (cc < 0 || cc > 127) return;
	if (target < 0 || target > (int) CcTarget::Wheel2) target = 0;
	const auto old = (CcTarget) m_ccMap[(std::size_t) cc].exchange((std::uint8_t) target, std::memory_order_relaxed);
	// Un-mapping (or re-targeting) a pad axis while its CC is held would strand the
	// touch gate: the release CC no longer routes to the old axis, so the held flag
	// would never clear and ADIN14 stays 0xFF. Release the axis eagerly instead.
	const auto newT = (CcTarget) target;
	if (old == CcTarget::PadX && newT != CcTarget::PadX && m_padXHeld.exchange(false))
	{
		(void) pushUiAdin(12, 0x80);
		publishControllerDisplayValue(12, 0x80);
		if (!m_padYHeld)
		{
			(void) pushUiAdin(14, 0x00);
			publishControllerDisplayValue(14, 0x00);
		}
	}
	if (old == CcTarget::PadY && newT != CcTarget::PadY && m_padYHeld.exchange(false))
	{
		(void) pushUiAdin(13, 0x74);
		publishControllerDisplayValue(13, 0x74);
		if (!m_padXHeld)
		{
			(void) pushUiAdin(14, 0x00);
			publishControllerDisplayValue(14, 0x00);
		}
	}
}

int ProphecyAudioProcessor::ccMapTarget(int cc) const
{
	if (cc < 0 || cc > 127) return 0;
	return (int) m_ccMap[(std::size_t) cc].load(std::memory_order_relaxed);
}

// WHEEL2 = ADIN9. Store the chosen rest as the host parameter and schedule its ADIN write
// at the next audio-block boundary. maybeBootEngine also publishes the retained value once
// this processor owns the MAME slot, so state restore remains safe before boot. Last write
// wins versus a live CC->Wheel2 remap, which is expected.
void ProphecyAudioProcessor::setWheel2(int value)
{
	setPerformanceValue(performanceControlForAdin(9), value, false);
}

void ProphecyAudioProcessor::setWheel2FromEditor(int value)
{
	setAdin(9, value);
}

void ProphecyAudioProcessor::setAdin(int source, int value)
{
	if (source < 0 || source > 15) return;
	value = std::clamp(value, 0, 255);
	const int controlIndex = performanceControlForAdin(source);
	if (controlIndex >= 0)
	{
		setPerformanceValue(controlIndex, value, true);
		return;
	}
	publishControllerDisplayValue(source, value);
	(void) pushUiAdin(source, value);
}

void ProphecyAudioProcessor::beginAdinGesture(int source)
{
	const int controlIndex = performanceControlForAdin(source);
	if (controlIndex >= 0)
		m_performanceParameters[(std::size_t)controlIndex]->beginChangeGesture();
}

void ProphecyAudioProcessor::endAdinGesture(int source)
{
	const int controlIndex = performanceControlForAdin(source);
	if (controlIndex >= 0)
		m_performanceParameters[(std::size_t)controlIndex]->endChangeGesture();
}

void ProphecyAudioProcessor::publishControllerDisplayValue(int source, int value)
{
	if (source < 0 || source >= (int)m_controllerDisplayValues.size()) return;
	const auto next = (std::uint8_t)std::clamp(value, 0, 255);
	m_controllerDisplayValues[(std::size_t)source].store(next, std::memory_order_relaxed);
}

void ProphecyAudioProcessor::controllerDisplaySnapshot(std::uint8_t out[16]) const
{
	for (std::size_t i = 0; i < m_controllerDisplayValues.size(); ++i)
		out[i] = m_controllerDisplayValues[i].load(std::memory_order_relaxed);
}

// Translate a mapped control-change to a front-panel ADIN write. The editor's software X-Y
// surface combines physical ribbon X (ADIN12, finger-up 0x80) with the separate sprung
// Log/Wheel 3 controller (ADIN13, rest 0x74); ADIN14 is the ribbon pressure/touch gate.
// Ribbon Z = ADIN14, wheel 1 = ADIN8, and wheel 2 = ADIN9. CC 0..127 scales to the physical
// control direction; ribbon X uses the service-manual range and reversed ADC polarity
// (left=0x7F, right=0x00). Audio-thread only.
void ProphecyAudioProcessor::handleMappedCc(int cc, int value, CcTarget target, std::uint64_t frame)
{
	juce::ignoreUnused(cc);
	const int s = (value * 255 + 63) / 127; // 0..127 -> 0..255 (rounded), matches editor
	const auto push = [this, frame](int source, int next)
	{
		(void) m_engine.pushAdinAtFrame(source, next, frame);
		publishControllerDisplayValue(source, next);
	};
	switch (target)
	{
	case CcTarget::PadX:
		if (value > 0)
		{
			// CC zero releases the virtual touch, so map the remaining 1..127
			// across the ribbon's complete 0x7f..0x00 active range.
			const int ribbonX = ((127 - value) * 127 + 63) / 126;
			push(12, ribbonX);
			m_padXHeld = true;
			push(14, 0xFF);
		}
		else
		{
			push(12, 0x80);
			m_padXHeld = false;
			if (!m_padYHeld) push(14, 0x00);
		}
		break;
	case CcTarget::PadY:
		if (value > 0)
		{
			push(13, s);
			m_padYHeld = true;
			push(14, 0xFF);
		}
		else
		{
			push(13, 0x74);
			m_padYHeld = false;
			if (!m_padXHeld) push(14, 0x00);
		}
		break;
	case CcTarget::RibbonZ: push(14, s); break; // Z pressure / touch gate directly
	case CcTarget::Wheel1:  push(8,  s); break;
	case CcTarget::Wheel2:  push(9,  s); break;
	case CcTarget::Off:     break; // unreachable (filtered in processBlock)
	}
}

void ProphecyAudioProcessor::writePatch()
{
	m_writeInProgress.store(true, std::memory_order_release);
	m_writeSeq.start();
}

void ProphecyAudioProcessor::WriteSequence::timerCallback()
{
	switch (m_step++)
	{
	case 0: m_proc.setParamG(0, 170, 0); break; // Program Memory Protect = off
	case 1: m_proc.panelPulse(0, 0);     break; // WRITE -> choose destination
	case 2: m_proc.panelPulse(1, 6);     break; // ENTER -> "Are you sure?"
	case 3: m_proc.panelPulse(1, 6);     break; // ENTER -> commit
	case 4: m_proc.setParamG(0, 170, 1); break; // restore protect (hardware default)
	default:
		stopTimer();
		m_proc.requestProgramDump(); // refresh the editor's view of the stored program
		m_proc.m_writeInProgress.store(false, std::memory_order_release);
		break;
	}
}

//============================================================
//  WebView editor: an HTML/CSS/JS panel (embedded via BinaryData). The JS calls the
//  registered native function "selectPatch"; C++ -> JS events come later for LCD/params.
//============================================================
class ProphecyEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
	explicit ProphecyEditor(ProphecyAudioProcessor &p) : AudioProcessorEditor(p), m_proc(p)
	{
		if (std::getenv("PROFLIGACY_EDITOR_SMOKE") != nullptr)
			std::fprintf(stderr, "[editor-smoke] editor created; waiting for embedded page bridge\n");
		if (std::getenv("PROFLIGACY_DIAGNOSTICS") != nullptr)
		{
			m_diag.reset(juce::FileLogger::createDateStampedLogger(
				"Profligacy", "diagnostic-", ".log",
				"Profligacy GUI diagnostic session; control values and LCD text may be recorded"));
			if (m_diag != nullptr)
			{
				m_diagStartMs = juce::Time::getMillisecondCounterHiRes();
				const auto path = m_diag->getLogFile().getFullPathName();
				std::fprintf(stderr, "[diagnostic] log=%s\n", path.toRawUTF8());
				diag("EDITOR open rom=" + m_proc.romPath());
				startTimer(1000);
			}
		}
		// Faceplate aspect: the panel is authored in a 1480x720 design space.
		setResizable(true, true);
		// Keep only a useful minimum.  The previous 1440 px ceiling made tall
		// editor layouts stop resizing for no UI or engine reason; the host/window
		// manager can impose its own practical screen bounds.
		setResizeLimits(740, 360, 16384, 16384);
		setSize(1184, 576);
		addAndMakeVisible(m_web);
	#if JUCE_WEB_BROWSER_RESOURCE_PROVIDER_AVAILABLE
		m_web.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
	#endif
	}
	~ProphecyEditor() override
	{
		stopTimer();
		diag("EDITOR close");
	}
	void resized() override { m_web.setBounds(getLocalBounds()); }

private:
	#if JUCE_WINDOWS
	static juce::File webView2UserDataFolder()
	{
		const auto host = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
			.getFileNameWithoutExtension();
		return juce::File::getSpecialLocation(juce::File::tempDirectory)
			.getChildFile("Profligacy-WebView2").getChildFile(host);
	}
	#endif

	void diag(const juce::String &message)
	{
		if (m_diag == nullptr) return;
		const double elapsed = (juce::Time::getMillisecondCounterHiRes() - m_diagStartMs) / 1000.0;
		m_diag->logMessage("t=" + juce::String(elapsed, 3) + " " + message);
	}

	void timerCallback() override
	{
		const auto s = m_proc.diagnosticSnapshot();
		std::uint8_t r1[40] {}, r2[40] {}, cg[64] {}, banks[12] {};
		const std::uint32_t lcdVersion = m_proc.lcdRawSnapshot(r1, r2, cg);
		const std::uint32_t ledVersion = m_proc.ledSnapshot(banks);
		std::uint8_t dump[1024] {};
		std::uint32_t dumpVersion = 0;
		const std::size_t dumpBytes = m_proc.getProgramData(dump, sizeof(dump), &dumpVersion);
		char line1[64] {}, line2[64] {};
		const bool haveLcd = m_proc.getLcd(line1, line2, sizeof(line1));
		const auto hex64 = [](std::uint64_t v) { return juce::String::toHexString((juce::int64) v).paddedLeft('0', 16); };
		const auto hex8 = [](std::uint32_t v) { return juce::String::toHexString((int) (v & 0xff)).paddedLeft('0', 2); };
		const std::uint32_t midi = s.lastHostMidi;
		juce::String message;
		message << "HEALTH engine=" << (s.engineRunning ? 1 : 0)
			<< " produced=" << (juce::int64) s.producedFrames
			<< " buffered=" << (juce::int64) s.bufferedFrames
			<< " audio_cb=" << (juce::int64) s.audioCallbacks
			<< " host_frames=" << (juce::int64) s.audioHostFrames
			<< " engine_pulled=" << (juce::int64) s.audioEngineFrames
			<< " underrun_frames=" << (juce::int64) s.audioUnderrunFrames
			<< " midi_events=" << (juce::int64) s.hostMidiEvents
			<< " last_midi=" << hex8(midi) << ":" << hex8(midi >> 8) << ":" << hex8(midi >> 16)
			<< "/" << (int) ((midi >> 24) & 0xff)
			<< " program=" << s.currentProgram
			<< " host_program=" << (juce::int64)s.hostProgramIntents
			<< "/" << (juce::int64)s.hostProgramSends
			<< " active_notes=" << hex64(s.activeNotesHigh) << hex64(s.activeNotesLow)
			<< " drops=imm:" << (juce::int64) s.droppedImmediateMidiBytes
			<< ",sched:" << (juce::int64) s.droppedScheduledMidiBytes
			<< ",ui_adin:" << (juce::int64) s.droppedUiAdinEvents
			<< ",audio_adin:" << (juce::int64) s.droppedAudioAdinEvents
			<< " oversized=" << (juce::int64) s.oversizedBlocks
			<< " lcd_ver=" << (juce::int64) lcdVersion << " led_ver=" << (juce::int64) ledVersion
			<< " dump_ver=" << (juce::int64) dumpVersion << " dump_bytes=" << (juce::int64) dumpBytes;
		if (haveLcd)
			message << " lcd1=\"" << juce::String::fromUTF8(line1).trimEnd() << "\""
				<< " lcd2=\"" << juce::String::fromUTF8(line2).trimEnd() << "\"";
		diag(message);
	}

	static std::optional<juce::WebBrowserComponent::Resource> provide(const juce::String &url)
	{
		const bool smoke = std::getenv("PROFLIGACY_EDITOR_SMOKE") != nullptr;
		if (smoke)
			std::fprintf(stderr, "[editor-smoke] resource request: %s\n", url.toRawUTF8());
		if (smoke && url == "/")
		{
			static constexpr char smokeHtml[] = R"html(<!doctype html><html><body><script>
window.addEventListener('load', () => window.__JUCE__.backend.emitEvent('profligacyEditorReadyV1', {
  token: 'profligacy-editor-v1', rowCount: 1425, canvasPixels: 1
}));
</script></body></html>)html";
			const auto *data = reinterpret_cast<const std::byte *>(smokeHtml);
			return juce::WebBrowserComponent::Resource{
				std::vector<std::byte>(data, data + sizeof(smokeHtml) - 1), juce::String("text/html") };
		}
		const auto path = url.upToFirstOccurrenceOf("?", false, false);
		if (path == "/" || path == "/index.html")
		{
			const auto *d = reinterpret_cast<const std::byte *>(BinaryData::index_html);
			return juce::WebBrowserComponent::Resource{
				std::vector<std::byte>(d, d + (size_t) BinaryData::index_htmlSize), juce::String("text/html") };
		}
		if (path == "/assets/NotoSans-Bold.ttf")
		{
			const auto *d = reinterpret_cast<const std::byte *>(BinaryData::NotoSansBold_ttf);
			return juce::WebBrowserComponent::Resource{
				std::vector<std::byte>(d, d + (size_t) BinaryData::NotoSansBold_ttfSize), juce::String("font/ttf") };
		}
		if (path == "/assets/hd44780-a00-glyphs.bin")
		{
			std::vector<std::byte> rows(ProphecyEngine::kLcdA00GlyphRowBytes);
			ProphecyEngine::lcdA00GlyphRows(reinterpret_cast<std::uint8_t *>(rows.data()));
			return juce::WebBrowserComponent::Resource{
				std::move(rows), juce::String("application/octet-stream") };
		}
		if (path == "/assets/deep_editor_manifest.js")
		{
			const auto *d = reinterpret_cast<const std::byte *>(BinaryData::deep_editor_manifest_js);
			return juce::WebBrowserComponent::Resource{
				std::vector<std::byte>(d, d + (size_t) BinaryData::deep_editor_manifest_jsSize),
				juce::String("text/javascript") };
		}
		return std::nullopt;
	}

	ProphecyAudioProcessor   &m_proc;
	std::unique_ptr<juce::FileLogger> m_diag;
	double m_diagStartMs = 0.0;
	juce::WebBrowserComponent m_web {
		juce::WebBrowserComponent::Options{}
	#if JUCE_WINDOWS
			.withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
			.withWinWebView2Options(
				juce::WebBrowserComponent::Options::WinWebView2{}
					.withUserDataFolder(webView2UserDataFolder()))
	#endif
			.withNativeIntegrationEnabled()
	#if JUCE_WEB_BROWSER_RESOURCE_PROVIDER_AVAILABLE
			.withResourceProvider([](const auto &url) { return provide(url); })
	#endif
			.withEventListener("profligacyEditorReadyV1",
				[this](const juce::var &payload)
				{
					const auto *object = payload.getDynamicObject();
					const bool ready = object != nullptr
						&& object->getProperty("token").toString() == "profligacy-editor-v1"
						&& (int)object->getProperty("rowCount") == 1425
						&& (int)object->getProperty("canvasPixels") > 0;
					if (std::getenv("PROFLIGACY_EDITOR_SMOKE") != nullptr)
						std::fprintf(stderr, "[editor-smoke] bridge callback ready=%d\n", (int)ready);
					diag("UI editorReady ready=" + juce::String((int)ready));
					if (ready)
						if (const char *path = std::getenv("PROFLIGACY_EDITOR_SMOKE_RECEIPT"))
							(void)juce::File(juce::String::fromUTF8(path))
								.replaceWithText("profligacy-editor-v1\n");
				})
			.withNativeFunction("selectPatch",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (! args.isEmpty()) { diag("UI selectPatch program=" + juce::String((int) args[0])); m_proc.selectPatch((int) args[0]); }
					complete(juce::var{});
				})
			.withNativeFunction("getCurrentPatch",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					auto *obj = new juce::DynamicObject();
					obj->setProperty("program", m_proc.getCurrentProgram());
					obj->setProperty("version", (juce::int64)m_proc.currentProgramVersion());
					complete(juce::var(obj));
				})
			.withNativeFunction("setParam",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (args.size() >= 2)
					{
						const int param = (int) args[0], value = (int) args[1];
						juce::String label;
						switch (param & 0x3fff)
						{
						case 105: label = " LFO1_FREQUENCY"; break;
						case 118: label = " LFO2_FREQUENCY"; break;
						case 131: label = " LFO3_FREQUENCY"; break;
						case 144: label = " LFO4_FREQUENCY"; break;
						default: break;
						}
						diag("UI setParam param=" + juce::String(param) + " value=" + juce::String(value) + label);
						m_proc.setParam(param, value);
					}
					complete(juce::var{});
				})
			.withNativeFunction("diagnosticEvent",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::String message = "UI event";
					for (const auto &arg : args) message << " " << arg.toString();
					diag(message);
					complete(juce::var{});
				})
			.withNativeFunction("setPatternParam",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (args.size() >= 2) m_proc.setPatternParam((int) args[0], (int) args[1]);
					complete(juce::var{});
				})
			.withNativeFunction("selectArpPattern",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (!args.isEmpty()) m_proc.selectArpeggioPattern((int) args[0]);
					complete(juce::var{});
				})
			.withNativeFunction("setArpControl",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (args.size() >= 2) m_proc.setArpeggiatorControl((int) args[0], (int) args[1]);
					complete(juce::var{});
				})
			.withNativeFunction("getLcd",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					char l1[64] = {0}, l2[64] = {0};
					auto *obj = new juce::DynamicObject();
					if (m_proc.getLcd(l1, l2, sizeof(l1)))
					{
						obj->setProperty("line1", juce::String::fromUTF8(l1));
						obj->setProperty("line2", juce::String::fromUTF8(l2));
					}
					else
					{
						obj->setProperty("line1", juce::var()); // -> null in JS: keep placeholder
						obj->setProperty("line2", juce::var());
					}
					complete(juce::var(obj)); // var(ReferenceCountedObject*) ctor takes ownership
				})
			.withNativeFunction("requestDump",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					const auto generation = m_proc.requestProgramDump();
					complete(juce::var((juce::int64)generation));
				})
			.withNativeFunction("getProgramData",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					std::uint8_t buf[1024];
					std::uint32_t ver = 0;
					std::uint64_t requestGeneration = 0;
					const std::size_t n = m_proc.getProgramData(
						buf, sizeof(buf), &ver, &requestGeneration);
					auto *obj = new juce::DynamicObject();
					obj->setProperty("version", (int) ver);
					obj->setProperty("requestGeneration", (juce::int64)requestGeneration);
					juce::Array<juce::var> bytes;
					bytes.ensureStorageAllocated((int) n);
					for (std::size_t i = 0; i < n; ++i) bytes.add((int) buf[i]);
					obj->setProperty("bytes", std::move(bytes)); // empty until a dump is captured
					complete(juce::var(obj));
				})
			.withNativeFunction("requestArpPatternDump",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (!args.isEmpty()) m_proc.requestArpeggioPatternDump((int) args[0]);
					complete(juce::var{});
				})
			.withNativeFunction("getArpPatternData",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					std::uint8_t buf[1280];
					std::uint32_t ver = 0;
					int pattern = -1;
					const std::size_t n = m_proc.getArpeggioPatternData(buf, sizeof(buf), &ver, &pattern);
					auto *obj = new juce::DynamicObject();
					obj->setProperty("version", (int) ver);
					obj->setProperty("pattern", pattern);
					juce::Array<juce::var> bytes;
					bytes.ensureStorageAllocated((int) n);
					for (std::size_t i = 0; i < n; ++i) bytes.add((int) buf[i]);
					obj->setProperty("bytes", std::move(bytes));
					complete(juce::var(obj));
				})
			.withNativeFunction("sendArpPatternData",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (args.size() >= 2 && args[1].isArray())
					{
						std::vector<std::uint8_t> raw;
						raw.reserve((std::size_t) args[1].getArray()->size());
						for (const juce::var &b : *args[1].getArray())
							raw.push_back((std::uint8_t) ((int) b & 0xff));
						m_proc.sendArpeggioPatternData((int) args[0], raw);
					}
					complete(juce::var{});
				})
			.withNativeFunction("getPatchNames",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					juce::Array<juce::var> names;
					for (const juce::String &n : m_proc.patchNames())
						names.add(n);
					complete(juce::var(std::move(names))); // empty until nvram is readable
				})
			.withNativeFunction("panelPulse",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (args.size() >= 2) { diag("UI panelPulse row=" + juce::String((int) args[0]) + " bit=" + juce::String((int) args[1])); m_proc.panelPulse((int) args[0], (int) args[1]); }
					complete(juce::var{});
				})
			.withNativeFunction("setAdin",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (args.size() >= 2) { diag("UI setAdin source=" + juce::String((int) args[0]) + " value=" + juce::String((int) args[1])); m_proc.setAdin((int) args[0], (int) args[1]); }
					complete(juce::var{});
				})
			.withNativeFunction("beginAdinGesture",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (!args.isEmpty()) m_proc.beginAdinGesture((int)args[0]);
					complete(juce::var{});
				})
			.withNativeFunction("endAdinGesture",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (!args.isEmpty()) m_proc.endAdinGesture((int)args[0]);
					complete(juce::var{});
				})
			.withNativeFunction("getLeds",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					std::uint8_t banks[12];
					const std::uint32_t ver = m_proc.ledVisualSnapshot(banks);
					auto *obj = new juce::DynamicObject();
					obj->setProperty("version", (int) ver);
					juce::Array<juce::var> arr;
					for (int i = 0; i < 12; i++) arr.add((int) banks[i]);
					obj->setProperty("banks", std::move(arr));
					complete(juce::var(obj));
				})
			.withNativeFunction("getControllers",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					std::uint8_t values[16];
					m_proc.controllerDisplaySnapshot(values);
					auto *obj = new juce::DynamicObject();
					juce::Array<juce::var> arr;
					for (std::uint8_t value : values) arr.add((int)value);
					obj->setProperty("values", std::move(arr));
					complete(juce::var(obj));
				})
			.withNativeFunction("sendMidi",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					// args = array of byte values (e.g. [0x90, note, vel] from the faceplate keybed)
					std::uint8_t buf[64];
					std::size_t n = 0;
					if (!args.isEmpty() && args[0].isArray())
						for (const juce::var &b : *args[0].getArray())
						{
							if (n >= sizeof(buf)) break;
							buf[n++] = (std::uint8_t) ((int) b & 0xff);
						}
					if (n > 0)
					{
						juce::String message = "UI sendMidi";
						for (std::size_t i = 0; i < n; ++i) message << " " << juce::String::toHexString((int) buf[i]).paddedLeft('0', 2);
						diag(message);
						m_proc.sendMidi(buf, n);
					}
					complete(juce::var{});
				})
			.withNativeFunction("getLcdRaw",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					std::uint8_t r1[40], r2[40], cg[64];
					const std::uint32_t ver = m_proc.lcdRawSnapshot(r1, r2, cg);
					auto *obj = new juce::DynamicObject();
					obj->setProperty("version", (int) ver); // 0 = nothing drawn yet
					auto pack = [](const std::uint8_t *p, int n) {
						juce::Array<juce::var> a;
						a.ensureStorageAllocated(n);
						for (int i = 0; i < n; i++) a.add((int) p[i]);
						return juce::var(std::move(a));
					};
					obj->setProperty("row1", pack(r1, 40));
					obj->setProperty("row2", pack(r2, 40));
					obj->setProperty("cgram", pack(cg, 64));
					complete(juce::var(obj));
				})
			.withNativeFunction("getRomStatus",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					// A host may instantiate without calling prepareToPlay for a while;
					// booting from here makes the editor's first paint the deciding poll.
					m_proc.maybeBootEngine();
					auto *obj = new juce::DynamicObject();
					obj->setProperty("ok", m_proc.romOk());
					obj->setProperty("path", m_proc.romPath());
					obj->setProperty("instanceUnavailable", m_proc.instanceUnavailable());
					if (m_proc.instanceUnavailable())
						obj->setProperty("error",
							"Profligacy v1 supports one active instance per host process. "
							"Close the other instance, then reload this one.");
					// PROPHECY_EDITOR_SELFTEST=1 makes the page run its built-in smoke test
					// (real WKWebView + real bridge + real engine) and report via selfTestReport.
					obj->setProperty("selftest", std::getenv("PROPHECY_EDITOR_SELFTEST") != nullptr);
					complete(juce::var(obj));
				})
			.withNativeFunction("renamePatch",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (!args.isEmpty()) m_proc.renamePatch(args[0].toString());
					complete(juce::var{});
				})
			.withNativeFunction("sendMacro",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (!args.isEmpty()) m_proc.sendMacro(args[0].toString());
					complete(juce::var{});
				})
			.withNativeFunction("setCcMap",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (args.size() >= 2) m_proc.setCcMap((int) args[0], (int) args[1]);
					complete(juce::var{});
				})
			.withNativeFunction("getCcMap",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					// Return every mapped CC as {cc, target} so the editor can restore its UI.
					juce::Array<juce::var> arr;
					for (int cc = 0; cc < 128; ++cc)
					{
						const int t = m_proc.ccMapTarget(cc);
						if (t == 0) continue;
						auto *e = new juce::DynamicObject();
						e->setProperty("cc", cc);
						e->setProperty("target", t);
						arr.add(juce::var(e));
					}
					complete(juce::var(std::move(arr)));
				})
			.withNativeFunction("setWheel2",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (!args.isEmpty()) m_proc.setWheel2FromEditor((int) args[0]);
					complete(juce::var{});
				})
			.withNativeFunction("getWheel2",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					complete(juce::var(m_proc.wheel2Pos()));
				})
			.withNativeFunction("writePatch",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					m_proc.writePatch();
					complete(juce::var(m_proc.writeInProgress()));
				})
			.withNativeFunction("isWriteInProgress",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					complete(juce::var(m_proc.writeInProgress()));
				})
			.withNativeFunction("setGlobalParam",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					if (args.size() >= 2) { diag("UI setGlobalParam param=" + juce::String((int) args[0]) + " value=" + juce::String((int) args[1])); m_proc.setParamG(0, (int) args[0], (int) args[1]); }
					complete(juce::var{});
				})
			.withNativeFunction("selfTestReport",
				[](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					// Editor self-test results -> stderr (and optional file), for headless runs.
					const juce::String report = args.isEmpty() ? juce::String() : args[0].toString();
					std::fprintf(stderr, "[editor-selftest] %s\n", report.toRawUTF8());
					if (const char *out = std::getenv("PROPHECY_EDITOR_SELFTEST_OUT"))
						juce::File(juce::String::fromUTF8(out)).replaceWithText(report);
					complete(juce::var{});
				})
			.withNativeFunction("chooseRomFolder",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					m_chooser = std::make_unique<juce::FileChooser>(
						"Select the folder containing your compatible Korg firmware (korgprop/)",
						juce::File::getSpecialLocation(juce::File::userHomeDirectory));
					m_chooser->launchAsync(
						juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
						[this, complete](const juce::FileChooser &fc)
						{
							const juce::File dir = fc.getResult();
							auto *obj = new juce::DynamicObject();
							if (dir == juce::File()) // cancelled
								obj->setProperty("ok", false);
							else if (!m_proc.setRomDirFromUser(dir))
							{
								obj->setProperty("ok", false);
								obj->setProperty("error",
									"That folder doesn't hold a compatible firmware set "
									"(need korgprop/ic12_v17.bin + ic22_v17.bin, or korgprop.zip).");
							}
							else
							{
								obj->setProperty("ok", true);
								obj->setProperty("path", m_proc.romPath());
							}
							complete(juce::var(obj));
						});
				})
	};
	std::unique_ptr<juce::FileChooser> m_chooser;
};

juce::AudioProcessorEditor *ProphecyAudioProcessor::createEditor()
{
	return new ProphecyEditor(*this);
}

// The JUCE plugin entry point.
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
	return new ProphecyAudioProcessor();
}
