// Realtime allocation and queue guards; --active uses an isolated firmware fixture.
#include "PluginProcessor.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {
thread_local bool g_watch_allocations = false;
std::atomic<std::uint64_t> g_watched_allocations { 0 };

struct WatchAllocations
{
	WatchAllocations() { g_watch_allocations = true; }
	~WatchAllocations() { g_watch_allocations = false; }
};

void *allocate(std::size_t size)
{
	if (g_watch_allocations)
		g_watched_allocations.fetch_add(1, std::memory_order_relaxed);
	if (void *p = std::malloc(size == 0 ? 1 : size)) return p;
	throw std::bad_alloc();
}

bool processWithoutAllocation(ProphecyAudioProcessor &processor,
	juce::AudioBuffer<float> &audio, juce::MidiBuffer &midi)
{
	const auto before = g_watched_allocations.load(std::memory_order_relaxed);
	{
		WatchAllocations watch;
		processor.processBlock(audio, midi);
	}
	return g_watched_allocations.load(std::memory_order_relaxed) == before;
}
} // namespace

void *operator new(std::size_t size) { return allocate(size); }
void *operator new[](std::size_t size) { return allocate(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

static int activeAudioTest()
{
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	ProphecyAudioProcessor processor;
	processor.setCcMap(1, (int) ProphecyAudioProcessor::CcTarget::Wheel1);
	juce::MidiBuffer empty, midi;
	midi.ensureSize(4096);
	const std::uint8_t note[] = {0x90, 60, 64};
	const std::uint8_t cc[] = {0xb0, 1, 64};
	std::array<std::uint8_t, 600> sysex{};
	sysex[0] = 0xf0; sysex[1] = 0x7d; sysex.back() = 0xf7;
	midi.addEvent(note, sizeof(note), 0);
	midi.addEvent(cc, sizeof(cc), 0);
	midi.addEvent(sysex.data(), (int) sysex.size(), 0);
	juce::AudioBuffer<float> block(2, 512);
	for (double rate : {48000.0, 44100.0, 96000.0, 192000.0})
	for (int prepared : {64, 128, 512})
	{
		processor.prepareToPlay(rate, prepared);
		if (!processor.diagnosticSnapshot().engineRunning)
		{
			std::fprintf(stderr, "active allocation test requires a running engine and isolated ROM/NVRAM fixture\n");
			return 1;
		}
		for (int count : {1, 17, 64, 127, 511, 512})
		{
			// Prime the same real worker and ring off the watched realtime callback.
			// This avoids turning a no-allocation test into a scheduling benchmark.
			block.setSize(2, prepared, false, false, true);
			processor.setNonRealtime(true);
			for (int prime = 0; prime < 16; ++prime) processor.processBlock(block, empty);
			processor.setNonRealtime(false);
			block.setSize(2, std::min(count, prepared), false, false, true);
			const auto before = processor.diagnosticSnapshot();
			if (!processWithoutAllocation(processor, block, midi)
				|| processor.diagnosticSnapshot().audioEngineFrames <= before.audioEngineFrames
				|| processor.diagnosticSnapshot().audioUnderrunFrames != before.audioUnderrunFrames)
			{
				std::fprintf(stderr, "active read/resampling allocated or failed to consume PCM: rate=%.0f block=%d\n", rate, count);
				return 1;
			}
		}
	}
	std::puts("PASS active realtime allocation guard: PCM reads, resampling, MIDI SysEx and timed ADIN");
	return 0;
}

// Native private-fixture regression: restoration across host lifecycle orders
// must be acknowledged and read back before a note at host sample 144 (3ms).
static int initialStateTest(const char* path, const std::string& order)
{
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	juce::MemoryBlock state;
	if (!juce::File(path).loadFileAsData(state) || state.getSize() < 8) return 2;
	const auto* bytes = static_cast<const std::uint8_t*>(state.getData());
	std::vector<std::uint8_t> expected;
	for (std::size_t i = 6; i + 1 < state.getSize(); )
	{
		const auto high = bytes[i++];
		for (int bit = 0; bit < 7 && i + 1 < state.getSize(); ++bit)
			expected.push_back((std::uint8_t) (bytes[i++] | (((high >> bit) & 1) << 7)));
	}
	ProphecyAudioProcessor processor;
	processor.setNonRealtime(true);
	if (order != "before" && order != "after" && order != "zero" && order != "reprepare"
		&& order != "before-reprepare" && order != "released" && order != "live") return 2;
	if (order == "before") processor.setStateInformation(state.getData(), (int) state.getSize());
	processor.prepareToPlay(48000, 128);
	juce::MidiBuffer midi;
	juce::AudioBuffer<float> audio(2, order == "zero" ? 0 : 128);
	if (order == "live") midi.addEvent(juce::MidiMessage::noteOn(1, 48, (juce::uint8) 64), 127);
	if (order != "before" && order != "after") processor.processBlock(audio, midi);
	midi.clear();
	if (order == "reprepare" || order == "released")
		processor.releaseResources();
	if (order == "reprepare") processor.prepareToPlay(48000, 128);
	if (order != "before") processor.setStateInformation(state.getData(), (int) state.getSize());
	if (order == "before-reprepare" || order == "released") processor.prepareToPlay(48000, 128);
	std::uint8_t actual[1024]{};
	std::uint32_t version = 0;
	const auto count = processor.getProgramData(actual, sizeof(actual), &version);
	if (!processor.romOk() || !version || count != expected.size()
		|| !std::equal(expected.begin(), expected.end(), actual))
	{
		std::fprintf(stderr, "initial state readback failed: %s\n", processor.initializationError());
		return 1;
	}
	juce::MemoryBlock saved;
	processor.getStateInformation(saved);
	if (saved.getSize() < state.getSize() || std::memcmp(
		static_cast<const char*>(saved.getData()) + saved.getSize() - state.getSize(),
		state.getData(), state.getSize()) != 0)
	{
		std::fprintf(stderr, "save before the first callback lost the restored program\n");
		return 1;
	}
	audio.setSize(2, 128);
	int onset = -1;
	for (int first = 0; first < 48000; first += 128)
	{
		midi.clear();
		if (144 >= first && 144 < first + 128)
			midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 64), 144 - first);
		if (9744 >= first && 9744 < first + 128)
			midi.addEvent(juce::MidiMessage::noteOff(1, 60), 9744 - first);
		processor.processBlock(audio, midi);
		for (int i = 0; i < 128; ++i)
		{
			if (!std::isfinite(audio.getSample(0, i)) || !std::isfinite(audio.getSample(1, i))) return 1;
			if (onset < 0 && std::max(std::abs(audio.getSample(0, i)), std::abs(audio.getSample(1, i))) >= .0001f)
				onset = first + i;
		}
	}
	if (onset < 144 + processor.getLatencySamples() || onset > 144 + 4800)
	{
		std::fprintf(stderr, "initial restored note onset invalid: %d\n", onset);
		return 1;
	}
	std::printf("PASS state lifecycle %s: %zu-byte readback, immediate save, first note %.3fms\n",
		order.c_str(), count, (onset - 144) / 48.0);
	return 0;
}

