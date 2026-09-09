// SPDX-License-Identifier: AGPL-3.0-only
//
// prophecy_engine.cpp - the ONLY translation unit that includes MAME.
//
// Runs the real korgprop machine on a background thread via emulator_info::start_frontend
// with a custom OSD whose add_audio_to_recording() pushes the mixed 48 kHz stereo stream
// into a bounded ring. pull() drains that ring as planar float for the host. Backpressure
// (the producer blocks when the ring is full) throttles MAME to the host's pull rate.

// Use MAME's common OSD substrate. The plugin supplies lifecycle and audio hooks
// without importing the executable-oriented Windows winmain.cpp.
#if defined(SDLMAME_WIN32)
#include "modules/lib/osdobj_common.h"
#include "winopts.h"
#else
#include "osdsdl.h"
#endif

// MAME headers
#include "emu.h"
#include "emuopts.h"
#include "main.h"
#include "osdepend.h"
#include "render.h"
#include "video/hd44780.h"

#include "led_store.h"
#include "audio_timeline.h"
#include "prophecy_engine.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>

static int prophecy_setenv(const char *name, const char *value, int overwrite)
{
	if (!overwrite && std::getenv(name) != nullptr) return 0;
	return _putenv_s(name, value);
}
#define setenv prophecy_setenv
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

// debugqt.cpp references `extern int sdl_entered_debugger;` (normally in sdlmain.cpp);
// define it here so the linker never pulls sdlmain.o (which would duplicate main()).
#if defined(SDLMAME_UNIX) || defined(SDLMAME_WIN32)
int sdl_entered_debugger = 0;
#endif

