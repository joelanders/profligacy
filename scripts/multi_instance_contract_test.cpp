// SPDX-License-Identifier: AGPL-3.0-only
//
// Deterministic, no-ROM test for ProphecyEngine's v1 process-instance contract.
// This deliberately tests synchronous ownership state rather than racing two MAME
// boots or inferring ownership from audio arrival. It documents that lifecycle-safe
// construction of a second engine is not independent multi-instance synthesis.

#include "prophecy_engine.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

int fail(const char *message)
{
	std::fprintf(stderr, "FAIL multi_instance_contract: %s\n", message);
	return 1;
}

} // anonymous namespace

int main()
{
	// A nonexistent system makes the owner worker return without needing copyrighted
	// ROM assets. Slot acquisition itself is synchronous, before that worker starts.
	const std::vector<std::string> args = {
		"prophecy", "__prophecy_multi_instance_contract_probe__",
		"-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo"
	};

	ProphecyEngine first;
	ProphecyEngine second;
	if (!first.start(args))
		return fail("first start was rejected");
	if (!second.enableMidiTxByteCapture(true))
		return fail("second engine could not stage its private pre-start configuration");
	if (second.start(args))
		return fail("second start hid the occupied process slot");
	if (!first.ownsMachineSlot())
		return fail("first engine did not claim the MAME process slot");
	if (second.ownsMachineSlot())
		return fail("second engine unexpectedly claimed the occupied MAME process slot");
	if (second.instanceStatus() != ProphecyEngine::InstanceStatus::Unavailable)
		return fail("second engine did not report unavailable status");
	if (second.running())
		return fail("second engine without a worker reported running");
	if (second.producedFrames() != 0 || second.available() != 0)
		return fail("non-owner exposed audio it cannot own");

	float left[8] = {}, right[8] = {};
	if (second.pull(left, right, 8) != 0)
		return fail("non-owner delivered audio");
	const std::uint8_t note_on[3] = { 0x90, 60, 100 };
	if (second.pushMidi(note_on, sizeof(note_on)))
		return fail("non-owner accepted immediate MIDI for the active instance");
	if (second.pushMidiAtFrame(note_on, sizeof(note_on), 0))
		return fail("non-owner accepted scheduled MIDI for the active instance");
	if (second.pushAdin(1, 127) || second.pushAdinFromAudio(1, 127))
		return fail("non-owner accepted panel analog input for the active instance");
	std::uint8_t leds[12];
	std::fill_n(leds, 12, std::uint8_t(0xff));
	if (second.ledSnapshot(leds) != 0)
		return fail("non-owner exposed the active instance's LED state");
	for (const std::uint8_t led : leds)
		if (led != 0) return fail("non-owner did not clear unavailable LED output");
	std::uint8_t raw1[40], raw2[40], cgram[64];
	std::fill_n(raw1, 40, std::uint8_t(0xff));
	std::fill_n(raw2, 40, std::uint8_t(0xff));
	std::fill_n(cgram, 64, std::uint8_t(0xff));
	if (second.lcdRawSnapshot(raw1, raw2, cgram) != 0)
		return fail("non-owner exposed the active instance's raw LCD state");
	for (const std::uint8_t byte : raw1) if (byte != 0) return fail("unavailable raw LCD row 1 not cleared");
	for (const std::uint8_t byte : raw2) if (byte != 0) return fail("unavailable raw LCD row 2 not cleared");
	for (const std::uint8_t byte : cgram) if (byte != 0) return fail("unavailable LCD CGRAM not cleared");
	char line1[41] = "sentinel", line2[41] = "sentinel";
	if (second.latestLcd(line1, line2, sizeof(line1)) || line1[0] != '\0' || line2[0] != '\0')
		return fail("non-owner exposed the active instance's LCD state");
	std::uint8_t dump[1024] = {};
	std::uint32_t version = 99;
	if (second.latestProgramData(dump, sizeof(dump), &version) != 0 || version != 0)
		return fail("non-owner exposed the active instance's program dump");
	int pattern = 99;
	version = 99;
	if (second.latestArpeggioPatternData(dump, sizeof(dump), &version, &pattern) != 0
			|| version != 0 || pattern != -1)
		return fail("non-owner exposed the active instance's arpeggio dump");
	ProphecyEngine::MidiTxByteEvent event;
	if (second.popMidiTx(dump, sizeof(dump)) != 0
			|| second.popMidiTxByteEvents(&event, 1) != 0)
		return fail("non-owner drained the active instance's MIDI output");

	// Stopping a non-owner must not release the first engine's slot. Once the actual
	// owner stops, a newly constructed engine can claim it; an existing silent engine
	// is never promoted.
	second.stop();
	if (!first.ownsMachineSlot())
		return fail("stopping non-owner released the owner's slot");
	first.stop();

	ProphecyEngine replacement;
	if (!replacement.start(args) || !replacement.ownsMachineSlot())
		return fail("slot was not reusable after owner teardown");
	replacement.stop();

	std::fprintf(stderr,
		"PASS multi_instance_contract: second start explicit and inert; slot reusable after teardown\n");
	return 0;
}
