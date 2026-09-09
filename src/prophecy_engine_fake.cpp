// SPDX-License-Identifier: AGPL-3.0-only
// Deterministic host stub used to validate the JUCE/Windows shell independently
// of MAME's executable-oriented OSD implementation.

#include "prophecy_engine.h"
#include "program_midi.h"
#include "prophecy_engine_fake.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace {
std::mutex initializationMutex;
std::condition_variable initializationReady;
bool initializationHeld = false;
std::atomic<bool> initializationIsWaiting{false};
std::mutex inputMutex;
std::condition_variable inputReady;
bool inputHeld = false;
std::atomic<bool> inputIsWaiting{false};
std::atomic<bool> readbackHeld{false};
std::atomic<bool> readbackRejected{false};
std::atomic<bool> snapshotRejected{false};
std::mutex writeMutex;
std::condition_variable writeReady;
bool writeHeld = false;
std::atomic<bool> writeIsWaiting{false}, writeRejected{false}, protection{true};
}

void prophecy::fake::holdInitialization(bool hold)
{
	std::lock_guard lock(initializationMutex);
	initializationHeld = hold;
	initializationReady.notify_all();
}

bool prophecy::fake::initializationWaiting() { return initializationIsWaiting.load(); }

void prophecy::fake::holdImmediateInput(bool hold)
{
	std::lock_guard lock(inputMutex);
	inputHeld = hold;
	inputReady.notify_all();
}

bool prophecy::fake::immediateInputWaiting() { return inputIsWaiting.load(); }
void prophecy::fake::holdReadbackCompletion(bool hold) { readbackHeld.store(hold); }
void prophecy::fake::rejectReadbacks(bool reject) { readbackRejected.store(reject); }
void prophecy::fake::rejectSnapshots(bool reject) { snapshotRejected.store(reject); }
void prophecy::fake::holdWriteCleanup(bool hold)
{
	std::lock_guard lock(writeMutex);
	writeHeld = hold;
	writeReady.notify_all();
}
bool prophecy::fake::writeCleanupWaiting() { return writeIsWaiting.load(); }
void prophecy::fake::rejectWrites(bool reject) { writeRejected.store(reject); }
bool prophecy::fake::memoryProtected() { return protection.load(); }