namespace {

#if defined(__APPLE__)
void configure_audio_producer_scheduling(std::uint32_t nativeFrames = 960)
{
	// Match the host's production cadence. Console clients keep their 20 ms
	// period; the plugin requests a new policy when prepare changes its block.
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	mach_timebase_info_data_t timebase{};
	kern_return_t result = mach_timebase_info(&timebase);
	if (result == KERN_SUCCESS && timebase.numer != 0)
	{
		auto ticks_for_ns = [&](std::uint64_t ns) {
			return static_cast<std::uint32_t>(ns * timebase.denom / timebase.numer);
		};
		const auto periodNs = std::uint64_t(nativeFrames) * 1'000'000'000ULL / 48000;
		thread_time_constraint_policy_data_t policy{};
		policy.period = ticks_for_ns(periodNs);
		policy.computation = ticks_for_ns(periodNs * 9 / 10);
		policy.constraint = policy.period;
		policy.preemptible = true;
		result = thread_policy_set(
			pthread_mach_thread_np(pthread_self()),
			THREAD_TIME_CONSTRAINT_POLICY,
			reinterpret_cast<thread_policy_t>(&policy),
			THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	}
	if (std::getenv("PROPHOST_SCHED_STATS"))
		std::fprintf(stderr, "[prophost-sched] producer_time_constraint=%d result=%d period_frames=%u\n",
			result == KERN_SUCCESS, int(result), unsigned(nativeFrames));
}
#endif

// Bounded SPSC ring; producer (MAME thread) blocks when full = backpressure.
class AudioRing
{
public:
	explicit AudioRing(std::size_t cap_frames)
		: m_cap(pow2(cap_frames * ProphecyEngine::kChannels)), m_mask(m_cap - 1), m_buf(m_cap) { }

	// producer (MAME worker thread): append; sleep-polls when full for backpressure. Never
	// takes a lock, so the audio-thread consumer can never stall on a lock the producer holds.
	void push(const int16_t *p, std::size_t nsamp)
	{
		std::size_t off = 0;
		while (off < nsamp)
		{
			const std::size_t h = m_head.load(std::memory_order_acquire);
			const std::size_t t = m_tail.load(std::memory_order_relaxed);
			const std::size_t space = m_cap - (t - h);
			if (space == 0)
			{
				if (m_abort.load(std::memory_order_relaxed)) return;
				std::this_thread::sleep_for(std::chrono::microseconds(200)); // wait for the consumer
				continue;
			}
			const std::size_t chunk = std::min(space, nsamp - off);
			for (std::size_t i = 0; i < chunk; i++) m_buf[(t + i) & m_mask] = p[off + i];
			m_tail.store(t + chunk, std::memory_order_release);
			off += chunk;
		}
	}

	// Consumer (audio thread): lock-free, wait-free, and allocation-free. Convert the
	// interleaved ring directly into caller-owned planar buffers so pull() never needs a
	// growable scratch vector on the real-time thread.
	std::size_t pop_planar(float *left, float *right, std::size_t frames)
	{
		const std::size_t t = m_tail.load(std::memory_order_acquire);
		const std::size_t h = m_head.load(std::memory_order_relaxed);
		const std::size_t got_frames = std::min((t - h) / ProphecyEngine::kChannels, frames);
		for (std::size_t i = 0; i < got_frames; ++i)
		{
			left[i] = float(m_buf[(h + i * 2) & m_mask]) * (1.0f / 32768.0f);
			right[i] = float(m_buf[(h + i * 2 + 1) & m_mask]) * (1.0f / 32768.0f);
		}
		m_head.store(h + got_frames * ProphecyEngine::kChannels, std::memory_order_release);
		return got_frames;
	}

	std::size_t count() const { return m_tail.load(std::memory_order_acquire) - m_head.load(std::memory_order_acquire); }
	std::size_t capacity_frames() const { return m_cap / ProphecyEngine::kChannels; }
	void set_done()  { }                                                // no waiters to wake (lock-free)
	void set_abort() { m_abort.store(true, std::memory_order_relaxed); } // unblock a full producer

private:
	static std::size_t pow2(std::size_t x) { std::size_t p = 1; while (p < x) p <<= 1; return p; }
	std::size_t              m_cap, m_mask;
	std::vector<int16_t>     m_buf;
	std::atomic<std::size_t> m_head{0}, m_tail{0};
	std::atomic<bool>        m_abort{false};
};

// MAME is single-instance-per-process (a global machine_manager singleton). This guard
// ensures at most ONE engine boots a machine at a time; a concurrent engine runs silent.
std::atomic<bool> g_engine_active{false};

// Lock-free SPSC byte ring for host MIDI: producer = audio thread (pushMidi),
// consumer = MAME worker thread (kprop_host_midi_pop). Monotonic indices + pow2 mask.
class MidiRing
{
public:
	void push(const uint8_t *p, size_t n)
	{
		const size_t t = m_tail.load(std::memory_order_relaxed);
		const size_t h = m_head.load(std::memory_order_acquire);
		size_t space = CAP - (t - h);
		if (n > space) n = space; // drop on overflow (MIDI is low-rate; should not happen)
		for (size_t i = 0; i < n; i++) m_buf[(t + i) & (CAP - 1)] = p[i];
		m_tail.store(t + n, std::memory_order_release);
	}
	size_t pop(uint8_t *out, size_t cap)
	{
		const size_t h = m_head.load(std::memory_order_relaxed);
		const size_t t = m_tail.load(std::memory_order_acquire);
		size_t n = t - h;
		if (n > cap) n = cap;
		for (size_t i = 0; i < n; i++) out[i] = m_buf[(h + i) & (CAP - 1)];
		m_head.store(h + n, std::memory_order_release);
		return n;
	}
	// All-or-nothing push for fixed-size records (panel/ADIN framing must not tear).
	bool pushAll(const uint8_t *p, size_t n)
	{
		const size_t t = m_tail.load(std::memory_order_relaxed);
		const size_t h = m_head.load(std::memory_order_acquire);
		if (CAP - (t - h) < n) return false;
		for (size_t i = 0; i < n; i++) m_buf[(t + i) & (CAP - 1)] = p[i];
		m_tail.store(t + n, std::memory_order_release);
		return true;
	}
	bool pushAllCounted(const uint8_t *p, size_t n)
	{
		if (pushAll(p, n)) return true;
		m_dropped.fetch_add(n, std::memory_order_relaxed);
		return false;
	}
	std::uint64_t dropped() const { return m_dropped.load(std::memory_order_acquire); }
	// Initialization only, after the consumer has observed playback input disabled.
	void discard() { m_head.store(m_tail.load(std::memory_order_acquire), std::memory_order_release); }
	void reset()
	{
		m_head.store(0, std::memory_order_relaxed);
		m_tail.store(0, std::memory_order_relaxed);
		m_dropped.store(0, std::memory_order_relaxed);
	}
private:
	static constexpr size_t CAP = 4096; // power of two
	uint8_t             m_buf[CAP];
	std::atomic<size_t> m_head{0}, m_tail{0};
	std::atomic<std::uint64_t> m_dropped{0};
};

MidiRing g_host_midi_ring;     // host -> emulated UART (RX)
MidiRing g_initialization_midi_ring; // non-realtime initialization -> UART
std::atomic<bool> g_playback_input_enabled{true};
MidiRing g_host_midi_tx_ring;  // emulated UART -> host (TX: patch dumps, param echoes)
MidiRing g_host_panel_ring;    // host -> panel scan matrix: (row, bit, len_ms lo, len_ms hi) records
MidiRing g_host_adin_ui_ring;  // message thread -> ADIN mux: (source, value) records
MidiRing g_host_adin_rt_ring;  // audio thread -> ADIN mux: (source, value) records

// Audio-thread host MIDI retains its JUCE sample offset as an engine-frame due time.
// It is separate from the immediate editor/UI queue so a future audio event can never
// head-of-line-block an immediate parameter SysEx message.
class TimedMidiRing
{
public:
	bool push(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame)
	{
		if (bytes == nullptr || n == 0) return true;
		const std::size_t t = m_tail.load(std::memory_order_relaxed);
		const std::size_t h = m_head.load(std::memory_order_acquire);
		if (CAP - (t - h) < n)
		{
			m_dropped.fetch_add(n, std::memory_order_relaxed);
			return false;
		}
		for (std::size_t i = 0; i < n; ++i)
			m_buf[(t + i) & (CAP - 1)] = Item{frame, bytes[i]};
		m_tail.store(t + n, std::memory_order_release);
		return true;
	}

	std::size_t popDue(std::uint8_t *out, std::size_t cap, std::uint64_t frame)
	{
		const std::size_t h = m_head.load(std::memory_order_relaxed);
		const std::size_t t = m_tail.load(std::memory_order_acquire);
		std::size_t n = 0;
		while (h + n < t && n < cap)
		{
			const Item &item = m_buf[(h + n) & (CAP - 1)];
			if (item.frame > frame) break;
			out[n++] = item.byte;
		}
		if (n > 0) m_head.store(h + n, std::memory_order_release);
		return n;
	}
	std::uint64_t dropped() const { return m_dropped.load(std::memory_order_acquire); }
	// Initialization only, after the consumer has observed playback input disabled.
	void discard() { m_head.store(m_tail.load(std::memory_order_acquire), std::memory_order_release); }
	void reset()
	{
		m_head.store(0, std::memory_order_relaxed);
		m_tail.store(0, std::memory_order_relaxed);
		m_dropped.store(0, std::memory_order_relaxed);
	}

private:
	struct Item { std::uint64_t frame; std::uint8_t byte; };
	static constexpr std::size_t CAP = 8192;
	Item m_buf[CAP] {};
	std::atomic<std::size_t> m_head {0}, m_tail {0};
	std::atomic<std::uint64_t> m_dropped {0};
};

TimedMidiRing g_host_timed_midi_ring;

// Exact-emulated-time control records for acceptance tests and host automation.
// Keep these separate from the immediate UI queues: a future scheduled record must
// never head-of-line-block a real front-panel edit.
template <std::size_t RecordSize, std::size_t Capacity = 256>
class TimedControlRing
{
public:
	bool push(const std::uint8_t *bytes, std::uint64_t frame)
	{
		const std::size_t t = m_tail.load(std::memory_order_relaxed);
		const std::size_t h = m_head.load(std::memory_order_acquire);
		if (t - h == Capacity)
		{
			m_dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		Item &item = m_buf[t & (Capacity - 1)];
		item.frame = frame;
		std::copy_n(bytes, RecordSize, item.bytes);
		m_tail.store(t + 1, std::memory_order_release);
		return true;
	}

	bool popDue(std::uint8_t *out, std::uint64_t frame)
	{
		const std::size_t h = m_head.load(std::memory_order_relaxed);
		const std::size_t t = m_tail.load(std::memory_order_acquire);
		if (h == t) return false;
		const Item &item = m_buf[h & (Capacity - 1)];
		if (item.frame > frame) return false;
		std::copy_n(item.bytes, RecordSize, out);
		m_head.store(h + 1, std::memory_order_release);
		return true;
	}

	std::uint64_t dropped() const { return m_dropped.load(std::memory_order_acquire); }
	void discard() { m_head.store(m_tail.load(std::memory_order_acquire), std::memory_order_release); }
	void reset()
	{
		m_head.store(0, std::memory_order_relaxed);
		m_tail.store(0, std::memory_order_relaxed);
		m_dropped.store(0, std::memory_order_relaxed);
	}

private:
	static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
	struct Item { std::uint64_t frame = 0; std::uint8_t bytes[RecordSize] {}; };
	Item m_buf[Capacity] {};
	std::atomic<std::size_t> m_head {0}, m_tail {0};
	std::atomic<std::uint64_t> m_dropped {0};
};

TimedControlRing<4> g_host_timed_panel_ring;
TimedControlRing<2> g_host_timed_adin_ring;
bool g_host_adin_prefer_rt = false; // MAME-thread-only arbitration state

// SPSC record ring for observing every firmware MIDI OUT byte. Producer is the MAME
// worker thread; consumer is a console/test or host message thread. This is deliberately
// separate from g_host_midi_tx_ring: raw timing inspection must never consume SysEx replies.
class MidiTxByteEventRing
{
public:
	bool push(std::uint8_t byte, double emu_seconds)
	{
		const std::size_t t = m_tail.load(std::memory_order_relaxed);
		const std::size_t h = m_head.load(std::memory_order_acquire);
		if (t - h == CAP)
		{
			m_dropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		m_buf[t & (CAP - 1)] = ProphecyEngine::MidiTxByteEvent{emu_seconds, byte};
		m_tail.store(t + 1, std::memory_order_release);
		return true;
	}

	std::size_t pop(ProphecyEngine::MidiTxByteEvent *out, std::size_t cap)
	{
		if (out == nullptr || cap == 0) return 0;
		const std::size_t h = m_head.load(std::memory_order_relaxed);
		const std::size_t t = m_tail.load(std::memory_order_acquire);
		const std::size_t n = std::min(cap, t - h);
		for (std::size_t i = 0; i < n; ++i) out[i] = m_buf[(h + i) & (CAP - 1)];
		m_head.store(h + n, std::memory_order_release);
		return n;
	}

	std::uint64_t dropped() const { return m_dropped.load(std::memory_order_acquire); }
	void reset()
	{
		m_head.store(0, std::memory_order_relaxed);
		m_tail.store(0, std::memory_order_relaxed);
		m_dropped.store(0, std::memory_order_relaxed);
	}

private:
	static constexpr std::size_t CAP = 1U << 16;
	ProphecyEngine::MidiTxByteEvent m_buf[CAP] {};
	std::atomic<std::size_t> m_head { 0 };
	std::atomic<std::size_t> m_tail { 0 };
	std::atomic<std::uint64_t> m_dropped { 0 };
};

MidiTxByteEventRing g_host_midi_tx_byte_ring;
std::atomic<bool> g_host_midi_tx_byte_capture_enabled { false };

// Front-panel LED state. The MAME worker publishes bank changes here; ordinary
// observers read electrical state, while the editor has a separate pulse-aware
// snapshot so a diagnostic read cannot steal a short visual event.
LedStore g_host_led_store;

// Latest HD44780 text. The MAME worker thread writes (via the kprop_set_host_lcd callback); the
// message thread reads (ProphecyEngine::latestLcd). A tiny mutex is fine here: writes are ~30 Hz
// and reads ~10 Hz, both OFF the audio thread. Mirrors g_host_midi_tx_ring (a namespace global).
class LcdStore
{
public:
	void store(const char *l1, const char *l2)
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_l1.assign(l1 ? l1 : "");
		m_l2.assign(l2 ? l2 : "");
		m_valid = true;
	}
	bool load(char *l1, char *l2, std::size_t cap) const
	{
		if (l1 == nullptr || l2 == nullptr || cap == 0) return false;
		std::lock_guard<std::mutex> lk(m_mu);
		if (!m_valid) { l1[0] = '\0'; l2[0] = '\0'; return false; }
		std::snprintf(l1, cap, "%s", m_l1.c_str());
		std::snprintf(l2, cap, "%s", m_l2.c_str());
		return true;
	}
	void reset()
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_l1.clear(); m_l2.clear(); m_valid = false;
	}
private:
	mutable std::mutex m_mu;
	std::string        m_l1, m_l2;
	bool               m_valid = false;
};

LcdStore g_host_lcd_store; // emulated HD44780 text -> host editor (message-thread poll)

// Raw LCD state: visible rows as HD44780 char codes + CGRAM, for the faceplate's
// dot-matrix renderer (custom glyphs). Same threading as LcdStore.
class LcdRawStore
{
public:
	void store(const std::uint8_t *r1, const std::uint8_t *r2, const std::uint8_t *cg)
	{
		std::lock_guard<std::mutex> lk(m_mu);
		std::memcpy(m_row1, r1, 40);
		std::memcpy(m_row2, r2, 40);
		std::memcpy(m_cgram, cg, 64);
		++m_version;
	}
	std::uint32_t load(std::uint8_t *r1, std::uint8_t *r2, std::uint8_t *cg) const
	{
		std::lock_guard<std::mutex> lk(m_mu);
		std::memcpy(r1, m_row1, 40);
		std::memcpy(r2, m_row2, 40);
		std::memcpy(cg, m_cgram, 64);
		return m_version;
	}
	void reset()
	{
		std::lock_guard<std::mutex> lk(m_mu);
		std::fill_n(m_row1, 40, std::uint8_t(0));
		std::fill_n(m_row2, 40, std::uint8_t(0));
		std::fill_n(m_cgram, 64, std::uint8_t(0));
		m_version = 0;
	}
private:
	mutable std::mutex m_mu;
	std::uint8_t m_row1[40] {}, m_row2[40] {}, m_cgram[64] {};
	std::uint32_t m_version = 0; // 0 = nothing pushed yet
};

LcdRawStore g_host_lcd_raw_store;

// Latest current-program dump, UNPACKED into raw program bytes (the editor reads knob values
// out of it by manifest offset). MAME thread writes when the firmware transmits a 0x40 dump;
// the message thread polls. A version counter lets a poller detect a fresh capture.
class ProgramDumpStore
{
public:
	void store(std::vector<std::uint8_t> raw)
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_raw = std::move(raw);
		++m_version;
	}
	std::size_t load(std::uint8_t *out, std::size_t cap, std::uint32_t *version) const
	{
		std::lock_guard<std::mutex> lk(m_mu);
		if (version) *version = m_version;
		const std::size_t n = (cap < m_raw.size()) ? cap : m_raw.size();
		for (std::size_t i = 0; out && i < n; ++i) out[i] = m_raw[i];
		return n;
	}
	void reset()
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_raw.clear(); m_version = 0;
	}
private:
	mutable std::mutex        m_mu;
	std::vector<std::uint8_t> m_raw;
	std::uint32_t             m_version = 0;
};

