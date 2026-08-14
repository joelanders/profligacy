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

#include "prophecy_engine.h"

#include <algorithm>
#include <atomic>
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
void configure_audio_producer_scheduling()
{
	// MAME publishes 960-frame chunks at 48 kHz, i.e. one production quantum every
	// 20 ms.  Give that worker an audio-style time constraint with some headroom;
	// ring backpressure still puts it to sleep whenever it has rendered far enough
	// ahead.  This mirrors JUCE's macOS realtime-thread implementation.
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	mach_timebase_info_data_t timebase{};
	kern_return_t result = mach_timebase_info(&timebase);
	if (result == KERN_SUCCESS && timebase.numer != 0)
	{
		auto ticks_for_ms = [&](std::uint64_t ms) {
			return static_cast<std::uint32_t>(ms * 1'000'000ULL * timebase.denom / timebase.numer);
		};
		thread_time_constraint_policy_data_t policy{};
		policy.period = ticks_for_ms(20);
		policy.computation = ticks_for_ms(18);
		policy.constraint = ticks_for_ms(20);
		policy.preemptible = true;
		result = thread_policy_set(
			pthread_mach_thread_np(pthread_self()),
			THREAD_TIME_CONSTRAINT_POLICY,
			reinterpret_cast<thread_policy_t>(&policy),
			THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	}
	if (std::getenv("PROPHOST_SCHED_STATS"))
		std::fprintf(stderr, "[prophost-sched] producer_time_constraint=%d result=%d\n",
			result == KERN_SUCCESS, int(result));
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

// Front-panel LED state. The MAME thread pushes (bank, data) snapshots via the
// kprop_set_host_led callback; the message thread polls the accumulated banks
// (12 banks x 8 bits = leds[96]). Atomics — no locks near either thread.
struct LedStore
{
	void set(std::uint8_t bank, std::uint8_t data)
	{
		if (bank >= 12) return;
		m_banks[bank].store(data, std::memory_order_relaxed);
		m_version.fetch_add(1, std::memory_order_release);
	}
	std::uint32_t snapshot(std::uint8_t out[12]) const
	{
		const std::uint32_t v = m_version.load(std::memory_order_acquire);
		for (int i = 0; i < 12; i++) out[i] = m_banks[i].load(std::memory_order_relaxed);
		return v;
	}
	void reset()
	{
		for (auto &bank : m_banks) bank.store(0, std::memory_order_relaxed);
		m_version.store(0, std::memory_order_release);
	}
private:
	std::atomic<std::uint8_t>  m_banks[12] {};
	std::atomic<std::uint32_t> m_version { 0 };
};

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
	// Also capture the current-program dump (F0 42 3n 41 40 00 <packed 7-bit> F7) for the editor's
	// param read-back: unpack the payload (message[6..n-1)) into raw program bytes + bump version.
	// Called per complete sysex on the MAME thread (not the audio thread), so allocation is fine.
	if (n >= 8 && bytes[0] == 0xF0 && bytes[4] == 0x40 && bytes[n - 1] == 0xF7)
		g_program_dump_store.store(korg_unpack(bytes + 6, n - 7));
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
	std::thread                      thread;
	std::atomic<bool>                started{false};
	std::atomic<bool>                finished{false};
	std::atomic<running_machine *>   machine{nullptr};
	std::atomic<uint64_t>            produced{0};
	int                              exitCode = 0;
	std::atomic<bool>                owns_singleton{false}; // this engine booted the machine
	bool                             midi_tx_byte_capture_enabled = false;
	std::atomic<ProphecyEngine::InstanceStatus> status{ProphecyEngine::InstanceStatus::NotStarted};
	std::atomic<std::uint64_t>       rejected_immediate_midi{0};
	std::atomic<std::uint64_t>       rejected_scheduled_midi{0};
	std::atomic<std::uint64_t>       rejected_ui_adin{0};
	std::atomic<std::uint64_t>       rejected_audio_adin{0};
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
		m_impl->machine.store(&machine); // captured so stop() can schedule_exit()
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
		if (buffer != nullptr && samples_this_frame > 0)
		{
			m_impl->produced.fetch_add(uint64_t(samples_this_frame));
			m_impl->ring.push(buffer, std::size_t(samples_this_frame) * ProphecyEngine::kChannels);
		}
		#if defined(SDLMAME_WIN32)
		osd_common_t::add_audio_to_recording(buffer, samples_this_frame);
		#else
		sdl_osd_interface::add_audio_to_recording(buffer, samples_this_frame);
		#endif
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
	m_impl->status.store(InstanceStatus::Active, std::memory_order_release);
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
	g_host_midi_tx_ring.reset();
	g_host_panel_ring.reset();
	g_host_adin_ui_ring.reset();
	g_host_adin_rt_ring.reset();
	g_host_adin_prefer_rt = false;
	g_host_led_store.reset();
	g_host_lcd_store.reset();
	g_host_lcd_raw_store.reset();
	g_program_dump_store.reset();
	g_arpeggio_pattern_dump_store.reset();
	g_host_midi_tx_byte_ring.reset();
	g_host_midi_tx_byte_capture_enabled.store(
		m_impl->midi_tx_byte_capture_enabled, std::memory_order_release);
	g_host_timed_midi_ring.reset();
	g_host_timed_panel_ring.reset();
	g_host_timed_adin_ring.reset();
	Impl *impl = m_impl.get();
	m_impl->thread = std::thread([impl, args]() {
#if defined(__APPLE__)
		configure_audio_producer_scheduling();
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
		// The machine is destroyed once start_frontend returns; anyone still holding the
		// pointer must see null. MAME's SDL OSD handles SIGTERM itself, so on a
		// signal-quit the machine is gone BEFORE JUCE shutdown reaches
		// ProphecyEngine::stop() — schedule_exit() on the stale pointer was a
		// crash-on-quit (2026-07-10 .ips: stop() -> running_machine::schedule_exit).
		impl->machine.store(nullptr);
		impl->finished.store(true);
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
	// finished first: once the worker returns from start_frontend the machine is freed
	// (it also nulls the pointer; the pair closes the signal-quit teardown race).
	if (!m_impl->finished.load())
		if (running_machine *m = m_impl->machine.load()) m->schedule_exit(); // ask MAME to leave its loop
	m_impl->ring.set_abort();                                            // unblock a producer stuck on a full ring

	// Bounded join: wait up to ~3 s for MAME to return from start_frontend, then join.
	// If it doesn't (a wedged teardown), detach rather than ever hanging the host on
	// unload. A detached machine keeps the process singleton, so no re-boot afterwards.
	for (int i = 0; i < 300 && !m_impl->finished.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	if (m_impl->finished.load())
	{
		if (m_impl->thread.joinable()) m_impl->thread.join();
		// Make this object's global-I/O methods inert before another engine can claim
		// the slot and install a replacement set of process-global callbacks.
		m_impl->owns_singleton.store(false, std::memory_order_release);
		m_impl->status.store(InstanceStatus::Stopped, std::memory_order_release);
		g_engine_active.store(false, std::memory_order_release);
	}
	else if (m_impl->thread.joinable())
	{
		m_impl->thread.detach();
	}
}

bool ProphecyEngine::running() const  { return ownsMachineSlot() && m_impl->started.load() && !m_impl->finished.load(); }
bool ProphecyEngine::finished() const { return m_impl->finished.load(); }
bool ProphecyEngine::ownsMachineSlot() const
{
	return m_impl->owns_singleton.load(std::memory_order_acquire);
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

bool ProphecyEngine::pushMidi(const std::uint8_t *bytes, std::size_t n)
{
	if (bytes == nullptr || n == 0) return true;
	if (!ownsMachineSlot())
	{
		m_impl->rejected_immediate_midi.fetch_add(n, std::memory_order_relaxed);
		return false;
	}
	return g_host_midi_ring.pushAllCounted(bytes, n);
}

bool ProphecyEngine::pushMidiAtFrame(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame)
{
	if (bytes == nullptr || n == 0) return true;
	if (!ownsMachineSlot())
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
	if (!ownsMachineSlot()) return;
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
	if (!ownsMachineSlot() || row < 0 || row > 7 || bit < 0 || bit > 7) return false;
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
	if (!ownsMachineSlot())
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
	if (!ownsMachineSlot())
	{
		m_impl->rejected_audio_adin.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const std::uint8_t rec[2] = { (std::uint8_t) source, (std::uint8_t) value };
	return g_host_adin_rt_ring.pushAllCounted(rec, 2);
}

bool ProphecyEngine::pushAdinAtFrame(int source, int value, std::uint64_t frame)
{
	if (!ownsMachineSlot() || source < 0 || source > 15 || value < 0 || value > 255)
		return false;
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
	return ownsMachineSlot() ? g_host_timed_adin_ring.dropped() : 0;
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

std::size_t   ProphecyEngine::available() const     { return m_impl->ring.count() / kChannels; }
std::size_t   ProphecyEngine::ringFrames() const    { return m_impl->ring.capacity_frames(); }
std::uint64_t ProphecyEngine::producedFrames() const { return m_impl->produced.load(); }