struct ProphecyEngine::Impl
{
	std::atomic<bool> started{false};
	std::atomic<bool> finished{false};
	std::atomic<bool> owns{false};
	std::atomic<std::uint64_t> produced{0};
	std::atomic<std::uint64_t> requested{0};
	std::atomic<bool> ready{false};
	std::atomic<std::uint64_t> origin{0};
	mutable std::mutex programMutex;
	std::vector<std::uint8_t> program; // cached MIDI dump only
	std::vector<std::uint8_t> firmware;
	std::array<std::uint8_t, 574> globals{};
	prophecy::ProgramMidiRouting routing;
	std::array<std::vector<std::uint8_t>, 128> storedPrograms;
	std::atomic<std::uint64_t> revision{1};
	std::atomic<std::uint64_t> programInputPending{0}, programInputComplete{0};
	std::uint64_t readbackTicket = 0, nextReadback = 0;
	std::uint64_t readbackDoneFrame = UINT64_MAX;
	std::uint32_t readbackVersion = 0;
	bool readbackAbandoned = false;
	bool readbackComplete() const { return requested.load() >= readbackDoneFrame && !readbackHeld.load(); }
	struct Command { std::vector<std::uint8_t> bytes; std::uint64_t frame; std::uint64_t revision; };
	std::deque<Command> immediate;
	struct Scheduled { std::array<std::uint8_t, 1024> bytes{}; std::size_t size = 0; std::uint64_t frame = 0; };
	std::array<Scheduled, 64> scheduled{};
	std::size_t scheduledCount = 0;
	void cacheProgram() { program = firmware; ++programVersion; }
	void setGlobal(int parameter, int value)
	{
		if (parameter == 184) globals[176] = std::uint8_t(value & 15);
		else if (parameter >= 185 && parameter <= 191)
		{
			const auto offset = std::size_t(parameter <= 187 ? 177 : parameter <= 189 ? 178 : 179);
			const auto bit = unsigned(parameter - (parameter <= 187 ? 185 : parameter <= 189 ? 188 : 190));
			globals[offset] = std::uint8_t((globals[offset] & ~(1u << bit)) | ((value & 1u) << bit));
		}
		else if (parameter >= 192 && parameter <= 197)
			globals[std::size_t(180 + ((parameter - 192) ^ 1))] = std::uint8_t(value);
		else if (parameter >= 198 && parameter <= 389)
			globals[std::size_t(parameter - 198 + 186)] = std::uint8_t(value & 127);
		else if (parameter == 170) protection.store(value != 0);
	}
	void selectProgram(int index)
	{
		if (index < 0 || index >= 128) return;
		const auto& stored = storedPrograms[std::size_t(index)];
		firmware = stored.empty() ? std::vector<std::uint8_t>(535, std::uint8_t(index)) : stored;
	}
	void apply(const std::uint8_t* bytes, std::size_t n)
	{
		if (n == 13 && bytes[4] == 0x10 && bytes[8] == 0x7e)
		{
			// Publish the dump before its identity reply, just like the serial
			// firmware. Tests can hold that final reply while the version changes.
			if (!readbackRejected.load()) cacheProgram();
			readbackDoneFrame = requested.load() + 9600;
			return;
		}
		(void)routing.reset({globals.begin(), globals.end()});
		const bool addressedSysex = routing.sysex(bytes, n);
		const auto target = routing.receive(bytes, n);
		if (target >= 0) selectProgram(target);
		else if (n == 8 && bytes[6] == 0xc0)
		{
			apply(bytes, 3);
			apply(bytes + 3, 3);
			apply(bytes + 6, 2);
		}
		else if (n == 11 && addressedSysex && bytes[4] == 0x41 && bytes[5] == 1)
		{
			const auto parameter = bytes[6] | (bytes[7] << 7);
			if (parameter >= 1 && parameter <= 16) firmware[std::size_t(parameter - 1)] = bytes[8];
		}
		else if (n == 11 && addressedSysex && bytes[4] == 0x41 && bytes[5] == 0)
			setGlobal(bytes[6] | (bytes[7] << 7), bytes[8] | (bytes[9] << 7));
		else if (routing.sysex(bytes, n)) loadProgram(bytes, n);
		if (n == 7 && bytes[0] == 0xf0 && bytes[4] == 0x10) cacheProgram();
	}
	void advance(std::uint64_t horizon)
	{
		std::lock_guard lock(programMutex);
		while (programInputPending.load() == programInputComplete.load()
			&& !immediate.empty() && immediate.front().frame <= horizon)
		{
			const auto& command = immediate.front();
			if (command.revision == 0 || command.revision == revision.load())
				apply(command.bytes.data(), command.bytes.size());
			immediate.pop_front();
		}
		std::size_t consumed = 0;
		while (consumed < scheduledCount && scheduled[consumed].frame <= horizon)
		{
			apply(scheduled[consumed].bytes.data(), scheduled[consumed].size);
			++consumed;
		}
		std::move(scheduled.begin() + (std::ptrdiff_t)consumed,
			scheduled.begin() + (std::ptrdiff_t)scheduledCount, scheduled.begin());
		scheduledCount -= consumed;
	}
	std::uint32_t programVersion = 0;
	std::atomic<std::uint64_t> firmwareErrors{0};
	void loadProgram(const std::uint8_t* state, std::size_t bytes)
	{
		if (bytes < 8 || state[0] != 0xf0 || state[4] != 0x40 || state[bytes - 1] != 0xf7) return;
		firmware.clear();
		for (std::size_t i = 6; i + 1 < bytes; )
		{
			const auto high = state[i++];
			for (int bit = 0; bit < 7 && i + 1 < bytes; ++bit)
				firmware.push_back(std::uint8_t(state[i++] | (((high >> bit) & 1) << 7)));
		}
	}
	const bool probe = std::getenv("PROPHECY_FAKE_TIMELINE_PROBE") != nullptr;
	std::array<std::uint64_t, 128> notes{};
	std::size_t note_count = 0;
	std::atomic<std::uint64_t> dropped_immediate{0};
	std::atomic<std::uint64_t> dropped_scheduled{0};
	std::atomic<std::uint64_t> dropped_ui_adin{0};
	std::atomic<std::uint64_t> dropped_audio_adin{0};
	std::atomic<std::uint64_t> dropped_scheduled_panel{0};
	std::atomic<std::uint64_t> dropped_scheduled_adin{0};
	std::atomic<std::uint32_t> lcd_version{0};
};

