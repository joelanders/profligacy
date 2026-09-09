// SPDX-License-Identifier: AGPL-3.0-only
#include "program_control.h"
#include <chrono>

namespace prophecy {

ProgramControl::ProgramControl(ProphecyEngine& engine) : m_engine(engine)
{
	m_thread = std::thread([this] { run(); });
}

ProgramControl::~ProgramControl() { stop(); }

void ProgramControl::stop()
{
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
		m_changed.notify_all();
	}
	if (m_thread.joinable()) m_thread.join();
}

bool ProgramControl::fail(const char* message)
{
	m_error = message;
	++m_statistics.dropped;
	return false;
}

void ProgramControl::reject(const char* message)
{
	std::lock_guard lock(m_mutex);
	fail(message);
}

std::string ProgramControl::error() const
{
	std::lock_guard lock(m_mutex);
	return m_error;
}

void ProgramControl::replace(ProgramDocument document)
{
	++m_revision;
	++m_version;
	m_statistics.cancelled += m_commands.size();
	m_commands.clear();
	m_document = std::move(document);
	m_failed = false;
	m_error.clear();
}

bool ProgramControl::restore(ProgramDocument document)
{
	std::lock_guard lock(m_mutex);
	if (document.edits.size() > ProgramDocument::maxEdits
		|| std::any_of(document.edits.begin(), document.edits.end(),
			[](const auto& edit) { return !edit.supported(); }))
		return fail("The saved program contains an unsupported edit.");
	replace(std::move(document));
	m_restore = true;
	return true;
}

bool ProgramControl::initialize(bool firmwareHandshake, bool ignoreRestore)
{
	std::lock_guard lock(m_mutex);
	const bool load = m_restore && !ignoreRestore && m_document.has_value();
	const auto bytes = load ? m_document->programMidi() : std::vector<std::uint8_t>{};
	const auto edits = load ? m_document->edits : std::vector<ProgramEdit>{};
	if (!m_engine.initializePlayback(bytes.data(), bytes.size(), firmwareHandshake, edits))
	{
		m_failed = true;
		return fail(m_engine.initializationError());
	}
	m_flight = {};
	if (load) m_commands.clear();
	const auto current = m_engine.snapshotProgram();
	if (firmwareHandshake && current.size() != ProgramDocument::programBytes)
		return fail("The firmware program could not be captured.");
	if (current.size() == ProgramDocument::programBytes)
	{
		if (!m_document || ignoreRestore) m_document.emplace();
		confirm(current, load ? edits.size() : 0);
	}
	const auto globals = m_engine.snapshotGlobals();
	if (globals.size() == 574) m_channel = globals[176] & 15;
	m_restore = false;
	m_failed = false;
	m_error.clear();
	m_changed.notify_all();
	return true;
}

bool ProgramControl::edit(const std::vector<ProgramEdit>& edits)
{
	std::lock_guard lock(m_mutex);
	if (!m_document) return fail("The program is not available yet.");
	if (edits.size() > ProgramDocument::maxEdits - m_document->edits.size()
		|| edits.size() > ProgramDocument::maxEdits - m_commands.size())
		return fail("The program edit queue is full.");
	if (std::any_of(edits.begin(), edits.end(), [](const auto& edit) { return !edit.supported(); }))
		return fail("The parameter or value is unsupported.");
	for (const auto edit : edits)
	{
		m_document->edits.push_back(edit);
		const auto bytes = edit.midi(m_channel);
		m_commands.push_back({{bytes.begin(), bytes.end()}, -1, -1, true});
	}
	m_changed.notify_all();
	return true;
}

bool ProgramControl::enqueue(Command command)
{
	if (!m_engine.readyForPlayback() || m_restore) return fail("The firmware is not ready for this control.");
	if (m_commands.size() >= ProgramDocument::maxEdits) return fail("The control queue is full.");
	m_commands.push_back(std::move(command));
	m_changed.notify_all();
	return true;
}

bool ProgramControl::midi(const std::uint8_t* bytes, std::size_t size)
{
	if (const auto edit = ProgramEdit::fromMidi(bytes, size)) return this->edit({*edit});
	std::lock_guard lock(m_mutex);
	if (!bytes || !size || size > ProphecyEngine::kMaxProgramBatchBytes)
		return fail("The MIDI control message is invalid.");
	if (bytes[0] >= 0x80 && bytes[0] < 0xf0 && (bytes[0] & 0xf0) != 0xc0)
		return m_engine.pushMidi(bytes, size);
	return enqueue({{bytes, bytes + size}});
}

bool ProgramControl::panel(int row, int bit)
{
	std::lock_guard lock(m_mutex);
	if (row < 0 || row > 7 || bit < 0 || bit > 7) return fail("The panel control is invalid.");
	return enqueue({{}, row, bit});
}