static int pendingStateTest()
{
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	ProphecyAudioProcessor processor;
	processor.setNonRealtime(true);
	processor.prepareToPlay(48000, 128);
	if (!processor.romOk()) return 1;
	for (const auto& [parameter, value] : {std::pair{1,int('Q')}, {635,99},
		{635,32}, {154,1}, {154,0}, {1,int('R')}}) processor.setParam(parameter, value);
	const auto stoppedFrame = processor.diagnosticSnapshot().producedFrames;
	juce::MemoryBlock pending;
	processor.getStateInformation(pending);
	if (pending.getSize() <= 6 || std::memcmp(pending.getData(), "PRP3", 4) != 0
		|| processor.diagnosticSnapshot().producedFrames != stoppedFrame) return 1;
	juce::MidiBuffer midi;
	juce::AudioBuffer<float> audio(2, 128);
	const auto request = processor.requestProgramDump();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	std::array<std::uint8_t, 535> live{}, restored{};
	std::uint64_t completed = 0;
	for (int block = 0; completed < request; ++block)
	{
		if (std::chrono::steady_clock::now() >= deadline || !processor.programError().empty())
		{
			std::fprintf(stderr, "pending live edits: %s\n", processor.programError().c_str());
			return 1;
		}
		processor.processBlock(audio, midi);
		processor.getProgramData(live.data(), live.size(), nullptr, &completed);
		if (block % 8 == 0) juce::Thread::sleep(1);
	}
	processor.releaseResources();
	processor.setStateInformation(pending.getData(), int(pending.getSize()));
	if (!processor.romOk() || processor.getProgramData(restored.data(), restored.size(), nullptr) != live.size()
		|| live != restored || live[0] != 'R') return 1;
	juce::MemoryBlock confirmed;
	processor.getStateInformation(confirmed);
	if (confirmed.getSize() <= 6 || std::memcmp(confirmed.getData(), "PRP2", 4) != 0) return 1;
	std::puts("PASS processor immediate PRP3 save and stopped restore match all 535 live program bytes");
	return 0;
}

