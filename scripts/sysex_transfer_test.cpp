// SPDX-License-Identifier: AGPL-3.0-only
// No-ROM checks for src/sysex_transfer.h: dump formats, file parsing, and the transfer
// state machine against a simulated synth. The ROM-backed round trip is separate.
#include "sysex_transfer.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

using namespace prophecy::sysex;

static int g_failures = 0;
static void check(bool ok, const char *what)
{
	if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); ++g_failures; }
}

static std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed)
{
	std::vector<std::uint8_t> v(n);
	for (std::size_t i = 0; i < n; ++i) v[i] = std::uint8_t((i * 37u + seed * 101u) ^ (i >> 3));
	return v;
}

// A synth that answers requests from its own memory and acknowledges loads, at a
// configurable UART rate. Protect switches (global p170/p171) are honoured.
struct FakeSynth : SysexPort
{
	std::vector<std::uint8_t> programs = pattern(kProgramBytes * kProgramCount, 1);
	std::vector<std::uint8_t> patterns = pattern(kPatternBytes * kPatternCount, 2);
	std::vector<std::uint8_t> global = pattern(kGlobalBytes, 3);
	std::vector<std::uint8_t> edit = pattern(kProgramBytes, 4);
	bool protectProgram = true, protectPattern = true;
	bool answer = true;       // false: ignore everything (a wedged or absent synth)
	bool ackLoads = true;
	int busyTicks = 0;        // ready() is false for this many calls
	std::vector<std::uint8_t> stream, received;
	std::deque<std::vector<std::uint8_t>> replies;
	int protectChanges = 0, opens = 0;
	bool open = false;

	FakeSynth()
	{
		global[kGlobalChannelOffset] = 0;
		global[kGlobalSysexOffset] = 0x02;
		global[kGlobalProtectOffset] = 0x03;
	}
	bool ready() override { if (busyTicks > 0) { --busyTicks; return false; } return true; }
	int channel() override { return 0; }
	bool openStream() override { if (open) return false; open = true; ++opens; return true; }
	std::size_t pushStream(const std::uint8_t *b, std::size_t n) override
	{
		const std::size_t take = std::min<std::size_t>(n, 4096 - stream.size());
		stream.insert(stream.end(), b, b + take);
		return take;
	}
	std::size_t streamPending() override { return stream.size(); }
	void closeStream() override { open = false; }
	bool popReply(std::vector<std::uint8_t> &out) override
	{
		if (replies.empty()) return false;
		out = std::move(replies.front());
		replies.pop_front();
		return true;
	}
	void clearReplies() override { replies.clear(); }

	// Drain the stream as the UART would and act on each complete message.
	void pump()
	{
		for (std::uint8_t b : stream)
		{
			if (b == 0xF0) received.assign(1, b);
			else if (!received.empty())
			{
				received.push_back(b);
				if (b == 0xF7) { handle(received); received.clear(); }
			}
		}
		stream.clear();
	}
	void reply(std::vector<std::uint8_t> msg) { if (answer) replies.push_back(std::move(msg)); }
	void ack(std::uint8_t f, int code = -1)
	{
		std::vector<std::uint8_t> m = {0xF0, 0x42, 0x30, 0x41, f};
		if (code >= 0) m.push_back(std::uint8_t(code));
		m.push_back(0xF7);
		if (ackLoads) reply(m);
	}
	void handle(const std::vector<std::uint8_t> &m)
	{
		if (!answer || m.size() < 6) return;
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
			return;
		}
		if (f == 0x10) reply(build({Unit::CurrentProgram, -1, edit}, 0));
		if (f == 0x1C)
		{
			const int unit = (m[5] >> 4) & 3, bank = m[5] & 1;
			if (unit == 0) reply(build({Unit::Program, bank * 64 + m[6], progs(bank * 64 + m[6], 1)}, 0));
			if (unit == 1) reply(build({Unit::Bank, bank, progs(bank * 64, 64)}, 0));
			if (unit == 2) reply(build({Unit::AllPrograms, -1, programs}, 0));
		}
		if (f == 0x34)
		{
			if (m[5] & 0x10) reply(build({Unit::AllPatterns, -1, patterns}, 0));
			else reply(build({Unit::Pattern, m[5], std::vector<std::uint8_t>(patterns.begin() + m[5] * 128,
				patterns.begin() + (m[5] + 1) * 128)}, 0));
		}
		if (f == 0x0E) reply(build({Unit::Global, -1, global}, 0));
		if (f == 0x0F)
		{
			std::vector<std::uint8_t> all = global;
			all.insert(all.end(), programs.begin(), programs.end());
			all.insert(all.end(), patterns.begin(), patterns.end());
			reply(build({Unit::AllData, -1, all}, 0));
		}
		if (f == 0x40 || f == 0x4C || f == 0x69 || f == 0x51 || f == 0x50)
		{
			std::optional<Dump> d = parse(m);
			if (!d) { ack(0x26); return; }
			const bool needsProgram = d->unit == Unit::Program || d->unit == Unit::Bank
				|| d->unit == Unit::AllPrograms || d->unit == Unit::AllData;
			const bool needsPattern = d->unit == Unit::Pattern || d->unit == Unit::AllPatterns
				|| d->unit == Unit::AllData;
			if (needsProgram && protectProgram) { ack(0x24, 1); return; }
			if (needsPattern && protectPattern) { ack(0x24, 2); return; }
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
			ack(0x23);
		}
	}
};

