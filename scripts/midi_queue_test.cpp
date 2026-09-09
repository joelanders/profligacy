// SPDX-License-Identifier: AGPL-3.0-only
#include "midi_queue.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

static void require(bool value, const char* message)
{
	if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main()
{
	prophecy::MidiQueue<16> queue;
	const std::array<std::uint8_t, 8> sysex{0xf0, 0x42, 0x30, 0x41, 0x10, 0, 0, 0xf7};
	const std::array<std::uint8_t, 3> note{0x90, 60, 64};
	std::array<std::uint8_t, 32> received{};
	for (int wrap = 0; wrap < 100; ++wrap)
	{
		require(queue.push(sysex.data(), sysex.size(), 0, 1), "enqueue old program command");
		require(queue.push(note.data(), note.size(), 20), "enqueue musical event");
		require(queue.popDue(received.data(), received.size(), 19, 2) == 0,
			"obsolete command survived replacement or future MIDI escaped");
		require(queue.popDue(received.data(), received.size(), 20, 2) == note.size()
			&& std::equal(note.begin(), note.end(), received.begin()), "replacement discarded musical MIDI");

		require(queue.push(sysex.data(), sysex.size(), 0, 2), "enqueue multi-pop SysEx");
		require(queue.popDue(received.data(), 3, 0, 2) == 3, "start SysEx");
		require(queue.popDue(received.data() + 3, received.size() - 3, 0, 3) == 5
			&& std::equal(sysex.begin(), sysex.end(), received.begin()), "replacement tore an in-progress SysEx");
	}

	require(!queue.push(received.data(), 17, 0) && queue.dropped() == 17,
		"oversize message must be rejected whole");
	require(queue.popDue(received.data(), received.size(), 0) == 0, "rejection published a partial message");
	require(queue.push(sysex.data(), sysex.size(), 0), "enqueue first capacity message");
	require(queue.push(sysex.data(), sysex.size(), 0), "enqueue second capacity message");
	require(!queue.push(note.data(), note.size(), 0) && queue.dropped() == 20, "capacity guard");
	queue.discard();
	require(queue.popDue(received.data(), received.size(), 0) == 0, "discard retained data");

	prophecy::MidiQueue<16> scheduled;
	require(scheduled.push(sysex.data(), sysex.size(), 0), "queue host SysEx");
	require(prophecy::popMidiInput(queue, scheduled, received.data(), 3, 0, 1) == 3,
		"start host SysEx across pop boundaries");
	require(queue.push(note.data(), note.size(), 0), "UI arrives during host SysEx");
	require(prophecy::popMidiInput(queue, scheduled, received.data() + 3, 29, 0, 1) == 5
		&& std::equal(sysex.begin(), sysex.end(), received.begin()), "UI interrupted host SysEx");
	require(prophecy::popMidiInput(queue, scheduled, received.data(), received.size(), 0, 1) == 3
		&& std::equal(note.begin(), note.end(), received.begin()), "UI packet lost after host SysEx");
	std::puts("MIDI queue: revisions, complete messages, scheduling, wrapping and capacity passed");
	const std::array<std::uint8_t, 2> program{0xc0, 9};
	require(queue.push(sysex.data(), sysex.size(), 0, 1), "held editor packet");
	require(scheduled.push(program.data(), program.size(), 29), "timed program");
	require(scheduled.push(note.data(), note.size(), 97), "following timed note");
	require(prophecy::popMidiInput(queue, scheduled, received.data(), received.size(), 28, 1, false) == 0,
		"control hold changed MIDI due time");
	require(prophecy::popMidiInput(queue, scheduled, received.data(), received.size(), 29, 1, false) == 2
		&& std::equal(program.begin(), program.end(), received.begin()), "program was held behind editor work");
	require(prophecy::popMidiInput(queue, scheduled, received.data(), received.size(), 97, 1, false) == 3
		&& std::equal(note.begin(), note.end(), received.begin()), "following note changed order");
	require(prophecy::popMidiInput(queue, scheduled, received.data(), received.size(), 97, 2) == 0,
		"obsolete editor packet survived ownership acknowledgement");
	require(queue.push(sysex.data(), sysex.size(), 0, 2), "started editor packet");
	require(prophecy::popMidiInput(queue, scheduled, received.data(), 3, 97, 2) == 3, "start editor packet");
	require(prophecy::popMidiInput(queue, scheduled, received.data() + 3, 29, 97, 2, false) == 5
		&& std::equal(sysex.begin(), sysex.end(), received.begin()), "control hold tore an editor packet");
	std::puts("MIDI control hold: original note/program due times and complete packets preserved");
}