int main(int argc, char** argv)
{
	if (argc == 2 && std::string(argv[1]) == "--pending-state") return pendingStateTest();
	if (argc == 3 && std::string(argv[1]) == "--export-initial-state")
	{
		juce::ScopedJuceInitialiser_GUI juceInitialiser;
		ProphecyAudioProcessor processor;
		processor.prepareToPlay(48000, 128);
		juce::MemoryBlock state;
		processor.getStateInformation(state);
		if (!processor.romOk() || state.getSize() <= 6) return 1;
		const auto* bytes = static_cast<const std::uint8_t*>(state.getData());
		const std::size_t offset = 6 + 2 * std::size_t(bytes[4]); // PRP2 mapping and wheel preference
		if (offset >= state.getSize()) return 1;
		return juce::File(argv[2]).replaceWithData(bytes + offset, state.getSize() - offset) ? 0 : 1;
	}
	if (argc == 4 && std::string(argv[1]) == "--initial-state")
		return initialStateTest(argv[2], argv[3]);
	if (argc == 2 && std::string(argv[1]) == "--active") return activeAudioTest();
#if defined(_WIN32)
	_putenv_s("PROPHECY_FORCE_NO_ROM", "1");
#else
	setenv("PROPHECY_FORCE_NO_ROM", "1", 1);
#endif
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	ProphecyAudioProcessor processor;
	juce::MidiBuffer emptyMidi;
	std::uint8_t controllerValues[16] = {};
	processor.controllerDisplaySnapshot(controllerValues);
	const std::uint8_t expectedControllerDefaults[16] = {
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa6,
		0x80, 0x80, 0x00, 0x00, 0x80, 0x74, 0x00, 0x00 };
	if (!std::equal(controllerValues, controllerValues + 16, expectedControllerDefaults))
	{
		std::fprintf(stderr, "controller display defaults are wrong\n");
		return 1;
	}

	constexpr double rates[] = { 44100.0, 48000.0, 96000.0 };
	constexpr int legalBlockSizes[] = { 1, 17, 64, 511, 512, 1024, 4096, 16384 };
	for (double rate : rates)
	{
		processor.prepareToPlay(rate, 64);
		for (int frames : legalBlockSizes)
		{
			juce::AudioBuffer<float> stereo(2, frames);
			juce::AudioBuffer<float> mono(1, frames);
			if (!processWithoutAllocation(processor, stereo, emptyMidi)
					|| !processWithoutAllocation(processor, mono, emptyMidi))
			{
				std::fprintf(stderr,
					"allocation observed in processBlock rate=%.1f frames=%d\n", rate, frames);
				return 1;
			}
		}
	}

	// Raw MIDI must remain on the UART path while also driving the corresponding
	// visual controller. Max bend -> PITCH full right; CC1=32 -> MOD ~= 64.
	juce::MidiBuffer visualMidi;
	const std::uint8_t bend[] = { 0xe0, 0x7f, 0x7f };
	const std::uint8_t mod[] = { 0xb0, 0x01, 0x20 };
	visualMidi.addEvent(bend, 3, 0);
	visualMidi.addEvent(mod, 3, 1);
	juce::AudioBuffer<float> visualAudio(2, 64);
	if (!processWithoutAllocation(processor, visualAudio, visualMidi))
	{
		std::fprintf(stderr, "controller display observation allocated in processBlock\n");
		return 1;
	}
	processor.controllerDisplaySnapshot(controllerValues);
	if (controllerValues[8] != 0xff || controllerValues[9] != 64)
	{
		std::fprintf(stderr, "controller display did not follow bend/CC1: pitch=%u mod=%u\n",
			(unsigned)controllerValues[8], (unsigned)controllerValues[9]);
		return 1;
	}

	// The processor reserves at least 16384 frames in prepareToPlay. A larger surprise
	// block must be silenced and counted, never resized on the callback.
	juce::AudioBuffer<float> oversized(2, 16385);
	for (int channel = 0; channel < oversized.getNumChannels(); ++channel)
		for (int sample = 0; sample < oversized.getNumSamples(); ++sample)
			oversized.setSample(channel, sample, 1.0f);
	if (!processWithoutAllocation(processor, oversized, emptyMidi)
			|| processor.oversizedAudioBlocks() != 1)
	{
		std::fprintf(stderr, "oversize block policy failed (count=%llu)\n",
			(unsigned long long) processor.oversizedAudioBlocks());
		return 1;
	}
	for (int channel = 0; channel < oversized.getNumChannels(); ++channel)
		for (int sample = 0; sample < oversized.getNumSamples(); ++sample)
			if (oversized.getSample(channel, sample) != 0.0f)
			{
				std::fprintf(stderr, "oversize block was not silenced\n");
				return 1;
			}

	// Queue overload has an explicit all-or-nothing/drop-counter policy. Claim the engine
	// slot with a no-ROM/nonexistent-system worker so these calls exercise the real global
	// queues without any firmware consumer draining them. The processor above deliberately
	// remains unstarted: a processor that owns no slot must never touch another's queues.
	ProphecyEngine queueEngine;
	const std::vector<std::string> queueArgs = {
		"prophecy", "__prophecy_rt_queue_probe__",
		"-video", "none", "-sound", "none", "-nothrottle", "-skip_gameinfo"
	};
	if (!queueEngine.start(queueArgs) || !queueEngine.ownsMachineSlot())
	{
		std::fprintf(stderr, "queue test engine did not claim machine slot\n");
		return 1;
	}
	std::vector<std::uint8_t> hugeMessage(4097, 0x7f);
	const auto oversizeStart = std::chrono::steady_clock::now();
	if (queueEngine.pushMidi(hugeMessage.data(), hugeMessage.size())
			|| queueEngine.droppedImmediateMidiBytes() != hugeMessage.size()
			|| std::chrono::steady_clock::now() - oversizeStart > std::chrono::milliseconds(250))
	{
		std::fprintf(stderr, "oversize immediate MIDI rejection failed\n");
		return 1;
	}
	// Now prove actual saturation, not merely n>capacity rejection: exactly fill the
	// empty 4096-byte queue with one accepted message, then require a small complete
	// message to be rejected promptly and counted byte-for-byte.
	std::vector<std::uint8_t> fillMessage(4096, 0x7e);
	if (!queueEngine.pushMidi(fillMessage.data(), fillMessage.size()))
	{
		std::fprintf(stderr, "empty immediate MIDI queue rejected exact-capacity message\n");
		return 1;
	}
	const auto beforeSaturatedDrop = queueEngine.droppedImmediateMidiBytes();
	const std::uint8_t smallMessage[] = { 0x90, 0x3c, 0x64 };
	const auto saturatedStart = std::chrono::steady_clock::now();
	if (queueEngine.pushMidi(smallMessage, sizeof(smallMessage))
			|| queueEngine.droppedImmediateMidiBytes() - beforeSaturatedDrop != sizeof(smallMessage)
			|| std::chrono::steady_clock::now() - saturatedStart > std::chrono::milliseconds(250))
	{
		std::fprintf(stderr, "saturated immediate MIDI rejection failed\n");
		return 1;
	}
	// Exercise real ADIN-ring saturation separately from the processor's unavailable
	// rejection below. 4096 bytes hold 2048 complete two-byte records.
	for (int i = 0; i < 3000; ++i)
		(void) queueEngine.pushAdinFromAudio(1, i & 0xff);
	if (queueEngine.droppedAudioAdinEvents() == 0)
	{
		std::fprintf(stderr, "audio ADIN queue did not report real saturation\n");
		return 1;
	}

	processor.setCcMap(1, (int) ProphecyAudioProcessor::CcTarget::Wheel1);
	juce::MidiBuffer adinBurst;
	const std::uint8_t cc[] = { 0xb0, 1, 64 };
	for (int i = 0; i < 3000; ++i) adinBurst.addEvent(cc, 3, i % 64);
	juce::AudioBuffer<float> stereo(2, 64);
	const auto adinStart = std::chrono::steady_clock::now();
	if (!processWithoutAllocation(processor, stereo, adinBurst)
			|| processor.droppedAudioAdinEvents() == 0
			|| std::chrono::steady_clock::now() - adinStart > std::chrono::milliseconds(250))
	{
		std::fprintf(stderr, "audio ADIN bounded-overflow policy failed\n");
		return 1;
	}
	// Display state records the latest host input even though this processor does not own
	// the deliberately saturated engine above; queue rejection remains visible separately
	// through droppedAudioAdinEvents(). It is not presented as an engine acknowledgement.
	processor.controllerDisplaySnapshot(controllerValues);
	if (controllerValues[8] != 129)
	{
		std::fprintf(stderr, "mapped CC did not update its target display: pitch=%u\n",
			(unsigned)controllerValues[8]);
		return 1;
	}
	queueEngine.stop();

	std::fprintf(stderr,
		"PASS rates=3 legal_block_sizes=8 allocations=0 oversize_blocks=%llu immediate_midi_bytes_dropped=%llu "
		"audio_adin_events_dropped=%llu\n",
		(unsigned long long) processor.oversizedAudioBlocks(),
		(unsigned long long) processor.droppedImmediateMidiBytes(),
		(unsigned long long) processor.droppedAudioAdinEvents());
	return 0;
}