ProphecyEngine::ProphecyEngine() : m_impl(std::make_unique<Impl>()) { }
ProphecyEngine::~ProphecyEngine() { stop(); }

bool ProphecyEngine::start(const std::vector<std::string> &)
{
	bool expected = false;
	if (!m_impl->started.compare_exchange_strong(expected, true)) return false;
	m_impl->owns.store(true);
	m_impl->finished.store(false);
	protection.store(true);
	m_impl->globals[177] = 1;
	m_impl->globals[178] = 2;
	m_impl->globals[179] = 3;
	for (std::size_t cc = 0; cc < 96; ++cc)
	{
		m_impl->globals[382 + cc * 2] = 3;
		m_impl->globals[383 + cc * 2] = std::uint8_t(cc + 2);
	}
	for (int bank = 0; bank < 3; ++bank)
	{
		m_impl->globals[std::size_t(180 + bank * 2)] = std::uint8_t(bank);
		for (int program = 0; program < 64; ++program)
			m_impl->globals[std::size_t(186 + bank * 64 + program)] = std::uint8_t(program);
	}
	return true;
}

bool ProphecyEngine::enableMidiTxByteCapture(bool) { return !m_impl->started.load(); }
bool ProphecyEngine::enableHostTimeline() { return !m_impl->started.load(); }
bool ProphecyEngine::initializePlayback(const std::uint8_t* state, std::size_t bytes, bool firmwareHandshake,
	const std::vector<prophecy::ProgramEdit>& edits)
{
	if (!running()) return false;
	if (!edits.empty() && (!state || !bytes || !firmwareHandshake
		|| edits.size() > prophecy::ProgramDocument::maxEdits
		|| std::any_of(edits.begin(), edits.end(), [](const auto& edit) { return !edit.supported(); })))
		return false;
	m_impl->ready.store(false);
	{
		std::unique_lock lock(initializationMutex);
		initializationIsWaiting.store(true);
		initializationReady.wait(lock, [] { return !initializationHeld; });
		initializationIsWaiting.store(false);
	}
	{
		std::lock_guard lock(m_impl->programMutex);
		if (state && bytes) m_impl->loadProgram(state, bytes);
		else if (m_impl->programVersion == 0)
		{
			m_impl->firmware.assign(535, 0);
		}
		for (const auto& edit : edits)
		{
			const auto message = edit.midi(m_impl->globals[176]);
			m_impl->apply(message.data(), message.size());
		}
		m_impl->cacheProgram();
		m_impl->immediate.clear();
		m_impl->scheduledCount = 0;
		m_impl->readbackTicket = 0;
		m_impl->readbackDoneFrame = UINT64_MAX;
	}
	if (state && bytes)
	{
		// Model initialization consuming native time outside the playback epoch.
		m_impl->requested.fetch_add(256);
		m_impl->origin.store(m_impl->requested.load());
		m_impl->note_count = 0;
	}
	m_impl->ready.store(true);
	return true;
}
const char* ProphecyEngine::initializationError() const { return ""; }
bool ProphecyEngine::storeProgram(int destination, const prophecy::ProgramDocument& document)
{
	if (destination < 0 || destination >= 128 || !readyForPlayback()) return false;
	const auto midi = document.programMidi();
	if (!initializePlayback(midi.data(), midi.size(), true, document.edits)) return false;
	m_impl->ready.store(false);
	const bool original = protection.exchange(false);
	const bool written = !writeRejected.load();
	if (written)
	{
		std::lock_guard lock(m_impl->programMutex);
		m_impl->storedPrograms[std::size_t(destination)] = m_impl->firmware;
	}
	{
		std::unique_lock lock(writeMutex);
		writeIsWaiting.store(true);
		writeReady.wait(lock, [] { return !writeHeld; });
		protection.store(original);
		writeIsWaiting.store(false);
	}
	m_impl->ready.store(true);
	return written;
}
bool ProphecyEngine::readyForPlayback() const { return running() && m_impl->ready.load(); }
std::uint64_t ProphecyEngine::playbackOrigin() const { return m_impl->origin.load(); }
bool ProphecyEngine::waitingForOutput() const { return false; }
bool ProphecyEngine::waitingForInput() const { return false; }
void ProphecyEngine::setHostBlockFrames(std::uint32_t) {}
void ProphecyEngine::requestThroughFrame(std::uint64_t frame) { m_impl->requested.store(frame); m_impl->advance(frame); }
std::uint64_t ProphecyEngine::requestedFrames() const { return m_impl->requested.load(); }
std::size_t ProphecyEngine::readAtFrame(std::uint64_t first, float* left, float* right,
	std::size_t frames, bool)
{
	std::fill(left, left + frames, 0.0f);
	std::fill(right, right + frames, 0.0f);
	if (!running()) return 0;
	const auto horizon = requestedFrames() / kAudioQuantum * kAudioQuantum;
	const auto count = horizon > first
		? (std::size_t) std::min<std::uint64_t>(horizon - first, frames) : 0;
	if (m_impl->probe)
		for (std::size_t i = 0; i < count; ++i)
			for (std::size_t note = 0; note < m_impl->note_count; ++note)
				if (first + i >= m_impl->notes[note] && first + i < m_impl->notes[note] + 32)
				{
					left[i] = 0.5f;
					right[i] = -0.25f;
				}
	m_impl->produced.store(horizon);
	return count;
}

