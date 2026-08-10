// SPDX-License-Identifier: AGPL-3.0-only
//
// console_main.cpp - headless host for the Prophecy engine ("the plugin minus JUCE").
//
// NOTE: this file includes NO MAME headers -- only prophecy_engine.h. MAME runs on a
// background thread inside the engine (the same thread model the JUCE plugin uses: the
// host owns the audio thread). This harness paces pulls at a real-time 48 kHz cadence
// and shows the ring backpressure self-clocks MAME, then writes the captured audio.
//
//   ./console_host korgprop -rompath ... -video none -sound none -nothrottle -seconds_to_run 14
//   PROPHOST_BLOCK=512 ./console_host ...      # real-time-paced (ring) mode
//
#include "prophecy_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace {

using steady_clock = std::chrono::steady_clock;
constexpr int kRate = ProphecyEngine::kSampleRate;
constexpr int kCh   = ProphecyEngine::kChannels;

void write_wav(const char *path, const std::vector<int16_t> &s, uint32_t rate, uint16_t ch)
{
	std::ofstream f(path, std::ios::binary);
	if (!f) { std::fprintf(stderr, "[console] cannot open %s\n", path); return; }
	const uint32_t data = uint32_t(s.size() * sizeof(int16_t));
	auto w32 = [&](uint32_t v){ f.write(reinterpret_cast<const char*>(&v), 4); };
	auto w16 = [&](uint16_t v){ f.write(reinterpret_cast<const char*>(&v), 2); };
	f.write("RIFF",4); w32(36u+data); f.write("WAVE",4);
	f.write("fmt ",4); w32(16u); w16(1); w16(ch); w32(rate); w32(rate*ch*2u); w16(uint16_t(ch*2u)); w16(16);
	f.write("data",4); w32(data);
	f.write(reinterpret_cast<const char*>(s.data()), data);
}

inline int16_t to_s16(float v)
{
	if (v >  1.0f) v =  1.0f;
	if (v < -1.0f) v = -1.0f;
	long x = std::lrintf(v * 32768.0f);
	if (x >  32767) x =  32767;
	if (x < -32768) x = -32768;
	return int16_t(x);
}

std::size_t env_size(const char *n, std::size_t d)
{
	const char *v = std::getenv(n);
	if (!v || !*v) return d;
	long long x = std::atoll(v);
	return x > 0 ? std::size_t(x) : d;
}

double env_double(const char *n, double d)
{
	const char *v = std::getenv(n);
	if (!v || !*v) return d;
	const double x = std::atof(v);
	return x >= 0.0 ? x : d;
}

} // namespace