ProgramDumpStore g_program_dump_store;
std::atomic<std::uint64_t> g_data_load_completed{0};
std::atomic<std::uint64_t> g_identity_replies{0};
std::atomic<std::uint64_t> g_data_load_failed{0};
std::atomic<std::uint8_t> g_firmware_channel{0};

// Latest 0x69 arpeggio-pattern dump. Keep this independent of popMidiTx(): dump
// read-back must remain reliable even when another UI consumer drains the raw TX ring.
class ArpeggioPatternDumpStore
{
public:
	void store(int pattern, std::vector<std::uint8_t> raw)
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_pattern = pattern;
		m_raw = std::move(raw);
		++m_version;
	}
	std::size_t load(std::uint8_t *out, std::size_t cap, std::uint32_t *version, int *pattern) const
	{
		std::lock_guard<std::mutex> lk(m_mu);
		if (version) *version = m_version;
		if (pattern) *pattern = m_pattern;
		const std::size_t n = std::min(cap, m_raw.size());
		for (std::size_t i = 0; out && i < n; ++i) out[i] = m_raw[i];
		return n;
	}
	void reset()
	{
		std::lock_guard<std::mutex> lk(m_mu);
		m_raw.clear(); m_version = 0; m_pattern = -1;
	}
private:
	mutable std::mutex        m_mu;
	std::vector<std::uint8_t> m_raw;
	std::uint32_t             m_version = 0;
	int                       m_pattern = -1;
};

ArpeggioPatternDumpStore g_arpeggio_pattern_dump_store;

// Korg 7-in-8 unpack: each group is a high-bits byte followed by up to 7 low-7-bit bytes.
std::vector<std::uint8_t> korg_unpack(const std::uint8_t *p, std::size_t len)
{
	std::vector<std::uint8_t> out;
	out.reserve(len);
	for (std::size_t pos = 0; pos < len; )
	{
		const std::uint8_t high = p[pos++];
		for (int i = 0; i < 7 && pos < len; ++i)
			out.push_back(std::uint8_t((p[pos++] & 0x7f) | (((high >> i) & 1) << 7)));
	}
	return out;
}

} // anonymous namespace

// The korgprophecy driver calls these (on the MAME thread) via function pointers we register at
// engine start: pop = host->UART bytes to drain; tx = a complete sysex the firmware transmitted.
extern "C" void kprop_set_host_midi_pop(bool (*fn)(uint8_t *out, size_t cap, size_t *n,
	double emu_seconds));
extern "C" void kprop_set_host_midi_tx(void (*fn)(const uint8_t *bytes, size_t n));
extern "C" void kprop_set_host_midi_tx_byte(void (*fn)(uint8_t data, double emu_seconds));
extern "C" void kprop_set_host_lcd(void (*fn)(const char *line1, const char *line2));
extern "C" void kprop_set_host_lcd_raw(void (*fn)(const uint8_t *row1, const uint8_t *row2, const uint8_t *cgram));
extern "C" void kprop_set_host_panel_pop(bool (*fn)(uint8_t *row, uint8_t *bit,
	uint16_t *len_ms, double emu_seconds));

static void host_lcd_raw_impl(const uint8_t *row1, const uint8_t *row2, const uint8_t *cgram)
{
	g_host_lcd_raw_store.store(row1, row2, cgram);
}
extern "C" void kprop_set_host_adin_pop(bool (*fn)(uint8_t *source, uint8_t *value,
	double emu_seconds));
extern "C" void kprop_set_host_led(void (*fn)(uint8_t bank, uint8_t data));

static bool host_panel_pop_impl(uint8_t *row, uint8_t *bit, uint16_t *len_ms,
	double emu_seconds)
{
	uint8_t rec[4];
	if (g_host_panel_ring.pop(rec, 4) < 4)
	{
		const auto frame = (std::uint64_t)std::llround(
			emu_seconds * (double)ProphecyEngine::kSampleRate);
		if (!g_host_timed_panel_ring.popDue(rec, frame)) return false;
	}
	*row = rec[0]; *bit = rec[1];
	*len_ms = (uint16_t) (rec[2] | (rec[3] << 8));
	return true;
}

static bool host_adin_pop_impl(uint8_t *source, uint8_t *value, double emu_seconds)
{
	uint8_t rec[2];
	// Each queue remains genuinely SPSC: editor/timer writes never contend with the
	// audio callback. Alternate the first choice so sustained traffic on either producer
	// cannot starve the other; cross-thread writes have no meaningful total ordering.
	g_host_adin_prefer_rt = !g_host_adin_prefer_rt;
	MidiRing &first = g_host_adin_prefer_rt ? g_host_adin_rt_ring : g_host_adin_ui_ring;
	MidiRing &second = g_host_adin_prefer_rt ? g_host_adin_ui_ring : g_host_adin_rt_ring;
	if (first.pop(rec, 2) < 2 && second.pop(rec, 2) < 2)
	{
		const auto frame = (std::uint64_t)std::llround(
			emu_seconds * (double)ProphecyEngine::kSampleRate);
		if (!g_host_timed_adin_ring.popDue(rec, frame)) return false;
	}
	*source = rec[0]; *value = rec[1];
	return true;
}

