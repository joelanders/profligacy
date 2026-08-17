// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginProcessor.h - the JUCE AudioProcessor. Includes ONLY the MAME-free engine
// header (prophecy_engine.h) + JUCE. MAME runs on the engine's worker thread; the
// host's audio callback pulls stereo blocks from the ring.
//
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "prophecy_engine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

class ProphecyAudioProcessor : public juce::AudioProcessor
{
public:
	ProphecyAudioProcessor();
	~ProphecyAudioProcessor() override;

	void prepareToPlay(double sampleRate, int samplesPerBlock) override;
	void releaseResources() override {}
	bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
	void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;

	// Headless timing-harness seam. Capture must be enabled before prepareToPlay()
	// starts the engine; ordinary plugin instances leave it disabled.
	bool enableMidiTxByteCapture(bool enabled = true)
	{ return m_engine.enableMidiTxByteCapture(enabled); }
	std::size_t popMidiTxByteEvents(ProphecyEngine::MidiTxByteEvent *out, std::size_t cap)
	{ return m_engine.popMidiTxByteEvents(out, cap); }
	std::uint64_t droppedMidiTxByteEvents() const
	{ return m_engine.droppedMidiTxByteEvents(); }
	std::uint64_t droppedScheduledMidiBytes() const
	{ return m_engine.droppedScheduledMidiBytes(); }
	std::uint64_t droppedImmediateMidiBytes() const
	{ return m_engine.droppedImmediateMidiBytes()
		+ m_contendedImmediateMidiBytes.load(std::memory_order_relaxed); }
	std::uint64_t droppedUiAdinEvents() const
	{ return m_engine.droppedUiAdinEvents()
		+ m_contendedUiAdinEvents.load(std::memory_order_relaxed); }
	std::uint64_t droppedAudioAdinEvents() const
	{ return m_engine.droppedAudioAdinEvents(); }
	std::uint64_t oversizedAudioBlocks() const
	{ return m_oversizedAudioBlocks.load(std::memory_order_relaxed); }
	struct DiagnosticSnapshot
	{
		std::uint64_t producedFrames = 0;
		std::size_t bufferedFrames = 0;
		bool engineRunning = false;
		std::uint64_t audioCallbacks = 0;
		std::uint64_t audioHostFrames = 0;
		std::uint64_t audioEngineFrames = 0;
		std::uint64_t audioUnderrunFrames = 0;
		std::uint64_t hostMidiEvents = 0;
		std::uint32_t lastHostMidi = 0;
		std::uint64_t activeNotesLow = 0;
		std::uint64_t activeNotesHigh = 0;
		std::uint64_t droppedImmediateMidiBytes = 0;
		std::uint64_t droppedScheduledMidiBytes = 0;
		std::uint64_t droppedUiAdinEvents = 0;
		std::uint64_t droppedAudioAdinEvents = 0;
		std::uint64_t oversizedBlocks = 0;
		std::uint64_t editorPatchIntents = 0;
		std::uint64_t editorPatchSends = 0;
		std::uint64_t editorDumpRequests = 0;
		std::uint64_t editorDumpSends = 0;
		std::uint64_t editorClockTicksSuppressed = 0;
		std::uint64_t patchLoadMidiEventsSuppressed = 0;
		std::uint64_t editorCommandsSent = 0;
		std::uint64_t editorCommandsCoalesced = 0;
		std::uint64_t editorCommandsCancelled = 0;
		std::uint64_t editorCommandsDropped = 0;
		std::size_t editorCommandsPending = 0;
	};
	DiagnosticSnapshot diagnosticSnapshot() const;

	juce::AudioProcessorEditor *createEditor() override;
	bool hasEditor() const override { return true; }

	const juce::String getName() const override { return "Profligacy"; }
	bool acceptsMidi() const override { return true; }
	bool producesMidi() const override { return false; }
	bool isMidiEffect() const override { return false; }
	double getTailLengthSeconds() const override { return 0.0; }

	int getNumPrograms() override { return 1; }
	int getCurrentProgram() override { return 0; }
	void setCurrentProgram(int) override {}
	const juce::String getProgramName(int) override { return "Default"; }
	void changeProgramName(int, const juce::String &) override {}

