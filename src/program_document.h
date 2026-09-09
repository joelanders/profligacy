// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "program_parameter_limits.h"

namespace prophecy {

// An edit is an operation, not a byte offset. OSC Set and the Performance Editor
// knobs can change other fields; replay must start from the saved base and retain
// operation order. Only a confirmed prefix may be folded into a new base.
struct ProgramEdit
{
	std::uint16_t parameter = 0;
	std::int16_t value = 0;

	bool valid() const { return parameter > 0 && parameter <= 0x3fff && value >= -8192 && value <= 8191; }
	bool supported() const { return valid() && supportedProgramParameter(parameter, value); }
	static std::optional<ProgramEdit> fromMidi(const std::uint8_t* bytes, std::size_t size)
	{
		if (!bytes || size != 11 || bytes[0] != 0xf0 || bytes[1] != 0x42
			|| (bytes[2] & 0xf0) != 0x30 || bytes[3] != 0x41 || bytes[4] != 0x41
			|| bytes[5] != 1 || bytes[10] != 0xf7
			|| std::any_of(bytes + 6, bytes + 10, [](auto byte) { return byte > 127; })) return {};
		const int value = bytes[8] | (bytes[9] << 7);
		const ProgramEdit edit{std::uint16_t(bytes[6] | (bytes[7] << 7)),
			static_cast<std::int16_t>(value >= 8192 ? value - 16384 : value)};
		return edit.valid() ? std::optional<ProgramEdit>{edit} : std::nullopt;
	}
	std::array<std::uint8_t, 11> midi(std::uint8_t channel = 0) const
	{
		const auto encoded = static_cast<std::uint16_t>(value) & 0x3fff;
		return {0xf0, 0x42, std::uint8_t(0x30 | (channel & 15)), 0x41, 0x41, 1,
			std::uint8_t(parameter & 127), std::uint8_t(parameter >> 7),
			std::uint8_t(encoded & 127), std::uint8_t(encoded >> 7), 0xf7};
	}
};

struct ProgramDocument
{
	static constexpr std::size_t programBytes = 535;
	static constexpr std::size_t maxEdits = 4096;
	std::array<std::uint8_t, programBytes> base{};
	std::vector<ProgramEdit> edits;

	bool append(ProgramEdit edit)
	{
		if (!edit.valid() || edits.size() >= maxEdits) return false;
		edits.push_back(edit);
		return true;
	}

	std::vector<std::uint8_t> programMidi() const
	{
		std::vector<std::uint8_t> out{0xf0, 0x42, 0x30, 0x41, 0x40, 1};
		for (std::size_t first = 0; first < base.size(); first += 7)
		{
			const auto count = std::min<std::size_t>(7, base.size() - first);
			std::uint8_t high = 0;
			for (std::size_t i = 0; i < count; ++i) high |= (base[first + i] >> 7) << i;
			out.push_back(high);
			for (std::size_t i = 0; i < count; ++i) out.push_back(base[first + i] & 127);
		}
		out.push_back(0xf7);
		return out;
	}

	static std::optional<ProgramDocument> fromMidi(const std::uint8_t* bytes, std::size_t size)
	{
		constexpr auto packedBytes = programBytes + (programBytes + 6) / 7;
		if (!bytes || size != packedBytes + 7 || bytes[0] != 0xf0 || bytes[1] != 0x42
			|| (bytes[2] & 0xf0) != 0x30 || bytes[3] != 0x41 || bytes[4] != 0x40
			|| bytes[5] != 1 || bytes[size - 1] != 0xf7) return {};
		if (std::any_of(bytes + 6, bytes + size - 1, [](auto byte) { return byte > 127; })) return {};
		ProgramDocument document;
		std::size_t position = 6;
		for (std::size_t first = 0; first < programBytes; first += 7)
		{
			const auto high = bytes[position++];
			const auto count = std::min<std::size_t>(7, programBytes - first);
			if ((high >> count) != 0) return {};
			for (std::size_t i = 0; i < count; ++i)
				document.base[first + i] = std::uint8_t(bytes[position++] | (((high >> i) & 1) << 7));
		}
		return document;
	}

	// Fixed-width little-endian fields; no padding, pointers or native struct data.
	std::vector<std::uint8_t> encode() const
	{
		if (edits.size() > maxEdits) return {};
		std::vector<std::uint8_t> out{'P', 'G', 'I', '1'};
		out.insert(out.end(), base.begin(), base.end());
		out.push_back(std::uint8_t(edits.size() & 255));
		out.push_back(std::uint8_t(edits.size() >> 8));
		for (const auto edit : edits)
		{
			if (!edit.valid()) return {};
			out.push_back(std::uint8_t(edit.parameter & 255));
			out.push_back(std::uint8_t(edit.parameter >> 8));
			const auto value = static_cast<std::uint16_t>(edit.value) & 0x3fff;
			out.push_back(std::uint8_t(value & 255));
			out.push_back(std::uint8_t(value >> 8));
		}
		return out;
	}

	static std::optional<ProgramDocument> decode(const std::uint8_t* bytes, std::size_t size)
	{
		constexpr auto header = 4 + programBytes + 2;
		if (!bytes || size < header || bytes[0] != 'P' || bytes[1] != 'G'
			|| bytes[2] != 'I' || bytes[3] != '1') return {};
		const auto count = std::size_t(bytes[header - 2]) | (std::size_t(bytes[header - 1]) << 8);
		if (count > maxEdits || size != header + count * 4) return {};
		ProgramDocument document;
		std::copy_n(bytes + 4, programBytes, document.base.begin());
		for (auto i = header; i < size; i += 4)
		{
			const auto parameter = std::uint16_t(bytes[i] | (bytes[i + 1] << 8));
			const int value = bytes[i + 2] | (bytes[i + 3] << 8);
			if (value > 0x3fff || !document.append({parameter,
				static_cast<std::int16_t>(value >= 8192 ? value - 16384 : value)})) return {};
		}
		return document;
	}
};

}