void ProphecyEngine::stop()
{
	if (m_impl->started.exchange(false))
	{
		m_impl->owns.store(false);
		m_impl->finished.store(true);
	}
}

bool ProphecyEngine::running() const { return m_impl->started.load() && !m_impl->finished.load(); }
bool ProphecyEngine::finished() const { return m_impl->finished.load(); }
bool ProphecyEngine::ownsMachineSlot() const { return m_impl->owns.load(); }
ProphecyEngine::InstanceStatus ProphecyEngine::instanceStatus() const
{
	if (running()) return InstanceStatus::Active;
	if (m_impl->finished.load()) return InstanceStatus::Stopped;
	return InstanceStatus::NotStarted;
}

std::size_t ProphecyEngine::pull(float *left, float *right, std::size_t frames)
{
	if (!running()) return 0;
	if (left) std::fill(left, left + frames, 0.0f);
	if (right) std::fill(right, right + frames, 0.0f);
	m_impl->produced.fetch_add(frames);
	return frames;
}

bool ProphecyEngine::pushMidi(const std::uint8_t* bytes, std::size_t n, std::uint64_t revision)
{
	if (!readyForPlayback()) { m_impl->dropped_immediate.fetch_add(n); return false; }
	{
		std::unique_lock lock(inputMutex);
		inputIsWaiting.store(true);
		inputReady.wait(lock, [] { return !inputHeld; });
		inputIsWaiting.store(false);
	}
	std::lock_guard lock(m_impl->programMutex);
	m_impl->immediate.push_back({{bytes, bytes + n}, requestedFrames() + kAudioQuantum, revision});
	return true;
}
bool ProphecyEngine::pushMidiAtFrame(const std::uint8_t *bytes, std::size_t n, std::uint64_t frame)
{
	if (!running()) { m_impl->dropped_scheduled.fetch_add(n); return false; }
	if ((n == 2 && (bytes[0] & 0xf0) == 0xc0) || (n > 0 && bytes[0] == 0xf0))
	{
		if (m_impl->scheduledCount == m_impl->scheduled.size() || n > 1024) return false;
		auto& command = m_impl->scheduled[m_impl->scheduledCount++];
		command.size = n;
		command.frame = frame + kAudioQuantum;
		std::copy_n(bytes, n, command.bytes.begin());
	}
	if (m_impl->probe && n == 3 && (bytes[0] & 0xf0) == 0x90 && bytes[2] > 0
			&& m_impl->note_count < m_impl->notes.size())
		m_impl->notes[m_impl->note_count++] = frame;
	return true;
}
std::uint64_t ProphecyEngine::droppedImmediateMidiBytes() const { return m_impl->dropped_immediate.load(); }
std::uint64_t ProphecyEngine::droppedScheduledMidiBytes() const { return m_impl->dropped_scheduled.load(); }
std::size_t ProphecyEngine::popMidiTx(std::uint8_t *, std::size_t) { return 0; }
std::size_t ProphecyEngine::popMidiTxByteEvents(MidiTxByteEvent *, std::size_t) { return 0; }
std::uint64_t ProphecyEngine::droppedMidiTxByteEvents() const { return 0; }