	void getStateInformation(juce::MemoryBlock &) override;
	void setStateInformation(const void *, int) override;

	// exposed for the editor
	double hostSampleRate() const { return m_hostSampleRate; }
	// ROM picker: where the engine's firmware comes from. If no valid ROM set is found
	// the engine stays unbooted and the editor shows the first-run picker.
	bool romOk() const { return m_engine.instanceStatus() == ProphecyEngine::InstanceStatus::Active; }
	bool instanceUnavailable() const
	{
		return m_engine.instanceStatus() == ProphecyEngine::InstanceStatus::Unavailable;
	}
	juce::String romPath() const { return m_romPath; }
	bool setRomDirFromUser(const juce::File &dir); // validate + persist + boot; false if invalid
	bool maybeBootEngine();                        // boot once iff a valid ROM set is locatable
	void selectPatch(int program);       // bank-select + program change (0..127 = A00..B63)
	// Raw MIDI from the editor (faceplate keybed note on/off etc.) into the emulated UART.
	void sendMidi(const std::uint8_t *bytes, std::size_t n);
	// Faceplate front-panel controls, through the engine's host seams (drained on the
	// MAME thread at 1 kHz; SCANQ reports pulses to the firmware as real scan codes).
	void panelPulse(int row, int bit);
	void setAdin(int source, int value);
	// CC -> analog-controller remap. Host control-change (0xB0) messages whose CC number is
	// mapped are translated to a front-panel ADIN write (X/Y pad, ribbon Z, wheel1/2) instead
	// of being forwarded raw. Target enum matches the editor's dropdown order.
	enum class CcTarget : std::uint8_t { Off = 0, PadX, PadY, RibbonZ, Wheel1, Wheel2 };
	void setCcMap(int cc, int target);           // message thread
	int  ccMapTarget(int cc) const;              // message thread (UI init / state)
	// WHEEL2 (ADIN9) resting position. The Prophecy's second wheel is a FRICTION wheel with
	// no sprung rest, so its "position" is a user choice, not a fixed constant — and several
	// patches route it to level/timbre, so its assumed position materially changes the sound.
	// Persisted in the plugin state and applied as an ADIN9 write. Default 0x80 matches the
	// driver's built-in ADIN9 rest, so an unset/legacy session sounds byte-for-byte identical.
	void setWheel2(int value);                   // message thread: stores + pushes ADIN9 (0..255)
	void setWheel2FromEditor(int value);         // editor path: stores + uses the shared pacer
	int  wheel2Pos() const { return m_wheel2Pos.load(std::memory_order_relaxed); } // 0..255
	std::uint32_t ledSnapshot(std::uint8_t out[12]) const { return m_engine.ledSnapshot(out); }
	std::uint32_t lcdRawSnapshot(std::uint8_t r1[40], std::uint8_t r2[40], std::uint8_t cg[64]) const
	{ return m_engine.lcdRawSnapshot(r1, r2, cg); }
	// 128 patch names (A00..B63) read from the sysram NVRAM file; empty when unavailable
	// (no nvram yet, blank sysram). Message-thread only.
	juce::StringArray patchNames() const;
	void setParam(int paramId, int value); // 0x41 param-change sysex (program group)
	void setParamG(int group, int paramId, int value); // group-aware variant (0 = global, e.g. p170 protect)
	// Rename the edit buffer: Program Name Char params are group-1 p1..p16 (ascii_char),
	// space-padded to 16. The rename lives in the edit buffer until a WRITE commits it.
	void renamePatch(const juce::String &name);
	// Quick-init patch-shaping macros (Init / Saw / Filter THRU / Bypass FX), ported from
	// the MAME-tree GUI (src/tools/korgprophecy_gui + scripts/korgprophecy_macros.py). Each
	// is a batch of Parameter Change writes delivered through a paced ParamBurst (the
	// firmware drops back-to-back sysex). OSC1-specific params are ExID-packed (paramId =
	// (1<<12)|param); a p154 (OSC Set) write, if present, is re-asserted LAST so the live
	// DSP1 engine reloads cleanly (a std-osc write right after a reconfig gets clobbered).
	void sendMacro(const juce::String &name);
	// Commit the edit buffer to its program slot, exactly as the hardware does it:
	// unprotect (global p170=0) -> WRITE (panel row 0 bit 0) -> ENTER to select the
	// target -> ENTER to confirm -> re-protect. The firmware refuses the write while
	// protected ("*WRITE ERROR<Program Memory is protected>"), so the unprotect is
	// mandatory, not optional. Scheduled on
	// a timer because the panel pulses must land in order, ~0.4 s apart, on the MAME
	// thread's scan cadence. Verified end-to-end (name reaches sysram).
	void writePatch();
	bool writeInProgress() const { return m_writeInProgress.load(std::memory_order_acquire); }
	// Copy the emulator's live HD44780 text (two NUL-terminated lines, >= 41 bytes each) for the
	// editor. Message-thread only. Returns false if the firmware has not drawn anything yet.
	bool getLcd(char *line1, char *line2, std::size_t cap) const { return m_engine.latestLcd(line1, line2, cap); }
	// Param read-back: ask the firmware for the current-program dump (its 0x40 reply is captured +
	// unpacked by the engine); the editor then reads knob values out of the raw program bytes.
	std::uint64_t requestProgramDump(); // returns the coalesced/retried transaction generation
	std::size_t getProgramData(std::uint8_t *out, std::size_t cap, std::uint32_t *version,
		std::uint64_t *completedRequestGeneration = nullptr) const;
	// Arpeggiator editor transport. Pattern numbers are the ten INT PAT slots:
	// 0..4 = UP/DOWN/ALT1/ALT2/RANDOM, 5..9 = PAT1..PAT5.
	void selectArpeggioPattern(int pattern);                 // documented NRPN 00/01
	void setArpeggiatorControl(int control, int value);      // NRPN LSB 2..5
	void setPatternParam(int paramId, int value) { setParamG(2, paramId, value); }
	void requestArpeggioPatternDump(int pattern);            // 0x34 request
	void sendArpeggioPatternData(int pattern, const std::vector<std::uint8_t> &raw); // 0x69 load
	std::size_t getArpeggioPatternData(std::uint8_t *out, std::size_t cap,
		std::uint32_t *version, int *pattern) const
	{ return m_engine.latestArpeggioPatternData(out, cap, version, pattern); }

private:
	// Drives writePatch()'s unprotect -> WRITE -> ENTER -> ENTER -> re-protect sequence off the
	// message thread's timer, so the firmware sees the panel pulses as distinct presses.
	class WriteSequence : private juce::Timer
	{
	public:
		explicit WriteSequence(ProphecyAudioProcessor &p) : m_proc(p) {}
		~WriteSequence() override { stopTimer(); }
		void start() { if (!isTimerRunning()) { m_step = 0; startTimer(400); } }
	private:
		void timerCallback() override;
		ProphecyAudioProcessor &m_proc;
		int m_step = 0;
	};
	WriteSequence      m_writeSeq { *this };
	std::atomic<bool>  m_writeInProgress { false };