bool ProgramControl::select(int program)
{
	std::lock_guard lock(m_mutex);
	if (m_restore || !m_engine.readyForPlayback()) return fail("The firmware is not ready to select a program.");
	const auto bytes = m_engine.snapshotStoredProgram(program);
	if (bytes.size() != ProgramDocument::programBytes) return fail("The selected program is unavailable.");
	ProgramDocument document;
	std::copy(bytes.begin(), bytes.end(), document.base.begin());
	replace(std::move(document));
	m_restore = false;
	const auto cc = std::uint8_t(0xb0 | m_channel), pc = std::uint8_t(0xc0 | m_channel);
	return enqueue({{cc, 0, 0, cc, 32, std::uint8_t(program / 64), pc, std::uint8_t(program % 64)}});
}

std::uint64_t ProgramControl::refresh()
{
	std::lock_guard lock(m_mutex);
	++m_requested;
	m_changed.notify_all();
	return m_requested;
}

void ProgramControl::confirm(const std::vector<std::uint8_t>& bytes, std::size_t edits)
{
	std::copy(bytes.begin(), bytes.end(), m_document->base.begin());
	m_document->edits.erase(m_document->edits.begin(), m_document->edits.begin() + std::ptrdiff_t(edits));
	++m_version;
}

std::optional<ProgramDocument> ProgramControl::save()
{
	std::lock_guard lock(m_mutex);
	// Do not overwrite the base under an accepted suffix, or replay would apply
	// partially executed edits twice. Saving itself never advances firmware time.
	if (m_document && !m_restore && !m_failed && !m_flight.ticket
		&& m_commands.empty() && m_document->edits.empty())
	{
		const auto current = m_engine.snapshotProgram();
		if (current.size() == ProgramDocument::programBytes) confirm(current, 0);
	}
	return m_document;
}

std::size_t ProgramControl::read(std::uint8_t* out, std::size_t cap,
	std::uint32_t* version, std::uint64_t* completedRequest) const
{
	std::lock_guard lock(m_mutex);
	if (version) *version = m_version;
	if (completedRequest) *completedRequest = m_completed;
	const auto count = m_document ? std::min(cap, m_document->base.size()) : 0;
	if (out && count) std::copy_n(m_document->base.begin(), count, out);
	return count;
}

ProgramControl::Statistics ProgramControl::statistics() const
{
	std::lock_guard lock(m_mutex);
	auto result = m_statistics;
	result.pending = m_commands.size() + (m_flight.ticket != 0);
	return result;
}

void ProgramControl::service()
{
	if (m_restore || m_failed || !m_engine.readyForPlayback()) return;
	if (m_flight.ticket)
	{
		std::vector<std::uint8_t> bytes;
		const auto status = m_engine.pollProgramExchange(m_flight.ticket, bytes);
		if (status == ProphecyEngine::ProgramExchangeStatus::Pending) return;
		if (status == ProphecyEngine::ProgramExchangeStatus::Failed)
		{
			m_failed = true;
			fail("The firmware program operation did not complete. Pending edits have been retained.");
			return;
		}
		if (m_flight.revision == m_revision)
		{
			if (status == ProphecyEngine::ProgramExchangeStatus::NoReply)
			{
				// Retry the inquiry only. Repeating the edit could apply a secondary
				// effect twice. Each failed inquiry has already drained its identity.
				if (++m_flight.retries <= 3)
				{
					m_flight.ticket = m_engine.beginProgramExchange(nullptr, 0);
					if (m_flight.ticket) return;
				}
				m_failed = true;
				fail("The firmware did not return the program. Pending edits have been retained.");
				return;
			}
			if (!m_document) m_document.emplace();
			confirm(bytes, m_flight.edits);
			m_completed = m_flight.refresh;
		}
		m_flight = {};
	}
	if (m_commands.empty() && m_requested == m_completed) return;
	Command command;
	if (!m_commands.empty()) command = m_commands.front();
	if (command.row >= 0) m_engine.pushPanelPulse(command.row, command.bit);
	const auto ticket = m_engine.beginProgramExchange(command.bytes.data(), command.bytes.size());
	if (!ticket) return;
	m_flight = {ticket, m_revision, m_commands.empty() ? m_requested : m_completed,
		std::size_t(command.semantic), 0};
	if (!m_commands.empty())
	{
		m_commands.pop_front();
		++m_statistics.sent;
	}
}

void ProgramControl::run()
{
	std::unique_lock lock(m_mutex);
	while (!m_stopping)
	{
		service();
		m_changed.wait_for(lock, std::chrono::milliseconds(10));
	}
}

}