static void host_led_impl(uint8_t bank, uint8_t data)
{
	g_host_led_store.set(bank, data);
}

static bool host_midi_pop_impl(uint8_t *out, size_t cap, size_t *n, double emu_seconds)
{
	*n = g_initialization_midi_ring.pop(out, cap);
	if (*n != 0) return true;
	if (!g_playback_input_enabled.load(std::memory_order_acquire)) return false;
	*n = g_host_midi_ring.pop(out, cap);
	if (*n == 0)
	{
		const auto frame = (std::uint64_t)std::llround(
			emu_seconds * (double)ProphecyEngine::kSampleRate);
		*n = g_host_timed_midi_ring.popDue(out, cap, frame);
	}
	return *n > 0;
}

static void host_midi_tx_impl(const uint8_t *bytes, size_t n)
{
	g_host_midi_tx_ring.push(bytes, n);
	if (n == 15 && bytes[0] == 0xf0 && bytes[1] == 0x7e && bytes[3] == 6
		&& bytes[4] == 2 && bytes[5] == 0x42 && bytes[6] == 0x41 && bytes[7] == 0
		&& bytes[8] == 1 && bytes[9] == 0 && bytes[14] == 0xf7)
	{
		g_firmware_channel.store(bytes[2] & 0x0f, std::memory_order_relaxed);
		g_identity_replies.fetch_add(1, std::memory_order_release);
	}
	// Also capture the current-program dump (F0 42 3n 41 40 01 <packed 7-bit> F7) for the editor's
	// param read-back: unpack the payload (message[6..n-1)) into raw program bytes + bump version.
	// Called per complete sysex on the MAME thread (not the audio thread), so allocation is fine.
	if (n >= 8 && bytes[0] == 0xF0 && bytes[4] == 0x40 && bytes[n - 1] == 0xF7)
	{
		g_firmware_channel.store(bytes[2] & 0x0f, std::memory_order_relaxed);
		g_program_dump_store.store(korg_unpack(bytes + 6, n - 7));
	}
	if (n == 6 && bytes[0] == 0xF0 && bytes[1] == 0x42 && bytes[3] == 0x41
		&& bytes[4] == 0x23 && bytes[5] == 0xF7)
		g_data_load_completed.fetch_add(1, std::memory_order_release);
	if (n >= 6 && bytes[0] == 0xf0 && bytes[1] == 0x42 && bytes[3] == 0x41
		&& (bytes[4] == 0x24 || bytes[4] == 0x26) && bytes[n - 1] == 0xf7)
		g_data_load_failed.fetch_add(1, std::memory_order_release);
	// ARPEGGIO PATTERN DATA DUMP: F0 42 3n 41 69 <unit/pattern> 00 <packed> F7.
	// A single-pattern reply unpacks to exactly 128 bytes. Preserve all-pattern replies
	// too (pattern=-1), though the first editor requests only one pattern at a time.
	if (n >= 9 && bytes[0] == 0xF0 && bytes[4] == 0x69 && bytes[n - 1] == 0xF7)
	{
		const int selector = bytes[5] & 0x1f;
		const int pattern = (selector & 0x10) ? -1 : (selector & 0x0f);
		g_arpeggio_pattern_dump_store.store(pattern, korg_unpack(bytes + 7, n - 8));
	}
}

static void host_midi_tx_byte_impl(uint8_t data, double emu_seconds)
{
	if (g_host_midi_tx_byte_capture_enabled.load(std::memory_order_relaxed))
		g_host_midi_tx_byte_ring.push(data, emu_seconds);
}

static void host_lcd_impl(const char *line1, const char *line2)
{
	g_host_lcd_store.store(line1, line2);
}

//============================================================
//  Impl
//============================================================
static std::size_t ring_frames_from_env()
{
	if (const char *e = std::getenv("PROPHOST_RING_FRAMES"))
		if (long v = std::atol(e); v >= 128) return (std::size_t) v;
	return 2048; // ~43 ms at 48 kHz: absorbs the worst-case spike with headroom, low enough to play
}

struct ProphecyEngine::Impl
{
	AudioRing                        ring{ring_frames_from_env()};
	prophecy::TimelineAudioRing       timeline{ProphecyEngine::kTimelineCapacity};
	bool                             host_timeline = false; // immutable after start
	std::atomic<bool>                playback_ready{false};
	std::atomic<const char*>         initialization_error{""};
	std::atomic<std::uint64_t>       playback_origin{0};
	std::mutex                       initialization_mutex; // non-realtime callers only

	std::mutex                       snapshot_mutex; // non-realtime callers only
	std::atomic<bool>                snapshot_requested{false};
	std::atomic<bool>                snapshot_enabled{false};
	std::array<std::uint8_t, 535>     program_snapshot{};
	const std::uint16_t*             program_ram = nullptr; // worker only
	bool                             snapshot_available = false;

	void serviceSnapshot()
	{
		if (!snapshot_requested.load(std::memory_order_acquire)) return;
		snapshot_available = program_ram != nullptr;
		if (snapshot_available)
			std::memcpy(program_snapshot.data(), program_ram + 0x4930 / 2, program_snapshot.size());
		snapshot_requested.store(false, std::memory_order_release);
	}
	std::atomic<std::uint64_t>       requested{0};
	std::atomic<std::uint64_t>       next_request{0};
	std::atomic<bool>                abort{false};
	std::atomic<std::uint32_t>       worker_period{128};
	std::uint32_t                    applied_period = 0; // worker only
	std::mutex                       output_mutex; // producer and offline reader only
	std::condition_variable          output_ready;
	std::uint64_t                    output_wait_end = 0; // protected by output_mutex
	std::uint64_t                    completed_horizon = 0; // protected by output_mutex
	std::uint64_t                    output_wait_horizon = 0; // protected by output_mutex
	std::thread                      thread;
	std::atomic<bool>                started{false};
	std::atomic<bool>                finished{false};
	std::atomic<uint64_t>            produced{0};
	int                              exitCode = 0;
	std::atomic<bool>                owns_singleton{false}; // this engine booted the machine
	bool                             midi_tx_byte_capture_enabled = false;
	std::atomic<ProphecyEngine::InstanceStatus> status{ProphecyEngine::InstanceStatus::NotStarted};
	std::atomic<std::uint64_t>       rejected_immediate_midi{0};
	std::atomic<std::uint64_t>       rejected_scheduled_midi{0};
	std::atomic<std::uint64_t>       rejected_ui_adin{0};
	std::atomic<std::uint64_t>       rejected_audio_adin{0};
	std::atomic<std::uint64_t>       rejected_scheduled_adin{0};

	void waitForRequest(std::uint64_t frame)
	{
		next_request.store(frame, std::memory_order_release);
		// Only the MAME worker sleeps. The realtime host publishes one atomic and
		// never waits on a mutex, condition variable, or the emulator.
		while (host_timeline && requested.load(std::memory_order_acquire) < frame
				&& !abort.load(std::memory_order_acquire))
		{
			serviceSnapshot();
			std::this_thread::sleep_for(std::chrono::microseconds(50));
		}
		serviceSnapshot();
#if defined(__APPLE__)
		const auto period = worker_period.load(std::memory_order_acquire);
		if (host_timeline && period != applied_period)
		{
			configure_audio_producer_scheduling(period);
			applied_period = period;
		}
#endif
	}
};

