// Headless JUCE-host timing probe for the real ProphecyAudioProcessor processBlock path.
#include "PluginProcessor.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic<std::uint64_t> g_allocationCount { 0 };
std::atomic<std::uint64_t> g_allocationBytes { 0 };
std::atomic<std::uint64_t> g_processBlockAllocationCount { 0 };
thread_local bool g_insideProcessBlock = false;

void *trackedAllocate(std::size_t size)
{
	g_allocationCount.fetch_add(1, std::memory_order_relaxed);
	g_allocationBytes.fetch_add(size, std::memory_order_relaxed);
	if (g_insideProcessBlock)
		g_processBlockAllocationCount.fetch_add(1, std::memory_order_relaxed);
	if (void *p = std::malloc(size == 0 ? 1 : size)) return p;
	throw std::bad_alloc();
}

struct ProcessBlockAllocationScope
{
	ProcessBlockAllocationScope() { g_insideProcessBlock = true; }
	~ProcessBlockAllocationScope() { g_insideProcessBlock = false; }
};

} // namespace

void *operator new(std::size_t size) { return trackedAllocate(size); }
void *operator new[](std::size_t size) { return trackedAllocate(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {

struct Event
{
	std::int64_t sample = 0;
	std::vector<std::uint8_t> bytes;
};

enum class EditorActionKind
{
	SelectPatch,
	RequestProgramDump,
	SetParam,
	SetGlobalParam,
	SetPatternParam,
	SelectArpPattern,
	SetArpControl,
	RequestArpPatternDump,
	SendArpPatternData,
	RenamePatch,
	SendMacro,
	WritePatch,
	PanelPulse,
	SetAdin,
	SetWheel2,
	SetCcMap,
	SendMidi
};

struct EditorAction
{
	std::int64_t sample = 0;
	EditorActionKind kind = EditorActionKind::SelectPatch;
	std::string op;
	std::vector<int> args;
	std::vector<std::uint8_t> bytes;
	std::string text;
};

bool readIntArray(const juce::var &value, std::vector<int> &out)
{
	if (!value.isArray()) return false;
	for (const auto &item : *value.getArray())
	{
		if (!item.isInt() && !item.isInt64()) return false;
		out.push_back((int)item);
	}
	return true;
}

bool loadScenarioFile(const char *path, double rate, std::vector<Event> &dawEvents,
	std::vector<EditorAction> &editorActions, std::string &scenarioName,
	std::string &error)
{
	const juce::File file(path);
	const juce::var root = juce::JSON::parse(file);
	if (!root.isObject()) { error = "scenario root must be a JSON object"; return false; }
	auto *object = root.getDynamicObject();
	if ((int)object->getProperty("schema") != 1)
	{
		error = "scenario schema must be 1";
		return false;
	}
	const juce::String name = object->getProperty("name").toString();
	if (name.isNotEmpty()) scenarioName = name.toStdString();
	const juce::var actions = object->getProperty("actions");
	if (!actions.isArray()) { error = "scenario actions must be an array"; return false; }
	for (const auto &item : *actions.getArray())
	{
		if (!item.isObject()) { error = "scenario action must be an object"; return false; }
		auto *actionObject = item.getDynamicObject();
		const double at = (double)actionObject->getProperty("at");
		const std::string op = actionObject->getProperty("op").toString().toStdString();
		if (at < 0.0 || op.empty()) { error = "scenario action needs nonnegative at and op"; return false; }
		std::vector<int> args;
		if (actionObject->hasProperty("args")
				&& !readIntArray(actionObject->getProperty("args"), args))
		{
			error = "scenario action args must be integers";
			return false;
		}
		std::vector<int> byteInts;
		if (actionObject->hasProperty("bytes")
				&& !readIntArray(actionObject->getProperty("bytes"), byteInts))
		{
			error = "scenario action bytes must be integers";
			return false;
		}
		std::vector<std::uint8_t> bytes;
		for (const int byte : byteInts)
		{
			if (byte < 0 || byte > 255) { error = "scenario byte outside 0..255"; return false; }
			bytes.push_back((std::uint8_t)byte);
		}
		const std::int64_t sample = (std::int64_t)std::llround(at * rate);
		if (op == "daw_midi")
		{
			if (bytes.empty()) { error = "daw_midi needs bytes"; return false; }
			dawEvents.push_back({sample, std::move(bytes)});
			continue;
		}
		EditorAction action;
		action.sample = sample;
		action.op = op;
		action.args = std::move(args);
		action.bytes = std::move(bytes);
		action.text = actionObject->getProperty("text").toString().toStdString();
		if (op == "select_patch") action.kind = EditorActionKind::SelectPatch;
		else if (op == "request_program_dump") action.kind = EditorActionKind::RequestProgramDump;
		else if (op == "set_param") action.kind = EditorActionKind::SetParam;
		else if (op == "set_global_param") action.kind = EditorActionKind::SetGlobalParam;
		else if (op == "set_pattern_param") action.kind = EditorActionKind::SetPatternParam;
		else if (op == "select_arp_pattern") action.kind = EditorActionKind::SelectArpPattern;
		else if (op == "set_arp_control") action.kind = EditorActionKind::SetArpControl;
		else if (op == "request_arp_pattern_dump") action.kind = EditorActionKind::RequestArpPatternDump;
		else if (op == "send_arp_pattern_data") action.kind = EditorActionKind::SendArpPatternData;
		else if (op == "rename_patch") action.kind = EditorActionKind::RenamePatch;
		else if (op == "send_macro") action.kind = EditorActionKind::SendMacro;
		else if (op == "write_patch") action.kind = EditorActionKind::WritePatch;
		else if (op == "panel_pulse") action.kind = EditorActionKind::PanelPulse;
		else if (op == "set_adin") action.kind = EditorActionKind::SetAdin;
		else if (op == "set_wheel2") action.kind = EditorActionKind::SetWheel2;
		else if (op == "set_cc_map") action.kind = EditorActionKind::SetCcMap;
		else if (op == "send_midi") action.kind = EditorActionKind::SendMidi;
		else { error = "unknown scenario op: " + op; return false; }
		editorActions.push_back(std::move(action));
	}
	return true;
}

std::vector<std::uint8_t> parameterChange(int group, int parameter, int value)
{
	value &= 0x3fff;
	parameter &= 0x3fff;
	return { 0xf0, 0x42, 0x30, 0x41, 0x41, (std::uint8_t)(group & 0x7f),
		(std::uint8_t)(parameter & 0x7f), (std::uint8_t)((parameter >> 7) & 0x7f),
		(std::uint8_t)(value & 0x7f), (std::uint8_t)((value >> 7) & 0x7f), 0xf7 };
}

void addEvent(std::vector<Event> &events, double rate, double seconds,
	std::initializer_list<std::uint8_t> bytes)
{
	events.push_back({ (std::int64_t)std::llround(seconds * rate), bytes });
}

void addEvent(std::vector<Event> &events, double rate, double seconds,
	std::vector<std::uint8_t> bytes)
{
	events.push_back({ (std::int64_t)std::llround(seconds * rate), std::move(bytes) });
}

} // namespace

int main(int argc, char **argv)
{
	double rate = 48000.0;
	double durationSeconds = 22.0;
	double healthInterval = 5.0;
	double healthTimeout = 3.0;
	double programInterval = 7.0;
	double programStart = 18.0;
	int block = 512;
	int phaseSamples = 0;
	int maxHealthMisses = 2;
	std::vector<int> programValues;
	std::uint32_t stressSeed = 0x50524f50; // 'PROP'
	bool stress = false, secondsSet = false;
	bool programIntervalSet = false, programStartSet = false;
	bool setupTraffic = true, clockTraffic = true, noteTraffic = true;
	bool controlTraffic = true, parameterTraffic = true, programTraffic = true;
	std::string scenario = "mixed-daw";
	const char *scenarioFile = nullptr;
	const char *output = nullptr;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
		else if (arg == "--block" && i + 1 < argc) block = std::atoi(argv[++i]);
		else if (arg == "--phase-samples" && i + 1 < argc) phaseSamples = std::atoi(argv[++i]);
		else if (arg == "--seconds" && i + 1 < argc) { durationSeconds = std::atof(argv[++i]); secondsSet = true; }
		else if (arg == "--output" && i + 1 < argc) output = argv[++i];
		else if (arg == "--stress") stress = true;
		else if (arg == "--no-setup") setupTraffic = false;
		else if (arg == "--no-clock") clockTraffic = false;
		else if (arg == "--no-notes") noteTraffic = false;
		else if (arg == "--no-controls") controlTraffic = false;
		else if (arg == "--no-params") parameterTraffic = false;
		else if (arg == "--no-programs") programTraffic = false;
		else if (arg == "--health-interval" && i + 1 < argc) healthInterval = std::atof(argv[++i]);
		else if (arg == "--health-timeout" && i + 1 < argc) healthTimeout = std::atof(argv[++i]);
		else if (arg == "--program-interval" && i + 1 < argc)
		{
			programInterval = std::atof(argv[++i]);
			programIntervalSet = true;
		}
		else if (arg == "--program-start" && i + 1 < argc)
		{
			programStart = std::atof(argv[++i]);
			programStartSet = true;
		}
		else if (arg == "--program-value" && i + 1 < argc) programValues.push_back(std::atoi(argv[++i]));
		else if (arg == "--scenario" && i + 1 < argc) scenario = argv[++i];
		else if (arg == "--scenario-file" && i + 1 < argc) scenarioFile = argv[++i];
		else if (arg == "--max-health-misses" && i + 1 < argc) maxHealthMisses = std::atoi(argv[++i]);
		else if (arg == "--seed" && i + 1 < argc)
			stressSeed = (std::uint32_t)std::strtoul(argv[++i], nullptr, 0);
	}
	std::vector<Event> fileDawEvents;
	std::vector<EditorAction> fileEditorActions;
	if (scenarioFile != nullptr)
	{
		std::string scenarioError;
		if (!loadScenarioFile(scenarioFile, rate, fileDawEvents, fileEditorActions,
				scenario, scenarioError))
		{
			std::fprintf(stderr, "invalid scenario file %s: %s\n", scenarioFile,
				scenarioError.c_str());
			return 2;
		}
	}
	if (scenarioFile == nullptr && scenario != "mixed-daw" && scenario != "rapid-patch-browse")
	{
		std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
		return 2;
	}
	if (scenario == "rapid-patch-browse")
	{
		if (!programTraffic)
		{
			std::fprintf(stderr, "rapid-patch-browse requires program traffic\n");
			return 2;
		}
		if (!programIntervalSet) programInterval = 0.070;
		if (!programStartSet) programStart = 15.0;
		setupTraffic = false;
		clockTraffic = false;
		controlTraffic = false;
		parameterTraffic = false;
		if (programValues.empty())
		{
			for (int program = 1; program <= 10; ++program) programValues.push_back(program);
			for (int program = 9; program >= 0; --program) programValues.push_back(program);
		}
	}
	if (stress && !secondsSet) durationSeconds = 60.0;
	const std::uint32_t configuredStressSeed = stressSeed;
	if (rate < 8000.0 || block <= 0 || phaseSamples < 0 || phaseSamples >= block
			|| durationSeconds < 16.0 || output == nullptr)
	{
		std::fprintf(stderr,
			"usage: %s --rate HZ --block N [--seconds N>=16] --output capture.jsonl "
			"[--stress] [--scenario mixed-daw|rapid-patch-browse] [--scenario-file FILE] [--seed N] "
			"[--health-interval S] [--health-timeout S] "
			"[--program-interval S] [--program-start S] [--program-value 0..127] "
			"[--max-health-misses N] [--phase-samples 0..BLOCK-1] "
			"[--no-setup] [--no-clock] [--no-notes] [--no-controls] "
			"[--no-params] [--no-programs]\n",
			argv[0]);
		return 2;
	}
	if (healthInterval <= 0.0 || healthTimeout <= 0.0 || programInterval <= 0.0
			|| programStart < 0.0
			|| std::any_of(programValues.begin(), programValues.end(), [](int value) {
				return value < 0 || value > 127;
			})
			|| maxHealthMisses < 0)
	{
		std::fprintf(stderr,
			"health/program intervals and timeout must be positive and max misses nonnegative\n");
		return 2;
	}

	std::FILE *capture = std::fopen(output, "w");
	if (!capture)
	{
		std::perror(output);
		return 2;
	}

	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	ProphecyAudioProcessor processor;
	if (!processor.enableMidiTxByteCapture())
	{
		std::fprintf(stderr, "could not enable MIDI capture before engine start\n");
		std::fclose(capture);
		return 2;
	}
	processor.prepareToPlay(rate, block);

	std::vector<Event> events = std::move(fileDawEvents);
	std::vector<EditorAction> editorEvents = std::move(fileEditorActions);
	const double phaseSeconds = (double)phaseSamples / rate;
	auto addAt = [&events, rate, phaseSeconds](double seconds,
			std::initializer_list<std::uint8_t> bytes) {
		addEvent(events, rate, seconds + phaseSeconds, bytes);
	};
	auto addBytesAt = [&events, rate, phaseSeconds](double seconds,
			std::vector<std::uint8_t> bytes) {
		addEvent(events, rate, seconds + phaseSeconds, std::move(bytes));
	};
	if (setupTraffic)
	{
		addBytesAt(8.8, parameterChange(0, 187, 1)); // CLOCK = EXT
		addAt(9.1, {0xb0,0x63,0x00}); // NRPN select: UP pattern
		addAt(9.1, {0xb0,0x62,0x01});
		addAt(9.1, {0xb0,0x06,0x00});
		addBytesAt(10.8, parameterChange(2, 18, 5)); // sixteenth triplet = 4 F8
		addAt(11.3, {0xb0,0x63,0x00}); // NRPN: ARP ON
		addAt(11.3, {0xb0,0x62,0x02});
		addAt(11.3, {0xb0,0x06,0x7f});
	}
	constexpr std::uint8_t chord[] = {0x3c, 0x40, 0x43, 0x47};
	if (noteTraffic)
	{
		for (std::uint8_t note : chord)
			addAt(12.0, {0x90,note,0x64});
		for (std::uint8_t note : chord)
			addAt(15.0, {0x80,note,0x00});
	}
	if (setupTraffic)
	{
		addAt(15.4, {0xb0,0x63,0x00}); // NRPN: ARP OFF
		addAt(15.4, {0xb0,0x62,0x02});
		addAt(15.4, {0xb0,0x06,0x00});
	}
	constexpr double externalBpm = 120.0;
	const double clockPeriod = 60.0 / (24.0 * externalBpm);
	if (clockTraffic)
		for (double when = 11.5; when <= 15.5 + 1e-9; when += clockPeriod)
			addAt(when, {0xf8});
	if (stress)
	{
		// Leave a quiet tail for generation-gated dump requests, note-offs, and
		// queued board traffic to recover and drain.  Without this boundary the
		// verdict depends on whether the last Program Change happens to overlap
		// the final health probe.
		const double trafficEndSeconds = std::max(16.0, durationSeconds - 5.0);
		auto random = [&stressSeed]() {
			stressSeed ^= stressSeed << 13;
			stressSeed ^= stressSeed >> 17;
			stressSeed ^= stressSeed << 5;
			return stressSeed;
		};
		for (double when = 16.0; when < trafficEndSeconds; when += 0.70)
		{
			const std::uint8_t note = (std::uint8_t)(36 + random() % 49);
			const std::uint8_t velocity = (std::uint8_t)(48 + random() % 80);
			if (noteTraffic)
			{
				addAt(when, {0x90, note, velocity});
				addAt(std::min(when + 0.42, durationSeconds - 0.5), {0x80, note, 0x00});
			}
		}
		for (double when = 16.15; when < trafficEndSeconds; when += 0.53)
		{
			const std::uint8_t controller = (std::uint8_t)(1 + random() % 31);
			const std::uint8_t controllerValue = (std::uint8_t)(random() % 128);
			const std::uint16_t bend = (std::uint16_t)(random() & 0x3fff);
			if (controlTraffic)
			{
				addAt(when, {0xb0, controller, controllerValue});
				addAt(when + 0.01,
					{0xe0, (std::uint8_t)(bend & 0x7f), (std::uint8_t)(bend >> 7)});
			}
		}
		constexpr int parameters[] = {105, 118, 131, 144, 355, 356, 364, 365, 371};
		std::size_t parameterIndex = 0;
		for (double when = 16.30; when < trafficEndSeconds; when += 0.85)
		{
			const int value = (int)(random() % 200);
			if (parameterTraffic)
				addBytesAt(when,
					parameterChange(1, parameters[parameterIndex % std::size(parameters)], value));
			++parameterIndex;
		}
		std::size_t programIndex = 0;
		for (double when = programStart; when < trafficEndSeconds; when += programInterval)
		{
			if (!programValues.empty() && programIndex >= programValues.size())
				break;
			const std::uint8_t randomProgram = (std::uint8_t)(random() % 128);
			const std::uint8_t program = !programValues.empty()
				? (std::uint8_t)programValues[programIndex] : randomProgram;
			++programIndex;
			if (programTraffic)
			{
				if (scenario == "rapid-patch-browse")
				{
					if (scenarioFile == nullptr)
						editorEvents.push_back({
							(std::int64_t)std::llround((when + phaseSeconds) * rate),
							EditorActionKind::SelectPatch, "select_patch", {program}, {}, {} });
				}
				else
				{
					addAt(when, {0xb0, 0x00, 0x00});
					addAt(when, {0xb0, 0x20, (std::uint8_t)(program / 64)});
					addAt(when, {0xc0, (std::uint8_t)(program % 64)});
				}
			}
		}
	}
	if (scenario == "rapid-patch-browse" && scenarioFile == nullptr
			&& programTraffic && !editorEvents.empty())
	{
		const std::int64_t settleSamples = (std::int64_t)std::llround(0.350 * rate);
		editorEvents.push_back({editorEvents.back().sample + settleSamples,
			EditorActionKind::RequestProgramDump, "request_program_dump", {}, {}, {}});
	}
	std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
		return a.sample < b.sample;
	});
	std::stable_sort(editorEvents.begin(), editorEvents.end(),
		[](const EditorAction &a, const EditorAction &b) { return a.sample < b.sample; });

	juce::AudioBuffer<float> audio(2, block);
	std::size_t eventIndex = 0;
	std::size_t editorEventIndex = 0;
	std::uint64_t captured = 0;
	const bool allocationStats = std::getenv("PROPHECY_ALLOCATION_STATS") != nullptr;
	std::uint64_t previousAllocations = g_allocationCount.load(std::memory_order_relaxed);
	std::uint64_t previousBytes = g_allocationBytes.load(std::memory_order_relaxed);
	std::uint64_t previousProcessBlockAllocations =
		g_processBlockAllocationCount.load(std::memory_order_relaxed);
	int allocationSecond = 1;
	std::uint64_t healthAttempts = 0, healthReplies = 0, healthMisses = 0;
	std::uint64_t consecutiveHealthMisses = 0, worstConsecutiveHealthMisses = 0;
	std::uint32_t healthBaselineVersion = 0;
	double nextHealthAt = 12.0 + phaseSeconds, healthRequestedAt = -1.0, maxHealthLatency = 0.0;
	double lastEngineProgressAt = 0.0;
	std::uint64_t lastProducedFrames = 0;
	float audioPeak = 0.0f;
	bool healthPending = false, healthFailed = false;
	bool editorRecoveryRequested = false, editorRecoveryPending = false,
		editorRecoveryReplied = false;
	std::uint64_t editorRecoveryGeneration = 0;
	bool arpRecoveryRequested = false, arpRecoveryPending = false, arpRecoveryReplied = false;
	std::uint32_t arpBaselineVersion = 0;
	int arpExpectedPattern = -1;
	double arpRequestedAt = -1.0, arpMaxLatency = 0.0;
	std::string healthFailure;
	// The stress generators above reserve a five-second drain/recovery tail.
	const std::int64_t totalSamples =
		(std::int64_t)std::llround(durationSeconds * rate);
	const auto period = std::chrono::duration<double>((double)block / rate);
	auto next = std::chrono::steady_clock::now();
	for (std::int64_t blockStart = 0; blockStart < totalSamples; blockStart += block)
	{
		juce::Timer::callPendingTimersSynchronously();
		juce::MidiBuffer midi;
		const std::int64_t blockEnd = blockStart + block;
		const double blockStartSeconds = (double)blockStart / rate;
		const double blockEndSeconds = (double)blockEnd / rate;
		while (eventIndex < events.size() && events[eventIndex].sample < blockEnd)
		{
			const Event &event = events[eventIndex++];
			const int offset = (int)std::max<std::int64_t>(0, event.sample - blockStart);
			midi.addEvent(event.bytes.data(), (int)event.bytes.size(), offset);
		}
		while (editorEventIndex < editorEvents.size()
				&& editorEvents[editorEventIndex].sample < blockEnd)
		{
			const EditorAction &event = editorEvents[editorEventIndex++];
			auto needArgs = [&event](std::size_t count) { return event.args.size() >= count; };
			if (event.kind == EditorActionKind::SelectPatch && needArgs(1))
			{
				if (editorRecoveryPending)
				{
					healthPending = false;
					editorRecoveryPending = false;
					nextHealthAt = (double)event.sample / rate + healthInterval;
					std::fprintf(stderr,
						"[editor-recovery] cancelled_by_patch host_t=%.3f\n",
						(double)event.sample / rate);
				}
				if (arpRecoveryPending)
				{
					arpRecoveryPending = false;
					std::fprintf(stderr,
						"[arp-recovery] cancelled_by_patch host_t=%.3f\n",
						(double)event.sample / rate);
				}
				processor.selectPatch(event.args[0]);
				std::fprintf(stderr, "[editor-stress] select_patch=%d host_t=%.3f\n",
					event.args[0], (double)event.sample / rate);
			}
			else if (event.kind == EditorActionKind::RequestProgramDump)
			{
				if (healthPending)
				{
					// A production refresh supersedes any older DAW health probe. Grade
					// from this action's causal generation and deadline; retaining the
					// deliberately gated request's expired deadline creates a false fail.
					std::uint8_t previousDump[1024];
					processor.getProgramData(previousDump, sizeof(previousDump),
						&healthBaselineVersion);
					editorRecoveryGeneration = std::max(editorRecoveryGeneration,
						processor.requestProgramDump());
					healthRequestedAt = (double)event.sample / rate;
					editorRecoveryRequested = true;
					editorRecoveryPending = true;
					++healthAttempts;
					std::fprintf(stderr,
						"[editor-recovery] superseded host_t=%.3f baseline_version=%u\n",
						(double)event.sample / rate, healthBaselineVersion);
					continue;
				}
				std::uint8_t previousDump[1024];
				processor.getProgramData(previousDump, sizeof(previousDump), &healthBaselineVersion);
				editorRecoveryGeneration = processor.requestProgramDump();
				healthRequestedAt = (double)event.sample / rate;
				healthPending = true;
				editorRecoveryRequested = true;
				editorRecoveryPending = true;
				++healthAttempts;
				std::fprintf(stderr,
					"[editor-recovery] request=%llu host_t=%.3f baseline_version=%u\n",
					(unsigned long long)healthAttempts, healthRequestedAt, healthBaselineVersion);
			}
			else if (event.kind == EditorActionKind::SetParam && needArgs(2))
				processor.setParam(event.args[0], event.args[1]);
			else if (event.kind == EditorActionKind::SetGlobalParam && needArgs(2))
				processor.setParamG(0, event.args[0], event.args[1]);
			else if (event.kind == EditorActionKind::SetPatternParam && needArgs(2))
				processor.setPatternParam(event.args[0], event.args[1]);
			else if (event.kind == EditorActionKind::SelectArpPattern && needArgs(1))
				processor.selectArpeggioPattern(event.args[0]);
			else if (event.kind == EditorActionKind::SetArpControl && needArgs(2))
				processor.setArpeggiatorControl(event.args[0], event.args[1]);
			else if (event.kind == EditorActionKind::RequestArpPatternDump && needArgs(1))
			{
				const bool superseded = arpRecoveryPending;
				std::uint8_t previous[128];
				int previousPattern = -1;
				processor.getArpeggioPatternData(previous, sizeof(previous),
					&arpBaselineVersion, &previousPattern);
				arpExpectedPattern = event.args[0];
				arpRequestedAt = (double)event.sample / rate;
				arpRecoveryRequested = true;
				arpRecoveryPending = true;
				processor.requestArpeggioPatternDump(arpExpectedPattern);
				std::fprintf(stderr,
					"[arp-recovery] request pattern=%d host_t=%.3f baseline_version=%u%s\n",
					arpExpectedPattern, arpRequestedAt, arpBaselineVersion,
					superseded ? " latest_wins=1" : "");
			}
			else if (event.kind == EditorActionKind::SendArpPatternData && needArgs(1))
				processor.sendArpeggioPatternData(event.args[0], event.bytes);
			else if (event.kind == EditorActionKind::RenamePatch)
				processor.renamePatch(juce::String(event.text));
			else if (event.kind == EditorActionKind::SendMacro)
				processor.sendMacro(juce::String(event.text));
			else if (event.kind == EditorActionKind::WritePatch)
				processor.writePatch();
			else if (event.kind == EditorActionKind::PanelPulse && needArgs(2))
				processor.panelPulse(event.args[0], event.args[1]);
			else if (event.kind == EditorActionKind::SetAdin && needArgs(2))
				processor.setAdin(event.args[0], event.args[1]);
			else if (event.kind == EditorActionKind::SetWheel2 && needArgs(1))
				processor.setWheel2FromEditor(event.args[0]);
			else if (event.kind == EditorActionKind::SetCcMap && needArgs(2))
				processor.setCcMap(event.args[0], event.args[1]);
			else if (event.kind == EditorActionKind::SendMidi && !event.bytes.empty())
				processor.sendMidi(event.bytes.data(), event.bytes.size());
			else
			{
				healthFailed = true;
				healthFailure = "invalid arguments for scenario action " + event.op;
			}
			std::fprintf(stderr, "[editor-action] op=%s host_t=%.6f\n",
				event.op.c_str(), (double)event.sample / rate);
		}
		if (stress && !healthFailed && !healthPending && nextHealthAt < blockEndSeconds
				&& nextHealthAt + healthTimeout < durationSeconds)
		{
			std::uint8_t previousDump[1024];
			processor.getProgramData(previousDump, sizeof(previousDump), &healthBaselineVersion);
			const std::uint8_t request[] = {0xf0, 0x42, 0x30, 0x41, 0x10, 0x00, 0xf7};
			const int requestOffset = std::clamp(
				(int)std::llround((nextHealthAt - blockStartSeconds) * rate), 0, block - 1);
			midi.addEvent(request, (int)sizeof(request), requestOffset);
			healthRequestedAt = blockStartSeconds + (double)requestOffset / rate;
			healthPending = true;
			++healthAttempts;
			std::fprintf(stderr, "[stress-health] request=%llu host_t=%.3f baseline_version=%u\n",
				(unsigned long long)healthAttempts, healthRequestedAt, healthBaselineVersion);
		}
		audio.clear();
		{
			ProcessBlockAllocationScope scope;
			processor.processBlock(audio, midi);
		}
		for (int channel = 0; channel < audio.getNumChannels(); ++channel)
			for (int sample = 0; sample < audio.getNumSamples(); ++sample)
				audioPeak = std::max(audioPeak, std::abs(audio.getSample(channel, sample)));
		const auto diagnostic = processor.diagnosticSnapshot();
		const double nowSeconds = (double)blockEnd / rate;
		if (diagnostic.producedFrames > lastProducedFrames)
		{
			lastProducedFrames = diagnostic.producedFrames;
			lastEngineProgressAt = nowSeconds;
		}
		else if (stress && !healthFailed && nowSeconds > 12.0
				&& nowSeconds - lastEngineProgressAt > healthTimeout)
		{
			healthFailed = true;
			healthFailure = "audio engine stopped producing frames";
			std::fprintf(stderr, "[stress-health] FAIL host_t=%.3f produced_frames=%llu\n",
				nowSeconds, (unsigned long long)diagnostic.producedFrames);
		}
		ProphecyEngine::MidiTxByteEvent observed[512];
		for (std::size_t n; (n = processor.popMidiTxByteEvents(observed, 512)) > 0; )
			for (std::size_t i = 0; i < n; ++i)
			{
				std::fprintf(capture, "{\"t\":%.9f,\"byte\":%u}\n",
					observed[i].emuSeconds, (unsigned)observed[i].byte);
				++captured;
			}
		if (stress && healthPending && !healthFailed)
		{
			std::uint8_t dump[1024];
			std::uint32_t version = 0;
			std::uint64_t completedGeneration = 0;
			const std::size_t dumpBytes = processor.getProgramData(
				dump, sizeof(dump), &version, &completedGeneration);
			if (version > healthBaselineVersion && (!editorRecoveryPending
					|| completedGeneration >= editorRecoveryGeneration))
			{
				const double latency = nowSeconds - healthRequestedAt;
				if (dumpBytes < 535)
				{
					healthFailed = true;
					healthFailure = "fresh current-program dump was shorter than 535 unpacked bytes";
				}
				else
				{
					++healthReplies;
					consecutiveHealthMisses = 0;
					maxHealthLatency = std::max(maxHealthLatency, latency);
					healthPending = false;
					if (editorRecoveryPending)
					{
						editorRecoveryReplied = true;
						editorRecoveryPending = false;
						std::fprintf(stderr,
							"[editor-recovery] reply host_t=%.3f latency=%.3f version=%u\n",
							nowSeconds, latency, version);
					}
					nextHealthAt = scenario == "rapid-patch-browse"
						? durationSeconds + healthInterval : nowSeconds + healthInterval;
					std::fprintf(stderr,
						"[stress-health] reply=%llu host_t=%.3f latency=%.3f bytes=%zu version=%u\n",
						(unsigned long long)healthReplies, nowSeconds, latency, dumpBytes, version);
				}
			}
			else if (nowSeconds - healthRequestedAt > healthTimeout)
			{
				++healthMisses;
				++consecutiveHealthMisses;
				worstConsecutiveHealthMisses = std::max(
					worstConsecutiveHealthMisses, consecutiveHealthMisses);
				if (editorRecoveryPending
						|| consecutiveHealthMisses > (std::uint64_t)maxHealthMisses)
				{
					healthFailed = true;
					healthFailure = editorRecoveryPending
						? "rapid patch browsing lost its post-burst program dump"
						: "too many consecutive current-program dump timeouts";
					std::fprintf(stderr, "[stress-health] FAIL host_t=%.3f attempt=%llu consecutive=%llu\n",
						nowSeconds, (unsigned long long)healthAttempts,
						(unsigned long long)consecutiveHealthMisses);
				}
				else
				{
					std::fprintf(stderr,
						"[stress-health] MISS attempt=%llu host_t=%.3f consecutive=%llu; retrying\n",
						(unsigned long long)healthAttempts, nowSeconds,
						(unsigned long long)consecutiveHealthMisses);
					healthPending = false;
					nextHealthAt = nowSeconds;
				}
			}
		}
		if (stress && arpRecoveryPending && !healthFailed)
		{
			std::uint8_t dump[128];
			std::uint32_t version = 0;
			int pattern = -1;
			const std::size_t bytes = processor.getArpeggioPatternData(
				dump, sizeof(dump), &version, &pattern);
			if (version > arpBaselineVersion)
			{
				const double latency = nowSeconds - arpRequestedAt;
				if (bytes != sizeof(dump) || pattern != arpExpectedPattern)
				{
					healthFailed = true;
					healthFailure = "arp dump reply was stale, short, or for the wrong pattern";
				}
				else
				{
					arpRecoveryPending = false;
					arpRecoveryReplied = true;
					arpMaxLatency = std::max(arpMaxLatency, latency);
					std::fprintf(stderr,
						"[arp-recovery] reply pattern=%d host_t=%.3f latency=%.3f bytes=%zu version=%u\n",
						pattern, nowSeconds, latency, bytes, version);
				}
			}
			else if (nowSeconds - arpRequestedAt > healthTimeout)
			{
				healthFailed = true;
				healthFailure = "arp dump freshness probe timed out";
				std::fprintf(stderr, "[arp-recovery] FAIL pattern=%d host_t=%.3f\n",
					arpExpectedPattern, nowSeconds);
			}
		}
		if (allocationStats && blockEnd >= (std::int64_t)std::llround(allocationSecond * rate))
		{
			const std::uint64_t allocations = g_allocationCount.load(std::memory_order_relaxed);
			const std::uint64_t bytes = g_allocationBytes.load(std::memory_order_relaxed);
			const std::uint64_t processBlockAllocations =
				g_processBlockAllocationCount.load(std::memory_order_relaxed);
			std::fprintf(stderr,
				"[alloc] host_second=%d allocations=%llu bytes=%llu process_block=%llu\n",
				allocationSecond,
				(unsigned long long)(allocations - previousAllocations),
				(unsigned long long)(bytes - previousBytes),
				(unsigned long long)(processBlockAllocations - previousProcessBlockAllocations));
			previousAllocations = allocations;
			previousBytes = bytes;
			previousProcessBlockAllocations = processBlockAllocations;
			++allocationSecond;
		}
		next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
		std::this_thread::sleep_until(next);
	}
	const auto finalDiagnostic = processor.diagnosticSnapshot();
	const bool editorPacerBounded = finalDiagnostic.editorCommandsDropped == 0
		&& finalDiagnostic.editorCommandsPending == 0;
	if (stress && !healthFailed && !editorPacerBounded)
	{
		healthFailed = true;
		healthFailure = "editor command pacer ended with dropped or pending work";
	}
	std::fprintf(stderr,
		"[editor-command-pacer] sent=%llu coalesced=%llu cancelled=%llu "
		"dropped=%llu pending=%zu verdict=%s\n",
		(unsigned long long)finalDiagnostic.editorCommandsSent,
		(unsigned long long)finalDiagnostic.editorCommandsCoalesced,
		(unsigned long long)finalDiagnostic.editorCommandsCancelled,
		(unsigned long long)finalDiagnostic.editorCommandsDropped,
		finalDiagnostic.editorCommandsPending,
		editorPacerBounded ? "PASS" : "FAIL");
	if (scenario == "rapid-patch-browse")
	{
		const bool schedulerBounded = finalDiagnostic.editorPatchIntents == editorEvents.size() - 1
			&& finalDiagnostic.editorPatchSends == 1
			&& finalDiagnostic.editorDumpRequests == 1
			&& finalDiagnostic.editorDumpSends >= 1
			&& finalDiagnostic.editorDumpSends <= 3;
		if (!healthFailed && !schedulerBounded)
		{
			healthFailed = true;
			healthFailure = "rapid patch browsing violated the bounded editor scheduler contract";
		}
		std::fprintf(stderr,
			"[editor-scheduler] patch_intents=%llu patch_sends=%llu dump_requests=%llu "
			"dump_sends=%llu verdict=%s\n",
			(unsigned long long)finalDiagnostic.editorPatchIntents,
			(unsigned long long)finalDiagnostic.editorPatchSends,
			(unsigned long long)finalDiagnostic.editorDumpRequests,
			(unsigned long long)finalDiagnostic.editorDumpSends,
			schedulerBounded ? "PASS" : "FAIL");
	}
	if (scenario == "rapid-patch-browse" && !healthFailed
			&& (!editorRecoveryRequested || !editorRecoveryReplied))
	{
		healthFailed = true;
		healthFailure = "rapid patch browsing did not complete its recovery probe";
	}
	if (scenario == "rapid-patch-browse" && !healthFailed && !programValues.empty())
	{
		char line1[64] = {0}, line2[64] = {0};
		const int finalProgram = programValues.back();
		const char bank = finalProgram < 64 ? 'A' : 'B';
		char expected[8] = {0};
		std::snprintf(expected, sizeof(expected), "%c%02d:", bank, finalProgram % 64);
		if (!processor.getLcd(line1, line2, sizeof(line1))
				|| std::string(line1).rfind(expected, 0) != 0)
		{
			healthFailed = true;
			healthFailure = "rapid patch browsing did not settle on the final requested patch";
		}
		std::fprintf(stderr, "[editor-recovery] final_lcd=\"%s\" expected=%s verdict=%s\n",
			line1, expected, healthFailed ? "FAIL" : "PASS");
	}
	if (stress && !healthFailed && (healthPending || healthReplies == 0))
	{
		healthFailed = true;
		healthFailure = healthPending
			? "host run ended with an outstanding current-program dump"
			: "host run ended before any current-program dump completed";
	}
	if (stress && !healthFailed && arpRecoveryRequested
			&& (arpRecoveryPending || !arpRecoveryReplied))
	{
		healthFailed = true;
		healthFailure = "host run ended before the arp dump freshness probe completed";
	}
	if (stress && !healthFailed && processor.writeInProgress())
	{
		healthFailed = true;
		healthFailure = "host run ended while an editor write sequence was still active";
	}
	if (stress && !healthFailed && consecutiveHealthMisses != 0)
	{
		healthFailed = true;
		healthFailure = "host run ended before timed-out health checks recovered";
	}
	if (stress && !healthFailed && !finalDiagnostic.engineRunning)
	{
		healthFailed = true;
		healthFailure = "audio engine was no longer running at the end of the host soak";
	}
	if (stress && !healthFailed
			&& (finalDiagnostic.activeNotesLow != 0 || finalDiagnostic.activeNotesHigh != 0))
	{
		healthFailed = true;
		healthFailure = "host note tracker still contained active notes at the end of the soak";
	}
	if (stress && !healthFailed && audioPeak < 1.0e-6f)
	{
		healthFailed = true;
		healthFailure = "host soak produced no nonzero audio";
	}
	processor.releaseResources();
	std::fclose(capture);
	const std::uint64_t scheduledDrops = processor.droppedScheduledMidiBytes();
	const std::uint64_t immediateDrops = processor.droppedImmediateMidiBytes();
	std::fprintf(stderr, "rate=%.1f block=%d seconds=%.1f host_events=%zu editor_events=%zu captured=%llu "
		"output_dropped=%llu input_dropped=%llu scheduled_input_dropped=%llu "
		"immediate_input_dropped=%llu ui_adin_dropped=%llu audio_adin_dropped=%llu "
		"oversized_blocks=%llu\n",
		rate, block, durationSeconds, events.size(), editorEvents.size(),
		(unsigned long long)captured,
		(unsigned long long)processor.droppedMidiTxByteEvents(),
		(unsigned long long)(scheduledDrops + immediateDrops),
		(unsigned long long)scheduledDrops, (unsigned long long)immediateDrops,
		(unsigned long long)processor.droppedUiAdinEvents(),
		(unsigned long long)processor.droppedAudioAdinEvents(),
		(unsigned long long)processor.oversizedAudioBlocks());
	std::fprintf(stderr, "[editor-clock-gate] suppressed=%llu patch_load_midi=%llu\n",
		(unsigned long long)finalDiagnostic.editorClockTicksSuppressed,
		(unsigned long long)finalDiagnostic.patchLoadMidiEventsSuppressed);
	if (stress)
	{
		std::fprintf(stderr,
			"[stress] CONFIG scenario=%s seed=0x%08x rate=%.1f block=%d phase_samples=%d seconds=%.1f "
			"health_interval=%.3f health_timeout=%.3f program_start=%.3f "
			"program_interval=%.3f program_values=%zu max_health_misses=%d "
			"traffic=setup:%d,clock:%d,notes:%d,controls:%d,params:%d,programs:%d\n",
			scenario.c_str(), configuredStressSeed, rate, block, phaseSamples, durationSeconds,
			healthInterval, healthTimeout, programStart, programInterval, programValues.size(),
			maxHealthMisses,
			setupTraffic, clockTraffic, noteTraffic, controlTraffic,
			parameterTraffic, programTraffic);
		std::fprintf(stderr,
			"[stress] AUDIO peak=%.9f produced_frames=%llu callbacks=%llu underrun_frames=%llu "
			"active_notes=%016llx/%016llx engine_running=%d\n",
			audioPeak, (unsigned long long)finalDiagnostic.producedFrames,
			(unsigned long long)finalDiagnostic.audioCallbacks,
			(unsigned long long)finalDiagnostic.audioUnderrunFrames,
			(unsigned long long)finalDiagnostic.activeNotesHigh,
			(unsigned long long)finalDiagnostic.activeNotesLow,
			finalDiagnostic.engineRunning ? 1 : 0);
		std::fprintf(stderr,
			"[stress-health] SUMMARY attempts=%llu replies=%llu misses=%llu "
			"worst_consecutive_misses=%llu max_latency=%.3f verdict=%s%s%s%s\n",
			(unsigned long long)healthAttempts, (unsigned long long)healthReplies,
			(unsigned long long)healthMisses,
			(unsigned long long)worstConsecutiveHealthMisses, maxHealthLatency,
			healthFailed ? "FAIL" : "PASS", healthFailed ? " reason=\"" : "",
			healthFailed ? healthFailure.c_str() : "", healthFailed ? "\"" : "");
		std::fprintf(stderr,
			"[arp-recovery] SUMMARY requested=%d replied=%d pending=%d max_latency=%.3f verdict=%s\n",
			arpRecoveryRequested ? 1 : 0, arpRecoveryReplied ? 1 : 0,
			arpRecoveryPending ? 1 : 0, arpMaxLatency,
			(!arpRecoveryRequested || (arpRecoveryReplied && !arpRecoveryPending)) ? "PASS" : "FAIL");
	}
	return processor.droppedMidiTxByteEvents() == 0
		&& scheduledDrops == 0 && immediateDrops == 0
		&& processor.droppedUiAdinEvents() == 0
		&& processor.droppedAudioAdinEvents() == 0
		&& processor.oversizedAudioBlocks() == 0
		&& !healthFailed ? 0 : 1;
}
