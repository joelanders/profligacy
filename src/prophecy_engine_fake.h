// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// Deterministic test control, implemented only by the no-ROM fake engine.
namespace prophecy::fake {
void holdInitialization(bool hold);
bool initializationWaiting();
void holdImmediateInput(bool hold);
bool immediateInputWaiting();
void holdReadbackCompletion(bool hold);
void rejectReadbacks(bool reject);
void rejectSnapshots(bool reject);
void holdWriteCleanup(bool hold);
bool writeCleanupWaiting();
void rejectWrites(bool reject);
bool memoryProtected();
}
