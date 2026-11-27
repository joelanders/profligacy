// SPDX-License-Identifier: AGPL-3.0-only
//
// sysex_transfer.h - Prophecy SysEx file import/export.
//
// Dump formats (Korg Prophecy MIDI implementation, confirmed against the v1.7 firmware):
//   function  header bytes                    raw payload
//   40        F0 42 3c 41 40 01               one program, the edit buffer (535 bytes)
//   4C        F0 42 3c 41 4C ub pp 00         unit u: 0 program pp of bank b (535),
//                                             1 bank b (64 x 535), 2 all programs (128 x 535)
//   69        F0 42 3c 41 69 us 00            unit u: 0 arpeggio pattern s (128), 1 all ten
//   51        F0 42 3c 41 51 00               global (574)
//   50        F0 42 3c 41 50 00 00            all data: global, all programs, all patterns
// Payloads use Korg 7-in-8 packing. Loads are answered with 23 (completed), 24 (error,
// code 1 = program memory protected, 2 = pattern memory protected) or 26 (format error).
//
// Everything here is JUCE- and MAME-free so the plugin and the ROM-backed round-trip
// test drive the same state machine. The synth is reached through SysexPort.
//
// Timing. Every wait that depends on the firmware (replies, acknowledgements, settle
// pauses) is measured on the emulated clock, so a slow emulator never fails a load that
// is still arriving. Wall time is used only to notice that the emulator has stopped
// altogether (no audio blocks: the plugin bypassed, or the host not processing its
// track) and to bound the wait for other editor traffic before a transfer starts.
//
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prophecy::sysex {

constexpr std::size_t kProgramBytes = 535;
constexpr std::size_t kPatternBytes = 128;
constexpr std::size_t kGlobalBytes = 574;
constexpr int kProgramsPerBank = 64;
constexpr int kProgramCount = 128;
constexpr int kPatternCount = 10;
constexpr std::size_t kNameLength = 16; // a program record starts with its name
// Global record fields an import must not change. Losing the channel or SysEx receive
// would cut the editor off from the synth; the plugin selects programs with Bank Select
// (MSB 0, LSB = bank) and Program Change, so Program Change receive and the bank and
// program select maps must stay as they are; memory protect is managed around loads.
// Offsets 163 and 179-377 are verified in src/editor/deep_editor_manifest.tsv (p170/p171,
// p190-p389). 176 (p184) and 178 (p188/p189) are not swept there: they sit between the
// verified Local/Omni/Clock byte (177) and Program Change byte (179), and the ROM round
// trip checks 176 against the channel the firmware reports, but 178 is unproven.
constexpr std::size_t kGlobalProtectOffset = 163;       // program and pattern memory protect
constexpr std::size_t kGlobalChannelOffset = 176;       // global MIDI channel
constexpr std::size_t kGlobalSysexOffset = 178;         // SysEx transmit/receive switches
constexpr std::size_t kGlobalProgramChangeOffset = 179; // Program Change transmit/receive
constexpr std::size_t kGlobalBankMapOffset = 180;       // bank A-C select LSB/MSB, 180-185
constexpr std::size_t kGlobalProgramMapOffset = 186;    // program select map A00-C63
constexpr std::size_t kGlobalProgramMapEnd = 378;       // one past the map's last entry
// [first, last] byte ranges an import keeps from the synth's current global record.
inline constexpr std::pair<std::size_t, std::size_t> kPreservedGlobalRanges[] = {
	{kGlobalProtectOffset, kGlobalProtectOffset},
	{kGlobalChannelOffset, kGlobalChannelOffset},
	{kGlobalSysexOffset, kGlobalProgramMapEnd - 1}};
constexpr double kUartBytesPerMs = 3.125;         // 31250 baud, 10 bits per byte

// Transfer pacing and timeouts. "Emulated" values are on the synth's own clock.
constexpr double kStreamLeadBytes = 1024.0;     // how far a dump may run ahead of the emulated UART
constexpr std::size_t kMaxStreamPending = 2048; // keeps room for a terminating F7 in the 4 KB ring
constexpr double kStallMs = 5000.0;             // wall: no emulated progress means it has stopped
constexpr double kReadyWaitMs = 10000.0;        // wall: other editor traffic before starting
constexpr double kAckTimeoutMs = 4000.0 + kStreamLeadBytes / kUartBytesPerMs; // emulated
constexpr double kCutOffAckTimeoutMs = 2000.0;  // emulated: answer to a dump ended early

enum class Unit { CurrentProgram, Program, Bank, AllPrograms, Pattern, AllPatterns, Global, AllData };

// The names the editor uses for each unit.
inline constexpr std::pair<Unit, const char *> kUnitKeys[] = {
	{Unit::CurrentProgram, "current"}, {Unit::Program, "program"}, {Unit::Bank, "bank"},
	{Unit::AllPrograms, "allPrograms"}, {Unit::Pattern, "pattern"}, {Unit::AllPatterns, "allPatterns"},
	{Unit::Global, "global"}, {Unit::AllData, "allData"}};

inline const char *unitKey(Unit unit)
{
	for (const auto &[u, key] : kUnitKeys)
		if (u == unit) return key;
	return "";
}

inline std::optional<Unit> unitFromKey(std::string_view key)
{
	for (const auto &[u, k] : kUnitKeys)
		if (key == k) return u;
	return std::nullopt;
}

inline std::size_t rawBytes(Unit unit)
{
	switch (unit)
	{
		case Unit::CurrentProgram:
		case Unit::Program:     return kProgramBytes;
		case Unit::Bank:        return kProgramBytes * kProgramsPerBank;
		case Unit::AllPrograms: return kProgramBytes * kProgramCount;
		case Unit::Pattern:     return kPatternBytes;
		case Unit::AllPatterns: return kPatternBytes * kPatternCount;
		case Unit::Global:      return kGlobalBytes;
		case Unit::AllData:     return kGlobalBytes + kProgramBytes * kProgramCount
		                               + kPatternBytes * kPatternCount;
	}
	return 0;
}

inline std::uint8_t function(Unit unit)
{
	switch (unit)
	{
		case Unit::CurrentProgram: return 0x40;
		case Unit::Program:
		case Unit::Bank:
		case Unit::AllPrograms:    return 0x4C;
		case Unit::Pattern:
		case Unit::AllPatterns:    return 0x69;
		case Unit::Global:         return 0x51;
		case Unit::AllData:        return 0x50;
	}
	return 0;
}

inline std::size_t headerBytes(Unit unit)
{
	switch (function(unit))
	{
		case 0x4C: return 8;
		case 0x69: case 0x50: return 7;
		default: return 6;
	}
}

inline std::size_t packedBytes(std::size_t raw) { return raw / 7 * 8 + (raw % 7 ? raw % 7 + 1 : 0); }
inline std::size_t messageBytes(Unit unit) { return headerBytes(unit) + packedBytes(rawBytes(unit)) + 1; }

// Korg 7-in-8 packing: a byte of high bits (bit i = bit 7 of byte i) before up to seven
// low-7-bit bytes.
inline std::vector<std::uint8_t> pack(const std::vector<std::uint8_t> &raw)
{
	std::vector<std::uint8_t> out;
	out.reserve(packedBytes(raw.size()));
	for (std::size_t pos = 0; pos < raw.size(); pos += 7)
	{
		const std::size_t count = std::min<std::size_t>(7, raw.size() - pos);
		std::uint8_t high = 0;
		for (std::size_t i = 0; i < count; ++i)
			high |= ((raw[pos + i] >> 7) & 1) << i;
		out.push_back(high);
		for (std::size_t i = 0; i < count; ++i)
			out.push_back(raw[pos + i] & 0x7F);
	}
	return out;
}

inline std::vector<std::uint8_t> unpack(const std::uint8_t *packed, std::size_t n)
{
	std::vector<std::uint8_t> out;
	out.reserve(n / 8 * 7 + 7);
	for (std::size_t pos = 0; pos < n; pos += 8)
	{
		const std::uint8_t high = packed[pos];
		for (std::size_t i = 1; i < 8 && pos + i < n; ++i)
			out.push_back(std::uint8_t(packed[pos + i] | (((high >> (i - 1)) & 1) << 7)));
	}
	return out;
}

// `index`: program 0..127 (A00..B63), bank 0..1, or pattern 0..9; ignored otherwise.
inline std::vector<std::uint8_t> request(Unit unit, int index, int channel)
{
	const std::uint8_t ch = std::uint8_t(0x30 | (channel & 0x0F));
	switch (unit)
	{
		case Unit::CurrentProgram: return {0xF0, 0x42, ch, 0x41, 0x10, 0x00, 0xF7};
		case Unit::Program:
			return {0xF0, 0x42, ch, 0x41, 0x1C, std::uint8_t((index >> 6) & 1),
				std::uint8_t(index & 0x3F), 0x00, 0xF7};
		case Unit::Bank:
			return {0xF0, 0x42, ch, 0x41, 0x1C, std::uint8_t(0x10 | (index & 1)), 0x00, 0x00, 0xF7};
		case Unit::AllPrograms: return {0xF0, 0x42, ch, 0x41, 0x1C, 0x20, 0x00, 0x00, 0xF7};
		case Unit::Pattern:
			return {0xF0, 0x42, ch, 0x41, 0x34, std::uint8_t(index & 0x0F), 0x00, 0xF7};
		case Unit::AllPatterns: return {0xF0, 0x42, ch, 0x41, 0x34, 0x10, 0x00, 0xF7};
		case Unit::Global:      return {0xF0, 0x42, ch, 0x41, 0x0E, 0x00, 0xF7};
		case Unit::AllData:     return {0xF0, 0x42, ch, 0x41, 0x0F, 0x00, 0xF7};
	}
	return {};
}

// Global-group PARAMETER CHANGE, used for the memory-protect switches (p170, p171).
inline std::vector<std::uint8_t> globalParameter(int param, int value, int channel)
{
	return {0xF0, 0x42, std::uint8_t(0x30 | (channel & 0x0F)), 0x41, 0x41, 0x00,
		std::uint8_t(param & 0x7F), std::uint8_t((param >> 7) & 0x7F),
		std::uint8_t(value & 0x7F), std::uint8_t((value >> 7) & 0x7F), 0xF7};
}

struct Dump
{
	Unit unit = Unit::CurrentProgram;
	int index = -1; // program 0..127, bank 0..1 or pattern 0..9 where the unit has one
	std::vector<std::uint8_t> raw;
};

inline std::vector<std::uint8_t> build(const Dump &dump, int channel)
{
	const std::uint8_t ch = std::uint8_t(0x30 | (channel & 0x0F));
	std::vector<std::uint8_t> msg = {0xF0, 0x42, ch, 0x41, function(dump.unit)};
	switch (dump.unit)
	{
		case Unit::CurrentProgram: msg.push_back(0x01); break;
		case Unit::Program:
			msg.insert(msg.end(), {std::uint8_t((dump.index >> 6) & 1), std::uint8_t(dump.index & 0x3F), 0x00});
			break;
		case Unit::Bank: msg.insert(msg.end(), {std::uint8_t(0x10 | (dump.index & 1)), 0x00, 0x00}); break;
		case Unit::AllPrograms: msg.insert(msg.end(), {0x20, 0x00, 0x00}); break;
		case Unit::Pattern: msg.insert(msg.end(), {std::uint8_t(dump.index & 0x0F), 0x00}); break;
		case Unit::AllPatterns: msg.insert(msg.end(), {0x10, 0x00}); break;
		case Unit::Global: msg.push_back(0x00); break;
		case Unit::AllData: msg.insert(msg.end(), {0x00, 0x00}); break;
	}
	const std::vector<std::uint8_t> packed = pack(dump.raw);
	msg.insert(msg.end(), packed.begin(), packed.end());
	msg.push_back(0xF7);
	return msg;
}

// Parse one complete message as a Prophecy dump. Any device channel is accepted; the
// payload must have exactly the documented size for its unit.
inline std::optional<Dump> parse(const std::vector<std::uint8_t> &msg, std::string *error = nullptr)
{
	auto fail = [&](const char *why) -> std::optional<Dump> {
		if (error != nullptr) *error = why;
		return std::nullopt;
	};
	if (msg.size() < 7 || msg.front() != 0xF0 || msg.back() != 0xF7)
		return fail("not a complete SysEx message");
	if (msg[1] != 0x42 || (msg[2] & 0xF0) != 0x30 || msg[3] != 0x41)
		return fail("not a Korg Prophecy message");
	Dump dump;
	switch (msg[4])
	{
		case 0x40: dump.unit = Unit::CurrentProgram; break;
		case 0x4C:
		{
			if (msg.size() < 9) return fail("truncated program dump");
			const int unit = (msg[5] >> 4) & 3, bank = msg[5] & 1;
			if (unit == 0) { dump.unit = Unit::Program; dump.index = bank * kProgramsPerBank + (msg[6] & 0x3F); }
			else if (unit == 1) { dump.unit = Unit::Bank; dump.index = bank; }
			else if (unit == 2) dump.unit = Unit::AllPrograms;
			else return fail("unknown program dump unit");
			break;
		}
		case 0x69:
		{
			if ((msg[5] & 0x10) != 0) dump.unit = Unit::AllPatterns;
			else
			{
				dump.unit = Unit::Pattern;
				dump.index = msg[5] & 0x0F;
				if (dump.index >= kPatternCount) return fail("pattern number out of range");
			}
			break;
		}
		case 0x51: dump.unit = Unit::Global; break;
		case 0x50: dump.unit = Unit::AllData; break;
		default: return fail("not a Prophecy data dump");
	}
	const std::size_t header = headerBytes(dump.unit);
	if (msg.size() != messageBytes(dump.unit)) return fail("dump has the wrong size for its type");
	for (std::size_t i = 1; i + 1 < msg.size(); ++i)
		if (msg[i] & 0x80) return fail("dump contains a status byte");
	dump.raw = unpack(msg.data() + header, msg.size() - header - 1);
	if (dump.raw.size() != rawBytes(dump.unit)) return fail("dump has the wrong size for its type");
	return dump;
}

// MIDI real-time messages (F8-FF: clock, start/stop, active sensing, reset) are single
// bytes that may legally appear anywhere, including inside SysEx.
inline bool isRealtime(std::uint8_t b) { return b >= 0xF8; }

inline std::vector<std::uint8_t> withoutRealtime(const std::vector<std::uint8_t> &msg)
{
	std::vector<std::uint8_t> out;
	out.reserve(msg.size());
	for (std::uint8_t b : msg)
		if (!isRealtime(b)) out.push_back(b);
	return out;
}

// True when a message (complete or not) starts like a Prophecy data dump.
inline bool isProphecyDumpHeader(const std::vector<std::uint8_t> &msg)
{
	if (msg.size() < 5 || msg[0] != 0xF0 || msg[1] != 0x42 || (msg[2] & 0xF0) != 0x30 || msg[3] != 0x41)
		return false;
	switch (msg[4])
	{
		case 0x40: case 0x4C: case 0x50: case 0x51: case 0x69: return true;
		default: return false;
	}
}

struct Scan
{
	std::vector<std::vector<std::uint8_t>> messages;   // complete F0..F7 messages
	std::vector<std::vector<std::uint8_t>> incomplete; // began with F0 but never reached F7
};

// Split a .syx file into SysEx messages. Real-time bytes are dropped wherever they are.
// A message cut off by a new F0, by another status byte (which ends SysEx in MIDI), or by
// the end of the file goes to `incomplete`. Other bytes outside messages are ignored.
inline Scan scan(const std::uint8_t *data, std::size_t n)
{
	Scan out;
	std::vector<std::uint8_t> cur;
	bool in = false;
	for (std::size_t i = 0; i < n; ++i)
	{
		const std::uint8_t b = data[i];
		if (isRealtime(b)) continue;
		if (b == 0xF0)
		{
			if (in) out.incomplete.push_back(std::move(cur));
			cur.assign(1, b);
			in = true;
		}
		else if (!in) continue;
		else if (b == 0xF7)
		{
			cur.push_back(b);
			out.messages.push_back(std::move(cur));
			cur.clear();
			in = false;
		}
		else if (b & 0x80)
		{
			out.incomplete.push_back(std::move(cur));
			cur.clear();
			in = false;
		}
		else cur.push_back(b);
	}
	if (in) out.incomplete.push_back(std::move(cur));
	return out;
}

// The complete messages of a .syx file (see scan()).
inline std::vector<std::vector<std::uint8_t>> split(const std::uint8_t *data, std::size_t n)
{
	return scan(data, n).messages;
}

inline std::string programSlot(int program)
{
	char s[8];
	std::snprintf(s, sizeof(s), "%c%02d", program < kProgramsPerBank ? 'A' : 'B', program % kProgramsPerBank);
	return s;
}

inline std::string label(const Dump &dump)
{
	static const char *const patterns[kPatternCount] = {
		"UP", "DOWN", "ALT1", "ALT2", "RANDOM", "PAT1", "PAT2", "PAT3", "PAT4", "PAT5"};
	switch (dump.unit)
	{
		case Unit::CurrentProgram: return "Program (edit buffer)";
		case Unit::Program: return "Program " + programSlot(dump.index);
		case Unit::Bank: return std::string("Bank ") + (dump.index ? "B" : "A");
		case Unit::AllPrograms: return "All programs";
		case Unit::Pattern:
			return std::string("Arpeggio pattern ")
				+ (dump.index >= 0 && dump.index < kPatternCount ? patterns[dump.index] : "?");
		case Unit::AllPatterns: return "All arpeggio patterns";
		case Unit::Global: return "Global settings";
		case Unit::AllData: return "All data";
	}
	return "";
}

inline std::string programName(const std::uint8_t *record)
{
	std::string name;
	for (std::size_t i = 0; i < kNameLength; ++i)
		name += (record[i] >= 32 && record[i] < 127) ? char(record[i]) : ' ';
	while (!name.empty() && name.back() == ' ') name.pop_back();
	return name;
}

// (program slot, name) for every stored program a dump carries.
inline std::vector<std::pair<int, std::string>> programNames(const Dump &dump)
{
	int first = 0, count = 0;
	std::size_t offset = 0;
	switch (dump.unit)
	{
		case Unit::Program: first = dump.index; count = 1; break;
		case Unit::Bank: first = dump.index * kProgramsPerBank; count = kProgramsPerBank; break;
		case Unit::AllPrograms: count = kProgramCount; break;
		case Unit::AllData: count = kProgramCount; offset = kGlobalBytes; break;
		case Unit::CurrentProgram: // the edit buffer is not stored to a slot
		case Unit::Pattern:
		case Unit::AllPatterns:
		case Unit::Global: return {};
	}
	std::vector<std::pair<int, std::string>> out;
	for (int i = 0; i < count; ++i)
		out.emplace_back(first + i, programName(dump.raw.data() + offset + std::size_t(i) * kProgramBytes));
	return out;
}

inline bool storesToMemory(Unit unit)
{
	return unit != Unit::CurrentProgram && unit != Unit::Global;
}

inline bool carriesGlobal(Unit unit) { return unit == Unit::Global || unit == Unit::AllData; }

// Keep the synth's own kPreservedGlobalRanges (channel, SysEx and Program Change
// switches, bank and program select maps, memory protect) in an imported global record
// (the first kGlobalBytes of a Global or All Data dump).
inline void preserveGlobalMidi(Dump &dump, const std::vector<std::uint8_t> &currentGlobal)
{
	if (!carriesGlobal(dump.unit) || currentGlobal.size() < kGlobalBytes || dump.raw.size() < kGlobalBytes)
		return;
	for (const auto &[first, last] : kPreservedGlobalRanges)
		std::copy(currentGlobal.begin() + long(first), currentGlobal.begin() + long(last) + 1,
			dump.raw.begin() + long(first));
}

// What a .syx file holds: the dumps that can be loaded, and what will be skipped.
struct FileContents
{
	std::vector<Dump> dumps;
	int foreign = 0;           // complete messages that are not Prophecy data dumps
	int damaged = 0;           // Prophecy dumps that fail validation
	std::string damagedReason; // why the first damaged dump failed
	int cutOffDumps = 0;       // Prophecy dumps that end without F7 (a truncated file)
	int cutOffOther = 0;       // other messages that end without F7
	bool hasMessages = false;  // any F0 at all

	// One line per kind of skipped data, for the import dialog.
	std::vector<std::string> skipped() const
	{
		auto count = [](int n, const char *one, const char *many) {
			return std::to_string(n) + " " + (n == 1 ? one : many);
		};
		std::vector<std::string> out;
		if (damaged > 0)
			out.push_back(count(damaged, "damaged Prophecy dump", "damaged Prophecy dumps")
				+ " (" + (damaged > 1 ? "the first: " : "") + damagedReason + ")");
		if (cutOffDumps > 0)
			out.push_back(count(cutOffDumps, "Prophecy dump", "Prophecy dumps")
				+ " cut off before the end, so the file may be truncated");
		if (cutOffOther > 0)
			out.push_back(count(cutOffOther, "other message", "other messages") + " cut off before the end");
		if (foreign > 0)
			out.push_back(count(foreign, "message that is not a Prophecy data dump",
				"messages that are not Prophecy data dumps"));
		return out;
	}
};

inline FileContents readFile(const std::uint8_t *data, std::size_t n)
{
	FileContents out;
	Scan s = scan(data, n);
	out.hasMessages = !s.messages.empty() || !s.incomplete.empty();
	for (const auto &msg : s.messages)
	{
		std::string why;
		if (auto dump = parse(msg, &why)) out.dumps.push_back(std::move(*dump));
		else if (isProphecyDumpHeader(msg))
		{
			if (out.damaged++ == 0) out.damagedReason = why;
		}
		else ++out.foreign;
	}
	for (const auto &msg : s.incomplete)
		++(isProphecyDumpHeader(msg) ? out.cutOffDumps : out.cutOffOther);
	return out;
}

// The synth side of a transfer. Message thread only.
struct SysexPort
{
	virtual ~SysexPort() = default;
	virtual bool ready() = 0; // booted, and no other editor SysEx in flight
	virtual int channel() = 0;
	// The synth's own clock in milliseconds. It stands still while the emulator does: the
	// plugin bypassed, or the host not processing its track.
	virtual double emulatedMs() = 0;
	virtual bool openStream() = 0;
	virtual std::size_t pushStream(const std::uint8_t *bytes, std::size_t n) = 0;
	virtual std::size_t streamPending() = 0;
	virtual void closeStream() = 0;
	virtual bool popReply(std::vector<std::uint8_t> &out) = 0;
	virtual void clearReplies() = 0;
};

inline constexpr const char *kStalledMessage =
	"The synth stopped running: no audio was processed for 5 seconds. The plugin may be "
	"bypassed, or the host may not be processing its track.";

// One export or import at a time, advanced by tick() every few milliseconds. A transfer
// is a fixed list of steps. Every byte goes through the exclusive stream, so nothing else
// reaches the UART mid-dump, and it is paced to the emulated UART, so a cancel can still
// cut a dump short. Loads to program or pattern memory are bracketed by unprotect and
// re-protect, matching WRITE. The re-protect steps sit after `cleanupFrom`; a failure or
// cancel skips ahead to them, but only after the synth has answered any dump that has
// gone out (the firmware ignores SysEx, the re-protect included, while it stores one).
class Transfer
{
public:
	enum class State { Idle, Running, Succeeded, Failed, Cancelled };
	struct Status
	{
		State state = State::Idle;
		std::string phase;
		double progress = 0.0;
		std::string message;
	};

	bool busy() const { return m_status.state == State::Running; }
	// After a stall the outcome is reported at once, but ending a cut-off message and
	// restoring memory protect wait until the synth runs again. Keep calling tick() while
	// this is true; no new transfer starts until it is false.
	bool settling() const { return m_settling; }
	bool needsTicks() const { return busy() || m_settling; }
	const Status &status() const { return m_status; }
	// The reply of a successful export, without real-time bytes.
	const std::vector<std::uint8_t> &exported() const { return m_exported; }
	// The dumps of an import (after any global preservation), and the indices of those the
	// synth acknowledged as stored.
	const std::vector<Dump> &imported() const { return m_dumps; }
	const std::vector<std::size_t> &stored() const { return m_stored; }

	bool startExport(Unit unit, int index)
	{
		const int count = unit == Unit::Program ? kProgramCount : unit == Unit::Bank ? 2
			: unit == Unit::Pattern ? kPatternCount : 1;
		if (needsTicks() || (count > 1 && (index < 0 || index >= count))) return false;
		reset();
		m_export = true;
		const double wireMs = double(messageBytes(unit)) / kUartBytesPerMs;
		m_steps.push_back(send([unit, index](int ch) { return request(unit, index, ch); },
			"Receiving " + label(Dump{unit, index, {}})));
		m_steps.push_back(await(Step::Kind::AwaitDump, 3000.0 + wireMs * 1.5, unit, index));
		m_cleanupFrom = m_steps.size();
		m_estimateMs = wireMs + 300.0;
		return true;
	}

	bool startImport(std::vector<Dump> dumps)
	{
		if (needsTicks() || dumps.empty()) return false;
		reset();
		m_dumps = std::move(dumps);
		bool memory = false, global = false;
		for (const Dump &d : m_dumps)
		{
			memory = memory || storesToMemory(d.unit);
			global = global || carriesGlobal(d.unit);
		}
		auto protect = [this](int value, const std::string &phase) {
			for (int param : {170, 171})
			{
				m_steps.push_back(send([param, value](int ch) { return globalParameter(param, value, ch); },
					param == 170 ? phase : std::string()));
				m_steps.push_back(pause(150.0));
			}
			m_estimateMs += 300.0;
		};
		if (memory) protect(0, "Preparing import");
		if (global)
		{
			const double wireMs = double(messageBytes(Unit::Global)) / kUartBytesPerMs;
			m_steps.push_back(send([](int ch) { return request(Unit::Global, 0, ch); },
				"Reading current global settings"));
			m_steps.push_back(await(Step::Kind::AwaitGlobal, 3000.0 + wireMs, Unit::Global, -1));
			m_estimateMs += wireMs + 100.0;
		}
		for (std::size_t i = 0; i < m_dumps.size(); ++i)
		{
			const double wireMs = double(messageBytes(m_dumps[i].unit)) / kUartBytesPerMs;
			Step s = send([this, i](int ch) {
				preserveGlobalMidi(m_dumps[i], m_currentGlobal);
				return build(m_dumps[i], ch);
			}, "Sending " + label(m_dumps[i]));
			s.dump = int(i);
			m_steps.push_back(std::move(s));
			Step a = await(Step::Kind::AwaitAck, kAckTimeoutMs, m_dumps[i].unit, -1);
			a.dump = int(i);
			m_steps.push_back(std::move(a));
			m_steps.push_back(pause(200.0));
			m_estimateMs += wireMs + 400.0;
		}
		m_cleanupFrom = m_steps.size();
		if (memory)
		{
			m_steps.push_back(pause(200.0));
			protect(1, "Restoring memory protect");
		}
		return true;
	}

	// Takes effect on the next tick, at any point of the transfer.
	void cancel() { if (busy()) m_cancelRequested = true; }

	// `wallMs` is the host's clock. Everything else is timed on port.emulatedMs().
	void tick(SysexPort &port, double wallMs)
	{
		if (!needsTicks()) return;
		const double emu = port.emulatedMs();
		if (m_lastProgressWallMs < 0 || std::abs(emu - m_lastEmuMs) > 1e-9)
		{
			m_lastEmuMs = emu;
			m_lastProgressWallMs = wallMs;
		}
		const bool stalled = wallMs - m_lastProgressWallMs > kStallMs;
		if (busy() && m_startEmuMs < 0)
		{
			// Wait for any editor command or patch load before taking the UART.
			if (m_cancelRequested) { finish(State::Cancelled, "Cancelled. Nothing was sent."); return; }
			if (!port.ready())
			{
				if (m_firstTickWallMs < 0) m_firstTickWallMs = wallMs;
				if (stalled) finish(State::Failed, std::string(kStalledMessage) + " Nothing was sent.");
				else if (wallMs - m_firstTickWallMs > kReadyWaitMs)
					finish(State::Failed, "The synth is busy with another editor command. Try again in a moment.");
				return;
			}
			m_startEmuMs = emu;
			port.clearReplies();
		}
		if (busy())
		{
			if (m_cancelRequested && !m_stopping) stop(State::Cancelled, "Cancelled.");
			if (stalled && !m_stopping && m_next < m_cleanupFrom) stop(State::Failed, kStalledMessage);
		}
		while (needsTicks() && advance(port, emu)) {}
		if (busy() && stalled)
		{
			// Report now; whatever cleanup is left resumes when the synth runs again.
			m_settling = true;
			finish(m_result, outcome(true));
		}
		else if (busy())
			m_status.progress = std::clamp((emu - m_startEmuMs) / std::max(m_estimateMs, 1.0), 0.0, 0.99);
	}

private:
	struct Step
	{
		enum class Kind { Send, AwaitDump, AwaitGlobal, AwaitAck, Pause } kind = Kind::Pause;
		std::function<std::vector<std::uint8_t>(int channel)> message; // Send
		std::string phase;    // shown while this step runs, if set
		Unit unit = Unit::CurrentProgram;
		int index = -1;       // AwaitDump: the reply to match
		int dump = -1;        // Send and AwaitAck of an import: which dump
		double ms = 0.0;      // emulated: Await timeout, or Pause length
	};
	enum class Reply { Waiting, Arrived, Stopped };
	enum class Answer { None, Stored, Refused };

	static Step send(std::function<std::vector<std::uint8_t>(int)> message, std::string phase)
	{
		Step s;
		s.kind = Step::Kind::Send;
		s.message = std::move(message);
		s.phase = std::move(phase);
		return s;
	}
	static Step await(Step::Kind kind, double timeoutMs, Unit unit, int index)
	{
		Step s;
		s.kind = kind;
		s.ms = timeoutMs;
		s.unit = unit;
		s.index = index;
		return s;
	}
	static Step pause(double ms) { Step s; s.ms = ms; return s; }

	void reset()
	{
		m_status = Status{State::Running, "Waiting for the synth", 0.0, {}};
		m_steps.clear();
		m_dumps.clear();
		m_exported.clear();
		m_currentGlobal.clear();
		m_out.clear();
		m_stored.clear();
		m_next = m_cleanupFrom = m_outPos = 0;
		m_stepStartEmuMs = m_streamStartEmuMs = m_estimateMs = m_lastEmuMs = 0.0;
		m_startEmuMs = m_lastProgressWallMs = m_firstTickWallMs = -1.0;
		m_export = m_stepStarted = m_streaming = m_cutOff = false;
		m_cancelRequested = m_stopping = m_settling = false;
		m_awaitingAnswer = m_answerCutOff = false;
		m_answerDump = -1;
		m_answer = Answer::None;
		m_result = State::Succeeded;
		m_message.clear();
	}

	void finish(State state, const std::string &message)
	{
		m_status.state = state;
		m_status.message = message;
		if (state == State::Succeeded) m_status.progress = 1.0;
	}

	// Record the outcome (the first one wins) and wind down: the next step boundary skips
	// to the cleanup steps, after hearing the answer to any dump already sent.
	void stop(State state, const std::string &message)
	{
		if (m_result == State::Succeeded) { m_result = state; m_message = message; }
		m_stopping = true;
		if (state == State::Cancelled) m_status.phase = "Cancelling";
	}

	bool skipToCleanup()
	{
		m_next = std::max(m_next, m_cleanupFrom);
		m_stepStarted = false;
		return true;
	}

	// The message for the final status.
	std::string outcome(bool stalledNow) const
	{
		std::string msg = m_result == State::Succeeded ? (m_export ? "Export complete." : "Import complete.")
			: m_message;
		const bool interrupted = m_result == State::Cancelled || (stalledNow && m_result != State::Succeeded);
		if (!m_export && interrupted)
		{
			if (m_answerDump >= 0)
			{
				const std::string what = label(m_dumps[std::size_t(m_answerDump)]);
				if (m_awaitingAnswer)
					msg += " " + what + " was on its way to the synth when it stopped.";
				else if (m_answerCutOff)
					msg += m_answer == Answer::Refused ? " The synth rejected the incomplete " + what + "."
						: m_answer == Answer::Stored ? " The synth still stored the incomplete " + what + "; check it."
						: " The synth did not answer the incomplete " + what + "; check it.";
				else
					msg += m_answer == Answer::Stored ? " " + what + " had already been sent in full and was stored."
						: m_answer == Answer::Refused ? " The synth refused " + what + "."
						: " " + what + " had already been sent in full, but the synth did not confirm it.";
			}
			else if (m_stored.empty()) msg += " Nothing was loaded.";
			if (m_dumps.size() > 1)
				msg += " " + std::to_string(m_stored.size()) + " of " + std::to_string(m_dumps.size())
					+ " dumps were loaded.";
		}
		if (stalledNow)
		{
			bool protectLeft = false;
			for (std::size_t i = std::max(m_next, m_cleanupFrom); i < m_steps.size(); ++i)
				protectLeft = protectLeft || m_steps[i].kind == Step::Kind::Send;
			if (protectLeft) msg += " Memory protect will be turned back on when the synth runs again.";
		}
		return msg;
	}

	// Stream m_out to the synth. Returns true once it has all been handed to the UART.
	bool stream(SysexPort &port, double emu)
	{
		if (!m_streaming)
		{
			if (!port.openStream()) return false;
			m_streaming = true;
			m_streamStartEmuMs = emu;
		}
		if (m_outPos < m_out.size())
		{
			// Stay at most kStreamLeadBytes ahead of the emulated UART, so bytes not yet
			// sent can still be held back by a cancel.
			const double due = kStreamLeadBytes + (emu - m_streamStartEmuMs) * kUartBytesPerMs;
			std::size_t n = due > double(m_outPos) ? std::size_t(due) - m_outPos : 0;
			const std::size_t pending = port.streamPending();
			n = std::min({n, m_out.size() - m_outPos,
				pending < kMaxStreamPending ? kMaxStreamPending - pending : std::size_t(0)});
			if (m_cutOff && m_outPos + 1 == m_out.size()) n = 1; // the closing F7 of a cut-off dump
			if (n > 0) m_outPos += port.pushStream(m_out.data() + m_outPos, n);
		}
		if (m_outPos < m_out.size() || port.streamPending() != 0) return false;
		port.closeStream();
		m_streaming = false;
		return true;
	}

	Reply awaitReply(SysexPort &port, const Step &step)
	{
		std::vector<std::uint8_t> raw;
		while (port.popReply(raw))
		{
			const std::vector<std::uint8_t> reply = withoutRealtime(raw);
			if (reply.size() < 6) continue;
			if (step.kind == Step::Kind::AwaitAck)
			{
				const std::string what = label(m_dumps[std::size_t(step.dump)]);
				switch (reply[4])
				{
					case 0x23:
						m_awaitingAnswer = false;
						m_answer = Answer::Stored;
						m_stored.push_back(std::size_t(step.dump));
						return Reply::Arrived;
					case 0x24:
					{
						m_awaitingAnswer = false;
						m_answer = Answer::Refused;
						const int code = reply.size() > 6 ? reply[5] & 7 : 0;
						stop(State::Failed, code == 1 ? "The synth refused " + what + ": program memory is protected."
							: code == 2 ? "The synth refused " + what + ": pattern memory is protected."
							: "The synth reported an error loading " + what + ".");
						return Reply::Stopped;
					}
					case 0x26:
						m_awaitingAnswer = false;
						m_answer = Answer::Refused;
						stop(State::Failed, "The synth rejected " + what + " as badly formatted.");
						return Reply::Stopped;
					default: continue;
				}
			}
			if (!matches(reply, step)) continue;
			std::string why;
			std::optional<Dump> dump = parse(reply, &why);
			if (!dump)
			{
				stop(State::Failed, "The synth sent a damaged " + label(Dump{step.unit, step.index, {}})
					+ " dump (" + why + ").");
				return Reply::Stopped;
			}
			if (step.kind == Step::Kind::AwaitGlobal) m_currentGlobal = std::move(dump->raw);
			else m_exported = reply;
			return Reply::Arrived;
		}
		return Reply::Waiting;
	}

	// Run the current step; true when it finished (or the transfer skipped ahead).
	bool advance(SysexPort &port, double emu)
	{
		if (m_next >= m_steps.size())
		{
			if (m_settling) m_settling = false; // the outcome went out at the stall
			else finish(m_result, outcome(false));
			return false;
		}
		Step &step = m_steps[m_next];
		const bool interruptible = m_next < m_cleanupFrom;
		if (!m_stepStarted)
		{
			// Winding down skips everything before cleanup except hearing how the synth
			// answered a dump that has (wholly or partly) gone out.
			if (m_stopping && interruptible && !(step.kind == Step::Kind::AwaitAck && m_awaitingAnswer))
				return skipToCleanup();
			m_stepStarted = true;
			m_stepStartEmuMs = emu;
			if (step.kind == Step::Kind::AwaitAck && m_stopping)
				m_status.phase = "Waiting for the synth to answer " + label(m_dumps[std::size_t(step.dump)]);
			else if (!step.phase.empty()) m_status.phase = step.phase;
			if (step.kind == Step::Kind::Send)
			{
				m_out = step.message(port.channel());
				m_outPos = 0;
				m_cutOff = false;
			}
		}
		switch (step.kind)
		{
			case Step::Kind::Send:
			{
				if (m_stopping && interruptible && !m_cutOff && m_outPos < m_out.size())
				{
					if (m_outPos == 0)
					{
						if (m_streaming) { port.closeStream(); m_streaming = false; }
						return skipToCleanup();
					}
					// End the message where it stands, so the synth sees a short dump rather
					// than a SysEx that swallows whatever comes next.
					m_out.resize(m_outPos);
					m_out.push_back(0xF7);
					m_cutOff = true;
				}
				const bool sent = stream(port, emu);
				if (step.dump >= 0 && m_outPos > 0)
				{
					m_awaitingAnswer = true;
					m_answerDump = step.dump;
					m_answerCutOff = m_cutOff;
					m_answer = Answer::None;
				}
				if (!sent) return false;
				break;
			}
			case Step::Kind::Pause:
				if (m_stopping && interruptible) return skipToCleanup();
				if (emu - m_stepStartEmuMs < step.ms) return false;
				break;
			case Step::Kind::AwaitDump:
			case Step::Kind::AwaitGlobal:
				if (m_stopping && interruptible) return skipToCleanup();
				[[fallthrough]];
			case Step::Kind::AwaitAck:
			{
				if (awaitReply(port, step) != Reply::Waiting) break;
				const bool ack = step.kind == Step::Kind::AwaitAck;
				if (emu - m_stepStartEmuMs <= (ack && m_answerCutOff ? kCutOffAckTimeoutMs : step.ms))
					return false;
				if (ack)
				{
					m_awaitingAnswer = false;
					stop(State::Failed, "The synth did not confirm " + label(m_dumps[std::size_t(step.dump)]) + ".");
				}
				else stop(State::Failed, "The synth did not send the requested data.");
				break;
			}
		}
		++m_next;
		m_stepStarted = false;
		return true;
	}

	static bool matches(const std::vector<std::uint8_t> &reply, const Step &step)
	{
		if (reply.size() < 7 || reply[4] != function(step.unit)) return false;
		switch (step.unit)
		{
			case Unit::Program:
				return reply[5] == ((step.index >> 6) & 1) && reply[6] == (step.index & 0x3F);
			case Unit::Bank: return reply[5] == (0x10 | (step.index & 1));
			case Unit::AllPrograms: return (reply[5] & 0x30) == 0x20;
			case Unit::Pattern: return reply[5] == (step.index & 0x0F);
			case Unit::AllPatterns: return (reply[5] & 0x10) != 0;
			case Unit::CurrentProgram:
			case Unit::Global:
			case Unit::AllData: return true;
		}
		return false;
	}

	Status m_status;
	std::vector<Step> m_steps;
	std::vector<Dump> m_dumps;
	std::vector<std::uint8_t> m_exported, m_currentGlobal, m_out;
	std::vector<std::size_t> m_stored;
	std::size_t m_next = 0, m_cleanupFrom = 0, m_outPos = 0;
	double m_stepStartEmuMs = 0.0, m_streamStartEmuMs = 0.0, m_estimateMs = 0.0, m_lastEmuMs = 0.0;
	double m_startEmuMs = -1.0, m_lastProgressWallMs = -1.0, m_firstTickWallMs = -1.0;
	bool m_export = false, m_stepStarted = false, m_streaming = false, m_cutOff = false;
	bool m_cancelRequested = false, m_stopping = false, m_settling = false;
	// The most recent dump that went out: is its answer still awaited, was it cut off,
	// and how did the synth answer.
	bool m_awaitingAnswer = false, m_answerCutOff = false;
	int m_answerDump = -1;
	Answer m_answer = Answer::None;
	State m_result = State::Succeeded; // outcome so far; cleanup still runs after a failure
	std::string m_message;
};

} // namespace prophecy::sysex
