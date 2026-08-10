// SPDX-License-Identifier: AGPL-3.0-only
//
// prophecy_engine.h - the MAME-free seam between the emulator and any host.
//
// This header deliberately includes NO MAME and NO JUCE headers. The engine runs
// the real korgprop MAME machine on a background thread and buffers its 48 kHz
// stereo output in a ring; a host (the console harness, or the JUCE AudioProcessor)
// pulls fixed blocks at its own real-time cadence. Backpressure inside the engine
// throttles MAME to the pull rate. All MAME types live in prophecy_engine.cpp.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ProphecyEngine
{
public:
	static constexpr int kSampleRate = 48000;
	static constexpr int kChannels   = 2;
	static constexpr std::size_t kLcdA00GlyphRowBytes = 256 * 8;
	enum class InstanceStatus : std::uint8_t
	{
		NotStarted,
		Active,
		Unavailable,
		Stopped
	};

	// One byte observed on the firmware's physical MIDI OUT UART. `emuSeconds` is the
	// MAME-machine time at which the byte's stop bit was sampled, not host wall time.
	// Keeping the wire-level bytes makes running status, realtime interleaving, and the
	// fixed 31.25-kbaud serialization cost visible to timing tests.
	struct MidiTxByteEvent
	{
		double       emuSeconds = 0.0;
		std::uint8_t byte = 0;
	};

	struct Impl; // opaque; defined in prophecy_engine.cpp (holds all MAME types)

	ProphecyEngine();
	~ProphecyEngine();
	ProphecyEngine(const ProphecyEngine &) = delete;
	ProphecyEngine &operator=(const ProphecyEngine &) = delete;

	// Boot the machine on a background thread. `args` is a MAME command line, e.g.
	// {"prophecy","korgprop","-rompath",romdir,"-video","none","-sound","none","-nothrottle"}.
	// Non-blocking; returns false if already started or another engine owns MAME's
	// process-global machine slot. instanceStatus() distinguishes those cases.
	bool start(const std::vector<std::string> &args);

	// Enable the raw MIDI OUT observer. Call before start(); it is disabled by default so
	// ordinary plugin sessions do not accumulate diagnostic traffic. Enabling resets the
	// observer ring and dropped-event count. Returns false once the engine has started.
	bool enableMidiTxByteCapture(bool enabled = true);

	// Ask the machine to exit and join the thread. Safe to call from a destructor.
	void stop();

	bool running() const;   // thread launched and machine not yet finished
	bool finished() const;  // machine returned on its own (e.g. -seconds_to_run)

	// True only for the engine that claimed MAME's process-global machine slot. A
	// concurrent start is rejected with Unavailable: it owns no worker and cannot become
	// active later. Hosts may use this off the audio thread to explain the v1 one-instance
	// limitation instead of presenting an apparently-live silent synth.
	bool ownsMachineSlot() const;
	InstanceStatus instanceStatus() const;

	// Pull up to `frames` stereo frames as planar float in [-1, 1]. Returns the
	// number of frames actually delivered (< frames on underrun). Non-blocking and
	// real-time-safe: no locks held across MAME, no allocation in steady state.
	std::size_t pull(float *left, float *right, std::size_t frames);

	// Push one complete raw host MIDI message (note/CC/bend/sysex) to the emulated
	// 31250-baud serial UART. This immediate queue has one producer: the message thread.
	// The write is all-or-nothing; false means the bounded queue was full and the complete
	// message was dropped (never torn). Bytes sent before boot (~2 s) are consumed by the
	// driver but ignored by the firmware.
	bool pushMidi(const std::uint8_t *bytes, std::size_t n);

	// Schedule one host MIDI message at a native 48 kHz engine-output frame. This is
	// used by processBlock to preserve host MidiBuffer sample offsets despite MAME
	// running asynchronously ahead of the consumed audio. Messages must be pushed in
	// nondecreasing target-frame order by a single producer.
	bool pushMidiAtFrame(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame);
	std::uint64_t droppedImmediateMidiBytes() const;
	std::uint64_t droppedScheduledMidiBytes() const;

	// Drain sysex the firmware transmitted (patch dumps, param echoes). Returns bytes copied
	// (0 if none). Poll from the message thread; the stream is complete 0xF0..0xF7 messages.
	std::size_t popMidiTx(std::uint8_t *out, std::size_t cap);

	// Drain timestamped raw MIDI OUT bytes without consuming or racing popMidiTx(). Returns
	// the number of complete event records copied. A nonzero droppedMidiTxByteEvents() means
	// the observer was not drained quickly enough and the timing capture is invalid.
	std::size_t popMidiTxByteEvents(MidiTxByteEvent *out, std::size_t cap);
	std::uint64_t droppedMidiTxByteEvents() const;

	// Faceplate front-panel button press: pulse scan-matrix (row, bit) for len_ms (default
	// 75 ms, the hardware GUI's pulse length). Lock-free; drained on the MAME thread.
	void pushPanelPulse(int row, int bit, int len_ms = 75);
	// Deterministic harness/automation form: make the pulse visible to the emulated
	// scan matrix at an exact native 48 kHz engine frame instead of depending on when
	// the host message thread happens to wake.
	bool pushPanelPulseAtFrame(int row, int bit, int len_ms, std::uint64_t frame);

	// Faceplate analog control (wheels / ribbon X-Y-Z / knobs / sliders): set ADIN source
	// (0..15) to value (0..255). The ordinary method is the message-thread producer;
	// processBlock uses the separate audio-thread queue. Writes are all-or-nothing and
	// return false on bounded-queue overflow, with counters available below.
	bool pushAdin(int source, int value);
	bool pushAdinFromAudio(int source, int value);
	bool pushAdinAtFrame(int source, int value, std::uint64_t frame);
	std::uint64_t droppedUiAdinEvents() const;
	std::uint64_t droppedAudioAdinEvents() const;
	std::uint64_t droppedScheduledPanelEvents() const;
	std::uint64_t droppedScheduledAdinEvents() const;

	// Snapshot the front-panel LED banks (12 banks x 8 bits; LED index = bank*8 + bit).
	// Returns a version counter that increments on every LED change — poll cheaply and
	// redraw only when it moves. Atomics only; any thread.
	std::uint32_t ledSnapshot(std::uint8_t out[12]) const;

	// Raw LCD snapshot: the visible 40-column rows as HD44780 char codes (codes 0x00-0x07
	// are the CGRAM custom glyphs) plus the 64-byte CGRAM (8 glyphs x 8 rows, low 5 bits).
	// Returns a version counter (0 = firmware has not drawn yet). Mutex-guarded store:
	// message thread, NOT the audio thread.
	std::uint32_t lcdRawSnapshot(std::uint8_t row1[40], std::uint8_t row2[40], std::uint8_t cgram[64]) const;

	// Copy the eight visible 5x8 rows for all 256 characters from the exact A00
	// datasheet reconstruction compiled into the emulated LCD device.  This is
	// process-static data and does not require a running machine instance.
	static void lcdA00GlyphRows(std::uint8_t out[kLcdA00GlyphRowBytes]);

	// Copy the emulator's current HD44780 LCD text into two caller-owned NUL-terminated buffers
	// (each >= 41 bytes: 40 visible columns + NUL). Returns false and empties the buffers if the
	// firmware has not drawn anything yet. A small mutex guards the store: poll from the message
	// thread, NOT the audio thread.
	bool latestLcd(char *line1, char *line2, std::size_t cap) const;

	// Copy the most recently captured current-program dump, UNPACKED into raw program bytes (the
	// editor reads knob values out of it by manifest offset). Returns bytes copied (0 if none yet);
	// *version is a monotonic counter so a poller can detect a fresh dump. Message thread only.
	std::size_t latestProgramData(std::uint8_t *out, std::size_t cap, std::uint32_t *version) const;

	// Copy the most recently captured single arpeggio-pattern dump, unpacked into its
	// documented 128-byte record. `pattern` receives 0..9 (UP..PAT5), or -1 when the
	// latest reply was an all-pattern dump. Message thread only.
	std::size_t latestArpeggioPatternData(std::uint8_t *out, std::size_t cap,
		std::uint32_t *version, int *pattern) const;

	std::size_t available() const;       // frames currently buffered
	std::size_t ringFrames() const;      // ring capacity in frames (~ steady-state latency)
	std::uint64_t producedFrames() const; // total frames MAME has emitted

private:
	std::unique_ptr<Impl> m_impl;
};
