// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginProcessor.cpp - JUCE AudioProcessor that plays the MAME Korg Prophecy engine.
//
// Rung 3 proof: this is a real VST3/AU/Standalone that boots the emulated machine on
// the engine's worker thread and streams its 48 kHz stereo output through processBlock.
// It links the MAME static archives via the prophecy_engine static library.
//
// Host MIDI (notes/CC/bend/sysex) is injected into the emulated serial UART at block
// start (see processBlock), and a Lagrange resampler adapts the engine's native
// 48 kHz to the host rate. Known limitation: only ONE instance produces audio per
// process (the MAME machine is a singleton); a second instance is explicitly unavailable.
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

#if defined(_WIN32)
static int prophecy_setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && std::getenv(name) != nullptr) return 0;
	return _putenv_s(name, value);
}
#define setenv prophecy_setenv
#endif

#include "rom_locator.h"

namespace {
enum class FirmwareInquiry { None, Identity, Program };
FirmwareInquiry firmwareInquiry(const std::uint8_t* bytes, std::size_t size)
{
	if (size >= 6 && bytes[0] == 0xf0 && bytes[1] == 0x7e && bytes[3] == 6
		&& bytes[4] == 1 && bytes[5] == 0xf7) return FirmwareInquiry::Identity;
	if (size >= 7 && bytes[0] == 0xf0 && bytes[1] == 0x42 && bytes[3] == 0x41
		&& bytes[4] == 0x10 && bytes[5] == 0 && bytes[6] == 0xf7) return FirmwareInquiry::Program;
	return FirmwareInquiry::None;
}

bool firmwareControlMessage(const std::uint8_t* bytes, std::size_t size)
{
	if (size < 6 || bytes[0] != 0xf0 || bytes[1] != 0x42 || bytes[3] != 0x41) return false;
	return (bytes[4] == 0x41 && bytes[5] != 1) || bytes[4] == 0x51
		|| bytes[4] == 0x69 || bytes[4] == 0x4c || bytes[4] == 0x50 || bytes[4] == 0x11;
}

// Control-thread operations first prevent new callbacks from entering, then
// wait for an existing callback to finish. The audio thread only tries once.
struct CallbackAccess
{
	explicit CallbackAccess(std::atomic<bool>& access) : flag(access)
	{
		bool expected = false;
		acquired = flag.compare_exchange_strong(expected, true, std::memory_order_acquire);
	}
	~CallbackAccess() { if (acquired) flag.store(false, std::memory_order_release); }
	std::atomic<bool>& flag;
	bool acquired = false;
};

struct ProcessingPause
{
	ProcessingPause(std::atomic<bool>& pausedIn, std::atomic<bool>& accessIn)
		: paused(pausedIn), access(accessIn)
	{
		paused.store(true, std::memory_order_release);
		bool expected = false;
		while (!access.compare_exchange_weak(expected, true, std::memory_order_acquire))
		{
			expected = false;
			juce::Thread::yield();
		}
	}
	~ProcessingPause()
	{
		access.store(false, std::memory_order_release);
		paused.store(false, std::memory_order_release);
	}
	std::atomic<bool>& paused;
	std::atomic<bool>& access;
};
}

ProphecyAudioProcessor::ProphecyAudioProcessor()
	: AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
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
	m_controlThread = std::thread([this] { runProgramControl(); });
}

ProphecyAudioProcessor::~ProphecyAudioProcessor()
{
	{
		std::lock_guard lock(m_programState.mutex);
		m_controlStopping = true;
		cancelEditorWork();
	}
	m_controlWake.notify_one();
	if (m_controlThread.joinable()) m_controlThread.join();
	m_engine.stop();
}

void ProphecyAudioProcessor::runProgramControl()
{
	std::unique_lock lock(m_programState.mutex);
	const std::array<ProgramTimer*, 4> tasks{&m_writeSeq, &m_editorCommandPacer, &m_patchSelectDelay, &m_programDumpSync};
	while (!m_controlStopping)
	{
		const auto now = ProgramTimer::Clock::now();
		try
		{
			drainProgramInput();
			for (auto* task : tasks) task->service(now);
		}
		catch (...)
		{
			cancelEditorWork();
			m_programState.fail(prophecy::ProgramState::Error::Control);
		}
		auto next = ProgramTimer::Clock::time_point::max();
		for (const auto* task : tasks) next = std::min(next, task->due());
		// The audio producer does not signal an OS condition variable. Poll its
		// bounded inbox here; performance MIDI has already retained its timestamp.
		m_controlWake.wait_until(lock, std::min(next, now + std::chrono::milliseconds(10)));
	}
}

