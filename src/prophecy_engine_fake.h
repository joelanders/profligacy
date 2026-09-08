// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// Deterministic test control, implemented only by the no-ROM fake engine.
namespace prophecy::fake {
void holdInitialization(bool hold);
bool initializationWaiting();
}