void ProphecyEngine::pushPanelPulse(int, int, int, std::uint64_t) { }
bool ProphecyEngine::pushPanelPulseAtFrame(int, int, int, std::uint64_t) { return running(); }
bool ProphecyEngine::pushAdin(int, int) { if (!running()) m_impl->dropped_ui_adin.fetch_add(1); return running(); }
bool ProphecyEngine::pushAdinFromAudio(int, int) { if (!running()) m_impl->dropped_audio_adin.fetch_add(1); return running(); }
bool ProphecyEngine::pushAdinAtFrame(int, int, std::uint64_t) { if (!running()) m_impl->dropped_scheduled_adin.fetch_add(1); return running(); }
std::uint64_t ProphecyEngine::droppedUiAdinEvents() const { return m_impl->dropped_ui_adin.load(); }
std::uint64_t ProphecyEngine::droppedAudioAdinEvents() const { return m_impl->dropped_audio_adin.load(); }
std::uint64_t ProphecyEngine::droppedScheduledPanelEvents() const { return m_impl->dropped_scheduled_panel.load(); }
std::uint64_t ProphecyEngine::droppedScheduledAdinEvents() const { return m_impl->dropped_scheduled_adin.load(); }

std::uint32_t ProphecyEngine::ledSnapshot(std::uint8_t out[12]) const
{
	if (out) std::memset(out, 0, 12);
	return 0;
}
std::uint32_t ProphecyEngine::ledVisualSnapshot(std::uint8_t out[12]) const
{
	return ledSnapshot(out);
}
std::uint32_t ProphecyEngine::lcdRawSnapshot(std::uint8_t row1[40], std::uint8_t row2[40], std::uint8_t cgram[64]) const
{
	if (row1) std::memset(row1, ' ', 40);
	if (row2) std::memset(row2, ' ', 40);
	if (cgram) std::memset(cgram, 0, 64);
	return m_impl->lcd_version.load();
}
void ProphecyEngine::lcdA00GlyphRows(std::uint8_t out[kLcdA00GlyphRowBytes]) { if (out) std::memset(out, 0, kLcdA00GlyphRowBytes); }
bool ProphecyEngine::latestLcd(char *line1, char *line2, std::size_t cap) const
{
	if (!line1 || !line2 || cap == 0) return false;
	line1[0] = line2[0] = '\0';
	return false;
}
std::size_t ProphecyEngine::latestProgramData(std::uint8_t* out, std::size_t cap, std::uint32_t* version) const
{
	std::lock_guard lock(m_impl->programMutex);
	if (version) *version = m_impl->programVersion;
	const auto count = std::min(cap, m_impl->program.size());
	if (out) std::copy_n(m_impl->program.data(), count, out);
	return count;
}

std::uint64_t ProphecyEngine::firmwareControlErrors() const { return m_impl->firmwareErrors.load(); }