	// Paced param-burst sender: the firmware drops sysex messages that arrive
	// back-to-back at line rate while it is busy (verified: mid-burst rename chars
	// lost with a Motion patch in the edit buffer). One param per tick, the way a
	// real editor paces sysex. A new start() replaces any burst still in flight.
	class ParamBurst : private juce::Timer
	{
	public:
		explicit ParamBurst(ProphecyAudioProcessor &p) : m_proc(p) {}
		~ParamBurst() override { stopTimer(); }
		void start(std::vector<std::pair<int, int>> paramValues, int intervalMs = 15)
		{
			stopTimer();
			m_items = std::move(paramValues);
			m_next = 0;
			startTimer(intervalMs);
		}
		bool cancel()
		{
			const bool wasRunning = isTimerRunning();
			stopTimer();
			m_items.clear();
			m_next = 0;
			return wasRunning;
		}
	private:
		void timerCallback() override
		{
			if (m_next >= m_items.size()) { stopTimer(); return; }
			m_proc.setParam(m_items[m_next].first, m_items[m_next].second);
			m_next++;
		}
		ProphecyAudioProcessor &m_proc;
		std::vector<std::pair<int, int>> m_items;
		std::size_t m_next = 0;
	};
	ParamBurst         m_renameBurst { *this };
	ParamBurst         m_macroBurst  { *this }; // quick-init patch macros (sendMacro)