namespace {

class prophecy_osd : public
#if defined(SDLMAME_WIN32)
	osd_common_t
#else
	sdl_osd_interface
#endif
{
public:
	prophecy_osd(
#if defined(SDLMAME_WIN32)
		windows_options &options,
#else
		sdl_options &options,
#endif
		ProphecyEngine::Impl *impl)
		:
#if defined(SDLMAME_WIN32)
		osd_common_t(options),
#else
		sdl_osd_interface(options),
#endif
		m_impl(impl)
	{
#if defined(SDLMAME_WIN32)
		m_com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
#endif
	}

	~prophecy_osd()
	{
#if defined(SDLMAME_WIN32)
		if (m_com_initialized) CoUninitialize();
#endif
	}

	virtual void init(running_machine &machine) override
	{
#if defined(SDLMAME_WIN32)
		osd_common_t::init(machine);
		osd_common_t::init_subsystems();
#else
		sdl_osd_interface::init(machine);
		#endif
		// No emulated time may pass before the first host input range is known.
		m_impl->waitForRequest(ProphecyEngine::kAudioQuantum);
		if (m_impl->abort.load(std::memory_order_acquire)) machine.schedule_exit();
	}

	std::uint32_t audio_recording_quantum() const override
	{
		return m_impl->host_timeline ? ProphecyEngine::kAudioQuantum : 0;
	}

	void update(bool skipRedraw) override
	{
#if defined(SDLMAME_WIN32)
		osd_common_t::update(skipRedraw);
#else
		sdl_osd_interface::update(skipRedraw);
#endif
		// Startup UI and paused machines also need a worker-owned cancellation
		// point, even when the sound timer is not advancing.
		if (m_impl->abort.load(std::memory_order_acquire)) machine().schedule_exit();
	}

#if defined(SDLMAME_WIN32)
	virtual void input_update(bool) override { }
	virtual void check_osd_inputs() override { }
	virtual void process_events() override { }
	virtual bool has_focus() const override { return true; }
	virtual bool video_init() override
	{
		// The common OSD base has no windows, but MAME's startup UI still
		// requires one non-hidden render target for its UI container.
		machine().render().target_alloc();
		return true;
	}
#endif

	virtual void add_audio_to_recording(const int16_t *buffer, int samples_this_frame) override
	{
		if (!m_impl->program_ram)
			if (auto* ram = machine().root_device().memshare("sysram"); ram && ram->bytes() >= 0x40000)
				m_impl->program_ram = static_cast<const std::uint16_t*>(ram->ptr());
		m_impl->serviceSnapshot();
		if (buffer != nullptr && samples_this_frame > 0)
		{
			const auto first = m_impl->produced.load(std::memory_order_relaxed);
			if (m_impl->host_timeline)
			{
				std::size_t offset = 0;
				while (offset < std::size_t(samples_this_frame)
						&& !m_impl->abort.load(std::memory_order_acquire))
				{
					std::size_t got;
					bool notifyReader;
					{
						// Pair publication with the offline predicate to avoid lost
						// wakeups. The realtime reader never takes this mutex.
						std::lock_guard lock(m_impl->output_mutex);
						got = m_impl->timeline.push(first + offset,
							buffer + offset * 2, std::size_t(samples_this_frame) - offset);
						notifyReader = m_impl->output_wait_end != 0
							&& m_impl->timeline.end() >= m_impl->output_wait_end;
					}
					if (notifyReader) m_impl->output_ready.notify_one();
					offset += got;
					if (got == 0)
					{
						m_impl->serviceSnapshot();
						std::this_thread::sleep_for(std::chrono::microseconds(50));
					}
				}
			}
			else
				m_impl->ring.push(buffer, std::size_t(samples_this_frame) * ProphecyEngine::kChannels);
			m_impl->produced.store(first + std::uint64_t(samples_this_frame), std::memory_order_release);
		}
		#if defined(SDLMAME_WIN32)
		osd_common_t::add_audio_to_recording(buffer, samples_this_frame);
		#else
		sdl_osd_interface::add_audio_to_recording(buffer, samples_this_frame);
		#endif
		// Gate the *next* batch, including its MIDI, before advancing the machine.
		// Timer periods can round just below a sample in attotime. Derive the
		// boundary from machine time, also tolerating startup/teardown flushes.
		if (m_impl->host_timeline)
		{
			const auto completed = (machine().time().as_ticks(ProphecyEngine::kSampleRate) + 1)
				/ ProphecyEngine::kAudioQuantum * ProphecyEngine::kAudioQuantum;
			bool notifyReader;
			{
				std::lock_guard lock(m_impl->output_mutex);
				m_impl->completed_horizon = completed;
				notifyReader = m_impl->output_wait_end != 0
					&& completed >= m_impl->output_wait_horizon;
			}
			if (notifyReader) m_impl->output_ready.notify_one();
			m_impl->waitForRequest(completed + ProphecyEngine::kAudioQuantum);
		}
		// Only the worker accesses running_machine. Cancellation before init is
		// remembered, and cancellation during a render or either wait is observed here.
		if (m_impl->abort.load(std::memory_order_acquire)) machine().schedule_exit();
	}

private:
	ProphecyEngine::Impl *m_impl;
#if defined(SDLMAME_WIN32)
	bool m_com_initialized = false;
#endif
};

} // anonymous namespace

//============================================================
//  ProphecyEngine
//============================================================
ProphecyEngine::ProphecyEngine() : m_impl(std::make_unique<Impl>()) { }

ProphecyEngine::~ProphecyEngine() { stop(); }

namespace {
// The audio-config knobs the shipping GUI sets before booting korgprop. Without these,
// the dsp-dynarec branch's bare default wires DSP2's input to a dsp3_so1_to_si0 feedback
// loop -> a constant clipped drone at the DSP3->DAC output. With them, idle is silent and
// notes play the full DSP1->DSP2->DSP3 voice. (Phase-3 cleanup: bake these into the driver
// by building from korgprophecy-release so no env is needed.) setenv() before the worker
// thread starts, so the machine sees them during init.
void apply_audio_config()
{
	setenv("KPROP_DISABLE_AUTO_STIM_SWEEP", "1", 1);
	setenv("KPROP_V55_ADC_BANKSW_EXPERIMENT", "6", 0);
	setenv("KPROP_ADC_MUX_FROM_P2", "0", 1);
	setenv("KPROP_ADC_MUX_FROM_SHADOW", "1", 1);
	setenv("KPROP_ADC_MUX_AUTOSCAN", "0", 1);
	setenv("KPROP_DSP_HOST_MAP", "1,2,3", 1);
	setenv("KPROP_DSP2_INPUT_ROUTE", "normal", 1);
	setenv("KPROP_TXSM_FALLBACK_FIX", "1", 0);
	setenv("KPROP_DSP_SERIAL_FRAME_MODEL", "0", 1);
	// Enable the TMS57002 pooled dynarec. The
	// default patch's DSP programs compile during the ~2 s boot, before the host pulls at
	// real-time, so there's no first-note stall; a patch change compiles a new program on
	// first use (a brief one-time cost). Byte-identical to the interpreter (pf4 gate-green).
	// Windows x64 uses the same default now that the native-call ABI, shadow-space, and
	// CMEM guards are covered by its packaged and real-firmware gates. The low-level
	// KPROP_DSP_PERFRAME variable remains the highest-priority diagnostic override;
	// PROPHECY_DSP_ENGINE=interpreter is the readable emergency fallback for users.
	const char *dsp_engine = std::getenv("PROPHECY_DSP_ENGINE");
	setenv("KPROP_DSP_PERFRAME",
		(dsp_engine && std::string(dsp_engine) == "interpreter") ? "0" : "4", 0);
	// Keep pooled execution active while firmware coefficient updates are pending. The
	// per-CMEM-op guard deopts only the read that must consume the update queue. This was
	// formerly blocked by a threaded partial-frame discrepancy; the reconciled clean A64
	// backend now passes the console-driven boot/note/dense exact-PCM oracle as well as the
	// direct propmin oracle. Preserve an explicit caller override as the emergency kill switch.
	setenv("KPROP_PF4_CMEM_DEOPT", "1", 0);
}
} // namespace

