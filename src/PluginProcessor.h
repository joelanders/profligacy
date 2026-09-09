// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginProcessor.h - the JUCE AudioProcessor. Includes ONLY the MAME-free engine
// header (prophecy_engine.h) + JUCE. MAME runs on the engine's worker thread; the
// host's audio callback pulls stereo blocks from the ring.
//
#pragma once

#include "audio_timeline.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include "prophecy_engine.h"
#include "program_control.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

class ProphecyAudioProcessor : public juce::AudioProcessor
{
public:
	ProphecyAudioProcessor();
	~ProphecyAudioProcessor() override;

	void prepareToPlay(double sampleRate, int samplesPerBlock) override;
	void releaseResources() override;
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
	{ return m_engine.droppedImmediateMidiBytes(); }
	std::uint64_t droppedUiAdinEvents() const
	{ return m_engine.droppedUiAdinEvents()
		+ m_contendedUiAdinEvents.load(std::memory_order_relaxed); }
	std::uint64_t droppedAudioAdinEvents() const
	{ return m_engine.droppedAudioAdinEvents() + m_engine.droppedScheduledAdinEvents(); }
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
		std::uint64_t hostMidiEventsForwarded = 0;
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
	bool romOk() const { return m_engine.readyForPlayback(); }
	std::string programError() const { return m_programControl.error(); }
	const char* initializationError() const { return m_engine.initializationError(); }
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
	// mapped are translated to a front-panel ADIN write (the editor's composite X-Y control,
	// ribbon Z, or wheel 1/2) instead of being forwarded raw. The X-Y control combines the
	// physical ribbon X input with the separate spring-centered Log/Wheel 3 controller.
	// Target enum matches the editor's dropdown order.
	enum class CcTarget : std::uint8_t { Off = 0, PadX, PadY, RibbonZ, Wheel1, Wheel2 };
	void setCcMap(int cc, int target);           // message thread
	int  ccMapTarget(int cc) const;              // message thread (UI init / state)
	// WHEEL2 (ADIN9) resting position. The Prophecy's second wheel is a FRICTION wheel with
	// no sprung rest, so its "position" is a user choice, not a fixed constant — and several
	// patches route it to level/timbre, so its assumed position materially changes the sound.
	// Persisted in the plugin state and applied as an ADIN9 write. Default 0x80 matches the
	// driver's built-in ADIN9 rest, so an unset/legacy session sounds byte-for-byte identical.
	void setWheel2(int value);                   // message thread: stores + pushes ADIN9 (0..255)
	void setWheel2FromEditor(int value);         // editor path: stores + pushes physical ADIN directly
	int  wheel2Pos() const { return m_wheel2Pos.load(std::memory_order_relaxed); } // 0..255
	// Latest host-observed performance-controller input for editor display. This is not an
	// engine acknowledgement: raw MIDI bend/CC remains on the synth's UART, while mapped CC
	// and panel gestures request ADIN writes. Atomics keep audio-thread observation lock-free;
	// each array element is an independent latest-value display sample.
	void controllerDisplaySnapshot(std::uint8_t out[16]) const;
	std::uint32_t ledSnapshot(std::uint8_t out[12]) const { return m_engine.ledSnapshot(out); }
	std::uint32_t ledVisualSnapshot(std::uint8_t out[12]) const { return m_engine.ledVisualSnapshot(out); }
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
	// is accepted as one ordered semantic edit sequence. OSC1-specific params are ExID-packed (paramId =
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
	std::array<std::atomic<std::uint8_t>, 16> m_controllerDisplayValues {};
	void publishControllerDisplayValue(int source, int value);
	void handleMappedCc(int cc, int value, CcTarget target, std::uint64_t frame); // audio thread
	// Host state callbacks are not guaranteed to share the JUCE message thread with the
	// editor. Serialize those non-RT producers with one try only: contention drops the
	// complete message/event and increments a metric. processBlock uses separate rings and
	// never touches these flags, so the audio thread cannot block or spin behind UI work.
	std::atomic_flag m_uiAdinProducer = ATOMIC_FLAG_INIT;
	std::atomic<std::uint64_t> m_contendedUiAdinEvents { 0 };
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
	prophecy::ProgramControl m_programControl{m_engine};
	std::atomic<bool>  m_started { false };
	juce::String       m_romPath;             // resolved ROM dir once booted
	juce::String       m_nvramPath;           // resolved NVRAM dir once booted
	double             m_hostSampleRate = 48000.0;
	prophecy::SampleTimeline m_timeline;
	std::uint64_t      m_timelineHostFrame = 0;
	bool               m_timelineAttached = false;
	int                m_preparedMaxBlock = 0;
	std::atomic<std::uint64_t> m_oversizedAudioBlocks { 0 };
	// Lock-free counters sampled by the optional GUI diagnostic logger. The audio callback
	// only updates atomics; all formatting and file I/O stays on the message thread.
	std::atomic<std::uint64_t> m_audioCallbacks { 0 };
	std::atomic<std::uint64_t> m_audioHostFrames { 0 };
	std::atomic<std::uint64_t> m_audioEngineFrames { 0 };
	std::atomic<std::uint64_t> m_audioUnderrunFrames { 0 };
	std::atomic<std::uint64_t> m_hostMidiEvents { 0 };
	std::atomic<std::uint64_t> m_hostMidiEventsForwarded { 0 };
	std::atomic<std::uint32_t> m_lastHostMidi { 0 };
	std::atomic<std::uint64_t> m_editorPatchIntents { 0 };
	std::atomic<std::uint64_t> m_editorPatchSends { 0 };
	std::atomic<std::uint64_t> m_editorDumpRequests { 0 };
	std::atomic<std::uint64_t> m_editorDumpSends { 0 };
	std::array<std::atomic<std::uint64_t>, 2> m_activeHostNotes {};
	bool               m_skipStateRestore = false;

	// host!=48k resampling (engine is authoritative at 48 kHz)
	std::vector<float>         m_rsIn[2];       // absolute native input window, preallocated

	// Host lifecycle/state operations are serialized off the audio thread.
	// A retained program becomes confirmed only after firmware readback succeeds.
	std::mutex m_stateMutex;
	bool bootEngine();
	bool restoreProgram(bool firmwareHandshake = true);
	std::atomic<bool> m_processingPaused{false};
	std::atomic<bool> m_callbackAccess{false};

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProphecyAudioProcessor)
};
