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
#include <optional>
#include <string>
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

// One export or import at a time, advanced by tick() every few milliseconds. Every byte
// goes through the exclusive stream so nothing else reaches the UART mid-dump. Loads to
// program or pattern memory are bracketed by unprotect/re-protect, matching WRITE; the
// re-protect runs on failure and cancel too.
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
		reset("Requesting " + label(Dump{unit, index, {}}));
		Step req;
		req.kind = Step::Kind::Request;
		req.unit = unit;
		req.index = index;
		m_steps.push_back(req);
		Step await;
		await.kind = Step::Kind::AwaitDump;
		await.unit = unit;
		await.index = index;
		await.timeoutMs = 3000.0 + double(messageBytes(unit)) / kUartBytesPerMs * 1.5;
		await.weight = double(messageBytes(unit));
		m_steps.push_back(await);
		m_totalWeight = await.weight;
		return true;
	}

	bool startImport(std::vector<Dump> dumps)
	{
		if (busy() || dumps.empty()) return false;
		reset("Preparing import");
		m_dumps = std::move(dumps);
		bool memory = false, global = false;
		for (const Dump &d : m_dumps)
		{
			memory = memory || storesToMemory(d.unit);
			global = global || carriesGlobal(d.unit);
		}
		if (memory)
		{
			m_steps.push_back(protectStep(false, 170));
			m_steps.push_back(waitStep(150));
			m_steps.push_back(protectStep(false, 171));
			m_steps.push_back(waitStep(150));
		}
		if (global)
		{
			Step req;
			req.kind = Step::Kind::Request;
			req.unit = Unit::Global;
			m_steps.push_back(req);
			Step await;
			await.kind = Step::Kind::AwaitGlobal;
			await.unit = Unit::Global;
			await.timeoutMs = 3000.0;
			m_steps.push_back(await);
		}
		for (std::size_t i = 0; i < m_dumps.size(); ++i)
		{
			Step send;
			send.kind = Step::Kind::SendDump;
			send.dump = i;
			send.weight = double(messageBytes(m_dumps[i].unit));
			m_totalWeight += send.weight;
			m_steps.push_back(send);
			Step ack;
			ack.kind = Step::Kind::AwaitAck;
			ack.dump = i;
			ack.timeoutMs = 4000.0 + double(messageBytes(m_dumps[i].unit)) / kUartBytesPerMs * 0.5;
			m_steps.push_back(ack);
			m_steps.push_back(waitStep(200));
		}
		if (memory)
		{
			m_cleanup.push_back(protectStep(true, 170));
			m_cleanup.push_back(waitStep(150));
			m_cleanup.push_back(protectStep(true, 171));
			m_cleanup.push_back(waitStep(150));
		}
		return true;
	}

	void cancel()
	{
		if (busy() && !m_cancelRequested) m_cancelRequested = true;
	}

	void tick(SysexPort &port, double nowMs)
	{
		if (!busy()) return;
		if (!m_started)
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
			m_started = true;
			port.clearReplies();
		}
		// Cancel between dumps: one already sent is acknowledged first, because the
		// firmware drops SysEx (including the re-protect) while it stores a dump.
		if (m_cancelRequested && !m_streaming && !m_onWire && !m_inCleanup)
		{
			m_failure = State::Cancelled;
			m_failureMessage = "Cancelled";
			enterCleanup();
		}
		for (int guard = 0; guard < 8 && busy(); ++guard)
			if (!advance(port, nowMs)) break;
	}

