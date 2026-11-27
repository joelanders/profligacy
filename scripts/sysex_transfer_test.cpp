// SPDX-License-Identifier: AGPL-3.0-only
// No-ROM checks for src/sysex_transfer.h: dump formats, file parsing, and the transfer
// state machine against a simulated synth with its own clock and a 31250-baud UART. The
// ROM-backed round trip is separate.
#include "sysex_transfer.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

using namespace prophecy::sysex;

static int g_failures = 0;
static void check(bool ok, const std::string &what)
{
	if (!ok) { std::fprintf(stderr, "FAIL %s\n", what.c_str()); ++g_failures; }
}
static bool contains(const std::string &s, const char *part) { return s.find(part) != std::string::npos; }

static std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed)
{
	std::vector<std::uint8_t> v(n);
	for (std::size_t i = 0; i < n; ++i) v[i] = std::uint8_t((i * 37u + seed * 101u) ^ (i >> 3));
	return v;
}

// A synth that answers requests from its own memory and acknowledges loads. It has its
// own clock (`emu`, which stops while `running` is false), receives through a UART that
// clocks bytes in at 3.125 per emulated ms, takes `storeMs` to store a load and, like the
// firmware, ignores SysEx while it stores. Protect switches (global p170/p171) are honoured.
struct FakeSynth : SysexPort
{
	std::vector<std::uint8_t> programs = pattern(kProgramBytes * kProgramCount, 1);
	std::vector<std::uint8_t> patterns = pattern(kPatternBytes * kPatternCount, 2);
	std::vector<std::uint8_t> global = pattern(kGlobalBytes, 3);
	std::vector<std::uint8_t> edit = pattern(kProgramBytes, 4);
	bool protectProgram = true, protectPattern = true;
	bool answer = true;       // false: ignore everything (a synth with SysEx switched off)
	int busyTicks = 0;        // ready() is false for this many calls
	bool running = true;      // false: the emulator is stopped (plugin bypassed)
	double speed = 1.0;       // emulated ms per wall ms
	double storeMs = 50.0;    // emulated time to store a load
	double emu = 0.0;
	bool damageReplies = false, realtimeInReplies = false;

	std::vector<std::uint8_t> ring;     // the 4 KB stream ring
	std::deque<std::uint8_t> uart;      // queued for the UART
	double uartCredit = 0.0;
	std::vector<std::uint8_t> rx;       // message being received
	double storingUntil = -1.0;
	std::deque<std::pair<double, std::vector<std::uint8_t>>> outgoing; // replies, due time
	std::deque<std::vector<std::uint8_t>> replies;                      // delivered
	std::vector<std::vector<std::uint8_t>> received;                    // complete messages
	int protectChanges = 0, ignoredWhileStoring = 0, formatErrors = 0;
	std::vector<double> protectTimes, ackTimes;
	bool open = false;

	FakeSynth()
	{
		global[kGlobalChannelOffset] = 0;
		global[kGlobalSysexOffset] = 0x02;
		global[kGlobalProtectOffset] = 0x03;
	}
	bool ready() override { if (busyTicks > 0) { --busyTicks; return false; } return true; }
	int channel() override { return 0; }
	double emulatedMs() override { return emu; }
	bool openStream() override { if (open) return false; open = true; return true; }
	std::size_t pushStream(const std::uint8_t *b, std::size_t n) override
	{
		const std::size_t take = std::min<std::size_t>(n, 4096 - ring.size());
		ring.insert(ring.end(), b, b + take);
		return take;
	}
	std::size_t streamPending() override { return ring.size(); }
	void closeStream() override { open = false; }
	bool popReply(std::vector<std::uint8_t> &out) override
	{
		if (replies.empty()) return false;
		out = std::move(replies.front());
		replies.pop_front();
		return true;
	}
	void clearReplies() override { replies.clear(); }

