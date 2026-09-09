// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "program_document.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace prophecy {

// All document access and revision changes are serialized by the control mutex.
// The audio thread records input and admission failures in a bounded inbox,
// but never reads the document or takes this mutex. An observation must
// carry the revision it was requested for.
class ProgramState
{
public:
	enum class Status { Empty, Pending, Confirmed, Failed };
	enum class Error { None, NoProgram, Capacity, InvalidEdit, InvalidProgram, Snapshot, Restore, Control, Busy, Write };
	struct Token { std::uint64_t revision = 0, through = 0; };

	std::uint64_t revision() const { return currentRevision.load(std::memory_order_acquire); }
	std::uint64_t advanceRevision() { return currentRevision.fetch_add(1, std::memory_order_acq_rel) + 1; }
	mutable std::recursive_mutex mutex;

	const ProgramDocument* document() const
	{ return m_revision == revision() && m_document ? &*m_document : nullptr; }
	bool restoreRequired() const { return document() && m_restoreRequired; }
	bool pendingIntent() const { return document() && (m_restoreRequired || !m_document->edits.empty()); }
	bool firmwareControlPending() const { return m_firmwarePending.load(std::memory_order_acquire); }
	void beginFirmwareControl()
	{
		m_firmwarePending.store(true, std::memory_order_release);
		clearError();
	}
	bool finishFirmwareControl(std::uint64_t owner, const std::vector<std::uint8_t>& raw)
	{
		if (!firmwareControlPending() || !observe(owner, raw)) return false;
		m_firmwarePending.store(false, std::memory_order_release);
		if (m_error.load(std::memory_order_acquire) == Error::None)
			m_status.store(Status::Confirmed, std::memory_order_release);
		return true;
	}
	Token token() const { return {revision(), m_accepted}; }
	Status status() const { return m_status.load(std::memory_order_acquire); }
	const char* error() const
	{
		switch (m_error.load(std::memory_order_acquire))
		{
			case Error::None: return "";
			case Error::NoProgram: return "The program is not available yet. The edit was not accepted.";
			case Error::Capacity: return "Too many program edits are pending. The edit was not accepted.";
			case Error::InvalidEdit: return "The parameter edit is invalid and was not accepted.";
			case Error::InvalidProgram: return "The saved program is invalid.";
			case Error::Snapshot: return "The current firmware program could not be captured.";
			case Error::Restore: return "The saved program edits could not be restored.";
			case Error::Control: return "A program operation failed. Save the pending edits and reload the program.";
			case Error::Busy: return "Another program operation is pending. This request was not accepted.";
			case Error::Write: return "The firmware did not confirm the program WRITE. Your program remains available to save.";
		}
		return "";
	}
	void fail(Error error)
	{
		m_error.store(error, std::memory_order_release);
		m_status.store(Status::Failed, std::memory_order_release);
	}
	// A newly accepted explicit operation may clear a previous rejected request.
	// Background observations and completions of older operations must not.
	void clearError()
	{
		m_error.store(Error::None, std::memory_order_release);
		m_status.store(firmwareControlPending() ? Status::Pending : !document() ? Status::Empty
			: m_restoreRequired || !m_document->edits.empty() ? Status::Pending : Status::Confirmed,
			std::memory_order_release);
	}
	void clear()
	{
		m_document.reset();
		m_revision = revision();
		m_accepted = m_confirmed = 0;
		m_restoreRequired = false;
		m_firmwarePending.store(false, std::memory_order_release);
		m_error.store(Error::None, std::memory_order_release);
		m_status.store(Status::Empty, std::memory_order_release);
	}
	void restore(ProgramDocument document)
	{
		clear();
		m_accepted = document.edits.size();
		m_document = std::move(document);
		m_restoreRequired = true;
		m_status.store(Status::Pending, std::memory_order_release);
	}
	bool accept(ProgramEdit edit)
	{ return accept(std::vector<ProgramEdit>{edit}); }
	bool accept(const std::vector<ProgramEdit>& edits)
	{
		if (edits.empty()) return true;
		if (firmwareControlPending()) { fail(Error::Busy); return false; }
		if (std::any_of(edits.begin(), edits.end(), [](const auto& edit) { return !edit.supported(); }))
			{ fail(Error::InvalidEdit); return false; }
		if (!document()) { fail(Error::NoProgram); return false; }
		if (edits.size() > ProgramDocument::maxEdits - m_document->edits.size())
			{ fail(Error::Capacity); return false; }
		auto accepted = m_document->edits;
		accepted.insert(accepted.end(), edits.begin(), edits.end());
		m_document->edits.swap(accepted);
		m_accepted += edits.size();
		m_error.store(Error::None, std::memory_order_release);
		m_status.store(Status::Pending, std::memory_order_release);
		return true;
	}
	bool observe(std::uint64_t owner, const std::vector<std::uint8_t>& raw)
	{
		if (owner != revision() || (document() && (m_restoreRequired || !m_document->edits.empty()))) return false;
		if (raw.size() != ProgramDocument::programBytes) { fail(Error::Snapshot); return false; }
		// A background observation cannot acknowledge an error from a rejected
		// user operation. Keep that error until a new explicit operation succeeds.
		// Sequence numbers are monotonic within a program revision. Reusing a
		// number after a clean snapshot could let an old readback consume a new,
		// still-undelivered edit with the same token.
		if (m_revision != owner) m_accepted = m_confirmed = 0;
		m_revision = owner;
		m_restoreRequired = false;
		m_document.emplace();
		std::copy(raw.begin(), raw.end(), m_document->base.begin());
		if (m_error.load(std::memory_order_acquire) == Error::None)
			m_status.store(firmwareControlPending() ? Status::Pending : Status::Confirmed, std::memory_order_release);
		return true;
	}
	// The caller must establish that this exact prefix completed. A newer dump
	// version alone is insufficient. Late, duplicate and superseded confirmations
	// never consume edits; a valid prefix retains all subsequently accepted intent.
	bool confirm(Token completed, const std::vector<std::uint8_t>& raw)
	{
		if (!document() || completed.revision != revision() || completed.through < m_confirmed
			|| completed.through > m_accepted || raw.size() != ProgramDocument::programBytes) return false;
		if (!m_restoreRequired && completed.through == m_confirmed) return false;
		const auto count = std::size_t(completed.through - m_confirmed);
		if (m_restoreRequired && count == 0
			&& !std::equal(raw.begin(), raw.end(), m_document->base.begin())) return false;
		m_document->edits.erase(m_document->edits.begin(),
			m_document->edits.begin() + static_cast<std::ptrdiff_t>(count));
		std::copy(raw.begin(), raw.end(), m_document->base.begin());
		m_confirmed = completed.through;
		m_restoreRequired = false;
		if (m_error.load(std::memory_order_acquire) == Error::None)
			m_status.store(m_document->edits.empty() ? Status::Confirmed : Status::Pending, std::memory_order_release);
		return true;
	}

private:
	std::atomic<std::uint64_t> currentRevision{1};
	std::optional<ProgramDocument> m_document;
	std::uint64_t m_revision = 1, m_accepted = 0, m_confirmed = 0;
	bool m_restoreRequired = false;
	std::atomic<bool> m_firmwarePending{false};
	std::atomic<Status> m_status{Status::Empty};
	std::atomic<Error> m_error{Error::None};
};

}