private:
	struct Step
	{
		enum class Kind { Request, AwaitDump, AwaitGlobal, SendDump, AwaitAck, Send, Wait } kind = Kind::Wait;
		Unit unit = Unit::CurrentProgram;
		int index = -1;
		std::size_t dump = 0;
		int value = 0;                   // Send: global parameter `index` = value
		double timeoutMs = 0.0;          // Await*, or the Wait duration
		double weight = 0.0;             // share of the progress bar
	};

	static Step waitStep(double ms) { Step s; s.kind = Step::Kind::Wait; s.timeoutMs = ms; return s; }
	static Step protectStep(bool on, int param)
	{
		Step s;
		s.kind = Step::Kind::Send;
		s.index = param;
		s.value = on ? 1 : 0;
		return s;
	}

	void reset(const std::string &phase)
	{
		m_status = Status{State::Running, phase, 0.0, {}};
		m_steps.clear();
		m_cleanup.clear();
		m_dumps.clear();
		m_exported.clear();
		m_currentGlobal.clear();
		m_next = 0;
		m_stepStartMs = -1.0;
		m_firstTickMs = -1.0;
		m_doneWeight = 0.0;
		m_totalWeight = 0.0;
		m_started = false;
		m_streaming = false;
		m_onWire = false;
		m_inCleanup = false;
		m_cancelRequested = false;
		m_failure = State::Succeeded;
		m_failureMessage.clear();
		m_out.clear();
		m_outPos = 0;
	}

	void finish(State state, const std::string &message)
	{
		m_status.state = state;
		m_status.message = message;
		m_status.progress = state == State::Succeeded ? 1.0 : m_status.progress;
		m_steps.clear();
		m_cleanup.clear();
	}

	void fail(const std::string &message)
	{
		if (m_inCleanup) return; // keep the first failure
		m_onWire = false;
		m_failure = State::Failed;
		m_failureMessage = message;
		enterCleanup();
	}

	void enterCleanup()
	{
		m_inCleanup = true;
		m_steps = std::move(m_cleanup);
		m_cleanup.clear();
		m_next = 0;
		m_stepStartMs = -1.0;
		if (!m_steps.empty()) m_status.phase = "Restoring memory protect";
	}

	void stepDone(double weight)
	{
		m_doneWeight += weight;
		++m_next;
		m_stepStartMs = -1.0;
	}

	void setProgress(double partial)
	{
		if (m_totalWeight > 0.0)
			m_status.progress = std::clamp((m_doneWeight + partial) / m_totalWeight, 0.0, 0.99);
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

	// Run the current step; false when it must wait for a later tick.
	bool advance(SysexPort &port, double nowMs)
	{
		if (m_next >= m_steps.size())
		{
			if (!m_inCleanup && !m_cleanup.empty()) { enterCleanup(); return true; }
			if (m_failure != State::Succeeded) finish(m_failure, m_failureMessage);
			else finish(State::Succeeded, m_exported.empty() ? "Import complete" : "Export complete");
			return false;
		}
		Step &step = m_steps[m_next];
		if (m_stepStartMs < 0)
		{
			m_stepStartMs = nowMs;
			m_out.clear();
			m_outPos = 0;
			switch (step.kind)
			{
				case Step::Kind::Request: m_out = request(step.unit, step.index, port.channel()); break;
				case Step::Kind::Send:
					m_out = globalParameter(step.index, step.value, port.channel());
					break;
				case Step::Kind::SendDump:
				{
					Dump &dump = m_dumps[step.dump];
					preserveGlobalMidi(dump, m_currentGlobal);
					m_out = build(dump, port.channel());
					m_status.phase = "Sending " + label(dump);
					break;
				}
				case Step::Kind::AwaitDump: m_status.phase = "Receiving " + label(Dump{step.unit, step.index, {}}); break;
				case Step::Kind::AwaitAck: break;
				case Step::Kind::AwaitGlobal: m_status.phase = "Reading current global settings"; break;
				case Step::Kind::Wait: break;
			}
		}
		const double elapsed = nowMs - m_stepStartMs;
		switch (step.kind)
		{
			case Step::Kind::Request:
			case Step::Kind::Send:
				if (!stream(port)) return false;
				stepDone(0.0);
				return true;
			case Step::Kind::SendDump:
				setProgress(std::min(step.weight, elapsed * kUartBytesPerMs));
				if (!stream(port)) return false;
				// The stream has handed the dump to the UART queue; it is still on the wire.
				m_sendRemainingMs = std::max(0.0, double(m_out.size()) / kUartBytesPerMs - elapsed);
				stepDone(0.0);
				m_onWire = true;
				m_wireStartMs = nowMs;
				m_wireWeight = step.weight;
				m_wireStartedWeight = std::min(step.weight, elapsed * kUartBytesPerMs);
				return true;
			case Step::Kind::AwaitDump:
			case Step::Kind::AwaitGlobal:
			{
				std::vector<std::uint8_t> reply;
				while (port.popReply(reply))
				{
					if (!matches(reply, step)) continue;
					if (step.kind == Step::Kind::AwaitGlobal)
					{
						std::optional<Dump> current = parse(reply);
						if (!current) { fail("The synth sent an unreadable global dump."); return true; }
						m_currentGlobal = current->raw;
					}
					else m_exported = std::move(reply);
					stepDone(step.weight);
					return true;
				}
				if (step.kind == Step::Kind::AwaitDump)
					setProgress(std::min(step.weight * 0.99, elapsed * kUartBytesPerMs));
				if (elapsed > step.timeoutMs)
				{
					fail("The synth did not send the requested data.");
					return true;
				}
				return false;
			}
			case Step::Kind::AwaitAck:
			{
				const double wire = m_wireStartedWeight + (nowMs - m_wireStartMs) * kUartBytesPerMs;
				setProgress(std::min(m_wireWeight * 0.99, wire));
				std::vector<std::uint8_t> reply;
				while (port.popReply(reply))
				{
					if (reply.size() < 6) continue;
					const std::uint8_t f = reply[4];
					if (f == 0x23) { m_onWire = false; m_doneWeight += m_wireWeight; stepDone(0.0); return true; }
					if (f == 0x24)
					{
						const int code = reply.size() > 6 ? reply[5] & 7 : 0;
						fail(code == 1 ? "The synth refused the data: program memory is protected."
							: code == 2 ? "The synth refused the data: pattern memory is protected."
							: "The synth reported a data load error.");
						return true;
					}
					if (f == 0x26) { fail("The synth rejected the data as badly formatted."); return true; }
				}
				if (elapsed > step.timeoutMs + m_sendRemainingMs)
				{
					fail("The synth did not confirm the load.");
					return true;
				}
				return false;
			}
			case Step::Kind::Wait:
				if (elapsed < step.timeoutMs) return false;
				stepDone(0.0);
				return true;
		}
		return false;
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
	std::vector<Step> m_steps, m_cleanup;
	std::vector<Dump> m_dumps;
	std::vector<std::uint8_t> m_exported, m_currentGlobal, m_out;
	std::size_t m_next = 0, m_outPos = 0;
	double m_stepStartMs = -1.0, m_firstTickMs = -1.0;
	double m_doneWeight = 0.0, m_totalWeight = 0.0;
	double m_sendRemainingMs = 0.0, m_wireStartMs = 0.0, m_wireWeight = 0.0, m_wireStartedWeight = 0.0;
	bool m_started = false, m_streaming = false, m_onWire = false, m_inCleanup = false;
	bool m_cancelRequested = false;
	State m_failure = State::Succeeded;
	std::string m_failureMessage;
};

} // namespace prophecy::sysex