int main(int argc, char **argv)
{
	std::vector<std::string> args(argv, argv + argc);
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	const bool        ring_mode = std::getenv("PROPHOST_BLOCK") != nullptr;
	const bool        fast_ring = std::getenv("PROPHOST_FAST_RING") != nullptr;
	const std::size_t block     = env_size("PROPHOST_BLOCK", 512);
	const double      metricWarmup = env_double("PROPHOST_METRIC_WARMUP", 0.0);
	const char       *wav       = std::getenv("PROPHOST_WAV_OUT");
	const char       *midiTxOut = std::getenv("PROPHOST_MIDI_TX_OUT");
	if (!wav) wav = ring_mode ? "console_ring_out.wav" : "console_out.wav";

#if defined(__APPLE__)
	// Ring mode stands in for a DAW's high-priority audio callback.  Without a
	// matching QoS, macOS may deschedule this ordinary command-line main thread
	// and sleep_until() then issues several catch-up pulls at once, manufacturing
	// a producer underrun that a real audio callback would not create.
	if (ring_mode)
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

	ProphecyEngine engine;
	if (midiTxOut && *midiTxOut) engine.enableMidiTxByteCapture();
	if (!engine.start(args)) { std::fprintf(stderr, "[console] engine.start failed\n"); return 1; }

	std::FILE *midiTxFile = nullptr;
	if (midiTxOut && *midiTxOut)
	{
		midiTxFile = std::fopen(midiTxOut, "w");
		if (!midiTxFile)
		{
			std::fprintf(stderr, "[console] cannot open MIDI TX capture %s\n", midiTxOut);
			return 1;
		}
	}
	std::uint64_t midiTxEvents = 0, midiTxClocks = 0;
	auto drainMidiTx = [&]() {
		ProphecyEngine::MidiTxByteEvent events[512];
		for (std::size_t n; (n = engine.popMidiTxByteEvents(events, 512)) > 0; )
		{
			for (std::size_t i = 0; i < n; ++i)
			{
				if (events[i].byte == 0xf8) ++midiTxClocks;
				if (midiTxFile)
					std::fprintf(midiTxFile, "{\"t\":%.9f,\"byte\":%u}\n",
						events[i].emuSeconds, unsigned(events[i].byte));
			}
			midiTxEvents += n;
		}
	};
	auto finishMidiTx = [&]() {
		drainMidiTx();
		if (midiTxFile) std::fclose(midiTxFile);
		if (midiTxOut && *midiTxOut)
			std::fprintf(stderr, "[console] midi-tx-events=%llu clocks=%llu dropped=%llu path=%s\n",
				(unsigned long long)midiTxEvents,
				(unsigned long long)midiTxClocks,
				(unsigned long long)engine.droppedMidiTxByteEvents(), midiTxOut);
	};

	std::vector<int16_t> outwav;
	std::vector<float>   L(block), R(block);

	if (!ring_mode)
	{
		// Rung 1 shape: drain as fast as available until the machine finishes.
		while (!(engine.finished() && engine.available() == 0))
		{
			std::size_t got = engine.pull(L.data(), R.data(), block);
			drainMidiTx();
			for (std::size_t i = 0; i < got; i++) { outwav.push_back(to_s16(L[i])); outwav.push_back(to_s16(R[i])); }
			if (got == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!outwav.empty()) write_wav(wav, outwav, kRate, uint16_t(kCh));
		finishMidiTx();
		std::fprintf(stderr, "\n[console] ===== capture (MAME on worker thread) =====\n"
				"[console] produced=%llu frames (%.3f s) | wav=%s\n",
				(unsigned long long)engine.producedFrames(),
				double(engine.producedFrames()) / kRate, wav);
		return 0;
	}

	// Rung 2 shape: pull one block every block/48000 s (a stand-in DAW audio callback).
	while (!engine.finished() && engine.available() < block) std::this_thread::sleep_for(std::chrono::milliseconds(1));

	// Optional: exercise the ProphecyEngine::pushMidi seam (host MIDI -> emulated UART) by
	// sending a note at a given emulated time. Format: PROPHOST_TEST_NOTE=onSec:offSec:note:vel
	double testOn = -1, testOff = -1; int testNote = 60, testVel = 100;
	if (const char *tn = std::getenv("PROPHOST_TEST_NOTE"))
		std::sscanf(tn, "%lf:%lf:%d:%d", &testOn, &testOff, &testNote, &testVel);
	if (testOn >= 0)
	{
		const std::uint8_t message[3] = {0x90, static_cast<std::uint8_t>(testNote),
			static_cast<std::uint8_t>(testVel)};
		engine.pushMidiAtFrame(message, 3,
			static_cast<std::uint64_t>(std::llround(testOn * kRate)));
	}
	if (testOff >= 0)
	{
		const std::uint8_t message[3] = {0x80, static_cast<std::uint8_t>(testNote), 0};
		engine.pushMidiAtFrame(message, 3,
			static_cast<std::uint64_t>(std::llround(testOff * kRate)));
	}

	// Arbitrary MIDI at scheduled emulated times, e.g.
	//   PROPHOST_TEST_MIDI="90 3c 64@11 ; e0 00 60@12 ; 80 3c 00@14"   (hex bytes @ seconds)
	struct TestEvt { double t; std::vector<uint8_t> bytes; };
	std::vector<TestEvt> testMidi;
	if (const char *tm = std::getenv("PROPHOST_TEST_MIDI"))
	{
		std::stringstream all(tm);
		std::string ev;
		while (std::getline(all, ev, ';'))
		{
			const auto at = ev.find('@');
			if (at == std::string::npos) continue;
			TestEvt e; e.t = std::atof(ev.c_str() + at + 1);
			std::stringstream bs(ev.substr(0, at));
			std::string tok;
			while (bs >> tok) e.bytes.push_back((uint8_t)std::strtol(tok.c_str(), nullptr, 16));
			if (!e.bytes.empty()) testMidi.push_back(std::move(e));
		}
	}
	std::stable_sort(testMidi.begin(), testMidi.end(),
		[](const TestEvt &left, const TestEvt &right) { return left.t < right.t; });
	for (const TestEvt &event : testMidi)
	{
		const auto frame = static_cast<std::uint64_t>(std::llround(event.t * kRate));
		engine.pushMidiAtFrame(event.bytes.data(), event.bytes.size(), frame);
	}

	// Soak mode: PROPHOST_SOAK=<sec> generates continuous traffic (random mono notes,
	// patch changes, mod-wheel CC sweeps, ADIN wiggles) and prints health stats every
	// 30 s of produced audio: underruns, ring level, produced time. Deterministic RNG.
	const char  *soakEnv  = std::getenv("PROPHOST_SOAK");
	const double soakSecs = soakEnv ? std::atof(soakEnv) : -1.0;
	std::uint32_t soakRng = 0x50524F50; // 'PROP'
	auto soakRand = [&soakRng](){ soakRng ^= soakRng << 13; soakRng ^= soakRng >> 17; soakRng ^= soakRng << 5; return soakRng; };
	double soakNextNoteOn = 12.0, soakNextNoteOff = -1.0, soakNextPc = 15.0, soakNextStats = 30.0;
	int    soakNote = -1;
	uint64_t soakNotes = 0, soakPcs = 0, soakCcs = 0;
	int      soakProfileTicks = 0;      // >0 = emit per-tick production trace this many ticks
	uint64_t soakLastProduced = 0;

	// LCD probe: PROPHOST_LCD_AT=<sec> prints the live HD44780 text once (e.g. to verify a
	// program change landed: the idle screen shows "<bank><nn>:<name>").
	const char  *lcdEnv  = std::getenv("PROPHOST_LCD_AT");
	const double lcdAt   = lcdEnv ? std::atof(lcdEnv) : -1.0;
	bool         lcdDone = false;

	// Panel probe: PROPHOST_PANEL_AT="<sec>:<row>,<bit>[;<sec>:<row>,<bit>...]"
	// presses front-panel buttons through the host seam and prints LED banks around
	// the sequence. The old second-item shorthand ";<row>,<bit>" remains +0.5 s.
	struct PanelEvt { double t; int row; int bit; bool logged; };
	std::vector<PanelEvt> panelEvts;
	bool panelChecked = false;
	if (const char *pp = std::getenv("PROPHOST_PANEL_AT"))
	{
		std::stringstream all(pp);
		std::string item;
		double firstTime = -1.0;
		while (std::getline(all, item, ';'))
		{
			PanelEvt event{-1.0, -1, -1, false};
			if (std::sscanf(item.c_str(), "%lf:%d,%d", &event.t, &event.row, &event.bit) == 3)
			{
				if (firstTime < 0.0)
					firstTime = event.t;
				panelEvts.push_back(event);
			}
			else if (firstTime >= 0.0 &&
				std::sscanf(item.c_str(), "%d,%d", &event.row, &event.bit) == 2)
			{
				event.t = firstTime + 0.5;
				panelEvts.push_back(event);
			}
		}
		std::sort(panelEvts.begin(), panelEvts.end(),
			[](const PanelEvt &left, const PanelEvt &right) { return left.t < right.t; });
	}
	for (const PanelEvt &event : panelEvts)
		engine.pushPanelPulseAtFrame(event.row, event.bit, 75,
			static_cast<std::uint64_t>(std::llround(event.t * kRate)));
	const double panelPreAt = panelEvts.empty() ? -1.0 : std::max(0.0, panelEvts.front().t - 0.25);
	bool panelPreChecked = false;
	const double panelCheckAt = panelEvts.empty() ? -1.0 :
		std::max(panelEvts.front().t + 2.0, panelEvts.back().t + 1.0);
	// Param sweep probe (dump-diff verification, e.g. offset discovery):
	//   PROPHOST_PARAM_SWEEP="<startSec>:<group>:<param>:<v0,v1,..>:<off0,off1,..>"
	// For each value: send 0x41 param change, request a program dump, wait for a fresh
	// version, print the unpacked bytes at the requested offsets. Prints "(none)" for a
	// value whose dump never arrived. Reusable for the OSC ExID sweep.
	// FIND-OFFSET mode: set the offsets field to the literal "diff" — instead of printing
	// fixed offsets, it captures the full 535-byte dump at each value and prints every
	// offset whose byte differs from the FIRST value's dump. This locates where a param
	// lands (incl. ExID-packed OSC-block params whose manifest offsets alias the name).
	int  swGroup = 1, swParam = -1;
	double swStart = -1.0;
	std::vector<int> swValues, swOffsets;
	bool swFind = false;
	std::vector<uint8_t> swBaseline;
	if (const char *sw = std::getenv("PROPHOST_PARAM_SWEEP"))
	{
		char vbuf[256] = {0}, obuf[256] = {0};
		if (std::sscanf(sw, "%lf:%d:%d:%255[^:]:%255s", &swStart, &swGroup, &swParam, vbuf, obuf) == 5)
		{
			auto parse = [](const char *s, std::vector<int> &out){
				std::stringstream ss(s); std::string t;
				while (std::getline(ss, t, ',')) if (!t.empty()) out.push_back(std::atoi(t.c_str()));
			};
			parse(vbuf, swValues);
			if (std::string(obuf) == "diff") swFind = true; else parse(obuf, swOffsets);
		}
	}
	std::size_t swIdx = 0;
	int  swPhase = 0;               // 0 = send param, 1 = request dump, 2 = await new version
	double swNext = swStart;
	std::uint32_t swSeenVer = 0;

	// ADIN probe: PROPHOST_ADIN_AT="<sec>:<src>,<val>[;<sec>:<src>,<val>...]" pushes analog
	// updates through the host seam (KPROP_LOG_GUI_ANALOG_DIAG=1 makes the driver log the
	// applied state). Multiple ';'-separated events allow sequences (e.g. touch gate + sweep).
	struct AdinEvt { double t; int src; int val; };
	std::vector<AdinEvt> adinEvts;
	if (const char *ap = std::getenv("PROPHOST_ADIN_AT"))
	{
		std::stringstream all(ap);
		std::string ev;
		while (std::getline(all, ev, ';'))
		{
			AdinEvt e{-1.0, 0, 0};
			if (std::sscanf(ev.c_str(), "%lf:%d,%d", &e.t, &e.src, &e.val) == 3 && e.t >= 0)
				adinEvts.push_back(e);
		}
		std::sort(adinEvts.begin(), adinEvts.end(),
			[](const AdinEvt &left, const AdinEvt &right) { return left.t < right.t; });
	}
	std::vector<bool> adinFired(adinEvts.size(), false);
	for (const AdinEvt &event : adinEvts)
		engine.pushAdinAtFrame(event.src, event.val,
			static_cast<std::uint64_t>(std::llround(event.t * kRate)));
	auto printLeds = [&](const char *tag, double t){
		std::uint8_t banks[12];
		const std::uint32_t ver = engine.ledSnapshot(banks);
		std::fprintf(stderr, "[console] LEDS %s@%.1fs ver=%u banks=", tag, t, ver);
		for (int i = 0; i < 12; i++) std::fprintf(stderr, "%02X%s", banks[i], i < 11 ? "." : "\n");
	};

	// TX-seam test: PROPHOST_DUMP_AT=<sec> sends a current-program-dump request and captures
	// the firmware's reply via popMidiTx (validates the driver->host half of the control seam).
	const char          *dumpEnv = std::getenv("PROPHOST_DUMP_AT");
	const double         dumpAt   = dumpEnv ? std::atof(dumpEnv) : -1.0;
	std::vector<uint8_t> txAll;
	uint8_t              txtmp[512];
	if (dumpAt >= 0)
	{
		const std::uint8_t request[7] = {0xF0, 0x42, 0x30, 0x41, 0x10, 0x00, 0xF7};
		engine.pushMidiAtFrame(request, 7,
			static_cast<std::uint64_t>(std::llround(dumpAt * kRate)));
	}

	// State round-trip test (PROPHOST_STATE_TEST=1): patch A08 note (ref) -> capture its dump ->
	// switch to A00 -> inject the captured dump -> note (should sound like A08 again = setState works).
	const bool           stateTest = std::getenv("PROPHOST_STATE_TEST") != nullptr;
	std::vector<uint8_t> capturedDump;
	bool sA08 = false, sNa = false, sOff1 = false, sReq = false, sCap = false,
	     sA00 = false, sNb = false, sOff2 = false, sInj = false, sNc = false, sOff3 = false;
	auto pm = [&](std::initializer_list<uint8_t> b){ std::vector<uint8_t> v(b); engine.pushMidi(v.data(), v.size()); };

	const auto period = std::chrono::duration_cast<steady_clock::duration>(
			std::chrono::duration<double>(double(block) / kRate));
	const auto t0 = steady_clock::now();
	auto       next = t0;
	uint64_t   ticks = 0, consumed = 0, underruns = 0, underrunFrames = 0;
	uint64_t   postWarmupUnderruns = 0, postWarmupFrames = 0, underrunStreak = 0, maxUnderrunStreak = 0;
	int        peak = 0;
	for (;;)
	{
		next += period;
		if (!fast_ring)
			std::this_thread::sleep_until(next);
		const double emu = double(engine.producedFrames()) / kRate;
		if (soakSecs > 0)
		{
			if (soakNote < 0 && emu >= soakNextNoteOn)
			{
				soakNote = 36 + int(soakRand() % 37);
				const uint8_t on[3] = {0x90, (uint8_t)soakNote, (uint8_t)(40 + soakRand() % 88)};
				engine.pushMidi(on, 3); soakNotes++;
				soakNextNoteOff = emu + 0.10 + (soakRand() % 90) / 100.0;
			}
			if (soakNote >= 0 && emu >= soakNextNoteOff)
			{
				const uint8_t off[3] = {0x80, (uint8_t)soakNote, 0};
				engine.pushMidi(off, 3);
				soakNote = -1;
				soakNextNoteOn = emu + 0.05 + (soakRand() % 40) / 100.0;
			}
			if (emu >= soakNextPc)
			{
				const int prog = int(soakRand() % 128);
				const uint8_t pc[8] = {0xB0,0x00,0x00, 0xB0,0x20,(uint8_t)(prog/64), 0xC0,(uint8_t)(prog%64)};
				engine.pushMidi(pc, 8); soakPcs++;
				soakNextPc = emu + 4.0 + (soakRand() % 60) / 10.0;
				if (std::getenv("PROPHOST_PROFILE")) { soakProfileTicks = 40; // ~4s window
					std::fprintf(stderr, "[prof] PC->%c%02d @%.2fs\n", "AB"[prog/64], prog%64, emu); }
			}
			// Phase-0 profiling (PROPHOST_PROFILE=1): per-tick production-vs-consumption
			// after each patch change. producedDelta ~0 => the DSP HALTED (reprogram
			// burst); positive but < block => the fast path just SLOWED. Resolves the
			// sustained-slow vs production-halt question (notes 2026-07-10).
			if (soakProfileTicks > 0)
			{
				const uint64_t pf = engine.producedFrames();
				std::fprintf(stderr, "[prof] t=%.3f producedDelta=%lld ring=%zu\n",
					emu, (long long)(pf - soakLastProduced), engine.available());
				soakProfileTicks--;
			}
			soakLastProduced = engine.producedFrames();
			if ((ticks % 47) == 0) // ~every 0.5s at 512-frame blocks: CC1 sweep + ADIN wiggle
			{
				const uint8_t cc[3] = {0xB0, 0x01, (uint8_t)(soakRand() % 128)};
				engine.pushMidi(cc, 3); soakCcs++;
				engine.pushAdin(int(soakRand() % 2) ? 9 : 8, int(soakRand() % 256));
			}
			if (emu >= soakNextStats)
			{
				std::fprintf(stderr, "[soak] t=%.0fs underruns=%llu notes=%llu pcs=%llu ccs=%llu ring=%zu\n",
					emu, (unsigned long long)underruns, (unsigned long long)soakNotes,
					(unsigned long long)soakPcs, (unsigned long long)soakCcs, engine.available());
				soakNextStats += 30.0;
			}
		}
		if (swParam >= 0 && swIdx < swValues.size() && emu >= swNext)
		{
			if (swPhase == 0)
			{
				const int v = swValues[swIdx] & 0x3FFF, pid = swParam & 0x3FFF;
				const uint8_t msg[11] = {0xF0,0x42,0x30,0x41,0x41,(uint8_t)(swGroup & 0x7F),
					(uint8_t)(pid & 0x7F), (uint8_t)((pid >> 7) & 0x7F),
					(uint8_t)(v & 0x7F), (uint8_t)((v >> 7) & 0x7F), 0xF7};
				engine.pushMidi(msg, sizeof(msg));
				swPhase = 1; swNext = emu + 0.30;
			}
			else if (swPhase == 1)
			{
				std::uint8_t tmp[1024]; std::uint32_t ver = 0;
				engine.latestProgramData(tmp, sizeof(tmp), &ver);
				swSeenVer = ver;
				const uint8_t req[7] = {0xF0,0x42,0x30,0x41,0x10,0x00,0xF7};
				engine.pushMidi(req, 7);
				swPhase = 2; swNext = emu + 0.05;
			}
			else
			{
				std::uint8_t buf[1024]; std::uint32_t ver = 0;
				const std::size_t n = engine.latestProgramData(buf, sizeof(buf), &ver);
				if (ver > swSeenVer && n >= 535)
				{
					if (swFind)
					{
						if (swBaseline.empty()) {
							swBaseline.assign(buf, buf + n);
							std::fprintf(stderr, "[find] g%d p%d baseline @val=%d captured (%zu bytes)\n",
								swGroup, swParam, swValues[swIdx], n);
						} else {
							std::fprintf(stderr, "[find] g%d p%d val=%d changed offsets:", swGroup, swParam, swValues[swIdx]);
							int nch = 0;
							for (std::size_t o = 0; o < n && o < swBaseline.size(); o++)
								if (buf[o] != swBaseline[o]) { std::fprintf(stderr, " [%zu]=%02X->%02X", o, swBaseline[o], buf[o]); nch++; }
							std::fprintf(stderr, "%s\n", nch ? "" : " (none — param did not reach)");
						}
					}
					else
					{
						std::fprintf(stderr, "[sweep] g%d p%d = %d ->", swGroup, swParam, swValues[swIdx]);
						for (int off : swOffsets)
							std::fprintf(stderr, " [%d]=%02X", off, off < (int)n ? buf[off] : 0);
						std::fprintf(stderr, "\n");
					}
					swIdx++; swPhase = 0; swNext = emu + 0.20;
				}
				else if (emu > swNext + 2.0)
				{
					std::fprintf(stderr, "[sweep] g%d p%d = %d -> (none)\n", swGroup, swParam, swValues[swIdx]);
					swIdx++; swPhase = 0; swNext = emu + 0.20;
				}
			}
		}
		if (!lcdDone && lcdAt >= 0 && emu >= lcdAt)
		{
			char l1[64] = {0}, l2[64] = {0};
			const bool ok = engine.latestLcd(l1, l2, sizeof(l1));
			std::fprintf(stderr, "[console] LCD@%.1fs ok=%d L1=\"%s\" L2=\"%s\"\n", emu, ok, l1, l2);
			std::uint8_t rr1[40], rr2[40], cg[64];
			const std::uint32_t rv = engine.lcdRawSnapshot(rr1, rr2, cg);
			int cgNonZero = 0; for (int i = 0; i < 64; i++) if (cg[i]) cgNonZero++;
			std::fprintf(stderr, "[console] LCDRAW ver=%u row1[0..3]=%02X %02X %02X %02X cgramNonZero=%d\n",
				rv, rr1[0], rr1[1], rr1[2], rr1[3], cgNonZero);
			lcdDone = true;
		}
		for (std::size_t i = 0; i < adinEvts.size(); i++)
			if (!adinFired[i] && emu >= adinEvts[i].t)
			{
				std::fprintf(stderr, "[console] pushAdin src=%d val=%02X @%.1fs\n", adinEvts[i].src, adinEvts[i].val, emu);
				adinFired[i] = true;
			}
		if (!panelPreChecked && panelPreAt >= 0.0 && emu >= panelPreAt)
		{
			printLeds("pre ", emu);
			panelPreChecked = true;
		}
		for (std::size_t i = 0; i < panelEvts.size(); ++i)
		{
			PanelEvt &event = panelEvts[i];
			if (!event.logged && emu >= event.t)
			{
				std::fprintf(stderr, "[console] panelPulse row=%d bit=%d @%.1fs\n",
					event.row, event.bit, emu);
				event.logged = true;
			}
		}
		if (!panelEvts.empty() && !panelChecked && emu >= panelCheckAt)
		{
			printLeds("post", emu);
			panelChecked = true;
		}
		for (std::size_t g; (g = engine.popMidiTx(txtmp, sizeof(txtmp))) > 0; )
			txAll.insert(txAll.end(), txtmp, txtmp + g);
		drainMidiTx();
		if (stateTest)
		{
			if (!sA08 && emu >= 11.0) { pm({0xC0, 0x08}); sA08 = true; }
			if (!sNa  && emu >= 11.3) { pm({0x90, 0x40, 0x64}); sNa = true; }
			if (!sOff1&& emu >= 12.2) { pm({0x80, 0x40, 0x00}); sOff1 = true; }
			if (!sReq && emu >= 12.5) { pm({0xF0, 0x42, 0x30, 0x41, 0x10, 0x00, 0xF7}); sReq = true; }
			if (!sCap)
				for (std::size_t i = 0; i + 4 < txAll.size(); ++i)
					if (txAll[i] == 0xF0 && txAll[i + 4] == 0x40)
					{
						std::size_t j = i + 1; while (j < txAll.size() && txAll[j] != 0xF7) ++j;
						if (j < txAll.size()) { capturedDump.assign(txAll.begin() + i, txAll.begin() + j + 1); sCap = true; }
						break;
					}
			if (!sA00 && emu >= 13.0) { pm({0xC0, 0x00}); sA00 = true; }
			if (!sNb  && emu >= 13.3) { pm({0x90, 0x40, 0x64}); sNb = true; }
			if (!sOff2&& emu >= 14.2) { pm({0x80, 0x40, 0x00}); sOff2 = true; }
			if (!sInj && emu >= 14.5 && !capturedDump.empty()) { engine.pushMidi(capturedDump.data(), capturedDump.size()); sInj = true; }
			if (!sNc  && emu >= 15.0) { pm({0x90, 0x40, 0x64}); sNc = true; }
			if (!sOff3&& emu >= 16.0) { pm({0x80, 0x40, 0x00}); sOff3 = true; }
		}
		std::size_t got = engine.pull(L.data(), R.data(), block);
		if (fast_ring && got == 0 && !engine.finished())
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		ticks++;
		for (std::size_t i = 0; i < got; i++)
		{
			int16_t l = to_s16(L[i]), r = to_s16(R[i]);
			outwav.push_back(l); outwav.push_back(r);
			int a = std::abs(int(l)); if (a > peak) peak = a;
			a = std::abs(int(r));     if (a > peak) peak = a;
		}
		consumed += got;
		if (!engine.finished() && got < block)
		{
			const std::uint64_t missing = block - got;
			underruns++;
			underrunFrames += missing;
			underrunStreak++;
			maxUnderrunStreak = std::max(maxUnderrunStreak, underrunStreak);
			if (emu >= metricWarmup)
			{
				postWarmupUnderruns++;
				postWarmupFrames += missing;
			}
		}
		else
		{
			underrunStreak = 0;
		}
		if (engine.finished()) break;
	}
	drainMidiTx();
	for (;;) // fast-drain residual
	{
		std::size_t got = engine.pull(L.data(), R.data(), block);
		if (!got) break;
		for (std::size_t i = 0; i < got; i++) { outwav.push_back(to_s16(L[i])); outwav.push_back(to_s16(R[i])); }
		consumed += got;
	}

	// TX seam report: parse the captured firmware sysex into messages, count 0x40 patch dumps.
	for (std::size_t g; (g = engine.popMidiTx(txtmp, sizeof(txtmp))) > 0; )
		txAll.insert(txAll.end(), txtmp, txtmp + g);
	if (dumpAt >= 0)
	{
		int msgs = 0, dumps = 0; std::size_t dumpLen = 0;
		for (std::size_t i = 0; i < txAll.size(); )
		{
			if (txAll[i] != 0xF0) { i++; continue; }
			std::size_t j = i + 1;
			while (j < txAll.size() && txAll[j] != 0xF7) j++;
			if (j >= txAll.size()) break;
			const std::size_t len = j - i + 1;
			msgs++;
			if (len >= 5 && txAll[i + 4] == 0x40) { dumps++; dumpLen = len; }
			i = j + 1;
		}
		std::fprintf(stderr, "[console] TX seam: %d sysex msg(s), %d current-program dump(s) (0x40), dump len=%zu bytes\n",
				msgs, dumps, dumpLen);
	}
	const double wall     = std::chrono::duration<double>(steady_clock::now() - t0).count();
	const double emulated = double(engine.producedFrames()) / kRate;
	const double ratio    = emulated > 0 ? wall / emulated : 0.0;
	const bool   selfclk  = ratio > 0.95 && ratio < 1.15 && underruns <= 2;
	if (!outwav.empty()) write_wav(wav, outwav, kRate, uint16_t(kCh));
	finishMidiTx();
	std::fprintf(stderr, "[console] scheduled-midi-dropped=%llu\n",
		(unsigned long long)engine.droppedScheduledMidiBytes());
	std::fprintf(stderr, "[console] scheduled-control-dropped=panel:%llu adin:%llu\n",
		(unsigned long long)engine.droppedScheduledPanelEvents(),
		(unsigned long long)engine.droppedScheduledAdinEvents());
	std::fprintf(stderr,
		"[console] underrun-detail total_callbacks=%llu total_frames=%llu "
		"post_warmup_seconds=%.3f post_warmup_callbacks=%llu post_warmup_frames=%llu "
		"max_callback_streak=%llu\n",
		(unsigned long long)underruns, (unsigned long long)underrunFrames, metricWarmup,
		(unsigned long long)postWarmupUnderruns, (unsigned long long)postWarmupFrames,
		(unsigned long long)maxUnderrunStreak);

	std::fprintf(stderr,
			"\n[console] ========= ring backpressure (MAME on worker thread) =========\n"
			"[console] block=%zu frames  produced=%.3f s emulated\n"
			"[console] wall=%.3f s  ratio=%.3f (free-run ~0.86)  ticks=%llu  underruns=%llu  peak=%d\n"
			"[console] VERDICT: %s  | wav=%s\n"
			"[console] ============================================================\n",
			block, emulated, wall, ratio,
			(unsigned long long)ticks, (unsigned long long)underruns, peak,
			selfclk ? "SELF-CLOCKED via ring backpressure" : "NOT self-clocked (CPU contention?)",
			wav);
	return 0;
}
