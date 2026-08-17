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

ProphecyAudioProcessor::ProphecyAudioProcessor()
	: AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
	// Read diagnostics outside processBlock. Function-local static initialization and
	// getenv() are both inappropriate on a host's real-time callback.
	m_skipStateRestore = std::getenv("PROPHECY_EDITOR_SELFTEST") != nullptr;
}

ProphecyAudioProcessor::~ProphecyAudioProcessor()
{
	m_engine.stop();
}

bool ProphecyAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const
{
	const auto &out = layouts.getMainOutputChannelSet();
	return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void ProphecyAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
	m_hostSampleRate = sampleRate > 0.0 ? sampleRate : (double) ProphecyEngine::kSampleRate;
	m_hostMidiFrameCursor = 0;
	m_oversizedAudioBlocks.store(0, std::memory_order_relaxed);
	m_editorPatchIntents.store(0, std::memory_order_relaxed);
	m_editorPatchSends.store(0, std::memory_order_relaxed);
	m_editorDumpRequests.store(0, std::memory_order_relaxed);
	m_editorDumpSends.store(0, std::memory_order_relaxed);
	m_patchLoadMidiGateUntilFrame.store(0, std::memory_order_relaxed);
	m_editorClockTicksSuppressed.store(0, std::memory_order_relaxed);
	m_patchLoadMidiEventsSuppressed.store(0, std::memory_order_relaxed);
	// samplesPerBlock is only a host hint in JUCE. Reserve a generous fixed floor so
	// ordinary offline/host block-size changes stay allocation-free; a still-larger block
	// is explicitly silenced and counted in processBlock rather than resizing there.
	m_preparedMaxBlock = std::max(samplesPerBlock, 16384);
	m_scratchL.assign((std::size_t) m_preparedMaxBlock, 0.0f);
	m_scratchR.assign((std::size_t) m_preparedMaxBlock, 0.0f);

	// Prepare the 48 kHz -> host-rate resampler (bypassed when the host runs at 48 kHz).
	const double ratio = (double) ProphecyEngine::kSampleRate / m_hostSampleRate;
	const int    cap   = (int) std::ceil(m_preparedMaxBlock * ratio) + 64;
	m_rsIn[0].assign((size_t) cap, 0.0f);
	m_rsIn[1].assign((size_t) cap, 0.0f);
	m_rsInCount = 0;
	m_resampler[0].reset();
	m_resampler[1].reset();

	// Report latency for host delay compensation: the backpressured ring stays ~full, so the
	// ring capacity (at 48k, scaled to the host rate) is the dominant latency.
	setLatencySamples((int) std::lround(m_engine.ringFrames() * sampleRate / (double) ProphecyEngine::kSampleRate));

	maybeBootEngine();
}

