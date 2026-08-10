// SPDX-License-Identifier: AGPL-3.0-only
// Deterministic host stub used to validate the JUCE/Windows shell independently
// of MAME's executable-oriented OSD implementation.

#include "prophecy_engine.h"

#include <algorithm>
#include <atomic>
#include <cstring>

struct ProphecyEngine::Impl
{
	std::atomic<bool> started{false};
	std::atomic<bool> finished{false};
	std::atomic<bool> owns{false};
	std::atomic<std::uint64_t> produced{0};
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
	return true;
}

bool ProphecyEngine::enableMidiTxByteCapture(bool) { return !m_impl->started.load(); }

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

bool ProphecyEngine::pushMidi(const std::uint8_t *, std::size_t n) { if (!running()) { m_impl->dropped_immediate.fetch_add(n); return false; } return true; }
bool ProphecyEngine::pushMidiAtFrame(const std::uint8_t *, std::size_t n, std::uint64_t) { if (!running()) { m_impl->dropped_scheduled.fetch_add(n); return false; } return true; }
std::uint64_t ProphecyEngine::droppedImmediateMidiBytes() const { return m_impl->dropped_immediate.load(); }
std::uint64_t ProphecyEngine::droppedScheduledMidiBytes() const { return m_impl->dropped_scheduled.load(); }
std::size_t ProphecyEngine::popMidiTx(std::uint8_t *, std::size_t) { return 0; }
std::size_t ProphecyEngine::popMidiTxByteEvents(MidiTxByteEvent *, std::size_t) { return 0; }
std::uint64_t ProphecyEngine::droppedMidiTxByteEvents() const { return 0; }

void ProphecyEngine::pushPanelPulse(int, int, int) { }
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
std::size_t ProphecyEngine::latestProgramData(std::uint8_t *, std::size_t, std::uint32_t *version) const { if (version) *version = 0; return 0; }
std::size_t ProphecyEngine::latestArpeggioPatternData(std::uint8_t *, std::size_t, std::uint32_t *version, int *pattern) const { if (version) *version = 0; if (pattern) *pattern = -1; return 0; }
std::size_t ProphecyEngine::available() const { return running() ? ringFrames() : 0; }
std::size_t ProphecyEngine::ringFrames() const { return 2048; }
std::uint64_t ProphecyEngine::producedFrames() const { return m_impl->produced.load(); }