bool ProphecyAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
	const auto &out = layouts.getMainOutputChannelSet();
	return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void ProphecyAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
	int latency = 0;
	{
		std::lock_guard stateLock(m_programState.mutex);
		ProcessingPause pause(m_processingPaused, m_callbackAccess);
		m_hostSampleRate = std::isfinite(sampleRate) && sampleRate > 0.0
			? sampleRate : (double) ProphecyEngine::kSampleRate;
		const auto quantum = ProphecyEngine::kAudioQuantum;
		m_timelineHostFrame = 0;
		m_oversizedAudioBlocks.store(0, std::memory_order_relaxed);
		m_editorPatchIntents.store(0, std::memory_order_relaxed);
		m_editorPatchSends.store(0, std::memory_order_relaxed);
		m_editorDumpRequests.store(0, std::memory_order_relaxed);
		m_editorDumpSends.store(0, std::memory_order_relaxed);
		m_hostMidiEventsForwarded.store(0, std::memory_order_relaxed);
		m_patchLoadBarrierUntilFrame.store(0, std::memory_order_relaxed);
		// samplesPerBlock is only a host hint in JUCE. Reserve a generous fixed floor so
		// ordinary offline/host block-size changes stay allocation-free; a still-larger block
		// is explicitly silenced and counted in processBlock rather than resizing there.
		m_preparedMaxBlock = std::max(samplesPerBlock, 16384);

		// Prepare the 48 kHz -> host-rate resampler (bypassed when the host runs at 48 kHz).
		const double ratio = (double) ProphecyEngine::kSampleRate / m_hostSampleRate;
		const int    cap   = (int) std::ceil(m_preparedMaxBlock * ratio) + 64;
		m_rsIn[0].assign((size_t) cap, 0.0f);
		m_rsIn[1].assign((size_t) cap, 0.0f);

		// Reserve the requested output block, one quantum for grant alignment,
		// one quantum for worker execution, and interpolation lookahead. A short
		// preceding callback must not remove the worker execution allowance. Output
		// is delayed by this exact integer number of host samples; storage capacity
		// is unrelated.
		const auto nativeLead = std::max(std::ceil(std::max(samplesPerBlock, 1) * ratio),
			(double) quantum) + 2 * quantum + 2;
		latency = (int) std::ceil(nativeLead / ratio);
		m_engine.setHostBlockFrames((std::uint32_t) std::ceil(std::max(samplesPerBlock, 1) * ratio));

		bootEngine();
		// Initialization and any initial patch load finish before the host epoch.
		// Reprepare starts beyond the previous input grant, independent of worker speed.
		const auto origin = ((m_engine.requestedFrames() + quantum - 1) / quantum) * quantum;
		m_timeline.reset(m_hostSampleRate, origin);
		m_timelineAttached = m_engine.readyForPlayback();
	}
	// Host listeners may query state synchronously when latency changes.
	setLatencySamples(latency);
}

void ProphecyAudioProcessor::releaseResources()
{
	std::lock_guard stateLock(m_programState.mutex);
	ProcessingPause pause(m_processingPaused, m_callbackAccess);
}

bool ProphecyAudioProcessor::maybeBootEngine()
{
	std::lock_guard stateLock(m_programState.mutex);
	ProcessingPause pause(m_processingPaused, m_callbackAccess);
	return bootEngine();
}

// Boot the engine once, iff a valid ROM set can be located (env / persisted picker
// choice / drop dir — see rom_locator.h). Without one the plugin stays silent and the
// editor shows the first-run ROM picker; the pick then boots via setRomDirFromUser().
bool ProphecyAudioProcessor::bootEngine()
{
	if (m_started.load())
		return m_engine.readyForPlayback();
	const juce::File romDir = romloc::locateRomDir();
	if (romDir == juce::File())
		return false;
	m_started.store(true);
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
		m_engine.setProgramRevision(m_programState.revision());
		// Clean-room CI firmware has its own sentinel protocol, not Korg's boot
		// screen or SysEx parser. Ordinary firmware must complete the handshake.
		const bool firmware = std::getenv("PROFLIGACY_CI_EXPOSE_LCD_STATE") == nullptr;
		if (!restoreProgram(firmware)) return false;
		// Host state can restore Wheel 2 before prepareToPlay. Pre-start engine writes are
		// intentionally rejected so an unclaimed/second processor cannot touch another
		// instance's global queue; publish the retained value once this engine owns it.
		(void) m_engine.pushAdin(9, m_wheel2Pos.load(std::memory_order_relaxed));
	}
	return started;
}