// Boot the engine once, iff a valid ROM set can be located (env / persisted picker
// choice / drop dir — see rom_locator.h). Without one the plugin stays silent and the
// editor shows the first-run ROM picker; the pick then boots via setRomDirFromUser().
bool ProphecyAudioProcessor::maybeBootEngine()
{
	if (m_started.load())
		return m_engine.instanceStatus() == ProphecyEngine::InstanceStatus::Active;
	const juce::File romDir = romloc::locateRomDir();
	if (romDir == juce::File())
		return false;
	if (m_started.exchange(true))
		return m_engine.instanceStatus() == ProphecyEngine::InstanceStatus::Active;
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
		// Host state can restore Wheel 2 before prepareToPlay. Pre-start engine writes are
		// intentionally rejected so an unclaimed/second processor cannot touch another
		// instance's global queue; publish the retained value once this engine owns it.
		(void) m_engine.pushAdin(9, m_wheel2Pos.load(std::memory_order_relaxed));
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
	m_audioCallbacks.fetch_add(1, std::memory_order_relaxed);
	m_audioHostFrames.fetch_add((std::uint64_t) std::max(numSamples, 0), std::memory_order_relaxed);
	if (numSamples > m_preparedMaxBlock)
	{
		// A host exceeded even our generous prepare-time reserve. Do bounded work only:
		// output silence, count the contract violation, and ignore this block's MIDI rather
		// than allocate or touch undersized resampler storage on the audio thread.
		buffer.clear();
		m_oversizedAudioBlocks.fetch_add(1, std::memory_order_relaxed);
		m_audioUnderrunFrames.fetch_add((std::uint64_t) numSamples, std::memory_order_relaxed);
		const double nativePerHost = (double)ProphecyEngine::kSampleRate / m_hostSampleRate;
		m_hostMidiFrameCursor = std::max(m_engine.producedFrames(), m_hostMidiFrameCursor)
			+ (std::uint64_t)std::llround(numSamples * nativePerHost);
		return;
	}

	// Forward every host MIDI event (note/CC/bend/sysex) into the emulated 31250-baud
	// UART at its host sample position. The engine runs ahead behind a backpressured audio
	// ring, so target native frames (not wall time) preserve both intra- and inter-block
	// timing. The hardware then adds its real serial and scan/voice-allocation latency.
	const double nativePerHost = (double)ProphecyEngine::kSampleRate / m_hostSampleRate;
	const std::uint64_t produced = m_engine.producedFrames();
	const std::uint64_t midiFrameBase = std::max(produced, m_hostMidiFrameCursor);
	const std::uint64_t hostBlockEnd = m_audioHostFrames.load(std::memory_order_relaxed);
	const std::uint64_t hostBlockStart = hostBlockEnd - (std::uint64_t)numSamples;
	for (const auto meta : midi)
	{
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
		if (hostEventFrame < m_patchLoadMidiGateUntilFrame.load(std::memory_order_acquire)
				&& suppressDuringPatchLoad(data, (std::size_t)numBytes))
		{
			if (numBytes == 1 && data[0] == 0xf8)
				m_editorClockTicksSuppressed.fetch_add(1, std::memory_order_relaxed);
			m_patchLoadMidiEventsSuppressed.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		if (numBytes == 2 && (data[0] & 0xf0) == 0xc0)
		{
			const std::uint64_t gateFrames = (std::uint64_t)std::ceil(
				m_hostSampleRate * kPatchLoadQuarantineSeconds);
			const std::uint64_t wanted = hostEventFrame + gateFrames;
			std::uint64_t previous = m_patchLoadMidiGateUntilFrame.load(std::memory_order_relaxed);
			while (previous < wanted && !m_patchLoadMidiGateUntilFrame.compare_exchange_weak(
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
		// write (X/Y pad, ribbon Z, wheel1/2) and NOT forwarded raw (double-apply). Every
		// other message — including unmapped CCs — passes through unchanged.
		if (numBytes == 3 && (data[0] & 0xf0) == 0xb0)
		{
			const int cc = data[1] & 0x7f;
			const auto tgt = (CcTarget) m_ccMap[(std::size_t) cc].load(std::memory_order_relaxed);
			if (tgt != CcTarget::Off)
			{
				handleMappedCc(cc, data[2] & 0x7f, tgt);
				continue;
			}
		}
		const auto offset = (std::uint64_t)std::llround(
			(double)meta.samplePosition * nativePerHost);
		(void) m_engine.pushMidiAtFrame(data, (std::size_t) numBytes, midiFrameBase + offset);
	}
	m_hostMidiFrameCursor = midiFrameBase + (std::uint64_t)std::llround(numSamples * nativePerHost);

	// Restore a saved patch once, after the machine is well past its ~2 s boot
	// (the 11 s threshold is a deliberate safety margin, not the boot time).
	// Skipped under the editor self-test: the injected dump's firmware apply is a modal
	// that swallows param sysex sent meanwhile, racing (and flaking) the rename check —
	// the self-test wants a deterministic clean boot. (State restore has its own console
	// coverage: PROPHOST_STATE_TEST.)
	if (!m_skipStateRestore && m_pendingReady.load() && !m_pendingInjected.load()
			&& m_engine.producedFrames() > (std::uint64_t) ProphecyEngine::kSampleRate * 11)
	{
		if (m_engine.pushMidiAtFrame(m_pendingState.data(), m_pendingState.size(), midiFrameBase))
			m_pendingInjected.store(true);
	}

	if (numChannels < 1) return;

	// -------- pass-through: host already runs at the engine's 48 kHz --------
	if (std::abs(m_hostSampleRate - (double) ProphecyEngine::kSampleRate) < 0.5)
	{
		if (numChannels >= 2)
		{
			const std::size_t got = m_engine.pull(buffer.getWritePointer(0), buffer.getWritePointer(1), (std::size_t) numSamples);
			m_audioEngineFrames.fetch_add(got, std::memory_order_relaxed);
			if (got < (std::size_t) numSamples)
				m_audioUnderrunFrames.fetch_add((std::size_t) numSamples - got, std::memory_order_relaxed);
			for (int i = (int) got; i < numSamples; ++i) { buffer.getWritePointer(0)[i] = 0.0f; buffer.getWritePointer(1)[i] = 0.0f; }
			for (int ch = 2; ch < numChannels; ++ch) buffer.clear(ch, 0, numSamples);
		}
		else
		{
			const std::size_t got = m_engine.pull(m_scratchL.data(), m_scratchR.data(), (std::size_t) numSamples);
			m_audioEngineFrames.fetch_add(got, std::memory_order_relaxed);
			if (got < (std::size_t) numSamples)
				m_audioUnderrunFrames.fetch_add((std::size_t) numSamples - got, std::memory_order_relaxed);
			float *out = buffer.getWritePointer(0);
			for (int i = 0; i < numSamples; ++i)
			{
				const auto index = (std::size_t) i;
				out[i] = (index < got) ? 0.5f * (m_scratchL[index] + m_scratchR[index]) : 0.0f;
			}
		}
		return;
	}

	// -------- resample the engine's 48 kHz stream to the host rate --------
	const double ratio = (double) ProphecyEngine::kSampleRate / m_hostSampleRate; // input per output
	const int    capN  = (int) m_rsIn[0].size();
	const int    need  = std::min((int) std::ceil(numSamples * ratio) + 4, capN);
	while (m_rsInCount < need)
	{
		const std::size_t got = m_engine.pull(m_rsIn[0].data() + m_rsInCount, m_rsIn[1].data() + m_rsInCount,
				(std::size_t) (need - m_rsInCount));
		m_audioEngineFrames.fetch_add(got, std::memory_order_relaxed);
		if (got == 0) // underrun: zero-fill the shortfall so the interpolator has input
		{
			m_audioUnderrunFrames.fetch_add((std::uint64_t) (need - m_rsInCount), std::memory_order_relaxed);
			for (int i = m_rsInCount; i < need; ++i)
			{
				const auto index = (std::size_t) i;
				m_rsIn[0][index] = 0.0f;
				m_rsIn[1][index] = 0.0f;
			}
			m_rsInCount = need;
			break;
		}
		m_rsInCount += (int) got;
	}

	const int used = m_resampler[0].process(ratio, m_rsIn[0].data(), buffer.getWritePointer(0), numSamples);
	m_resampler[1].process(ratio, m_rsIn[1].data(),
		numChannels >= 2 ? buffer.getWritePointer(1) : m_scratchR.data(), numSamples);
	for (int ch = 2; ch < numChannels; ++ch) buffer.clear(ch, 0, numSamples);

	const int rem = std::max(m_rsInCount - used, 0);
	if (rem > 0)
	{
		std::memmove(m_rsIn[0].data(), m_rsIn[0].data() + used, (size_t) rem * sizeof(float));
		std::memmove(m_rsIn[1].data(), m_rsIn[1].data() + used, (size_t) rem * sizeof(float));
	}
	m_rsInCount = rem;
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
	s.editorClockTicksSuppressed =
		m_editorClockTicksSuppressed.load(std::memory_order_relaxed);
	s.patchLoadMidiEventsSuppressed =
		m_patchLoadMidiEventsSuppressed.load(std::memory_order_relaxed);
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

	// Header + mapping (non-Off CC entries). PRP2 adds a WHEEL2 (ADIN9) rest byte after the
	// map; PRP1 (no wheel byte) still loads, defaulting the wheel to the driver rest (0x80).
	dest.append("PRP2", 4);
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
	if (data == nullptr || size <= 0) return;
	const auto *p = static_cast<const std::uint8_t *>(data);

	const std::uint8_t *dump = p;
	int dumpLen = size;
	const bool prp2 = (size >= 5 && std::memcmp(p, "PRP2", 4) == 0);
	const bool prp1 = (size >= 5 && std::memcmp(p, "PRP1", 4) == 0);
	if (prp1 || prp2)
	{
		const int n = p[4];
		int mapEnd = 5 + n * 2;
		// A blob whose declared map (+ the PRP2 wheel byte) overruns the buffer is corrupt —
		// bail entirely rather than feeding the magic bytes to the firmware as a dump.
		if (mapEnd + (prp2 ? 1 : 0) > size) return;
		for (int cc = 0; cc < 128; ++cc) setCcMap(cc, 0);
		for (int i = 0; i < n; ++i) setCcMap(p[5 + i * 2], p[5 + i * 2 + 1]);
		// PRP2 carries the WHEEL2 rest; legacy PRP1 predates it -> restore the driver default.
		setWheel2(prp2 ? p[mapEnd] : 0x80);
		if (prp2) mapEnd += 1;
		dump = p + mapEnd;
		dumpLen = size - mapEnd;
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
	// Start gating at intent time so already-debounced clicks cannot schedule more
	// external clocks ahead of the Program Change. An accepted send refreshes the
	// window to cover the complete firmware load.
	holdMidiForPatchLoad(2.5);
	m_patchSelectDelay.schedule(program);
}

bool ProphecyAudioProcessor::sendPatchNow(int program)
{
	// A quiet-click debounce is not enough: two individually valid Program
	// Changes can still overlap the firmware's long inter-board load transaction.
	// PatchSelectDelay will retry, retaining only its latest program, until the
	// prior load has completed.
	const double now = juce::Time::getMillisecondCounterHiRes();
	if (now - m_lastPatchSendMs < kPatchSelectMinIntervalMs)
	{
		// Keep both barriers closed while PatchSelectDelay retains the latest
		// requested program and waits for the previous transaction to settle.
		holdMidiForPatchLoad(0.2);
		m_editorCommandPacer.extendPatchLoad(200);
		return false;
	}
	// Bank select then program change (verified on the emulated firmware via the LCD:
	// a bare 0xC0 only ever reaches bank A; CC0=0 + CC32=bank + 0xC0 lands "B52:...").
	const std::uint8_t msg[8] = {
		0xB0, 0x00, 0x00,                          // bank select MSB
		0xB0, 0x20, (std::uint8_t) (program / 64), // bank select LSB: 0=A, 1=B
		0xC0, (std::uint8_t) (program % 64) };     // program within the bank
	const bool accepted = pushImmediateMidi(msg, sizeof(msg));
	if (accepted)
	{
		holdMidiForPatchLoad(kPatchLoadQuarantineSeconds);
		m_editorCommandPacer.extendPatchLoad(
			(int)std::lround(kPatchLoadQuarantineSeconds * 1000.0));
		m_lastPatchSendMs = now;
		m_editorPatchSends.fetch_add(1, std::memory_order_relaxed);
	}
	return accepted;
}

void ProphecyAudioProcessor::holdMidiForPatchLoad(double seconds)
{
	const std::uint64_t current = m_audioHostFrames.load(std::memory_order_relaxed);
	const std::uint64_t wanted = current
		+ (std::uint64_t)std::ceil(std::max(seconds, 0.0) * m_hostSampleRate);
	std::uint64_t previous = m_patchLoadMidiGateUntilFrame.load(std::memory_order_relaxed);
	while (previous < wanted && !m_patchLoadMidiGateUntilFrame.compare_exchange_weak(
		previous, wanted, std::memory_order_release, std::memory_order_relaxed)) {}
}

bool ProphecyAudioProcessor::suppressDuringPatchLoad(
	const std::uint8_t *bytes, std::size_t size)
{
	if (bytes == nullptr || size == 0) return false;
	const std::uint8_t status = bytes[0];
	// Note releases must always pass so a selection cannot create a stuck note.
	// Other channel voice messages and SysEx create firmware/control-link work and
	// are held out of the bounded patch-load transaction. System transport other
	// than MIDI clock remains safe and useful to forward.
	if ((status & 0xf0) == 0x80) return false;
	if ((status & 0xf0) == 0x90 && size >= 3 && bytes[2] == 0) return false;
	if (status < 0xf0 || status == 0xf0 || status == 0xf8) return true;
	return false;
}

void ProphecyAudioProcessor::sendMidi(const std::uint8_t *bytes, std::size_t size)
{
	if (patchLoadMidiGateActive() && suppressDuringPatchLoad(bytes, size))
	{
		if (size == 1 && bytes[0] == 0xf8)
			m_editorClockTicksSuppressed.fetch_add(1, std::memory_order_relaxed);
		m_patchLoadMidiEventsSuppressed.fetch_add(1, std::memory_order_relaxed);
		return;
	}
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
	constexpr double patchSettleMs = kPatchLoadQuarantineSeconds * 1000.0;
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
		if (!m_padYHeld) (void) pushUiAdin(14, 0x00);
	}
	if (old == CcTarget::PadY && newT != CcTarget::PadY && m_padYHeld.exchange(false))
	{
		(void) pushUiAdin(13, 0x74);
		if (!m_padXHeld) (void) pushUiAdin(14, 0x00);
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
	if (value < 0)   value = 0;
	if (value > 255) value = 255;
	m_wheel2Pos.store((std::uint8_t) value, std::memory_order_relaxed);
	(void) pushUiAdin(9, value);
}

void ProphecyAudioProcessor::setWheel2FromEditor(int value)
{
	if (value < 0) value = 0;
	if (value > 255) value = 255;
	m_wheel2Pos.store((std::uint8_t)value, std::memory_order_relaxed);
	m_editorCommandPacer.enqueueAdin(9, value);
}

void ProphecyAudioProcessor::setAdin(int source, int value)
{
	if (source < 0 || source > 15) return;
	m_editorCommandPacer.enqueueAdin(source, std::clamp(value, 0, 255));
}

// Translate a mapped control-change to a front-panel ADIN write. Mirrors the editor's own
// X-Y handler: pad X = ADIN12 (rest 0x80), pad Y = ADIN13 (rest 0x74), touch gate = ADIN14
// (0xFF while a pad CC is held, 0x00 once neither axis is held — many patches ignore X/Y
// without the gate), ribbon Z = ADIN14, wheel1 = ADIN8, wheel2 = ADIN9. CC 0..127 scales to
// 0..255. Audio-thread only.
void ProphecyAudioProcessor::handleMappedCc(int cc, int value, CcTarget target)
{
	juce::ignoreUnused(cc);
	const int s = (value * 255 + 63) / 127; // 0..127 -> 0..255 (rounded), matches editor
	switch (target)
	{
	case CcTarget::PadX:
		if (value > 0) { (void) m_engine.pushAdinFromAudio(12, s); m_padXHeld = true; (void) m_engine.pushAdinFromAudio(14, 0xFF); }
		else           { (void) m_engine.pushAdinFromAudio(12, 0x80); m_padXHeld = false; if (!m_padYHeld) (void) m_engine.pushAdinFromAudio(14, 0x00); }
		break;
	case CcTarget::PadY:
		if (value > 0) { (void) m_engine.pushAdinFromAudio(13, s); m_padYHeld = true; (void) m_engine.pushAdinFromAudio(14, 0xFF); }
		else           { (void) m_engine.pushAdinFromAudio(13, 0x74); m_padYHeld = false; if (!m_padXHeld) (void) m_engine.pushAdinFromAudio(14, 0x00); }
		break;
	case CcTarget::RibbonZ: (void) m_engine.pushAdinFromAudio(14, s); break; // Z pressure / touch gate directly
	case CcTarget::Wheel1:  (void) m_engine.pushAdinFromAudio(8,  s); break;
	case CcTarget::Wheel2:  (void) m_engine.pushAdinFromAudio(9,  s); break;
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
					const std::uint32_t ver = m_proc.ledSnapshot(banks);
					auto *obj = new juce::DynamicObject();
					obj->setProperty("version", (int) ver);
					juce::Array<juce::var> arr;
					for (int i = 0; i < 12; i++) arr.add((int) banks[i]);
					obj->setProperty("banks", std::move(arr));
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