bool ProphecyEngine::start(const std::vector<std::string> &args)
{
	if (m_impl->started.exchange(true)) return false;
	// Claim the process-global machine before installing process-global callbacks or
	// exposing any host queues. A rejected engine is inert: callers get an explicit
	// false/unavailable result and none of its APIs can target the active instance.
	if (g_engine_active.exchange(true))
	{
		m_impl->finished.store(true, std::memory_order_release);
		m_impl->status.store(InstanceStatus::Unavailable, std::memory_order_release);
		return false;
	}
	m_impl->owns_singleton = true;
	apply_audio_config();
	kprop_set_host_midi_pop(host_midi_pop_impl);   // host MIDI -> emulated UART
	kprop_set_host_midi_tx(host_midi_tx_impl);     // emulated UART sysex -> host
	kprop_set_host_midi_tx_byte(host_midi_tx_byte_impl); // timestamped raw MIDI OUT -> diagnostics
	kprop_set_host_lcd(host_lcd_impl);             // emulated HD44780 text -> host editor
	kprop_set_host_lcd_raw(host_lcd_raw_impl);     // raw char codes + CGRAM -> faceplate LCD
	kprop_set_host_panel_pop(host_panel_pop_impl); // faceplate buttons -> scan matrix
	kprop_set_host_adin_pop(host_adin_pop_impl);   // faceplate analog -> ADIN mux
	kprop_set_host_led(host_led_impl);             // front-panel LEDs -> host editor
	g_host_midi_tx_byte_capture_enabled.store(false, std::memory_order_release);
	// A previous machine in this process must not leak queued UI traffic or observer
	// bytes into a reconstructed engine. Reset only after acquiring the singleton.
	g_host_midi_ring.reset();
	g_initialization_midi_ring.reset();
	g_playback_input_enabled.store(!m_impl->host_timeline, std::memory_order_release);
	g_host_midi_tx_ring.reset();
	g_host_panel_ring.reset();
	g_host_adin_ui_ring.reset();
	g_host_adin_rt_ring.reset();
	g_host_adin_prefer_rt = false;
	g_host_led_store.reset();
	g_host_lcd_store.reset();
	g_host_lcd_raw_store.reset();
	g_program_dump_store.reset();
	g_data_load_completed.store(0);
	g_identity_replies.store(0);
	g_data_load_failed.store(0);
	g_firmware_channel.store(0);
	g_arpeggio_pattern_dump_store.reset();
	g_host_midi_tx_byte_ring.reset();
	g_host_midi_tx_byte_capture_enabled.store(
		m_impl->midi_tx_byte_capture_enabled, std::memory_order_release);
	g_host_timed_midi_ring.reset();
	g_host_timed_panel_ring.reset();
	g_host_timed_adin_ring.reset();
	// Publish queue readiness only after initialization, before the worker can
	// consume input. A ROM-picker boot may race the DAW's audio callback.
	m_impl->status.store(InstanceStatus::Active, std::memory_order_release);
	m_impl->playback_ready.store(!m_impl->host_timeline, std::memory_order_release);
	Impl *impl = m_impl.get();
	m_impl->thread = std::thread([impl, args]() {
#if defined(__APPLE__)
		impl->applied_period = impl->host_timeline ? impl->worker_period.load() : 960;
		configure_audio_producer_scheduling(impl->applied_period);
#endif
		std::vector<std::string> a = args; // start_frontend wants a non-const ref
		#if defined(SDLMAME_WIN32)
		windows_options options;
		#else
		sdl_options options;
		#endif
		prophecy_osd osd(options, impl);
		osd.register_options();
		impl->exitCode = emulator_info::start_frontend(options, osd, a);
		{
			std::lock_guard lock(impl->output_mutex);
			impl->finished.store(true);
		}
		impl->output_ready.notify_all();
		impl->ring.set_done();
	});
	return true;
}

bool ProphecyEngine::enableMidiTxByteCapture(bool enabled)
{
	if (m_impl->started.load(std::memory_order_acquire)) return false;
	// Remember the request per engine. Touch the process-global observer only after this
	// engine has successfully acquired the MAME slot in start(); otherwise configuring a
	// future/rejected instance could reset the active instance's diagnostic stream.
	m_impl->midi_tx_byte_capture_enabled = enabled;
	return true;
}

void ProphecyEngine::stop()
{
	if (!m_impl->started.load()) return;
	if (!m_impl->owns_singleton.load(std::memory_order_acquire)) return;
	{
		std::lock_guard lock(m_impl->output_mutex);
		m_impl->abort.store(true, std::memory_order_release);
	}
	m_impl->output_ready.notify_all();
	m_impl->ring.set_abort(); // release legacy FIFO backpressure too
	// A plugin cannot unload code or storage still used by a detached worker.
	// All engine waits observe abort; let the worker request its own MAME exit.
	if (m_impl->thread.joinable()) m_impl->thread.join();
	m_impl->owns_singleton.store(false, std::memory_order_release);
	m_impl->status.store(InstanceStatus::Stopped, std::memory_order_release);
	g_engine_active.store(false, std::memory_order_release);
}

bool ProphecyEngine::running() const  { return ownsMachineSlot() && m_impl->started.load() && !m_impl->finished.load(); }
bool ProphecyEngine::finished() const { return m_impl->finished.load(); }
bool ProphecyEngine::ownsMachineSlot() const
{
	return m_impl->owns_singleton.load(std::memory_order_acquire)
		&& m_impl->status.load(std::memory_order_acquire) == InstanceStatus::Active;
}
ProphecyEngine::InstanceStatus ProphecyEngine::instanceStatus() const
{
	return m_impl->status.load(std::memory_order_acquire);
}

std::size_t ProphecyEngine::pull(float *left, float *right, std::size_t frames)
{
	if (left == nullptr || right == nullptr || frames == 0) return 0;
	return m_impl->ring.pop_planar(left, right, frames);
}

bool ProphecyEngine::enableHostTimeline()
{
	if (m_impl->started.load(std::memory_order_acquire)) return false;
	m_impl->host_timeline = true;
	return true;
}

const char* ProphecyEngine::initializationError() const
{
	return m_impl->initialization_error.load(std::memory_order_acquire);
}

bool ProphecyEngine::readyForPlayback() const
{
	return running() && m_impl->playback_ready.load(std::memory_order_acquire);
}

std::uint64_t ProphecyEngine::playbackOrigin() const
{
	return m_impl->playback_origin.load(std::memory_order_acquire);
}

bool ProphecyEngine::waitingForOutput() const
{
	std::lock_guard lock(m_impl->output_mutex);
	return m_impl->output_wait_end != 0;
}

bool ProphecyEngine::waitingForInput() const
{
	return running() && m_impl->host_timeline
		&& m_impl->next_request.load(std::memory_order_acquire) > requestedFrames();
}

