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
#pragma once

#include <algorithm>
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
// Global record fields an import must not change: losing the channel or SysEx receive
// would cut the editor off from the synth, and memory protect is managed around loads.
constexpr std::size_t kGlobalProtectOffset = 163; // bit0 program, bit1 pattern memory
constexpr std::size_t kGlobalChannelOffset = 176;
constexpr std::size_t kGlobalSysexOffset = 178;   // bit0 transmit, bit1 receive
constexpr double kUartBytesPerMs = 3.125;         // 31250 baud, 10 bits per byte

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

// Split a .syx file into complete F0..F7 messages. Bytes outside a message are ignored,
// a new F0 restarts an unterminated one, and a trailing unterminated message is dropped.
inline std::vector<std::vector<std::uint8_t>> split(const std::uint8_t *data, std::size_t n)
{
	std::vector<std::vector<std::uint8_t>> out;
	std::vector<std::uint8_t> cur;
	bool in = false;
	for (std::size_t i = 0; i < n; ++i)
	{
		const std::uint8_t b = data[i];
		if (b == 0xF0) { cur.assign(1, b); in = true; }
		else if (in)
		{
			cur.push_back(b);
			if (b == 0xF7) { out.push_back(std::move(cur)); cur.clear(); in = false; }
		}
	}
	return out;
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

// Keep the synth's own channel, SysEx switches and memory-protect state in an imported
// global record (the first kGlobalBytes of a Global or All Data dump).
inline void preserveGlobalMidi(Dump &dump, const std::vector<std::uint8_t> &currentGlobal)
{
	if (!carriesGlobal(dump.unit) || currentGlobal.size() < kGlobalBytes || dump.raw.size() < kGlobalBytes)
		return;
	for (std::size_t offset : {kGlobalProtectOffset, kGlobalChannelOffset, kGlobalSysexOffset})
		dump.raw[offset] = currentGlobal[offset];
}

// The synth side of a transfer. Message thread only.
struct SysexPort
{
	virtual ~SysexPort() = default;
	virtual bool ready() = 0; // booted, and no other editor SysEx in flight
	virtual int channel() = 0;
	virtual bool openStream() = 0;
	virtual std::size_t pushStream(const std::uint8_t *bytes, std::size_t n) = 0;
	virtual std::size_t streamPending() = 0;
	virtual void closeStream() = 0;
	virtual bool popReply(std::vector<std::uint8_t> &out) = 0;
	virtual void clearReplies() = 0;
};

// One export or import at a time, advanced by tick() every few milliseconds. A transfer
// is a fixed list of steps; every byte goes through the exclusive stream so nothing else
// reaches the UART mid-dump. Loads to program or pattern memory are bracketed by
// unprotect/re-protect, matching WRITE. The re-protect steps sit after `cleanupFrom`
// and also run after a failure or cancel, which jump straight to them.
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
	const Status &status() const { return m_status; }
	// The reply of a successful export; the dumps of a successful import (after any
	// global preservation), for refreshing program names.
	const std::vector<std::uint8_t> &exported() const { return m_exported; }
	const std::vector<Dump> &imported() const { return m_dumps; }

	bool startExport(Unit unit, int index)
	{
		if (busy()) return false;
		reset();
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
		if (busy() || dumps.empty()) return false;
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
			m_steps.push_back(send([](int ch) { return request(Unit::Global, 0, ch); },
				"Reading current global settings"));
			m_steps.push_back(await(Step::Kind::AwaitGlobal, 3000.0, Unit::Global, -1));
			m_estimateMs += double(messageBytes(Unit::Global)) / kUartBytesPerMs;
		}
		for (std::size_t i = 0; i < m_dumps.size(); ++i)
		{
			const double wireMs = double(messageBytes(m_dumps[i].unit)) / kUartBytesPerMs;
			m_steps.push_back(send([this, i](int ch) {
				preserveGlobalMidi(m_dumps[i], m_currentGlobal);
				return build(m_dumps[i], ch);
			}, "Sending " + label(m_dumps[i])));
			m_steps.push_back(await(Step::Kind::AwaitAck, 4000.0 + wireMs, m_dumps[i].unit, -1));
			m_steps.push_back(pause(200.0));
			m_estimateMs += wireMs + 200.0;
		}
		m_cleanupFrom = m_steps.size();
		if (memory) protect(1, "Restoring memory protect");
		return true;
	}

	void cancel() { if (busy()) m_cancelRequested = true; }

	void tick(SysexPort &port, double nowMs)
	{
		if (!busy()) return;
		if (m_startMs < 0)
		{
			// Wait for any editor command or patch load before taking the UART.
			if (m_cancelRequested) { finish(State::Cancelled, "Cancelled"); return; }
			if (!port.ready())
			{
				if (m_firstTickMs < 0) m_firstTickMs = nowMs;
				if (nowMs - m_firstTickMs > 10000.0)
					finish(State::Failed, "The synth is busy or not running. Try again in a moment.");
				return;
			}
			m_startMs = nowMs;
			port.clearReplies();
		}
		// Never cancel mid-stream or while the firmware stores a dump: it drops SysEx
		// (including the re-protect) until it has acknowledged the load.
		const bool storing = m_next < m_steps.size() && m_steps[m_next].kind == Step::Kind::AwaitAck;
		if (m_cancelRequested && !m_streaming && !storing) stop(State::Cancelled, "Cancelled");
		while (busy() && advance(port, nowMs)) {}
		if (busy()) m_status.progress = std::clamp((nowMs - m_startMs) / m_estimateMs, 0.0, 0.99);
	}