	// All non-musical editor mutations share one bounded, coalescing path. The
	// firmware can wedge its internal H8/V55 link if a patch load overlaps a burst
	// of independent editor commands even though the host MIDI rings never drop a
	// byte. Musical note input remains immediate; edit/control work is paced at a
	// hardware-scale interval and held behind the patch-load barrier.
	class EditorCommandPacer : private juce::Timer
	{
	public:
		explicit EditorCommandPacer(ProphecyAudioProcessor &p) : m_proc(p) {}
		~EditorCommandPacer() override { stopTimer(); }
		void holdForPatchLoad(int milliseconds)
		{
			m_cancelled.fetch_add(m_queue.size(), std::memory_order_relaxed);
			m_queue.clear();
			m_holdUntilMs = juce::Time::getMillisecondCounterHiRes()
				+ (double)std::max(milliseconds, 0);
			startTimer(10);
		}
		void extendPatchLoad(int milliseconds)
		{
			m_holdUntilMs = std::max(m_holdUntilMs,
				juce::Time::getMillisecondCounterHiRes()
					+ (double)std::max(milliseconds, 0));
			if (!isTimerRunning()) startTimer(10);
		}
		void enqueueMidi(int key, const std::uint8_t *bytes, std::size_t size)
		{
			if (bytes == nullptr || size == 0) return;
			for (auto it = m_queue.rbegin(); it != m_queue.rend(); ++it)
			{
				if (key >= 0 && it->kind == Command::Kind::Midi && it->key == key)
				{
					it->bytes.assign(bytes, bytes + size);
					m_coalesced.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			if (m_queue.size() >= kCapacity)
			{
				m_dropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			Command command;
			command.kind = Command::Kind::Midi;
			command.key = key;
			command.bytes.assign(bytes, bytes + size);
			m_queue.push_back(std::move(command));
			if (!isTimerRunning()) startTimer(1);
		}
		void enqueuePanel(int row, int bit)
		{
			if (m_queue.size() >= kCapacity)
			{
				m_dropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			Command command;
			command.kind = Command::Kind::Panel;
			command.a = row;
			command.b = bit;
			m_queue.push_back(std::move(command));
			if (!isTimerRunning()) startTimer(1);
		}
		void enqueueAdin(int source, int value)
		{
			for (auto it = m_queue.rbegin(); it != m_queue.rend(); ++it)
			{
				if (it->kind == Command::Kind::Adin && it->key == source)
				{
					it->b = value;
					m_coalesced.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			if (m_queue.size() >= kCapacity)
			{
				m_dropped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			Command command;
			command.kind = Command::Kind::Adin;
			command.key = source;
			command.a = source;
			command.b = value;
			m_queue.push_back(std::move(command));
			if (!isTimerRunning()) startTimer(1);
		}
		bool busy() const
		{
			return !m_queue.empty()
				|| juce::Time::getMillisecondCounterHiRes() < m_holdUntilMs
				|| m_proc.patchLoadMidiGateActive();
		}
		std::uint64_t sent() const { return m_sent.load(std::memory_order_relaxed); }
		std::uint64_t coalesced() const { return m_coalesced.load(std::memory_order_relaxed); }
		std::uint64_t cancelled() const { return m_cancelled.load(std::memory_order_relaxed); }
		std::uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }
		std::size_t pending() const { return m_queue.size(); }
	private:
		struct Command
		{
			enum class Kind { Midi, Panel, Adin } kind = Kind::Midi;
			int key = -1;
			int a = 0, b = 0;
			std::vector<std::uint8_t> bytes;
		};
		void timerCallback() override
		{
			const double now = juce::Time::getMillisecondCounterHiRes();
			if (m_proc.patchLoadMidiGateActive())
			{
				// DAW Program Changes extend the frame-based gate on the audio
				// callback. Observe that atomic gate here instead of starting or
				// touching a JUCE timer from the real-time thread.
				startTimer(25);
				return;
			}
			if (now < m_holdUntilMs)
			{
				startTimer(std::max(1, (int)std::ceil(m_holdUntilMs - now)));
				return;
			}
			if (m_queue.empty()) { stopTimer(); return; }
			Command &command = m_queue.front();
			bool accepted = true;
			if (command.kind == Command::Kind::Midi)
				accepted = m_proc.pushImmediateMidi(command.bytes.data(), command.bytes.size());
			else if (command.kind == Command::Kind::Panel)
				m_proc.m_engine.pushPanelPulse(command.a, command.b);
			else
				accepted = m_proc.pushUiAdin(command.a, command.b);
			if (accepted)
			{
				m_queue.pop_front();
				m_sent.fetch_add(1, std::memory_order_relaxed);
			}
			startTimer(accepted ? kIntervalMs : 25);
		}
		static constexpr std::size_t kCapacity = 256;
		static constexpr int kIntervalMs = 75;
		ProphecyAudioProcessor &m_proc;
		std::deque<Command> m_queue;
		double m_holdUntilMs = -1.0e9;
		std::atomic<std::uint64_t> m_sent { 0 };
		std::atomic<std::uint64_t> m_coalesced { 0 };
		std::atomic<std::uint64_t> m_cancelled { 0 };
		std::atomic<std::uint64_t> m_dropped { 0 };
	};
	EditorCommandPacer m_editorCommandPacer { *this };
	// Program loading plus concurrent MIDI can wedge the emulated H8/V55 control
	// link even when every host-side byte was accepted. Treat loading as an atomic
	// transaction: releases always pass, while new musical/control work waits for a
	// bounded host-frame window. This covers editor and DAW Program Changes.
	void holdMidiForPatchLoad(double seconds);
	bool patchLoadMidiGateActive() const
	{
		return m_audioHostFrames.load(std::memory_order_relaxed)
			< m_patchLoadMidiGateUntilFrame.load(std::memory_order_acquire);
	}
	static bool suppressDuringPatchLoad(const std::uint8_t *bytes, std::size_t size);
	static constexpr double kPatchLoadQuarantineSeconds = 2.0;
	static constexpr double kPatchSelectMinIntervalMs = 2500.0;
	bool sendPatchNow(int program);
	class PatchSelectDelay : private juce::Timer
	{
	public:
		explicit PatchSelectDelay(ProphecyAudioProcessor &p) : m_proc(p) {}
		~PatchSelectDelay() override { stopTimer(); }
		bool pending() const { return isTimerRunning(); }
		void schedule(int program)
		{
			m_program = program;
			startTimer(500);
		}
	private:
		void timerCallback() override
		{
			stopTimer();
			if (!m_proc.sendPatchNow(m_program)) startTimer(25);
		}
		ProphecyAudioProcessor &m_proc;
		int m_program = 0;
	};
	PatchSelectDelay   m_patchSelectDelay { *this };

	// Current-program dumps share the firmware MIDI task with Program Change. Keep one
	// editor request in flight, wait for a recently selected patch to finish loading,
	// and retry a request the firmware discarded while busy. The WebView and headless
	// editor-stress scenario both enter through requestProgramDump(), so this is the one
	// production scheduling policy rather than a second test-only approximation.
	class ProgramDumpSync : private juce::Timer
	{
	public:
		explicit ProgramDumpSync(ProphecyAudioProcessor &p) : m_proc(p) {}
		~ProgramDumpSync() override { stopTimer(); }
		std::uint64_t request(int delayMs)
		{
			std::uint8_t byte = 0;
			std::uint32_t currentVersion = 0;
			m_proc.getProgramData(&byte, 1, &currentVersion);
			if (isTimerRunning())
			{
				// Reuse a genuinely pending transaction, but do not suppress a new manual
				// refresh merely because the completed transaction's 800 ms check has not run.
				if (!m_sent.load(std::memory_order_acquire)
						|| currentVersion <= m_baselineVersion.load(std::memory_order_acquire))
					return m_generation.load(std::memory_order_acquire);
				stopTimer();
			}
			m_baselineVersion.store(currentVersion, std::memory_order_release);
			m_generation.store(m_nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1,
				std::memory_order_release);
			m_attempts = 0;
			m_sent.store(false, std::memory_order_release);
			if (delayMs > 0) startTimer(delayMs);
			else sendRequest();
			return m_generation.load(std::memory_order_acquire);
		}
		void cancel()
		{
			stopTimer();
			m_attempts = 0;
			m_sent.store(false, std::memory_order_release);
		}
		std::uint64_t observe(std::uint32_t version) const
		{
			const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
			if (generation != 0 && m_sent.load(std::memory_order_acquire)
					&& version > m_baselineVersion.load(std::memory_order_acquire))
			{
				std::uint64_t completed = m_completedGeneration.load(std::memory_order_relaxed);
				while (completed < generation && !m_completedGeneration.compare_exchange_weak(
					completed, generation, std::memory_order_release, std::memory_order_relaxed)) {}
			}
			return m_completedGeneration.load(std::memory_order_acquire);
		}
	private:
		void sendRequest()
		{
			if (m_proc.m_editorCommandPacer.busy()) { startTimer(100); return; }
			const bool firstAttempt = m_attempts == 0;
			++m_attempts;
			(void)m_proc.sendProgramDumpNow();
			if (firstAttempt)
			{
				// A request may have waited behind a patch load for seconds. Refresh
				// the baseline immediately after the causal enqueue boundary so an
				// obsolete reply that arrived during that delay cannot complete this
				// generation. The firmware cannot round-trip the new serial request
				// before this same-thread snapshot.
				std::uint8_t byte = 0;
				std::uint32_t version = 0;
				m_proc.getProgramData(&byte, 1, &version);
				m_baselineVersion.store(version, std::memory_order_release);
			}
			m_sent.store(true, std::memory_order_release);
			startTimer(800);
		}
		void timerCallback() override
		{
			if (!m_sent.load(std::memory_order_acquire)) { sendRequest(); return; }
			std::uint8_t byte = 0;
			std::uint32_t version = 0;
			m_proc.getProgramData(&byte, 1, &version);
			if (observe(version) >= m_generation.load(std::memory_order_acquire))
				{ stopTimer(); return; }
			if (m_attempts >= 3) { stopTimer(); return; }
			sendRequest();
		}
		ProphecyAudioProcessor &m_proc;
		std::atomic<std::uint32_t> m_baselineVersion { 0 };
		int m_attempts = 0;
		std::atomic<bool> m_sent { false };
		std::atomic<std::uint64_t> m_nextGeneration { 0 };
		std::atomic<std::uint64_t> m_generation { 0 };
		mutable std::atomic<std::uint64_t> m_completedGeneration { 0 };
	};
	ProgramDumpSync    m_programDumpSync { *this };
	bool sendProgramDumpNow();
	double             m_lastPatchSendMs = -1.0e9;

	// CC->ADIN remap table (target per CC number; 0 = Off = pass through raw). Written on
	// the message thread, read on the audio thread. m_padXHeld/m_padYHeld gate ADIN14
	// (touch) 0xFF while a mapped pad CC is held, mirroring the editor's X-Y
	// touch/release behavior; the audio thread is the normal writer, and setCcMap
	// clears them (message thread) when a pad mapping is removed so a remap-while-held
	// can't wedge the gate — hence atomics.
	std::array<std::atomic<std::uint8_t>, 128> m_ccMap {};
	std::atomic<bool> m_padXHeld { false }, m_padYHeld { false };
	// WHEEL2 (ADIN9) rest position, persisted. 0x80 == the driver's built-in ADIN9 default,
	// so a fresh session that never touches this is bit-identical to pre-parameter behavior.
	std::atomic<std::uint8_t> m_wheel2Pos { 0x80 };
	void handleMappedCc(int cc, int value, CcTarget target); // audio thread
	// Host state callbacks are not guaranteed to share the JUCE message thread with the
	// editor. Serialize those non-RT producers with one try only: contention drops the
	// complete message/event and increments a metric. processBlock uses separate rings and
	// never touches these flags, so the audio thread cannot block or spin behind UI work.
	std::atomic_flag m_immediateMidiProducer = ATOMIC_FLAG_INIT;
	std::atomic_flag m_uiAdinProducer = ATOMIC_FLAG_INIT;
	std::atomic<std::uint64_t> m_contendedImmediateMidiBytes { 0 };
	std::atomic<std::uint64_t> m_contendedUiAdinEvents { 0 };
	bool pushImmediateMidi(const std::uint8_t *bytes, std::size_t n)
	{
		if (m_immediateMidiProducer.test_and_set(std::memory_order_acquire))
		{
			m_contendedImmediateMidiBytes.fetch_add(n, std::memory_order_relaxed);
			return false;
		}
		const bool accepted = m_engine.pushMidi(bytes, n);
		m_immediateMidiProducer.clear(std::memory_order_release);
		return accepted;
	}
	bool pushUiAdin(int source, int value)
	{
		if (m_uiAdinProducer.test_and_set(std::memory_order_acquire))
		{
			m_contendedUiAdinEvents.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		const bool accepted = m_engine.pushAdin(source, value);
		m_uiAdinProducer.clear(std::memory_order_release);
		return accepted;
	}

	ProphecyEngine     m_engine;
	std::atomic<bool>  m_started { false };
	juce::String       m_romPath;             // resolved ROM dir once booted
	juce::String       m_nvramPath;           // resolved NVRAM dir once booted
	double             m_hostSampleRate = 48000.0;
	std::uint64_t      m_hostMidiFrameCursor = 0;
	int                m_preparedMaxBlock = 0;
	std::vector<float> m_scratchL, m_scratchR; // mono/resampler output; allocated in prepare
	std::atomic<std::uint64_t> m_oversizedAudioBlocks { 0 };
	// Lock-free counters sampled by the optional GUI diagnostic logger. The audio callback
	// only updates atomics; all formatting and file I/O stays on the message thread.
	std::atomic<std::uint64_t> m_audioCallbacks { 0 };
	std::atomic<std::uint64_t> m_audioHostFrames { 0 };
	std::atomic<std::uint64_t> m_audioEngineFrames { 0 };
	std::atomic<std::uint64_t> m_audioUnderrunFrames { 0 };
	std::atomic<std::uint64_t> m_hostMidiEvents { 0 };
	std::atomic<std::uint32_t> m_lastHostMidi { 0 };
	std::atomic<std::uint64_t> m_editorPatchIntents { 0 };
	std::atomic<std::uint64_t> m_editorPatchSends { 0 };
	std::atomic<std::uint64_t> m_editorDumpRequests { 0 };
	std::atomic<std::uint64_t> m_editorDumpSends { 0 };
	std::atomic<std::uint64_t> m_patchLoadMidiGateUntilFrame { 0 };
	std::atomic<std::uint64_t> m_editorClockTicksSuppressed { 0 };
	std::atomic<std::uint64_t> m_patchLoadMidiEventsSuppressed { 0 };
	std::array<std::atomic<std::uint64_t>, 2> m_activeHostNotes {};
	bool               m_skipStateRestore = false;

	// host!=48k resampling (engine is authoritative at 48 kHz)
	juce::LagrangeInterpolator m_resampler[2];
	std::vector<float>         m_rsIn[2];       // leftover 48k input, preallocated
	int                        m_rsInCount = 0;

	// DAW patch persistence (current-program SysEx dump). setState stashes a dump to inject
	// once the machine has booted; processBlock does the one-shot injection. The standalone
	// intentionally ignores this dump and boots from explicitly written synth NVRAM instead.
	std::vector<std::uint8_t> m_pendingState;
	std::atomic<bool>         m_pendingReady { false };
	std::atomic<bool>         m_pendingInjected { false };

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProphecyAudioProcessor)
};