	// Advance by `wallMs` of host time.
	void step(double wallMs)
	{
		if (!running) return;
		const double dt = wallMs * speed;
		emu += dt;
		uart.insert(uart.end(), ring.begin(), ring.end()); // MAME's drain into the UART queue
		ring.clear();
		uartCredit += dt * kUartBytesPerMs;
		while (uartCredit >= 1.0 && !uart.empty())
		{
			feed(uart.front());
			uart.pop_front();
			uartCredit -= 1.0;
		}
		if (uart.empty()) uartCredit = std::min(uartCredit, 1.0);
		while (!outgoing.empty() && outgoing.front().first <= emu)
		{
			replies.push_back(std::move(outgoing.front().second));
			outgoing.pop_front();
		}
	}
	void feed(std::uint8_t b)
	{
		if (b == 0xF0) rx.assign(1, b);
		else if (!rx.empty())
		{
			rx.push_back(b);
			if (b == 0xF7) { received.push_back(rx); handle(rx); rx.clear(); }
		}
	}
	void send(std::vector<std::uint8_t> msg, double delayMs)
	{
		if (!answer) return;
		if (realtimeInReplies && msg.size() > 20) msg.insert(msg.begin() + 10, 0xF8);
		outgoing.emplace_back(emu + delayMs, std::move(msg));
	}
	void dumpReply(const Dump &d)
	{
		std::vector<std::uint8_t> msg = build(d, 0);
		if (damageReplies) msg.erase(msg.begin() + 12);
		send(msg, double(msg.size()) / kUartBytesPerMs);
	}
	void ack(std::uint8_t f, int code, double delayMs)
	{
		std::vector<std::uint8_t> m = {0xF0, 0x42, 0x30, 0x41, f};
		if (code >= 0) m.push_back(std::uint8_t(code));
		m.push_back(0xF7);
		if (f == 0x23) ackTimes.push_back(emu + delayMs);
		send(m, delayMs);
	}
	void handle(const std::vector<std::uint8_t> &m)
	{
		if (!answer || m.size() < 6) return;
		if (emu < storingUntil) { ++ignoredWhileStoring; return; }
		const std::uint8_t f = m[4];
		auto progs = [&](int first, int count) {
			return std::vector<std::uint8_t>(programs.begin() + first * long(kProgramBytes),
				programs.begin() + (first + count) * long(kProgramBytes));
		};
		if (f == 0x41 && m[5] == 0x00)
		{
			const int p = m[6] | (m[7] << 7), v = m[8] | (m[9] << 7);
			if (p == 170) protectProgram = v != 0;
			if (p == 171) protectPattern = v != 0;
			++protectChanges;
			protectTimes.push_back(emu);
			return;
		}
		if (f == 0x10) dumpReply({Unit::CurrentProgram, -1, edit});
		if (f == 0x1C)
		{
			const int unit = (m[5] >> 4) & 3, bank = m[5] & 1;
			if (unit == 0) dumpReply({Unit::Program, bank * 64 + m[6], progs(bank * 64 + m[6], 1)});
			if (unit == 1) dumpReply({Unit::Bank, bank, progs(bank * 64, 64)});
			if (unit == 2) dumpReply({Unit::AllPrograms, -1, programs});
		}
		if (f == 0x34)
		{
			if (m[5] & 0x10) dumpReply({Unit::AllPatterns, -1, patterns});
			else dumpReply({Unit::Pattern, m[5], std::vector<std::uint8_t>(patterns.begin() + m[5] * 128,
				patterns.begin() + (m[5] + 1) * 128)});
		}
		if (f == 0x0E) dumpReply({Unit::Global, -1, global});
		if (f == 0x0F)
		{
			std::vector<std::uint8_t> all = global;
			all.insert(all.end(), programs.begin(), programs.end());
			all.insert(all.end(), patterns.begin(), patterns.end());
			dumpReply({Unit::AllData, -1, all});
		}
		if (f == 0x40 || f == 0x4C || f == 0x69 || f == 0x51 || f == 0x50)
		{
			std::optional<Dump> d = parse(m);
			if (!d) { ++formatErrors; ack(0x26, -1, 5.0); return; }
			const bool needsProgram = d->unit == Unit::Program || d->unit == Unit::Bank
				|| d->unit == Unit::AllPrograms || d->unit == Unit::AllData;
			const bool needsPattern = d->unit == Unit::Pattern || d->unit == Unit::AllPatterns
				|| d->unit == Unit::AllData;
			if (needsProgram && protectProgram) { ack(0x24, 1, 5.0); return; }
			if (needsPattern && protectPattern) { ack(0x24, 2, 5.0); return; }
			switch (d->unit)
			{
				case Unit::CurrentProgram: edit = d->raw; break;
				case Unit::Program: std::copy(d->raw.begin(), d->raw.end(), programs.begin() + d->index * long(kProgramBytes)); break;
				case Unit::Bank: std::copy(d->raw.begin(), d->raw.end(), programs.begin() + d->index * 64 * long(kProgramBytes)); break;
				case Unit::AllPrograms: programs = d->raw; break;
				case Unit::Pattern: std::copy(d->raw.begin(), d->raw.end(), patterns.begin() + d->index * 128); break;
				case Unit::AllPatterns: patterns = d->raw; break;
				case Unit::Global: global = d->raw; break;
				case Unit::AllData:
					global.assign(d->raw.begin(), d->raw.begin() + kGlobalBytes);
					programs.assign(d->raw.begin() + kGlobalBytes, d->raw.begin() + kGlobalBytes + programs.size());
					patterns.assign(d->raw.end() - long(patterns.size()), d->raw.end());
					break;
			}
			storingUntil = emu + storeMs;
			ack(0x23, -1, storeMs);
		}
	}
};

