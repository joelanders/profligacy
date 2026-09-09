// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "program_document.h"
#include "prophecy_engine.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace prophecy {

// All methods are non-realtime. The document contains the last confirmed base
// and every accepted semantic edit after it, including edits already on the wire.
// Only this owner starts/polls program exchanges. Its thread never grants audio.
class ProgramControl
{
public:
	explicit ProgramControl(ProphecyEngine& engine);
	~ProgramControl();
	void stop();
	bool initialize(bool firmwareHandshake = true, bool ignoreRestore = false);
	bool restore(ProgramDocument document);
	bool edit(const std::vector<ProgramEdit>& edits);
	bool midi(const std::uint8_t* bytes, std::size_t size);
	bool panel(int row, int bit);
	bool select(int program);
	std::uint64_t refresh();
	std::optional<ProgramDocument> save();
	std::size_t read(std::uint8_t* out, std::size_t cap, std::uint32_t* version,
		std::uint64_t* completedRequest) const;
	std::string error() const;
	void reject(const char* message);
	struct Statistics { std::uint64_t sent = 0, cancelled = 0, dropped = 0; std::size_t pending = 0; };
	Statistics statistics() const;

private:
	struct Command
	{
		std::vector<std::uint8_t> bytes;
		int row = -1, bit = -1;
		bool semantic = false;
	};
	struct Flight
	{
		std::uint64_t ticket = 0, revision = 0, refresh = 0;
		std::size_t edits = 0;
		int retries = 0;
	};
	bool fail(const char* message);
	bool enqueue(Command command);
	void replace(ProgramDocument document);
	void confirm(const std::vector<std::uint8_t>& bytes, std::size_t edits);
	void service();
	void run();
	ProphecyEngine& m_engine;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::optional<ProgramDocument> m_document;
	std::deque<Command> m_commands;
	Flight m_flight;
	std::uint64_t m_revision = 0, m_requested = 0, m_completed = 0;
	std::uint32_t m_version = 0;
	std::uint8_t m_channel = 0;
	bool m_restore = false, m_stopping = false, m_failed = false;
	std::string m_error;
	Statistics m_statistics;
	std::thread m_thread;
};

}