bool ProphecyEngine::initializePlayback(const std::uint8_t *state, std::size_t bytes,
	bool firmwareHandshake)
{
	std::lock_guard initializationLock(m_impl->initialization_mutex);
	if (!running() || !m_impl->host_timeline) return false;
	const bool wasReady = m_impl->playback_ready.exchange(false, std::memory_order_acq_rel);
	g_playback_input_enabled.store(false, std::memory_order_release);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	auto advance = [&] {
		if (std::chrono::steady_clock::now() >= deadline) return false;
		const auto end = (requestedFrames() / kAudioQuantum + 4) * kAudioQuantum;
		// Initialization audio precedes the host epoch and must not fill the ring.
		m_impl->timeline.seek(end);
		std::unique_lock lock(m_impl->output_mutex);
		m_impl->output_wait_end = 1;
		m_impl->output_wait_horizon = end;
		requestThroughFrame(end);
		const bool completed = m_impl->output_ready.wait_until(lock, deadline, [&] {
			return m_impl->completed_horizon >= end || !running() || m_impl->abort.load();
		});
		m_impl->output_wait_end = 0;
		return completed && running() && !m_impl->abort.load();
	};
	const char* failure = "Firmware startup did not complete. Reload the plugin to try again.";
	auto fail = [&] {
		m_impl->initialization_error.store(failure, std::memory_order_release);
		std::fprintf(stderr, "Profligacy initialization failed: %s\n", failure);
		stop();
		return false;
	};
	if (wasReady && state && bytes)
	{
		// The processor has paused its callback before a subsequent restore.
		// Let the worker observe disabled playback input before discarding queued
		// events from the old epoch; they must not play after the new program loads.
		if (!advance()) return fail();
		g_host_midi_ring.discard();
		g_host_timed_midi_ring.discard();
		g_host_panel_ring.discard();
		g_host_timed_panel_ring.discard();
		g_host_adin_ui_ring.discard();
		g_host_adin_rt_ring.discard();
		g_host_timed_adin_ring.discard();
	}
	if (firmwareHandshake && !wasReady)
	{
		// The program page is firmware evidence that startup has reached the
		// MIDI parser. Sending bytes during the power-on self-test can wedge it.
		char line1[41]{}, line2[41]{};
		while (!latestLcd(line1, line2, sizeof(line1))
			|| (line1[0] != 'A' && line1[0] != 'B')
			|| line1[1] < '0' || line1[1] > '9' || line1[2] < '0' || line1[2] > '9'
			|| line1[3] != ':')
			if (!advance()) return fail();
		// Universal inquiry is independent of the Korg SysEx receive filter and
		// broadcasts across device channels. Its reply also identifies the channel.
		failure = "Firmware did not answer its MIDI identity request. Reload the plugin to try again.";
		const auto identity = g_identity_replies.load(std::memory_order_acquire);
		const std::uint8_t request[] = {0xf0, 0x7e, 0x7f, 6, 1, 0xf7};
		if (!g_initialization_midi_ring.pushAll(request, sizeof(request))) return fail();
		while (g_identity_replies.load(std::memory_order_acquire) == identity)
			if (!advance()) return fail();
	}
	if (state && bytes)
	{
		failure = "The saved program is invalid and could not be restored.";
		if (bytes < 8 || state[0] != 0xf0 || state[1] != 0x42 || state[3] != 0x41
			|| state[4] != 0x40 || state[bytes - 1] != 0xf7) return fail();
		std::vector<std::uint8_t> message(state, state + bytes);
		message[2] = 0x30 | g_firmware_channel.load(std::memory_order_acquire);
		failure = "The firmware did not accept the saved program. Check that MIDI SysEx reception is enabled, then reload the plugin.";
		const auto acknowledged = g_data_load_completed.load(std::memory_order_acquire);
		const auto rejected = g_data_load_failed.load(std::memory_order_acquire);
		if (!g_initialization_midi_ring.pushAll(message.data(), message.size())) return fail();
		while (g_data_load_completed.load(std::memory_order_acquire) == acknowledged)
			if (g_data_load_failed.load(std::memory_order_acquire) != rejected || !advance()) return fail();
	}
	if (firmwareHandshake)
	{
		// Cache the actual edit buffer, and verify an initial restore rather than
		// relying on an acknowledgement alone. A dump is optional for ordinary
		// boot: disabling Korg SysEx reception must not prevent MIDI playback.
		std::uint32_t before = 0, version = 0;
		g_program_dump_store.load(nullptr, 0, &before);
		const std::uint8_t request[] = {0xf0, 0x42,
			std::uint8_t(0x30 | g_firmware_channel.load(std::memory_order_acquire)), 0x41, 0x10, 0, 0xf7};
		// A load acknowledgement can precede completion of the firmware's
		// patch transition. Requests received while that task is busy are
		// discarded. Retry one complete query after a bounded response window.
		do
		{
			if (std::getenv("PROPHOST_INIT_STATS")) std::fprintf(stderr, "init query at %llu\n", (unsigned long long) requestedFrames());
			if (!g_initialization_midi_ring.pushAll(request, sizeof(request))) return fail();
			const auto end = requestedFrames() + kSampleRate / 2;
			do
			{
				if (!advance()) return fail();
				g_program_dump_store.load(nullptr, 0, &version);
			} while (version == before && requestedFrames() < end);
		} while (version == before && state && bytes);
		if (state && bytes)
		{
			failure = "The firmware program readback did not match the saved state. Reload the plugin to try again.";
			std::uint8_t actual[1024]{};
			const auto size = g_program_dump_store.load(actual, sizeof(actual), nullptr);
			const auto expected = korg_unpack(state + 6, bytes - 7);
			if (version == before || size != expected.size()
				|| !std::equal(expected.begin(), expected.end(), actual))
			{
				std::size_t first = 0;
				while (first < std::min(size, expected.size()) && actual[first] == expected[first]) ++first;
				std::fprintf(stderr, "program readback: version %u -> %u, bytes %zu/%zu, first difference %zu\n",
					before, version, size, expected.size(), first);
				return fail();
			}
		}
	}
	if (firmwareHandshake && state && bytes)
	{
		// Drain any outstanding query through the firmware parser before notes.
		const auto identity = g_identity_replies.load(std::memory_order_acquire);
		const std::uint8_t request[] = {0xf0, 0x7e, 0x7f, 6, 1, 0xf7};
		if (!g_initialization_midi_ring.pushAll(request, sizeof(request))) return fail();
		while (g_identity_replies.load(std::memory_order_acquire) == identity)
			if (!advance()) return fail();
	}
	if (std::getenv("PROPHOST_INIT_STATS")) std::fprintf(stderr, "init ready at %llu\n", (unsigned long long) requestedFrames());
	m_impl->snapshot_enabled.store(firmwareHandshake, std::memory_order_release);
	m_impl->initialization_error.store("", std::memory_order_release);
	m_impl->playback_origin.store(requestedFrames(), std::memory_order_release);
	g_playback_input_enabled.store(true, std::memory_order_release);
	m_impl->playback_ready.store(true, std::memory_order_release);
	return true;
}

std::vector<std::uint8_t> ProphecyEngine::snapshotProgram()
{
	// Copy applied firmware state at a worker boundary even when the DAW has
	// stopped requesting audio. Saving neither sends MIDI nor advances time.
	if (!readyForPlayback() || !m_impl->snapshot_enabled.load(std::memory_order_acquire)) return {};
	std::lock_guard lock(m_impl->snapshot_mutex);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	auto wait = [&] {
		while (m_impl->snapshot_requested.load(std::memory_order_acquire))
		{
			if (!running() || std::chrono::steady_clock::now() >= deadline) return false;
			std::this_thread::sleep_for(std::chrono::microseconds(50));
		}
		return running();
	};
	if (!wait()) return {};
	m_impl->snapshot_requested.store(true, std::memory_order_release);
	if (!wait() || !m_impl->snapshot_available) return {};
	return {m_impl->program_snapshot.begin(), m_impl->program_snapshot.end()};
}

void ProphecyEngine::setHostBlockFrames(std::uint32_t frames)
{
	m_impl->worker_period.store(std::max(frames, kAudioQuantum), std::memory_order_release);
}

void ProphecyEngine::requestThroughFrame(std::uint64_t frame)
{
	m_impl->requested.store(frame, std::memory_order_release);
}

std::uint64_t ProphecyEngine::requestedFrames() const
{
	return m_impl->requested.load(std::memory_order_acquire);
}

std::size_t ProphecyEngine::readAtFrame(std::uint64_t first, float *left,
	float *right, std::size_t frames, bool offline)
{
	if (!left || !right || frames == 0) return 0;
	if (!ownsMachineSlot() || frames > m_impl->timeline.capacity())
	{
		std::fill(left, left + frames, 0.0f);
		std::fill(right, right + frames, 0.0f);
		return 0;
	}
	m_impl->timeline.seek(first);
	if (offline && running())
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		std::unique_lock lock(m_impl->output_mutex);
		m_impl->output_wait_end = first + frames;
		// A host may return from prefetch to realtime on its very next callback.
		// Finish every complete quantum of the current input grant, preserving
		// the same producer lead that realtime processing relies on.
		m_impl->output_wait_horizon = requestedFrames() / kAudioQuantum * kAudioQuantum;
		m_impl->output_ready.wait_until(lock, deadline, [&] {
			return (m_impl->timeline.end() >= first + frames
				&& m_impl->completed_horizon >= m_impl->output_wait_horizon) || !running()
				|| m_impl->abort.load(std::memory_order_acquire);
		});
		m_impl->output_wait_end = 0;
	}
	return m_impl->timeline.read(first, left, right, frames);
}

bool ProphecyEngine::pushMidi(const std::uint8_t *bytes, std::size_t n)
{
	if (bytes == nullptr || n == 0) return true;
	if (!ownsMachineSlot() || (m_impl->host_timeline && !m_impl->playback_ready.load(std::memory_order_acquire)))
	{
		m_impl->rejected_immediate_midi.fetch_add(n, std::memory_order_relaxed);
		return false;
	}
	return g_host_midi_ring.pushAllCounted(bytes, n);
}

bool ProphecyEngine::pushMidiAtFrame(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame)
{
	if (bytes == nullptr || n == 0) return true;
	if (!ownsMachineSlot() || (m_impl->host_timeline && !m_impl->playback_ready.load(std::memory_order_acquire)))
	{
		m_impl->rejected_scheduled_midi.fetch_add(n, std::memory_order_relaxed);
		return false;
	}
	return g_host_timed_midi_ring.push(bytes, n, frame);
}

