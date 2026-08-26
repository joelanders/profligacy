// SPDX-License-Identifier: AGPL-3.0-only
#include "led_store.h"

#include <array>
#include <cstdio>

namespace {

bool expect(bool condition, const char *message)
{
	if (condition) return true;
	std::fprintf(stderr, "led_store_test: %s\n", message);
	return false;
}

} // namespace

int main()
{
	LedStore store;
	std::array<std::uint8_t, LedStore::kBankCount> banks {};
	bool ok = true;

	ok &= expect(store.snapshot(banks.data()) == 0, "new store has version zero");
	ok &= expect(banks[10] == 0, "new store is dark");

	// Model a 15 ms SPEED LED flash entirely between two 33 ms UI polls.
	store.set(10, 0x01);
	store.set(10, 0x00);
	ok &= expect(store.snapshot(banks.data()) == 2 && banks[10] == 0,
		"electrical observer sees the current off state");
	ok &= expect(store.visualSnapshot(banks.data()) == 2 && banks[10] == 0x01,
		"visual observer sees the intervening rising edge");
	ok &= expect(store.visualSnapshot(banks.data()) == 2 && banks[10] == 0,
		"visual latch lasts exactly one poll");

	store.set(3, 0x24);
	store.visualSnapshot(banks.data());
	ok &= expect(banks[3] == 0x24, "steady LEDs remain illuminated");
	store.visualSnapshot(banks.data());
	ok &= expect(banks[3] == 0x24, "steady LEDs do not depend on the latch");

	store.reset();
	ok &= expect(store.snapshot(banks.data()) == 0 && banks[3] == 0,
		"reset clears state and version");
	ok &= expect(store.visualSnapshot(banks.data()) == 0 && banks[10] == 0,
		"reset also clears pending visual edges");

	return ok ? 0 : 1;
}