bool ProphecyEngine::programReadbackPending()
{
	std::lock_guard lock(m_impl->programMutex);
	if (m_impl->readbackAbandoned && m_impl->readbackComplete()) m_impl->readbackTicket = 0;
	return m_impl->readbackTicket != 0;
}

std::uint64_t ProphecyEngine::beginProgramReadback()
{
	std::lock_guard lock(m_impl->programMutex);
	if (!readyForPlayback()) return 0;
	if (m_impl->readbackAbandoned && m_impl->readbackComplete()) m_impl->readbackTicket = 0;
	if (m_impl->readbackTicket) return 0;
	m_impl->readbackTicket = ++m_impl->nextReadback;
	m_impl->readbackVersion = m_impl->programVersion;
	m_impl->readbackAbandoned = false;
	m_impl->readbackDoneFrame = UINT64_MAX;
	m_impl->immediate.push_back({{0xf0, 0x42, 0x30, 0x41, 0x10, 0, 0xf7, 0xf0, 0x7e, 0x7f, 6, 1, 0xf7},
		requestedFrames() + kAudioQuantum, 0});
	return m_impl->readbackTicket;
}

ProphecyEngine::ReadbackStatus ProphecyEngine::pollProgramReadback(std::uint64_t ticket,
	std::vector<std::uint8_t>& raw)
{
	std::lock_guard lock(m_impl->programMutex);
	raw.clear();
	if (!ticket || ticket != m_impl->readbackTicket || m_impl->readbackAbandoned) return ReadbackStatus::Invalid;
	if (!m_impl->readbackComplete()) return ReadbackStatus::Pending;
	m_impl->readbackTicket = 0;
	if (m_impl->programVersion == m_impl->readbackVersion) return ReadbackStatus::NoReply;
	raw = m_impl->program;
	return ReadbackStatus::Complete;
}

void ProphecyEngine::cancelProgramReadback(std::uint64_t ticket)
{
	std::lock_guard lock(m_impl->programMutex);
	if (ticket && ticket == m_impl->readbackTicket) m_impl->readbackAbandoned = true;
}
std::size_t ProphecyEngine::latestArpeggioPatternData(std::uint8_t *, std::size_t, std::uint32_t *version, int *pattern) const { if (version) *version = 0; if (pattern) *pattern = -1; return 0; }
std::size_t ProphecyEngine::available() const { return running() ? ringFrames() : 0; }
std::size_t ProphecyEngine::ringFrames() const { return 2048; }
std::uint64_t ProphecyEngine::producedFrames() const { return m_impl->produced.load(); }

std::vector<std::uint8_t> ProphecyEngine::snapshotProgram()
{
	if (snapshotRejected.load()) return {};
	std::lock_guard lock(m_impl->programMutex);
	return m_impl->firmware;
}

std::vector<std::uint8_t> ProphecyEngine::snapshotStoredProgram(int program)
{
	if (program < 0 || program >= 128 || !readyForPlayback()) return {};
	std::lock_guard lock(m_impl->programMutex);
	const auto& stored = m_impl->storedPrograms[std::size_t(program)];
	return stored.empty() ? std::vector<std::uint8_t>(535, std::uint8_t(program)) : stored;
}

std::vector<std::uint8_t> ProphecyEngine::snapshotGlobals()
{
	if (!readyForPlayback()) return {};
	std::lock_guard lock(m_impl->programMutex);
	auto globals = m_impl->globals;
	globals[163] = std::uint8_t((globals[163] & ~1u) | unsigned(protection.load()));
	return {globals.begin(), globals.end()};
}

void ProphecyEngine::setProgramRevision(std::uint64_t revision)
{
	auto current = m_impl->revision.load(std::memory_order_acquire);
	while (current < revision && !m_impl->revision.compare_exchange_weak(
		current, revision, std::memory_order_release, std::memory_order_acquire)) {}
}

void ProphecyEngine::setProgramInputPending(std::uint64_t sequence)
{ m_impl->programInputPending.store(sequence, std::memory_order_release); }

void ProphecyEngine::acknowledgeProgramInput(std::uint64_t sequence)
{ m_impl->programInputComplete.store(sequence, std::memory_order_release); }