// Called only with m_programState.mutex and ProcessingPause held. A host restore is
// complete when firmware has acknowledged and read back the program, never when
// its SysEx has merely been queued. Initial and subsequent loads share this path.
bool ProphecyAudioProcessor::restoreProgram(bool firmwareHandshake)
{
	const bool restore = !m_skipStateRestore && m_programState.restoreRequired();
	const auto token = m_programState.token();
	const auto* document = m_programState.document();
	const auto program = restore ? document->programMidi() : std::vector<std::uint8_t>{};
	const auto edits = restore ? document->edits : std::vector<prophecy::ProgramEdit>{};
	if (restore) cancelEditorWork();
	if (!m_engine.initializePlayback(program.data(), program.size(), firmwareHandshake, edits))
	{
		m_programState.fail(prophecy::ProgramState::Error::Restore);
		return false;
	}
	if (restore)
	{
		if (!m_programState.confirm(token, m_engine.snapshotProgram())) return false;
		(void)m_engine.pushAdin(9, m_wheel2Pos.load(std::memory_order_relaxed));
	}
	else if (firmwareHandshake)
		(void)m_programState.observe(token.revision, m_engine.snapshotProgram());
	if (firmwareHandshake) (void)m_programMidiRouting.reset(m_engine.snapshotGlobals());
	m_timelineAttached = false;
	return true;
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
	m_audioCallbacks.fetch_add(1, std::memory_order_relaxed);
	m_audioHostFrames.fetch_add((std::uint64_t) std::max(numSamples, 0), std::memory_order_relaxed);
	if (m_processingPaused.load(std::memory_order_acquire))
	{
		buffer.clear();
		return;
	}
	CallbackAccess callbackAccess(m_callbackAccess);
	if (!callbackAccess.acquired)
	{
		buffer.clear();
		return;
	}
	const bool engineActive = m_engine.readyForPlayback();
	if (engineActive && (!m_timelineAttached || m_timeline.origin() < m_engine.playbackOrigin()))
	{
		// A ROM may be selected long after the DAW started calling processBlock.
		// Adopt its completed initialization epoch, with the same fixed delay.
		m_timeline.reset(m_hostSampleRate, m_engine.playbackOrigin());
		m_timelineHostFrame = 0;
		m_timelineAttached = true;
	}
	if (numSamples > m_preparedMaxBlock)
	{
		// A host exceeded even our generous prepare-time reserve. Do bounded work only:
		// output silence, count the contract violation, and ignore this block's MIDI rather
		// than allocate or touch undersized resampler storage on the audio thread.
		buffer.clear();
		m_oversizedAudioBlocks.fetch_add(1, std::memory_order_relaxed);
		m_audioUnderrunFrames.fetch_add((std::uint64_t) numSamples, std::memory_order_relaxed);
		m_timelineHostFrame += (std::uint64_t) numSamples;
		if (engineActive) m_engine.requestThroughFrame(m_timeline.horizon(m_timelineHostFrame));
		return;
	}

	// Forward host performance MIDI into the emulated 31250-baud
	// UART at its absolute sample position, before granting this input range to
	// the worker. Hardware serial and scan/voice-allocation latency stays intact.
	const auto timelineBlockStart = m_timelineHostFrame;
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
		// This plugin has no MIDI output. Firmware inquiries belong to its private
		// control transport; forwarding host inquiries could alias its replies.
		if (firmwareInquiry(data, std::size_t(numBytes)) != FirmwareInquiry::None) continue;
		// CC->ADIN remap: a mapped control-change is translated to a front-panel analog
		// write (the composite X-Y control, ribbon Z, or wheel 1/2). Forwarding that
		// same CC through the UART would apply it twice. Unmapped CCs pass through.
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
		if (forwardHostMidi(data, (std::size_t) numBytes, eventFrame))
		{
			m_hostMidiEventsForwarded.fetch_add(1, std::memory_order_relaxed);
		}
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
	std::lock_guard controlLock(m_programState.mutex);
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
// State container (backward compatible): a legacy state is the bare sysex program dump
// (starts with 0xF0). A new state starts with the magic "PRP1" and carries the CC->ADIN
// map alongside the dump, so old saves still load and the mapping survives even before the
// engine has booted (no dump yet). Layout: "PRP1" | u8 N | N*(u8 cc, u8 target) | dump...
void ProphecyAudioProcessor::getStateInformation(juce::MemoryBlock &dest)
{
	std::lock_guard stateLock(m_programState.mutex);
	drainProgramInput();
	dest.reset();

	// Exact edit-buffer recall is part of a DAW project save. The standalone has the
	// hardware-like policy instead: only an explicit front-panel WRITE persists a patch.
	std::vector<std::uint8_t> dump;
	bool pendingEdits = false;
	if (wrapperType != juce::AudioProcessor::wrapperType_Standalone
		&& std::getenv("PROFLIGACY_CI_EXPOSE_LCD_STATE") == nullptr)
	{
		const auto* document = m_programState.document();
		if ((!document || (!m_programState.restoreRequired() && document->edits.empty()))
			&& m_engine.readyForPlayback())
		{
			const auto owner = m_programState.revision();
			const auto raw = m_engine.snapshotProgram();
			// JUCE has no failed-save return value. Leave the destination empty
			// and expose the capture error; returning the previous clean snapshot
			// would silently replace the current program with stale state.
			if (!m_programState.observe(owner, raw)) return;
		}
		if ((document = m_programState.document()) != nullptr)
		{
			pendingEdits = !document->edits.empty();
			dump = pendingEdits ? document->encode() : document->programMidi();
		}
	}

	// Header + mapping (non-Off CC entries). PRP2 adds a WHEEL2 (ADIN9) rest byte after the
	// map; PRP1 (no wheel byte) still loads, defaulting the wheel to the driver rest (0x80).
	dest.append(pendingEdits ? "PRP3" : "PRP2", 4);
	std::vector<std::uint8_t> map;
	for (int cc = 0; cc < 128; ++cc)
	{
		const int t = ccMapTarget(cc);
		if (t != 0) { map.push_back((std::uint8_t) cc); map.push_back((std::uint8_t) t); }
	}
	const std::uint8_t n = (std::uint8_t) (map.size() / 2);
	dest.append(&n, 1);
	if (!map.empty()) dest.append(map.data(), map.size());
	const std::uint8_t w2 = (std::uint8_t) wheel2Pos();
	dest.append(&w2, 1);
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
	std::lock_guard stateLock(m_programState.mutex);
	if (data == nullptr || size <= 0) return;
	const auto* bytes = static_cast<const std::uint8_t*>(data);
	const bool prp1 = size >= 5 && std::memcmp(bytes, "PRP1", 4) == 0;
	const bool prp2 = size >= 5 && std::memcmp(bytes, "PRP2", 4) == 0;
	const bool prp3 = size >= 5 && std::memcmp(bytes, "PRP3", 4) == 0;
	const bool container = prp1 || prp2 || prp3;
	const auto invalid = [&] { m_programState.fail(prophecy::ProgramState::Error::InvalidProgram); };
	std::size_t offset = 0;
	int count = 0;
	if (container)
	{
		count = bytes[4];
		offset = 5 + std::size_t(count) * 2 + (prp1 ? 0 : 1);
		if (count > 128 || offset > std::size_t(size)) { invalid(); return; }
		for (int i = 0; i < count; ++i)
			if (bytes[5 + i * 2] > 127 || bytes[6 + i * 2] > int(CcTarget::Wheel2))
				{ invalid(); return; }
	}
	std::optional<prophecy::ProgramDocument> document;
	if (offset < std::size_t(size))
	{
		document = prp3
			? prophecy::ProgramDocument::decode(bytes + offset, std::size_t(size) - offset)
			: prophecy::ProgramDocument::fromMidi(bytes + offset, std::size_t(size) - offset);
		if (!document) { invalid(); return; }
		if (std::any_of(document->edits.begin(), document->edits.end(),
			[](const auto& edit) { return !edit.supported(); })) { invalid(); return; }
	}
	else if (prp3 || !container) { invalid(); return; }

	// Validate the whole blob before changing either controller preferences or
	// program ownership. A malformed tail must not partially apply the header.
	ProcessingPause pause(m_processingPaused, m_callbackAccess);
	drainProgramInput();
	if (container)
	{
		for (int cc = 0; cc < 128; ++cc) setCcMap(cc, 0);
		for (int i = 0; i < count; ++i) setCcMap(bytes[5 + i * 2], bytes[6 + i * 2]);
		setWheel2(prp1 ? 0x80 : bytes[offset - 1]);
	}
	if (wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		// Standalone preference recall does not replace the live edit buffer or
		// discard editor operations already accepted by its program owner.
		return;
	}
	if (document)
		loadProgramDocument(std::move(*document));
}

void ProphecyAudioProcessor::loadProgramDocument(prophecy::ProgramDocument document)
{
	replaceProgramRevision();
	cancelEditorWork();
	m_lastPatchSendMs = -1.0e9;
	m_programState.restore(std::move(document));
	if (!m_skipStateRestore && m_engine.readyForPlayback()) (void)restoreProgram();
}

void ProphecyAudioProcessor::replaceProgramRevision()
{
	m_engine.setProgramRevision(m_programState.advanceRevision());
}

void ProphecyAudioProcessor::cancelEditorWork()
{
	m_patchSelectDelay.cancel();
	m_writeSeq.cancel();
	m_editorCommandPacer.cancel();
	m_programDumpSync.cancel();
	m_patchLoadBarrierUntilFrame.store(0, std::memory_order_release);
}

bool ProphecyAudioProcessor::forwardHostMidi(const std::uint8_t* bytes, std::size_t size, std::uint64_t frame)
{
	if (!m_engine.readyForPlayback()) return m_engine.pushMidiAtFrame(bytes, size, frame);
	const bool parameterInput = size >= 6 && bytes[0] == 0xf0 && bytes[1] == 0x42
		&& bytes[3] == 0x41 && bytes[4] == 0x41 && bytes[5] == 1;
	const bool firmwareInput = firmwareControlMessage(bytes, size);
	const bool replacementInput = (size == 2 && (bytes[0] & 0xf0) == 0xc0)
		|| (size >= 5 && bytes[0] == 0xf0 && bytes[1] == 0x42 && bytes[3] == 0x41 && bytes[4] == 0x40);
	if (replacementInput && (m_programState.firmwareControlPending() || m_programMidiInbox.firmwareInputPending()))
	{
		m_programMidiInbox.reject(prophecy::ProgramMidiInbox::Rejection::Busy);
		return false;
	}
	const bool programInput = (size == 2 && (bytes[0] & 0xf0) == 0xc0)
		|| (size == 3 && (bytes[0] & 0xf0) == 0xb0)
		|| (size >= 5 && bytes[0] == 0xf0 && bytes[1] == 0x42 && bytes[3] == 0x41
			&& (bytes[4] == 0x40 || bytes[4] == 0x51 || (size == 11 && bytes[4] == 0x41 && bytes[5] == 0)))
		|| parameterInput || firmwareInput;
	if (!programInput) return m_engine.pushMidiAtFrame(bytes, size, frame);
	auto* record = m_programMidiInbox.reserve();
	if (!record || size > record->bytes.size())
	{
		m_programMidiInbox.reject();
		return false;
	}
	std::copy_n(bytes, size, record->bytes.begin());
	record->size = size;
	if (firmwareInput) m_programMidiInbox.markFirmwareInput();
	m_engine.setProgramInputPending(m_programMidiInbox.nextSequence());
	// Parameter SysEx is an asynchronous program edit, sharing the editor's
	// paced transaction order. Do not also transmit its raw bytes: that would
	// apply it twice and could put it ahead of earlier accepted editor intent.
	record->accepted = parameterInput || firmwareInput || m_engine.pushMidiAtFrame(bytes, size, frame);
	const bool accepted = record->accepted;
	m_programMidiInbox.publish();
	return accepted;
}

void ProphecyAudioProcessor::drainProgramInput()
{
	if (m_drainingProgramInput) return;
	const juce::ScopedValueSetter<bool> draining(m_drainingProgramInput, true);
	const auto through = m_programMidiInbox.publishedSequence();
	for (;;)
	{
		const auto rejected = m_programMidiInbox.takeRejection();
		if (rejected != prophecy::ProgramMidiInbox::Rejection::None)
			m_programState.fail(rejected == prophecy::ProgramMidiInbox::Rejection::Busy
				? prophecy::ProgramState::Error::Busy : prophecy::ProgramState::Error::Capacity);
		if (m_programMidiInbox.consumedSequence() >= through) break;
		const auto* record = m_programMidiInbox.front();
		try
		{
			if (record->accepted) receiveProgramInput(record->bytes.data(), record->size);
			else m_programState.fail(prophecy::ProgramState::Error::Capacity);
		}
		catch (...)
		{
			m_engine.acknowledgeProgramInput(m_programMidiInbox.pop());
			throw;
		}
		m_engine.acknowledgeProgramInput(m_programMidiInbox.pop());
	}
}

void ProphecyAudioProcessor::receiveProgramInput(const std::uint8_t* bytes, std::size_t size)
{
	if (firmwareControlMessage(bytes, size))
	{
		if ((bytes[2] & 0xf0) != 0x30 || bytes[size - 1] != 0xf7
			|| std::any_of(bytes + 1, bytes + size - 1, [](auto byte) { return byte >= 128; })
			|| (bytes[4] == 0x41 && (size != 11 || bytes[5] > 2)))
			{ m_programState.fail(prophecy::ProgramState::Error::InvalidEdit); return; }
		// A queued channel change may alter which subsequent raw messages the
		// firmware accepts. Preserve their original addresses and delivery order.
		if (m_programState.firmwareControlPending() || m_programMidiRouting.sysex(bytes, size))
			(void)enqueueFirmwareMidi(-1, bytes, size, false);
		return;
	}
	if (size == 8 && (bytes[0] & 0xf0) == 0xb0 && (bytes[3] & 0xf0) == 0xb0 && (bytes[6] & 0xf0) == 0xc0)
	{
		receiveProgramInput(bytes, 3);
		receiveProgramInput(bytes + 3, 3);
		receiveProgramInput(bytes + 6, 2);
		return;
	}
	if (size >= 6 && bytes[0] == 0xf0 && bytes[1] == 0x42 && bytes[3] == 0x41
		&& bytes[4] == 0x41 && bytes[5] == 1)
	{
		const auto edit = prophecy::ProgramEdit::fromMidi(bytes, size);
		if (!edit) { m_programState.fail(prophecy::ProgramState::Error::InvalidEdit); return; }
		if (m_programMidiRouting.sysex(bytes, size)) (void)requestProgramEdits({*edit});
		return;
	}
	std::optional<prophecy::ProgramDocument> document;
	if (m_programMidiRouting.sysex(bytes, size) && bytes[4] == 0x40)
	{
		document = prophecy::ProgramDocument::fromMidi(bytes, size);
		if (!document) { m_programState.fail(prophecy::ProgramState::Error::InvalidProgram); return; }
	}
	const int program = m_programMidiRouting.receive(bytes, size);
	if (program >= 0)
	{
		const auto stored = m_engine.snapshotStoredProgram(program);
		if (stored.size() != prophecy::ProgramDocument::programBytes)
			{ m_programState.fail(prophecy::ProgramState::Error::NoProgram); return; }
		document.emplace();
		std::copy(stored.begin(), stored.end(), document->base.begin());
	}
	if (!document) return;
	replaceProgramRevision();
	cancelEditorWork();
	m_programState.restore(std::move(*document));
	holdEditorCommandsForPatchLoad(kPatchLoadSettleSeconds);
	m_programDumpSync.request(1);
}

void ProphecyAudioProcessor::selectPatch(int program)
{
	std::lock_guard controlLock(m_programState.mutex);
	drainProgramInput();
	if (m_programState.firmwareControlPending() || m_programMidiInbox.firmwareInputPending())
		{ m_programState.fail(prophecy::ProgramState::Error::Busy); return; }
	if (program < 0 || program > 127) return;
	const auto stored = m_engine.snapshotStoredProgram(program);
	if (stored.size() != prophecy::ProgramDocument::programBytes)
	{
		m_programState.fail(prophecy::ProgramState::Error::NoProgram);
		return;
	}
	prophecy::ProgramDocument document;
	std::copy(stored.begin(), stored.end(), document.base.begin());
	replaceProgramRevision();
	cancelEditorWork();
	m_programState.restore(std::move(document));
	m_editorPatchIntents.fetch_add(1, std::memory_order_relaxed);
	// A patch change discards the current edit buffer, so cancel work belonging to the old
	// buffer and any obsolete read-back. Program loading costs roughly half a second inside
	// the firmware; forwarding every arrow click can therefore create far more work than a
	// user can consume. Always debounce to the latest requested program. This also gives a
	// possibly in-flight rename/macro SysEx time to finish before the bank/program message.
	// The production WebView and headless editor-stress host both use this exact method.
	m_editorCommandPacer.holdForPatchLoad(2500);
	// Start the editor-command barrier at intent time. An accepted send refreshes
	// it to cover the complete firmware load; host and editor-play MIDI still pass.
	holdEditorCommandsForPatchLoad(2.5);
	m_patchSelectDelay.schedule(program);
	m_programDumpSync.request(1);
}

bool ProphecyAudioProcessor::sendPatchNow(int program)
{
	if (m_engine.programReadbackPending()) return false;
	// A quiet-click debounce is not enough: two individually valid Program
	// Changes can still overlap the firmware's long inter-board load transaction.
	// PatchSelectDelay will retry, retaining only its latest program, until the
	// prior load has completed.
	const double now = juce::Time::getMillisecondCounterHiRes();
	if (now - m_lastPatchSendMs < kPatchSelectMinIntervalMs)
	{
		// Keep both barriers closed while PatchSelectDelay retains the latest
		// requested program and waits for the previous transaction to settle.
		holdEditorCommandsForPatchLoad(0.2);
		m_editorCommandPacer.extendPatchLoad(200);
		return false;
	}
	// Bank select then program change (verified on the emulated firmware via the LCD:
	// a bare 0xC0 only ever reaches bank A; CC0=0 + CC32=bank + 0xC0 lands "B52:...").
	const std::uint8_t msg[8] = {
		0xB0, 0x00, 0x00,                          // bank select MSB
		0xB0, 0x20, (std::uint8_t) (program / 64), // bank select LSB: 0=A, 1=B
		0xC0, (std::uint8_t) (program % 64) };     // program within the bank
	const bool accepted = pushImmediateMidi(msg, sizeof(msg), m_programState.revision());
	if (accepted)
	{
		(void)m_programMidiRouting.receive(msg, 3);
		(void)m_programMidiRouting.receive(msg + 3, 3);
		holdEditorCommandsForPatchLoad(kPatchLoadSettleSeconds);
		m_editorCommandPacer.extendPatchLoad(
			(int)std::lround(kPatchLoadSettleSeconds * 1000.0));
		m_lastPatchSendMs = now;
		m_editorPatchSends.fetch_add(1, std::memory_order_relaxed);
	}
	return accepted;
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
	std::lock_guard controlLock(m_programState.mutex);
	drainProgramInput();
	if (bytes == nullptr || size == 0) return;
	const auto inquiry = firmwareInquiry(bytes, size);
	if (inquiry == FirmwareInquiry::Identity) return;
	if (inquiry == FirmwareInquiry::Program) { (void)requestProgramDump(); return; }
	if (firmwareControlMessage(bytes, size)) { receiveProgramInput(bytes, size); return; }
	if (size >= 6 && bytes[0] == 0xf0 && bytes[1] == 0x42 && bytes[3] == 0x41
		&& bytes[4] == 0x41 && bytes[5] == 1)
	{
		receiveProgramInput(bytes, size);
		return;
	}
	if (size >= 5 && bytes[0] == 0xf0 && bytes[1] == 0x42 && bytes[3] == 0x41 && bytes[4] == 0x40)
	{
		if (m_programState.firmwareControlPending() || m_programMidiInbox.firmwareInputPending())
			{ m_programState.fail(prophecy::ProgramState::Error::Busy); return; }
		auto document = prophecy::ProgramDocument::fromMidi(bytes, size);
		if (!document) { m_programState.fail(prophecy::ProgramState::Error::InvalidProgram); return; }
		ProcessingPause pause(m_processingPaused, m_callbackAccess);
		loadProgramDocument(std::move(*document));
		return;
	}
	if ((size == 2 && (bytes[0] & 0xf0) == 0xc0)
		|| (size == 8 && (bytes[6] & 0xf0) == 0xc0))
		if (m_programState.firmwareControlPending() || m_programMidiInbox.firmwareInputPending())
			{ m_programState.fail(prophecy::ProgramState::Error::Busy); return; }
	if (pushImmediateMidi(bytes, size)) receiveProgramInput(bytes, size);
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
	std::lock_guard controlLock(m_programState.mutex);
	m_editorDumpRequests.fetch_add(1, std::memory_order_relaxed);
	// Program Change and current-program dump assembly share the firmware MIDI task. A dump
	// sent during the patch-load transaction is silently discarded, so wait out any pending/recent
	// selection and let ProgramDumpSync retry the one in-flight editor transaction if needed.
	constexpr double patchSettleMs = kPatchLoadSettleSeconds * 1000.0;
	const double now = juce::Time::getMillisecondCounterHiRes();
	int delayMs = 0;
	if (m_patchSelectDelay.pending())
		delayMs = 800; // debounce checkpoint; the shared pacer remains held through the load
	else
		delayMs = std::max(0, (int)std::ceil(patchSettleMs - (now - m_lastPatchSendMs)));
	return m_programDumpSync.request(delayMs);
}

std::size_t ProphecyAudioProcessor::getProgramData(std::uint8_t *out, std::size_t cap,
	std::uint32_t *version, std::uint64_t *completedRequestGeneration) const
{
	std::uint32_t observedVersion = 0;
	const std::size_t bytes = m_engine.latestProgramData(out, cap, &observedVersion);
	if (version != nullptr) *version = observedVersion;
	const std::uint64_t completed = m_programDumpSync.completed();
	if (completedRequestGeneration != nullptr) *completedRequestGeneration = completed;
	return bytes;
}

void ProphecyAudioProcessor::ProgramDumpSync::run()
{
	if (!m_ticket)
	{
		const bool replacement = m_proc.m_programState.restoreRequired();
		if ((replacement && (m_proc.m_patchSelectDelay.pending() || !m_proc.m_editorCommandPacer.settled()))
			|| (!replacement && m_proc.m_editorCommandPacer.busy())) { startTimer(10); return; }
		m_ticket = m_proc.m_engine.beginProgramReadback();
		if (!m_ticket) { startTimer(10); return; }
		++m_attempts;
		m_prefix = replacement ? prophecy::ProgramState::Token{m_proc.m_programState.revision(), 0}
			: m_proc.m_editorCommandPacer.delivered();
		m_proc.m_editorDumpSends.fetch_add(1, std::memory_order_relaxed);
	}
	std::vector<std::uint8_t> raw;
	const auto result = m_proc.m_engine.pollProgramReadback(m_ticket, raw);
	if (result == ProphecyEngine::ReadbackStatus::Pending) { startTimer(10); return; }
	if (result == ProphecyEngine::ReadbackStatus::NoReply && m_attempts < 3)
	{
		// The identity reply proves this request was drained before retrying.
		m_ticket = 0;
		startTimer(100);
		return;
	}
	if (result != ProphecyEngine::ReadbackStatus::Complete)
	{
		cancel();
		m_proc.m_programState.fail(prophecy::ProgramState::Error::Snapshot);
		return;
	}
	m_ticket = 0;
	const bool replacing = m_proc.m_programState.restoreRequired();
	const bool confirmed = m_proc.m_programState.confirm(m_prefix, raw);
	if (replacing && !confirmed)
	{
		if (m_attempts < 3) { startTimer(100); return; }
		stopTimer();
		m_proc.m_programState.fail(prophecy::ProgramState::Error::Restore);
		return;
	}
	if (!confirmed)
		(void)m_proc.m_programState.observe(m_proc.m_programState.revision(), raw);
	bool moreFirmwareWork = false;
	if (m_proc.m_programState.firmwareControlPending())
	{
		moreFirmwareWork = m_proc.m_editorCommandPacer.busy() || m_proc.m_programMidiInbox.firmwareInputPending();
		if (!moreFirmwareWork)
		{
			if (!m_proc.m_programMidiRouting.reset(m_proc.m_engine.snapshotGlobals()))
			{
				stopTimer();
				m_proc.m_programState.fail(prophecy::ProgramState::Error::Snapshot);
				return;
			}
			(void)m_proc.m_programState.finishFirmwareControl(m_proc.m_programState.revision(), raw);
			if (m_proc.m_engine.firmwareControlErrors() != m_proc.m_firmwareErrorStart)
				m_proc.m_programState.fail(prophecy::ProgramState::Error::Control);
		}
	}
	m_completed.store(m_generation, std::memory_order_release);
	stopTimer();
	const auto* document = m_proc.m_programState.document();
	if (moreFirmwareWork || (document && !document->edits.empty())) request(1);
}

bool ProphecyAudioProcessor::firmwareControlAllowed()
{
	drainProgramInput();
	if (writeInProgress() || m_programState.pendingIntent())
		{ m_programState.fail(prophecy::ProgramState::Error::Busy); return false; }
	if (!m_engine.readyForPlayback())
		{ m_programState.fail(prophecy::ProgramState::Error::NoProgram); return false; }
	return true;
}

void ProphecyAudioProcessor::firmwareControlAccepted()
{
	if (!m_programState.firmwareControlPending()) m_firmwareErrorStart = m_engine.firmwareControlErrors();
	m_programState.beginFirmwareControl();
	m_programDumpSync.request(75);
}

bool ProphecyAudioProcessor::enqueueFirmwareMidi(int key, const std::uint8_t* bytes, std::size_t size, bool bindChannel)
{
	if (!firmwareControlAllowed()) return false;
	if (!m_editorCommandPacer.enqueueMidi(key, bytes, size, bindChannel))
		{ m_programState.fail(prophecy::ProgramState::Error::Capacity); return false; }
	firmwareControlAccepted();
	return true;
}

void ProphecyAudioProcessor::selectArpeggioPattern(int pattern)
{
	std::lock_guard controlLock(m_programState.mutex);
	if (pattern < 0 || pattern > 9) return;
	// NRPN MSB=0, LSB=1 (Arpeggio Pattern Select), Data Entry MSB=INT pattern 0..9.
	const std::uint8_t msg[9] = {0xB0, 0x63, 0x00, 0xB0, 0x62, 0x01,
		0xB0, 0x06, (std::uint8_t) pattern};
	(void)enqueueFirmwareMidi(0x20000, msg, sizeof(msg));
}

void ProphecyAudioProcessor::setArpeggiatorControl(int control, int value)
{
	std::lock_guard controlLock(m_programState.mutex);
	// Documented NRPNs: 2=On/Off, 3=Octaves, 4=Latch, 5=Key Sync.
	if (control < 2 || control > 5) return;
	value = std::clamp(value, 0, control == 3 ? 3 : 127);
	const std::uint8_t msg[9] = {0xB0, 0x63, 0x00, 0xB0, 0x62, (std::uint8_t) control,
		0xB0, 0x06, (std::uint8_t) value};
	(void)enqueueFirmwareMidi(0x20010 + control, msg, sizeof(msg));
}

void ProphecyAudioProcessor::requestArpeggioPatternDump(int pattern)
{
	std::lock_guard controlLock(m_programState.mutex);
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
	std::lock_guard controlLock(m_programState.mutex);
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
	(void)enqueueFirmwareMidi(0x20040 + pattern, msg.data(), msg.size());
}

void ProphecyAudioProcessor::setParam(int paramId, int value)
{
	setParamG(1, paramId, value); // program group
}

bool ProphecyAudioProcessor::requestProgramEdits(const std::vector<prophecy::ProgramEdit>& edits, int intervalMs)
{
	std::lock_guard controlLock(m_programState.mutex);
	drainProgramInput();
	if (edits.empty()) return true;
	if (writeInProgress() || m_programState.firmwareControlPending()
		|| (!m_drainingProgramInput && m_programMidiInbox.firmwareInputPending()))
		{ m_programState.fail(prophecy::ProgramState::Error::Busy); return false; }
	const auto owner = m_programState.revision();
	if (std::any_of(edits.begin(), edits.end(), [](const auto& edit) { return !edit.supported(); }))
		{ m_programState.fail(prophecy::ProgramState::Error::InvalidEdit); return false; }
	const auto* document = m_programState.document();
	if ((!document || (document->edits.empty() && !m_programState.restoreRequired())) && m_engine.readyForPlayback())
		if (!m_programState.observe(owner, m_engine.snapshotProgram())) return false;
	if (!m_programState.document())
		{ m_programState.fail(prophecy::ProgramState::Error::NoProgram); return false; }
	if (!m_editorCommandPacer.enqueueProgramEdits(edits, intervalMs)) return false;
	m_programDumpSync.request(intervalMs);
	return true;
}

void ProphecyAudioProcessor::setParamG(int group, int paramId, int value)
{
	std::lock_guard controlLock(m_programState.mutex);
	if (group == 1)
	{
		if (paramId <= 0 || paramId > 0x3fff || value < -8192 || value > 8191)
			{ m_programState.fail(prophecy::ProgramState::Error::InvalidEdit); return; }
		(void)requestProgramEdits({{static_cast<std::uint16_t>(paramId), static_cast<std::int16_t>(value)}});
		return;
	}
	const int p = paramId & 0x3fff;
	const int v = value & 0x3fff;
	const std::uint8_t msg[11] = {0xf0, 0x42, 0x30, 0x41, 0x41, std::uint8_t(group & 127),
		std::uint8_t(p & 127), std::uint8_t(p >> 7), std::uint8_t(v & 127), std::uint8_t(v >> 7), 0xf7};
	(void)enqueueFirmwareMidi(0x10000 + ((group & 127) << 14) + p, msg, sizeof(msg));
}

void ProphecyAudioProcessor::panelPulse(int row, int bit)
{
	std::lock_guard controlLock(m_programState.mutex);
	if (row < 0 || bit < 0) return;
	if (!firmwareControlAllowed()) return;
	if (!m_editorCommandPacer.enqueuePanel(row, bit))
		{ m_programState.fail(prophecy::ProgramState::Error::Capacity); return; }
	firmwareControlAccepted();
}

void ProphecyAudioProcessor::renamePatch(const juce::String &name)
{
	std::lock_guard controlLock(m_programState.mutex);
	// Program Name Char 1..16 = group-1 params 1..16 (ascii_char, manifest-verified).
	// Paced by the shared queue: 16 sysexes sent back-to-back at line rate get
	// partially dropped by the firmware when the edit buffer holds a busy patch.
	const juce::String padded = name.paddedRight(' ', 16).substring(0, 16);
	std::vector<prophecy::ProgramEdit> burst;
	for (int i = 0; i < 16; i++)
	{
		int c = (int) (juce::juce_wchar) padded[i];
		if (c < 32 || c > 126) c = ' ';
		burst.push_back({static_cast<std::uint16_t>(1 + i), static_cast<std::int16_t>(c)});
	}
	// Name characters are unusually easy to lose while a motion-heavy patch is running.
	// 100 ms remains quick enough for a 16-character name edit, while giving the busy
	// firmware a full hardware-scale interval to consume each parameter message.
	(void)requestProgramEdits(burst, 100);
}

// Quick-init patch-shaping macros. Recipes ported EXACTLY from the MAME-tree GUI
// (src/tools/korgprophecy_gui/korgprophecy_gui.mm sendPatchMacro + kInit/kSaw/
// kFilterThru/kBypassFx) and scripts/korgprophecy_macros.py (dump-verified 2026-06-29).
// All params are program group 1. OSC1-specific params are ExID-packed as (1<<12)|param;
// setParamG splits the 14-bit id, so passing 4096|388 emits the correct ExID addressing.
void ProphecyAudioProcessor::sendMacro(const juce::String &name)
{
	std::lock_guard controlLock(m_programState.mutex);
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

	std::vector<prophecy::ProgramEdit> edits;
	for (const auto& pv : burst)
		edits.push_back({static_cast<std::uint16_t>(pv.first), static_cast<std::int16_t>(pv.second)});
	(void)requestProgramEdits(edits);
}

void ProphecyAudioProcessor::setCcMap(int cc, int target)
{
	std::lock_guard controlLock(m_programState.mutex);
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

// WHEEL2 = ADIN9. Store the chosen rest and push it into the ADIN mux. drain_host_adin()
// no-ops when the value already matches (so pushing the 0x80 default costs nothing), and the
// retained value is published by maybeBootEngine once this processor owns the MAME slot,
// so this is safe to call pre-boot (e.g. from setStateInformation). Last write wins vs. a
// live CC->Wheel2 remap, which is expected.
void ProphecyAudioProcessor::setWheel2(int value)
{
	std::lock_guard controlLock(m_programState.mutex);
	if (value < 0)   value = 0;
	if (value > 255) value = 255;
	m_wheel2Pos.store((std::uint8_t) value, std::memory_order_relaxed);
	publishControllerDisplayValue(9, value);
	(void) pushUiAdin(9, value);
}

void ProphecyAudioProcessor::setWheel2FromEditor(int value)
{
	std::lock_guard controlLock(m_programState.mutex);
	if (value < 0) value = 0;
	if (value > 255) value = 255;
	m_wheel2Pos.store((std::uint8_t)value, std::memory_order_relaxed);
	publishControllerDisplayValue(9, value);
	(void) pushUiAdin(9, value);
}

void ProphecyAudioProcessor::setAdin(int source, int value)
{
	std::lock_guard controlLock(m_programState.mutex);
	if (source < 0 || source > 15) return;
	value = std::clamp(value, 0, 255);
	publishControllerDisplayValue(source, value);
	(void) pushUiAdin(source, value);
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

std::uint64_t ProphecyAudioProcessor::writePatch(int destination)
{
	std::lock_guard controlLock(m_programState.mutex);
	drainProgramInput();
	if (destination < 0 || destination >= 128) return 0;
	if (writeInProgress() || m_programState.firmwareControlPending() || m_programMidiInbox.firmwareInputPending())
	{
		m_programState.fail(prophecy::ProgramState::Error::Busy);
		return 0;
	}
	const auto* document = m_programState.document();
	if ((!document || (!m_programState.restoreRequired() && document->edits.empty())) && m_engine.readyForPlayback())
		if (!m_programState.observe(m_programState.revision(), m_engine.snapshotProgram())) return 0;
	document = m_programState.document();
	if (!document || !m_engine.readyForPlayback())
	{
		m_programState.fail(prophecy::ProgramState::Error::NoProgram);
		return 0;
	}
	m_writeSeq.start(destination, *document);
	m_programState.clearError();
	const auto id = m_writeRequestId.fetch_add(1, std::memory_order_acq_rel) + 1;
	m_writeStatus.store(WriteStatus::Pending, std::memory_order_release);
	return id;
}

void ProphecyAudioProcessor::ProgramWrite::run()
{
	stopTimer();
	ProcessingPause pause(m_proc.m_processingPaused, m_proc.m_callbackAccess);
	m_proc.drainProgramInput();
	// Close the callback/revision race before touching persistent storage.
	if (!current()) { cancelled(); return; }
	m_proc.m_patchSelectDelay.cancel();
	m_proc.m_editorCommandPacer.cancel();
	m_proc.m_programDumpSync.cancel();
	m_proc.m_patchLoadBarrierUntilFrame.store(0, std::memory_order_release);
	const bool written = m_proc.m_engine.storeProgram(m_destination, m_document);
	if (m_proc.m_engine.readyForPlayback())
	{
		const auto raw = m_proc.m_engine.snapshotProgram();
		if (!m_proc.m_programState.confirm(m_token, raw))
			(void)m_proc.m_programState.observe(m_token.revision, raw);
		(void)m_proc.m_engine.pushAdin(9, m_proc.m_wheel2Pos.load(std::memory_order_relaxed));
	}
	m_proc.m_timelineAttached = false;
	if (!written)
		m_proc.m_programState.fail(prophecy::ProgramState::Error::Write);
	m_proc.m_writeStatus.store(written ? WriteStatus::Succeeded : WriteStatus::Failed, std::memory_order_release);
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
					else if (*m_proc.initializationError())
						obj->setProperty("error", m_proc.initializationError());
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
					complete(juce::var(juce::int64(args.isEmpty() ? 0 : m_proc.writePatch((int)args[0]))));
				})
			.withNativeFunction("isWriteInProgress",
				[this](const juce::Array<juce::var> &args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					juce::ignoreUnused(args);
					complete(juce::var(m_proc.writeInProgress()));
				})
			.withNativeFunction("getWriteStatus",
				[this](const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					auto result = std::make_unique<juce::DynamicObject>();
					result->setProperty("request", juce::int64(m_proc.writeRequestId()));
					result->setProperty("pending", m_proc.writeInProgress());
					result->setProperty("succeeded", m_proc.writeStatus() == ProphecyAudioProcessor::WriteStatus::Succeeded);
					result->setProperty("error", juce::String(m_proc.programStateError()));
					complete(juce::var(result.release()));
				})
			.withNativeFunction("getProgramStateStatus",
				[this](const juce::Array<juce::var>&, juce::WebBrowserComponent::NativeFunctionCompletion complete)
				{
					auto result = std::make_unique<juce::DynamicObject>();
					result->setProperty("pending", m_proc.writeInProgress()
						|| m_proc.programStateStatus() == prophecy::ProgramState::Status::Pending);
					result->setProperty("error", juce::String(m_proc.programStateError()));
					complete(juce::var(result.release()));
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