static Transfer::State run(Transfer &t, FakeSynth &synth, double limitMs = 200000.0, int cancelAtTick = -1)
{
	double now = 0.0;
	for (int tick = 0; t.busy() && now < limitMs; ++tick, now += 10.0)
	{
		if (tick == cancelAtTick) t.cancel();
		t.tick(synth, now);
		synth.pump();
	}
	return t.status().state;
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

	// Files: concatenated dumps, junk between them, and a truncated tail.
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

	// Names and global preservation.
	{
		Dump bank{Unit::Bank, 1, std::vector<std::uint8_t>(rawBytes(Unit::Bank), ' ')};
		const char name[] = "Hello Prophecy";
		std::copy(name, name + 14, bank.raw.begin() + 3 * long(kProgramBytes));
		const auto names = programNames(bank);
		check(names.size() == 64 && names[3].first == 67 && names[3].second == "Hello Prophecy", "bank program names");
		Dump all = samples[7];
		std::vector<std::uint8_t> current(kGlobalBytes, 0);
		current[kGlobalChannelOffset] = 9;
		current[kGlobalSysexOffset] = 0x02;
		current[kGlobalProtectOffset] = 0x00;
		preserveGlobalMidi(all, current);
		check(all.raw[kGlobalChannelOffset] == 9 && all.raw[kGlobalSysexOffset] == 0x02
			&& all.raw[kGlobalProtectOffset] == 0 && all.raw[0] == samples[7].raw[0], "preserve global MIDI");
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
		check(run(t, synth) == Transfer::State::Succeeded, ("export " + label(d)).c_str());
		auto got = parse(t.exported());
		check(got && got->unit == d.unit, "exported dump parses");
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
	}

	// An imported global keeps the synth's channel, SysEx and protect bytes.
	{
		FakeSynth synth;
		Transfer t;
		Dump g{Unit::Global, -1, std::vector<std::uint8_t>(kGlobalBytes, 0x55)};
		check(t.startImport({g}), "start global import");
		check(run(t, synth) == Transfer::State::Succeeded, "global import succeeds");
		check(synth.global[0] == 0x55 && synth.global[kGlobalChannelOffset] == 0
			&& synth.global[kGlobalSysexOffset] == 0x02 && synth.global[kGlobalProtectOffset] == 0x03,
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
		double now = 0.0;
		for (int tick = 0; t.busy() && tick < 100000; ++tick, now += 10.0)
		{
			t.tick(synth, now);
			synth.pump();
			if (tick == 40) synth.protectProgram = true;
		}
		check(t.status().state == Transfer::State::Failed
			&& t.status().message.find("protected") != std::string::npos, "protected load reported");
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
		check(run(t, synth, 200000.0, 60) == Transfer::State::Cancelled, "import cancelled");
		check(synth.protectProgram && synth.protectPattern, "re-protected after cancel");
		check(!std::equal(dumps[3].raw.begin(), dumps[3].raw.end(), synth.programs.begin() + 3 * long(kProgramBytes)),
			"later dumps not sent after cancel");
	}

	if (g_failures == 0) std::puts("PASS sysex transfer");
	return g_failures == 0 ? 0 : 1;
}
