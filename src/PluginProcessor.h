// SPDX-License-Identifier: AGPL-3.0-only
//
// PluginProcessor.h - the JUCE AudioProcessor. Includes ONLY the MAME-free engine
// header (prophecy_engine.h) + JUCE. MAME runs on the engine's worker thread; the
// host's audio callback pulls stereo blocks from the ring.
//
#pragma once

#include "audio_timeline.h"
#include "program_state.h"
#include "program_midi.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include "prophecy_engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
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
	{ return m_engine.droppedUiAdinEvents(); }
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
	const char* initializationError() const { return m_engine.initializationError(); }
	const char* programStateError() const { return m_programState.error(); }
	prophecy::ProgramState::Status programStateStatus() const { return m_programState.status(); }
	bool instanceUnavailable() const
	{
		return m_engine.instanceStatus() == ProphecyEngine::InstanceStatus::Unavailable;
	}
	juce::String romPath() const { return m_romPath; }
	bool setRomDirFromUser(const juce::File &dir); // validate + persist + boot; false if invalid
	bool maybeBootEngine();                        // boot once iff a valid ROM set is locatable
	void selectPatch(int program);       // bank-select + program change (0..127 = A00..B63)
	// Non-realtime editor input. Full-program imports and dump inquiries use the
	// program owner; performance MIDI (for example the keybed) enters the UART.
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
	// is a batch of Parameter Change writes delivered through the shared command queue (the
	// firmware drops back-to-back sysex). OSC1-specific params are ExID-packed (paramId =
	// (1<<12)|param); a p154 (OSC Set) write, if present, is re-asserted LAST so the live
	// DSP1 engine reloads cleanly (a std-osc write right after a reconfig gets clobbered).
	void sendMacro(const juce::String &name);
	// Accept an explicit persistent WRITE of the current document to A00..B63.
	// Returns a request ID, or zero if rejected. A queued request can be cancelled;
	// a started transaction is joined, including protection cleanup, before restore
	// or destruction. It briefly pauses playback while firmware performs the WRITE.
	enum class WriteStatus { Idle, Pending, Succeeded, Failed, Cancelled };
	std::uint64_t writePatch(int destination);
	bool writeInProgress() const { return writeStatus() == WriteStatus::Pending; }
	WriteStatus writeStatus() const { return m_writeStatus.load(std::memory_order_acquire); }
	std::uint64_t writeRequestId() const { return m_writeRequestId.load(std::memory_order_acquire); }
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
	friend struct ProphecyProgramControlTestAccess;
	// Declared before timers/engine so their dependencies outlive them.
	prophecy::ProgramState m_programState;
	std::condition_variable_any m_controlWake;
	std::thread m_controlThread;
	bool m_controlStopping = false; // control mutex
	void runProgramControl();
	prophecy::ProgramMidiRouting m_programMidiRouting; // control mutex
	prophecy::ProgramMidiInbox m_programMidiInbox;
	bool m_drainingProgramInput = false; // control mutex; nested requests reuse this prefix
	void drainProgramInput(); // control mutex
	void receiveProgramInput(const std::uint8_t* bytes, std::size_t size);
	bool forwardHostMidi(const std::uint8_t* bytes, std::size_t size, std::uint64_t frame);
	void replaceProgramRevision();
	void cancelEditorWork();
	bool requestProgramEdits(const std::vector<prophecy::ProgramEdit>& edits, int intervalMs = 75);
	bool firmwareControlAllowed(); // control mutex; admission precedes queue publication
	bool enqueueFirmwareMidi(int key, const std::uint8_t* bytes, std::size_t size, bool bindChannel = true);
	void firmwareControlAccepted();
	std::uint64_t m_firmwareErrorStart = 0; // control mutex; captured before an admitted batch

	// Control work runs on one owned thread. In particular, stopping a JUCE
	// message timer does not join a callback already selected for dispatch.
	class ProgramTimer
	{
	public:
		explicit ProgramTimer(ProphecyAudioProcessor& p) : m_proc(p) {}
		virtual ~ProgramTimer() = default; // owner joins before destroying tasks
		void cancel() { stopTimer(); cancelled(); }
		using Clock = std::chrono::steady_clock;
		Clock::time_point due() const { return m_running ? m_due : Clock::time_point::max(); }
		void service(Clock::time_point now)
		{
			if (!m_running || now < m_due) return;
			m_due = now + m_interval;
			if (!current()) { cancel(); return; }
			run();
		}
	protected:
		void startTimer(int milliseconds)
		{
			m_interval = std::chrono::milliseconds(std::max(1, milliseconds));
			m_due = Clock::now() + m_interval;
			m_running = true;
			m_proc.m_controlWake.notify_one();
		}
		void stopTimer() { m_running = false; m_proc.m_controlWake.notify_one(); }
		bool isTimerRunning() const { return m_running; }
		void startForProgram(int milliseconds)
		{
			m_revision = m_proc.m_programState.revision();
			startTimer(milliseconds);
		}
		bool current() const { return m_revision == m_proc.m_programState.revision(); }
		virtual void run() = 0;
		virtual void cancelled() {}
	private:
		ProphecyAudioProcessor& m_proc;
		std::uint64_t m_revision = 0; // control mutex
		Clock::time_point m_due{};
		std::chrono::milliseconds m_interval{1};
		bool m_running = false;
	};

	class ProgramWrite : public ProgramTimer
	{
	public:
		explicit ProgramWrite(ProphecyAudioProcessor &p) : ProgramTimer(p), m_proc(p) {}
		using ProgramTimer::cancel;
		void start(int destination, const prophecy::ProgramDocument& document)
		{
			m_document = document;
			m_destination = destination;
			m_token = m_proc.m_programState.token();
			startForProgram(10);
		}
	private:
		void run() override;
		void cancelled() override
		{
			if (m_proc.writeInProgress()) m_proc.m_writeStatus.store(WriteStatus::Cancelled, std::memory_order_release);
		}
		ProphecyAudioProcessor &m_proc;
		prophecy::ProgramDocument m_document;
		prophecy::ProgramState::Token m_token;
		int m_destination = 0;
	};
	ProgramWrite m_writeSeq{*this};
	std::atomic<WriteStatus> m_writeStatus{WriteStatus::Idle};
	std::atomic<std::uint64_t> m_writeRequestId{0};


	// Serialized editor commands share one bounded, coalescing path. The
	// firmware can wedge its internal H8/V55 link if a patch load overlaps a burst
	// of independent editor commands even though the host MIDI rings never drop a
	// byte. MIDI edit commands and panel presses are therefore paced and held behind
	// the patch-load barrier. ADIN writes model physical controls polled by the
	// firmware, so they bypass this serial-command pacer.
	class EditorCommandPacer : public ProgramTimer
	{
	public:
		explicit EditorCommandPacer(ProphecyAudioProcessor &p) : ProgramTimer(p), m_proc(p) {}
		using ProgramTimer::cancel;
		~EditorCommandPacer() override { stopTimer(); }
		bool enqueueProgramEdits(const std::vector<prophecy::ProgramEdit>& edits, int intervalMs)
		{
			if (!current()) cancel();
			if (edits.size() > kCapacity - m_queue.size())
			{
				m_dropped.fetch_add(edits.size(), std::memory_order_relaxed);
				m_proc.m_programState.fail(prophecy::ProgramState::Error::Capacity);
				return false;
			}
			// Prepare both changes before publishing either. Allocation failure or
			// queue exhaustion must not accept only the beginning of a macro/name.
			auto queued = m_queue;
			auto through = m_proc.m_programState.token().through;
			for (const auto& edit : edits)
			{
				Command command;
				command.revision = m_proc.m_programState.revision();
				command.through = ++through;
				command.intervalMs = intervalMs;
				const auto midi = edit.midi();
				command.bytes.assign(midi.begin(), midi.end());
				queued.push_back(std::move(command));
			}
			if (!m_proc.m_programState.accept(edits)) return false;
			m_queue.swap(queued);
			if (!isTimerRunning()) startForProgram(1);
			return true;
		}
		void holdForPatchLoad(int milliseconds)
		{
			m_cancelled.fetch_add(m_queue.size(), std::memory_order_relaxed);
			m_queue.clear();
			m_holdUntilMs = juce::Time::getMillisecondCounterHiRes()
				+ (double)std::max(milliseconds, 0);
			startForProgram(10);
		}
		void extendPatchLoad(int milliseconds)
		{
			m_holdUntilMs = std::max(m_holdUntilMs,
				juce::Time::getMillisecondCounterHiRes()
					+ (double)std::max(milliseconds, 0));
			if (!isTimerRunning()) startForProgram(10);
		}
		bool enqueueMidi(int key, const std::uint8_t *bytes, std::size_t size, bool bindChannel = true)
		{
			if (!current()) cancel();
			if (bytes == nullptr || size == 0) return false;
			const bool orderedProgramEdit = size == 11 && bytes[0] == 0xf0 && bytes[4] == 0x41 && bytes[5] == 1;
			// Only consecutive assignments may replace one another. Crossing an
			// intervening command can erase a required unprotect/load/protect order.
			if (!orderedProgramEdit && key >= 0 && !m_queue.empty())
			{
				auto& previous = m_queue.back();
				if (previous.kind == Command::Kind::Midi && previous.key == key && previous.bindChannel == bindChannel)
				{
					previous.bytes.assign(bytes, bytes + size);
					m_coalesced.fetch_add(1, std::memory_order_relaxed);
					return true;
				}
			}
			if (m_queue.size() >= kCapacity)
			{
				m_dropped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			Command command;
			command.kind = Command::Kind::Midi;
			command.key = key;
			command.bindChannel = bindChannel;
			command.revision = m_proc.m_programState.revision();
			command.bytes.assign(bytes, bytes + size);
			m_queue.push_back(std::move(command));
			if (!isTimerRunning() || !current()) startForProgram(1);
			return true;
		}
		bool enqueuePanel(int row, int bit)
		{
			if (!current()) cancel();
			if (m_queue.size() >= kCapacity)
			{
				m_dropped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			Command command;
			command.kind = Command::Kind::Panel;
			command.revision = m_proc.m_programState.revision();
			command.a = row;
			command.b = bit;
			m_queue.push_back(std::move(command));
			if (!isTimerRunning() || !current()) startForProgram(1);
			return true;
		}
		bool busy() const
		{
			return !m_queue.empty() || !settled();
		}
		bool settled() const
		{
			return juce::Time::getMillisecondCounterHiRes() >= m_holdUntilMs
				&& m_proc.m_engine.producedFrames() >= m_notBeforeFrame
				&& m_proc.m_engine.producedFrames() >= m_confirmNotBeforeFrame
				&& !m_proc.patchLoadBarrierActive();
		}
		std::uint64_t sent() const { return m_sent.load(std::memory_order_relaxed); }
		std::uint64_t coalesced() const { return m_coalesced.load(std::memory_order_relaxed); }
		std::uint64_t cancelled() const { return m_cancelled.load(std::memory_order_relaxed); }
		std::uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }
		std::size_t pending() const { return m_queue.size(); }
		prophecy::ProgramState::Token delivered() const { return m_delivered; }
	private:
		struct Command
		{
			enum class Kind { Midi, Panel } kind = Kind::Midi;
			int key = -1;
			std::uint64_t revision = 0;
			std::uint64_t through = 0;
			int a = 0, b = 0;
			int intervalMs = kIntervalMs;
			bool bindChannel = true;
			std::vector<std::uint8_t> bytes;
		};
		void run() override
		{
			if (m_proc.m_engine.programReadbackPending()) { startTimer(10); return; }
			if (m_proc.m_programState.restoreRequired()) { startTimer(10); return; }
			const double now = juce::Time::getMillisecondCounterHiRes();
			if (m_proc.patchLoadBarrierActive())
			{
				// DAW Program Changes extend the frame-based editor-command barrier
				// on the audio callback. Observe it here instead of starting or
				// touching a JUCE timer from the real-time thread. Host MIDI itself
				// remains unfiltered.
				startTimer(25);
				return;
			}
			if (now < m_holdUntilMs)
			{
				startTimer(std::max(1, (int)std::ceil(m_holdUntilMs - now)));
				return;
			}
			if (m_queue.empty()) { stopTimer(); return; }
			if (m_proc.m_engine.producedFrames() < m_notBeforeFrame)
			{
				// Wall time passes while a DAW is stopped. Keep unsent operations in
				// the savable queue until the firmware has actually progressed.
				startTimer(10);
				return;
			}
			Command &command = m_queue.front();
			const auto intervalMs = command.intervalMs;
			bool accepted = true;
			if (command.kind == Command::Kind::Midi)
			{
				// Editor commands address this synth, regardless of its configured
				// MIDI channel. Bind at delivery because an earlier queued global
				// edit may have changed that channel after this command was accepted.
				const auto channel = m_proc.m_programMidiRouting.channelNumber();
				if (command.bindChannel && command.bytes.front() == 0xf0 && command.bytes.size() >= 3)
					command.bytes[2] = std::uint8_t(0x30 | channel);
				else if (command.bindChannel)
					for (auto& byte : command.bytes)
						if (byte >= 0x80 && byte < 0xf0) byte = std::uint8_t((byte & 0xf0) | channel);
				accepted = m_proc.pushImmediateMidi(command.bytes.data(), command.bytes.size(), command.revision);
			}
			else
			{
				m_proc.m_engine.pushPanelPulse(command.a, command.b, 75, command.revision);
				// A panel key can start a program load. Confirm only after the same
				// firmware settling interval used for an explicit patch selection.
				m_confirmNotBeforeFrame = m_proc.m_engine.producedFrames()
					+ std::uint64_t(kPatchLoadSettleSeconds * ProphecyEngine::kSampleRate);
			}
			if (accepted)
			{
				if (command.through != 0) m_delivered = {command.revision, command.through};
				m_notBeforeFrame = m_proc.m_engine.producedFrames()
					+ std::uint64_t(ProphecyEngine::kSampleRate) * std::uint64_t(intervalMs) / 1000;
				m_queue.pop_front();
				m_sent.fetch_add(1, std::memory_order_relaxed);
			}
			startTimer(accepted ? intervalMs : 25);
		}
		void cancelled() override
		{
			m_cancelled.fetch_add(m_queue.size(), std::memory_order_relaxed);
			m_queue.clear();
			m_holdUntilMs = 0;
			m_notBeforeFrame = 0;
			m_confirmNotBeforeFrame = 0;
			m_delivered = {};
		}
		static constexpr std::size_t kCapacity = 256;
		static constexpr int kIntervalMs = 75;
		ProphecyAudioProcessor &m_proc;
		std::deque<Command> m_queue;
		double m_holdUntilMs = -1.0e9;
		std::uint64_t m_notBeforeFrame = 0;
		std::uint64_t m_confirmNotBeforeFrame = 0;
		prophecy::ProgramState::Token m_delivered;
		std::atomic<std::uint64_t> m_sent { 0 };
		std::atomic<std::uint64_t> m_coalesced { 0 };
		std::atomic<std::uint64_t> m_cancelled { 0 };
		std::atomic<std::uint64_t> m_dropped { 0 };
	};
	EditorCommandPacer m_editorCommandPacer { *this };
	// Program loading plus concurrent editor mutations can wedge the emulated H8/V55
	// control link. Hold editor commands and dump requests for a bounded interval after
	// editor or DAW Program Change, but never discard the host's MIDI stream.
	void holdEditorCommandsForPatchLoad(double seconds);
	bool patchLoadBarrierActive() const
	{
		return m_audioHostFrames.load(std::memory_order_relaxed)
			< m_patchLoadBarrierUntilFrame.load(std::memory_order_acquire);
	}
	static constexpr double kPatchLoadSettleSeconds = 2.0;
	static constexpr double kPatchSelectMinIntervalMs = 2500.0;
	bool sendPatchNow(int program);
	class PatchSelectDelay : public ProgramTimer
	{
	public:
		explicit PatchSelectDelay(ProphecyAudioProcessor &p) : ProgramTimer(p), m_proc(p) {}
		using ProgramTimer::cancel;
		~PatchSelectDelay() override { stopTimer(); }
		bool pending() const { return isTimerRunning() && current(); }
		void schedule(int program)
		{
			m_program = program;
			startForProgram(500);
		}
	private:
		void run() override
		{
			stopTimer();
			if (!m_proc.sendPatchNow(m_program)) startTimer(25);
		}
		ProphecyAudioProcessor &m_proc;
		int m_program = 0;
	};
	PatchSelectDelay   m_patchSelectDelay { *this };

	// Readback belongs to the delivered edit prefix. New intent may be accepted
	// while it runs, but the command queue waits until that exchange is drained.
	class ProgramDumpSync : public ProgramTimer
	{
	public:
		explicit ProgramDumpSync(ProphecyAudioProcessor& p) : ProgramTimer(p), m_proc(p) {}
		using ProgramTimer::cancel;
		std::uint64_t request(int delayMs)
		{
			if (!current()) cancel();
			if (isTimerRunning()) return m_generation;
			++m_generation;
			m_attempts = 0;
			startForProgram(std::max(1, delayMs));
			return m_generation;
		}
		std::uint64_t completed() const { return m_completed.load(std::memory_order_acquire); }
	private:
		void run() override;
		void cancelled() override
		{
			if (m_ticket) m_proc.m_engine.cancelProgramReadback(m_ticket);
			m_ticket = 0;
		}
		ProphecyAudioProcessor& m_proc;
		std::uint64_t m_generation = 0, m_ticket = 0;
		unsigned m_attempts = 0;
		prophecy::ProgramState::Token m_prefix;
		std::atomic<std::uint64_t> m_completed{0};
	};
	ProgramDumpSync m_programDumpSync{*this};
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
	std::array<std::atomic<std::uint8_t>, 16> m_controllerDisplayValues {};
	void publishControllerDisplayValue(int source, int value);
	void handleMappedCc(int cc, int value, CcTarget target, std::uint64_t frame); // audio thread
	// The control mutex serializes editor and host-state producers. Audio input
	// uses separate SPSC queues and never takes that mutex.
	bool pushImmediateMidi(const std::uint8_t *bytes, std::size_t n, std::uint64_t revision = 0)
	{
		if (!m_engine.pushMidi(bytes, n, revision)) return false;
		if (n == 11 && bytes[0] == 0xf0 && bytes[4] == 0x41 && bytes[5] == 0)
			(void)m_programMidiRouting.receive(bytes, n);
		else if (n == 663 && bytes[4] == 0x51 && m_programMidiRouting.sysex(bytes, n))
		{
			std::vector<std::uint8_t> globals;
			for (std::size_t i = 6; i < n - 1;)
			{
				const auto high = bytes[i++];
				for (int bit = 0; bit < 7 && i < n - 1; ++bit)
					globals.push_back(std::uint8_t(bytes[i++] | (((high >> bit) & 1) << 7)));
			}
			(void)m_programMidiRouting.reset(globals);
		}
		return true;
	}
	bool pushUiAdin(int source, int value)
	{ return m_engine.pushAdin(source, value); }

	ProphecyEngine     m_engine;
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
	std::atomic<std::uint64_t> m_patchLoadBarrierUntilFrame { 0 };
	std::array<std::atomic<std::uint64_t>, 2> m_activeHostNotes {};
	bool               m_skipStateRestore = false;

	// host!=48k resampling (engine is authoritative at 48 kHz)
	std::vector<float>         m_rsIn[2];       // absolute native input window, preallocated

	// Host lifecycle/state operations are serialized off the audio thread.
	// A retained program becomes confirmed only after firmware readback succeeds.
	bool bootEngine();
	bool restoreProgram(bool firmwareHandshake = true);
	void loadProgramDocument(prophecy::ProgramDocument document); // control mutex and ProcessingPause held
	std::atomic<bool> m_processingPaused{false};
	std::atomic<bool> m_callbackAccess{false};

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProphecyAudioProcessor)
};