private:
	struct Step
	{
		enum class Kind { Send, AwaitDump, AwaitGlobal, AwaitAck, Pause } kind = Kind::Pause;
		std::function<std::vector<std::uint8_t>(int channel)> message; // Send
		std::string phase;    // shown while this step runs, if set
		Unit unit = Unit::CurrentProgram;
		int index = -1;       // AwaitDump: the reply to match
		double ms = 0.0;      // Await timeout, or Pause length
	};
	enum class Reply { Waiting, Arrived, Stopped };

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
		m_next = m_cleanupFrom = 0;
		m_out.clear();
		m_outPos = 0;
		m_stepStartMs = m_startMs = m_firstTickMs = -1.0;
		m_estimateMs = 0.0;
		m_streaming = m_cancelRequested = false;
		m_result = State::Succeeded;
		m_message.clear();
	}

	void finish(State state, const std::string &message)
	{
		m_status.state = state;
		m_status.message = message;
		if (state == State::Succeeded) m_status.progress = 1.0;
	}

	// Record the outcome (the first failure wins) and skip to the cleanup steps.
	void stop(State state, const std::string &message)
	{
		if (m_result == State::Succeeded) { m_result = state; m_message = message; }
		if (m_next < m_cleanupFrom) { m_next = m_cleanupFrom; m_stepStartMs = -1.0; }
	}

	// Stream m_out to the synth. Returns true once it has drained into the UART.
	bool stream(SysexPort &port)
	{
		if (!m_streaming)
		{
			if (!port.openStream()) return false;
			m_streaming = true;
		}
		if (m_outPos < m_out.size())
			m_outPos += port.pushStream(m_out.data() + m_outPos, m_out.size() - m_outPos);
		if (m_outPos < m_out.size() || port.streamPending() != 0) return false;
		port.closeStream();
		m_streaming = false;
		return true;
	}

	Reply awaitReply(SysexPort &port, const Step &step)
	{
		std::vector<std::uint8_t> reply;
		while (port.popReply(reply))
		{
			if (step.kind == Step::Kind::AwaitAck)
			{
				switch (reply[4])
				{
					case 0x23: return Reply::Arrived;
					case 0x24:
					{
						const int code = reply.size() > 6 ? reply[5] & 7 : 0;
						stop(State::Failed, code == 1 ? "The synth refused the data: program memory is protected."
							: code == 2 ? "The synth refused the data: pattern memory is protected."
							: "The synth reported a data load error.");
						return Reply::Stopped;
					}
					case 0x26:
						stop(State::Failed, "The synth rejected the data as badly formatted.");
						return Reply::Stopped;
					default: continue;
				}
			}
			if (!matches(reply, step)) continue;
			if (step.kind == Step::Kind::AwaitGlobal)
			{
				std::optional<Dump> current = parse(reply);
				if (!current) { stop(State::Failed, "The synth sent an unreadable global dump."); return Reply::Stopped; }
				m_currentGlobal = std::move(current->raw);
			}
			else m_exported = std::move(reply);
			return Reply::Arrived;
		}
		return Reply::Waiting;
	}

	// Run the current step; true when it finished (or the transfer jumped to cleanup).
	bool advance(SysexPort &port, double nowMs)
	{
		if (m_next >= m_steps.size())
		{
			if (m_result != State::Succeeded) finish(m_result, m_message);
			else finish(State::Succeeded, m_exported.empty() ? "Import complete" : "Export complete");
			return false;
		}
		Step &step = m_steps[m_next];
		if (m_stepStartMs < 0)
		{
			m_stepStartMs = nowMs;
			if (!step.phase.empty()) m_status.phase = step.phase;
			if (step.kind == Step::Kind::Send) { m_out = step.message(port.channel()); m_outPos = 0; }
		}
		switch (step.kind)
		{
			case Step::Kind::Send:
				if (!stream(port)) return false;
				break;
			case Step::Kind::Pause:
				if (nowMs - m_stepStartMs < step.ms) return false;
				break;
			case Step::Kind::AwaitDump:
			case Step::Kind::AwaitGlobal:
			case Step::Kind::AwaitAck:
			{
				const Reply r = awaitReply(port, step);
				if (r == Reply::Stopped) return true;
				if (r == Reply::Waiting)
				{
					if (nowMs - m_stepStartMs <= step.ms) return false;
					stop(State::Failed, step.kind == Step::Kind::AwaitAck ? "The synth did not confirm the load."
						: "The synth did not send the requested data.");
					return true;
				}
				break;
			}
		}
		++m_next;
		m_stepStartMs = -1.0;
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
	std::size_t m_next = 0, m_cleanupFrom = 0, m_outPos = 0;
	double m_stepStartMs = -1.0, m_startMs = -1.0, m_firstTickMs = -1.0, m_estimateMs = 0.0;
	bool m_streaming = false, m_cancelRequested = false;
	State m_result = State::Succeeded; // outcome so far; cleanup still runs after a failure
	std::string m_message;
};

} // namespace prophecy::sysex