std::uint64_t ProphecyEngine::droppedImmediateMidiBytes() const
{
	const std::uint64_t rejected = m_impl->rejected_immediate_midi.load(std::memory_order_acquire);
	return rejected + (ownsMachineSlot() ? g_host_midi_ring.dropped() : 0);
}

std::uint64_t ProphecyEngine::droppedScheduledMidiBytes() const
{
	const std::uint64_t rejected = m_impl->rejected_scheduled_midi.load(std::memory_order_acquire);
	return rejected + (ownsMachineSlot() ? g_host_timed_midi_ring.dropped() : 0);
}

void ProphecyEngine::pushPanelPulse(int row, int bit, int len_ms)
{
	if (!ownsMachineSlot() || (m_impl->host_timeline && !m_impl->playback_ready.load(std::memory_order_acquire))) return;
	if (row < 0 || row > 7 || bit < 0 || bit > 7) return;
	if (len_ms <= 0) len_ms = 75;
	if (len_ms > 2000) len_ms = 2000;
	const std::uint8_t rec[4] = {
		(std::uint8_t) row, (std::uint8_t) bit,
		(std::uint8_t) (len_ms & 0xff), (std::uint8_t) ((len_ms >> 8) & 0xff) };
	g_host_panel_ring.pushAll(rec, 4);
}

bool ProphecyEngine::pushPanelPulseAtFrame(int row, int bit, int len_ms, std::uint64_t frame)
{
	if (!ownsMachineSlot() || (m_impl->host_timeline && !m_impl->playback_ready.load(std::memory_order_acquire)) || row < 0 || row > 7 || bit < 0 || bit > 7) return false;
	if (len_ms <= 0) len_ms = 75;
	if (len_ms > 2000) len_ms = 2000;
	const std::uint8_t rec[4] = {
		(std::uint8_t) row, (std::uint8_t) bit,
		(std::uint8_t) (len_ms & 0xff), (std::uint8_t) ((len_ms >> 8) & 0xff) };
	return g_host_timed_panel_ring.push(rec, frame);
}

bool ProphecyEngine::pushAdin(int source, int value)
{
	if (source < 0 || source > 15 || value < 0 || value > 255) return false;
	if (!ownsMachineSlot() || (m_impl->host_timeline && !m_impl->playback_ready.load(std::memory_order_acquire)))
	{
		m_impl->rejected_ui_adin.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const std::uint8_t rec[2] = { (std::uint8_t) source, (std::uint8_t) value };
	return g_host_adin_ui_ring.pushAllCounted(rec, 2);
}

bool ProphecyEngine::pushAdinFromAudio(int source, int value)
{
	if (source < 0 || source > 15 || value < 0 || value > 255) return false;
	if (!ownsMachineSlot() || (m_impl->host_timeline && !m_impl->playback_ready.load(std::memory_order_acquire)))
	{
		m_impl->rejected_audio_adin.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const std::uint8_t rec[2] = { (std::uint8_t) source, (std::uint8_t) value };
	return g_host_adin_rt_ring.pushAllCounted(rec, 2);
}

bool ProphecyEngine::pushAdinAtFrame(int source, int value, std::uint64_t frame)
{
	if (source < 0 || source > 15 || value < 0 || value > 255) return false;
	if (!ownsMachineSlot() || (m_impl->host_timeline && !m_impl->playback_ready.load(std::memory_order_acquire)))
	{
		m_impl->rejected_scheduled_adin.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const std::uint8_t rec[2] = { (std::uint8_t) source, (std::uint8_t) value };
	return g_host_timed_adin_ring.push(rec, frame);
}

std::uint64_t ProphecyEngine::droppedUiAdinEvents() const
{
	const std::uint64_t rejected = m_impl->rejected_ui_adin.load(std::memory_order_acquire);
	return rejected + (ownsMachineSlot() ? g_host_adin_ui_ring.dropped() / 2 : 0);
}

std::uint64_t ProphecyEngine::droppedAudioAdinEvents() const
{
	const std::uint64_t rejected = m_impl->rejected_audio_adin.load(std::memory_order_acquire);
	return rejected + (ownsMachineSlot() ? g_host_adin_rt_ring.dropped() / 2 : 0);
}

std::uint64_t ProphecyEngine::droppedScheduledPanelEvents() const
{
	return ownsMachineSlot() ? g_host_timed_panel_ring.dropped() : 0;
}

std::uint64_t ProphecyEngine::droppedScheduledAdinEvents() const
{
	return m_impl->rejected_scheduled_adin.load(std::memory_order_acquire)
		+ (ownsMachineSlot() ? g_host_timed_adin_ring.dropped() : 0);
}

std::uint32_t ProphecyEngine::ledSnapshot(std::uint8_t out[12]) const
{
	if (!ownsMachineSlot())
	{
		if (out != nullptr) std::fill_n(out, 12, std::uint8_t(0));
		return 0;
	}
	return g_host_led_store.snapshot(out);
}

std::uint32_t ProphecyEngine::ledVisualSnapshot(std::uint8_t out[12]) const
{
	if (!ownsMachineSlot())
	{
		if (out != nullptr) std::fill_n(out, 12, std::uint8_t(0));
		return 0;
	}
	return g_host_led_store.visualSnapshot(out);
}

std::uint32_t ProphecyEngine::lcdRawSnapshot(std::uint8_t row1[40], std::uint8_t row2[40], std::uint8_t cgram[64]) const
{
	if (!ownsMachineSlot())
	{
		if (row1 != nullptr) std::fill_n(row1, 40, std::uint8_t(0));
		if (row2 != nullptr) std::fill_n(row2, 40, std::uint8_t(0));
		if (cgram != nullptr) std::fill_n(cgram, 64, std::uint8_t(0));
		return 0;
	}
	return g_host_lcd_raw_store.load(row1, row2, cgram);
}

void ProphecyEngine::lcdA00GlyphRows(std::uint8_t out[kLcdA00GlyphRowBytes])
{
	if (out == nullptr)
		return;
	u8 const *const cgrom = hd44780_a00_reconstructed_cgrom();
	for (std::size_t ch = 0; ch < 256; ++ch)
		std::memcpy(out + ch * 8, cgrom + ch * 16, 8);
}

std::size_t ProphecyEngine::popMidiTx(std::uint8_t *out, std::size_t cap)
{
	if (!ownsMachineSlot()) return 0;
	return g_host_midi_tx_ring.pop(out, cap);
}

std::size_t ProphecyEngine::popMidiTxByteEvents(MidiTxByteEvent *out, std::size_t cap)
{
	if (!ownsMachineSlot()) return 0;
	return g_host_midi_tx_byte_ring.pop(out, cap);
}

std::uint64_t ProphecyEngine::droppedMidiTxByteEvents() const
{
	if (!ownsMachineSlot()) return 0;
	return g_host_midi_tx_byte_ring.dropped();
}

bool ProphecyEngine::latestLcd(char *line1, char *line2, std::size_t cap) const
{
	if (!ownsMachineSlot())
	{
		if (line1 != nullptr && cap > 0) line1[0] = '\0';
		if (line2 != nullptr && cap > 0) line2[0] = '\0';
		return false;
	}
	return g_host_lcd_store.load(line1, line2, cap);
}

std::size_t ProphecyEngine::latestProgramData(std::uint8_t *out, std::size_t cap, std::uint32_t *version) const
{
	if (!ownsMachineSlot())
	{
		if (version != nullptr) *version = 0;
		return 0;
	}
	return g_program_dump_store.load(out, cap, version);
}

std::size_t ProphecyEngine::latestArpeggioPatternData(std::uint8_t *out, std::size_t cap,
	std::uint32_t *version, int *pattern) const
{
	if (!ownsMachineSlot())
	{
		if (version != nullptr) *version = 0;
		if (pattern != nullptr) *pattern = -1;
		return 0;
	}
	return g_arpeggio_pattern_dump_store.load(out, cap, version, pattern);
}

std::size_t   ProphecyEngine::available() const     { return m_impl->host_timeline ? m_impl->timeline.available() : m_impl->ring.count() / kChannels; }
std::size_t   ProphecyEngine::ringFrames() const    { return m_impl->ring.capacity_frames(); }
std::uint64_t ProphecyEngine::producedFrames() const { return m_impl->produced.load(); }