// Ticks every 10 ms of host time. `hook(tick)` runs before each tick.
template<class Hook>
static Transfer::State run(Transfer &t, FakeSynth &synth, Hook hook, double limitMs = 400000.0)
{
	double wall = 0.0;
	for (int tick = 0; t.needsTicks() && wall < limitMs; ++tick, wall += 10.0)
	{
		hook(tick, wall);
		synth.step(10.0);
		t.tick(synth, wall);
	}
	return t.status().state;
}
static Transfer::State run(Transfer &t, FakeSynth &synth)
{
	return run(t, synth, [](int, double) {});
}

static std::vector<std::uint8_t> concat(std::initializer_list<std::vector<std::uint8_t>> parts)
{
	std::vector<std::uint8_t> out;
	for (const auto &p : parts) out.insert(out.end(), p.begin(), p.end());
	return out;
}

int main()
{
	// Sizes match what the v1.7 firmware sends (scripts/sysex_roundtrip_test.cpp checks them live).
	check(messageBytes(Unit::CurrentProgram) == 619, "current program message size");
	check(messageBytes(Unit::Program) == 621, "program message size");
	check(messageBytes(Unit::Bank) == 39141, "bank message size");
	check(messageBytes(Unit::AllPrograms) == 78272, "all programs message size");
	check(messageBytes(Unit::Pattern) == 155, "pattern message size");
	check(messageBytes(Unit::AllPatterns) == 1471, "all patterns message size");
	check(messageBytes(Unit::Global) == 663, "global message size");
	check(messageBytes(Unit::AllData) == 80390, "all data message size");

	for (std::size_t n : {std::size_t(1), std::size_t(7), std::size_t(8), kProgramBytes, rawBytes(Unit::AllData)})
	{
		const auto raw = pattern(n, unsigned(n));
		const auto packed = pack(raw);
		check(packed.size() == packedBytes(n), "packed size");
		check(unpack(packed.data(), packed.size()) == raw, "pack/unpack round trip");
	}

	// Every unit builds, parses back, and keeps its index.
	const std::vector<Dump> samples = {
		{Unit::CurrentProgram, -1, pattern(rawBytes(Unit::CurrentProgram), 1)},
		{Unit::Program, 69, pattern(rawBytes(Unit::Program), 2)},
		{Unit::Bank, 1, pattern(rawBytes(Unit::Bank), 3)},
		{Unit::AllPrograms, -1, pattern(rawBytes(Unit::AllPrograms), 4)},
		{Unit::Pattern, 7, pattern(rawBytes(Unit::Pattern), 5)},
		{Unit::AllPatterns, -1, pattern(rawBytes(Unit::AllPatterns), 6)},
		{Unit::Global, -1, pattern(rawBytes(Unit::Global), 7)},
		{Unit::AllData, -1, pattern(rawBytes(Unit::AllData), 8)},
	};
	for (const Dump &d : samples)
	{
		const auto msg = build(d, 5);
		check(msg.size() == messageBytes(d.unit), "built message size");
		check(msg[2] == 0x35, "built message channel");
		auto back = parse(msg);
		check(back && back->unit == d.unit && back->index == d.index && back->raw == d.raw, "parse(build(d))");
	}
	std::string why;
	auto bad = build(samples[1], 0);
	bad.erase(bad.begin() + 20);
	check(!parse(bad, &why) && !why.empty(), "short dump rejected");
	check(!parse({0xF0, 0x43, 0x30, 0x41, 0x40, 0x01, 0xF7}), "other manufacturer rejected");

	// Files: concatenated dumps, junk and real-time bytes between them, a truncated tail.
	{
		std::vector<std::uint8_t> file = {0x00, 0x12};
		for (int i : {1, 4})
		{
			const auto m = build(samples[std::size_t(i)], 0);
			file.insert(file.end(), m.begin(), m.end());
			file.push_back(0xFE);
		}
		file.insert(file.end(), {0xF0, 0x42, 0x30});
		const auto msgs = split(file.data(), file.size());
		check(msgs.size() == 2 && parse(msgs[0]) && parse(msgs[1]) && parse(msgs[1])->unit == Unit::Pattern,
			"split concatenated .syx");
	}
	// Real-time bytes inside a dump are legal MIDI and are dropped.
	{
		auto m = build(samples[1], 0);
		for (std::size_t at : {std::size_t(3), std::size_t(100), std::size_t(400)}) m.insert(m.begin() + long(at), 0xF8);
		m.insert(m.begin() + 200, 0xFE);
		const FileContents c = readFile(m.data(), m.size());
		check(c.dumps.size() == 1 && c.dumps[0].raw == samples[1].raw && c.skipped().empty(),
			"real-time bytes inside a dump are dropped");
	}
	// Truncated last dump, a dump cut off by the next F0, a damaged dump, and a foreign
	// message are each counted and described separately.
	{
		const auto bankA = build(samples[2], 0);
		auto bankB = build({Unit::Bank, 0, pattern(rawBytes(Unit::Bank), 9)}, 0);
		bankB.pop_back(); // the file ends before Bank B's F7
		const FileContents truncated = readFile(concat({bankA, bankB}).data(), bankA.size() + bankB.size());
		check(truncated.dumps.size() == 1 && truncated.cutOffDumps == 1 && truncated.foreign == 0
			&& truncated.damaged == 0, "truncated last dump counted");
		check(truncated.skipped().size() == 1 && contains(truncated.skipped()[0], "1 Prophecy dump cut off")
			&& contains(truncated.skipped()[0], "truncated"), "truncated last dump described");

		auto cut = build(samples[4], 0);
		cut.resize(40); // restarted by the next message's F0
		auto damaged = build(samples[1], 0);
		damaged.erase(damaged.begin() + 30);
		const std::vector<std::uint8_t> yamaha = {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7};
		const std::vector<std::uint8_t> paramChange = globalParameter(170, 0, 0); // Prophecy, not a dump
		auto ended = build(samples[0], 0);
		ended.resize(30);
		ended.push_back(0x90); // a channel status byte inside a message ends it
		const auto file = concat({cut, damaged, yamaha, paramChange, ended, bankA, {0xF0, 0x01, 0x02}});
		const FileContents c = readFile(file.data(), file.size());
		check(c.dumps.size() == 1 && c.dumps[0].unit == Unit::Bank, "the good dump survives");
		check(c.damaged == 1 && contains(c.damagedReason, "wrong size"), "damaged Prophecy dump counted");
		check(c.cutOffDumps == 2 && c.cutOffOther == 1, "cut-off messages counted");
		check(c.foreign == 2, "foreign messages counted");
		const auto lines = c.skipped();
		check(lines.size() == 4 && contains(lines[0], "1 damaged Prophecy dump (dump has the wrong size")
			&& contains(lines[1], "2 Prophecy dumps cut off") && contains(lines[2], "1 other message cut off")
			&& contains(lines[3], "2 messages that are not Prophecy data dumps"), "skipped lines");
		const FileContents empty = readFile(nullptr, 0);
		check(empty.dumps.empty() && !empty.hasMessages && empty.skipped().empty(), "empty file");
	}

	// Names and global preservation.
	{
		Dump bank{Unit::Bank, 1, std::vector<std::uint8_t>(rawBytes(Unit::Bank), ' ')};
		const char name[] = "Hello Prophecy";
		std::copy(name, name + 14, bank.raw.begin() + 3 * long(kProgramBytes));
		const auto names = programNames(bank);
		check(names.size() == 64 && names[3].first == 67 && names[3].second == "Hello Prophecy", "bank program names");
		Dump all = samples[7];
		std::vector<std::uint8_t> current(kGlobalBytes, 0);
		for (std::size_t i = 0; i < kGlobalBytes; ++i) current[i] = std::uint8_t(i * 7);
		preserveGlobalMidi(all, current);
		bool kept = true, imported = true;
		for (std::size_t i = 0; i < kGlobalBytes; ++i)
		{
			const bool preserved = i == kGlobalProtectOffset || i == kGlobalChannelOffset
				|| (i >= kGlobalSysexOffset && i < kGlobalProgramMapEnd);
			if (preserved) kept = kept && all.raw[i] == current[i];
			else imported = imported && all.raw[i] == samples[7].raw[i];
		}
		check(kept, "protect, channel, SysEx, Program Change, bank and program maps kept");
		check(imported && all.raw[kGlobalProgramMapEnd] == samples[7].raw[kGlobalProgramMapEnd]
			&& all.raw[177] == samples[7].raw[177], "the rest of the global is imported");
		check(std::equal(all.raw.begin() + long(kGlobalBytes), all.raw.end(), samples[7].raw.begin() + long(kGlobalBytes)),
			"programs and patterns untouched by preservation");
		check(programSlot(0) == "A00" && programSlot(127) == "B63", "slot names");
		for (const auto &[unit, key] : kUnitKeys)
			check(unitFromKey(unitKey(unit)) == unit, "unit key round trip");
		check(!unitFromKey("bogus"), "unknown unit key");
	}

	// Export every unit from the simulated synth.
	for (const Dump &d : samples)
	{
		FakeSynth synth;
		Transfer t;
		check(t.startExport(d.unit, d.index < 0 ? 0 : d.index), "start export");
		check(!t.startExport(d.unit, 0), "second transfer refused while busy");
		check(run(t, synth) == Transfer::State::Succeeded, "export " + label(d));
		auto got = parse(t.exported());
		check(got && got->unit == d.unit, "exported dump parses");
	}

	// Out-of-range numbers are refused rather than wrapped into another slot.
	{
		Transfer t;
		check(!t.startExport(Unit::Program, -1) && !t.startExport(Unit::Program, kProgramCount)
			&& !t.startExport(Unit::Bank, 2) && !t.startExport(Unit::Pattern, kPatternCount)
			&& t.startExport(Unit::Global, 0), "export numbers checked");
	}

	// A reply with real-time bytes inside is cleaned; a damaged reply fails the export.
	{
		FakeSynth synth;
		synth.realtimeInReplies = true;
		Transfer t;
		t.startExport(Unit::Bank, 1);
		check(run(t, synth) == Transfer::State::Succeeded, "export with a real-time byte in the reply");
		check(std::find(t.exported().begin(), t.exported().end(), 0xF8) == t.exported().end()
			&& parse(t.exported()), "exported reply is clean");
	}
	{
		FakeSynth synth;
		synth.damageReplies = true;
		Transfer t;
		t.startExport(Unit::Program, 3);
		check(run(t, synth) == Transfer::State::Failed && contains(t.status().message, "damaged")
			&& t.exported().empty(), "damaged reply fails the export");
	}

	// Import into protected memory: unprotect, load, re-protect.
	{
		FakeSynth synth;
		Transfer t;
		Dump bank{Unit::Bank, 0, pattern(rawBytes(Unit::Bank), 42)};
		check(t.startImport({bank}), "start bank import");
		check(run(t, synth) == Transfer::State::Succeeded, "bank import succeeds");
		check(std::equal(bank.raw.begin(), bank.raw.end(), synth.programs.begin()), "bank stored");
		check(synth.protectProgram && synth.protectPattern && synth.protectChanges == 4, "memory re-protected");
		check(t.stored().size() == 1 && t.stored()[0] == 0, "stored dump reported");
	}

	// An imported global keeps the synth's channel, SysEx, Program Change, maps and protect.
	{
		FakeSynth synth;
		for (std::size_t i = kGlobalSysexOffset; i < kGlobalProgramMapEnd; ++i) synth.global[i] = std::uint8_t(i & 0x7F);
		const auto before = synth.global;
		Transfer t;
		Dump g{Unit::Global, -1, std::vector<std::uint8_t>(kGlobalBytes, 0x55)};
		check(t.startImport({g}), "start global import");
		check(run(t, synth) == Transfer::State::Succeeded, "global import succeeds");
		check(synth.global[0] == 0x55 && synth.global[177] == 0x55 && synth.global[kGlobalProgramMapEnd] == 0x55
			&& synth.global[kGlobalChannelOffset] == before[kGlobalChannelOffset]
			&& synth.global[kGlobalProtectOffset] == 0x03
			&& std::equal(before.begin() + long(kGlobalSysexOffset), before.begin() + long(kGlobalProgramMapEnd),
				synth.global.begin() + long(kGlobalSysexOffset)),
			"global MIDI settings preserved");
		check(synth.protectChanges == 0, "global-only import leaves protect alone");
	}

	// A refused load fails with the firmware's reason and still re-protects.
	{
		FakeSynth synth;
		Transfer t;
		Dump p{Unit::Program, 3, pattern(kProgramBytes, 9)};
		check(t.startImport({p, p}), "start import");
		// Re-protect behind the transfer's back after the unprotect has gone out.
		run(t, synth, [&](int tick, double) { if (tick == 40) synth.protectProgram = true; });
		check(t.status().state == Transfer::State::Failed
			&& contains(t.status().message, "protected"), "protected load reported");
		check(synth.protectProgram && synth.protectPattern, "re-protected after failure");
	}

	// No answer: export times out instead of hanging.
	{
		FakeSynth synth;
		synth.answer = false;
		Transfer t;
		t.startExport(Unit::Bank, 0);
		check(run(t, synth) == Transfer::State::Failed, "silent synth times out");
	}

	// Busy synth: the transfer waits, then starts.
	{
		FakeSynth synth;
		synth.busyTicks = 50;
		Transfer t;
		t.startExport(Unit::Global, 0);
		check(run(t, synth) == Transfer::State::Succeeded, "waits for a busy synth");
	}

	// Cancel during a multi-dump import stops between dumps and restores protect.
	{
		FakeSynth synth;
		Transfer t;
		std::vector<Dump> dumps;
		for (int i = 0; i < 4; ++i) dumps.push_back({Unit::Program, i, pattern(kProgramBytes, unsigned(20 + i))});
		t.startImport(dumps);
		check(run(t, synth, [&](int tick, double) { if (tick == 60) t.cancel(); }) == Transfer::State::Cancelled,
			"import cancelled");
		check(synth.protectProgram && synth.protectPattern, "re-protected after cancel");
		check(!std::equal(dumps[3].raw.begin(), dumps[3].raw.end(), synth.programs.begin() + 3 * long(kProgramBytes)),
			"later dumps not sent after cancel");
		check(contains(t.status().message, "of 4 dumps were loaded"), "cancel reports what was loaded");
	}

	// Cancel mid-dump: the bank (12.5 s on the wire) is ended with F7 where it stands; the
	// synth rejects the short dump and memory is protected again.
	{
		FakeSynth synth;
		const auto before = synth.programs;
		Transfer t;
		t.startImport({{Unit::Bank, 0, pattern(rawBytes(Unit::Bank), 43)}});
		const auto st = run(t, synth, [&](int, double wall) { if (wall == 3000.0) t.cancel(); });
		check(st == Transfer::State::Cancelled, "mid-dump cancel");
		check(synth.formatErrors == 1 && synth.programs == before, "cut-off bank rejected, memory unchanged");
		bool cut = false;
		for (const auto &m : synth.received)
			cut = cut || (m.size() > 100 && m.size() < messageBytes(Unit::Bank) && m.back() == 0xF7);
		check(cut, "the synth received a short dump ending in F7");
		check(synth.protectProgram && synth.protectPattern, "re-protected after mid-dump cancel");
		check(contains(t.status().message, "rejected the incomplete Bank A"), "mid-dump cancel message: " + t.status().message);
	}

	// Cancel while the synth stores a dump that has gone out in full: wait for its answer,
	// then re-protect. Re-protecting earlier would be ignored and leave memory open.
	{
		FakeSynth synth;
		synth.storeMs = 3000.0;
		Transfer t;
		Dump all{Unit::AllData, -1, pattern(rawBytes(Unit::AllData), 44)};
		t.startImport({all});
		const double wire = double(messageBytes(Unit::AllData)) / kUartBytesPerMs;
		bool cancelled = false;
		const auto st = run(t, synth, [&](int, double) {
			if (!cancelled && synth.emu > wire + 1000.0) { t.cancel(); cancelled = true; }
		});
		check(st == Transfer::State::Cancelled && contains(t.status().message, "was stored"),
			"cancel while storing: " + t.status().message);
		check(std::equal(all.raw.begin() + long(kGlobalBytes), all.raw.begin() + long(kGlobalBytes + kProgramBytes * kProgramCount),
			synth.programs.begin()), "the dump already sent was stored");
		check(synth.ignoredWhileStoring == 0 && synth.protectProgram && synth.protectPattern
			&& !synth.ackTimes.empty() && synth.protectTimes.back() > synth.ackTimes.back(),
			"re-protect waits for the store");
	}

	// A slow emulator (a quarter of real time) still completes an all-data import: every
	// timeout is on the emulated clock.
	{
		FakeSynth synth;
		synth.speed = 0.25;
		synth.storeMs = 2000.0;
		Transfer t;
		Dump all{Unit::AllData, -1, pattern(rawBytes(Unit::AllData), 45)};
		t.startImport({all});
		check(run(t, synth) == Transfer::State::Succeeded, "all data import on a slow emulator: " + t.status().message);
		check(synth.protectProgram && synth.protectPattern && synth.ignoredWhileStoring == 0, "slow import re-protected");
	}

	// The emulator stops mid-dump (plugin bypassed): the transfer fails within about 5 s
	// naming the cause, then ends the dump and re-protects once it runs again.
	{
		FakeSynth synth;
		const auto before = synth.programs;
		Transfer t;
		t.startImport({{Unit::Bank, 1, pattern(rawBytes(Unit::Bank), 46)}});
		double stoppedAt = -1.0, reportedAt = -1.0;
		bool resumed = false;
		const auto st = run(t, synth, [&](int, double wall) {
			if (stoppedAt < 0 && synth.emu > 3000.0) { synth.running = false; stoppedAt = wall; }
			if (reportedAt < 0 && !t.busy()) reportedAt = wall;
			if (reportedAt >= 0 && !resumed)
			{
				if (wall == reportedAt)
				{
					check(t.settling() && !synth.protectProgram, "settling with memory still open");
					check(!t.startExport(Unit::Global, 0), "no new transfer while settling");
				}
				if (wall >= reportedAt + 2000.0) { synth.running = true; resumed = true; }
			}
		});
		check(st == Transfer::State::Failed && contains(t.status().message, "bypassed")
			&& contains(t.status().message, "Memory protect will be turned back on"),
			"stall reported: " + t.status().message);
		check(reportedAt - stoppedAt > kStallMs - 100.0 && reportedAt - stoppedAt < kStallMs + 100.0,
			"stall reported after about 5 s");
		check(!t.settling() && synth.protectProgram && synth.protectPattern, "re-protected once running again");
		check(synth.formatErrors == 1 && synth.programs == before, "cut-off bank rejected after the stall");
	}

	// Never running at all: fail after about 5 s, nothing sent.
	{
		FakeSynth synth;
		synth.running = false;
		Transfer t;
		t.startImport({{Unit::Program, 3, pattern(kProgramBytes, 47)}});
		run(t, synth, [](int, double) {}, 60000.0);
		check(t.status().state == Transfer::State::Failed && contains(t.status().message, "bypassed"),
			"stopped synth reported: " + t.status().message);
		synth.running = true;
		for (int i = 0; i < 400 && t.needsTicks(); ++i) { synth.step(10.0); t.tick(synth, 60000.0 + i * 10.0); }
		check(!t.needsTicks() && synth.protectProgram && synth.protectPattern
			&& synth.programs == FakeSynth().programs, "stopped synth: memory unchanged and protected");
	}

	// Stopping while an export waits for its reply: no cleanup, fail at once.
	{
		FakeSynth synth;
		Transfer t;
		t.startExport(Unit::AllData, 0);
		run(t, synth, [&](int tick, double) { if (tick == 100) synth.running = false; });
		check(t.status().state == Transfer::State::Failed && !t.settling() && contains(t.status().message, "bypassed"),
			"stalled export");
	}

	if (g_failures == 0) std::puts("PASS sysex transfer");
	return g_failures == 0 ? 0 : 1;
}
