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

struct ParameterListener final : juce::AudioProcessorParameter::Listener
{
	void parameterValueChanged(int, float) override { ++values; }
	void parameterGestureChanged(int, bool starting) override
	{
		if (starting) ++begins;
		else ++ends;
	}
	int values = 0;
	int begins = 0;
	int ends = 0;
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
	if (processor.getNumPrograms() != ProphecyAudioProcessor::kProgramCount
			|| processor.getCurrentProgram() != 0
			|| processor.getProgramName(0) != "A00"
			|| processor.getProgramName(63) != "A63"
			|| processor.getProgramName(64) != "B00"
			|| processor.getProgramName(127) != "B63")
	{
		std::fprintf(stderr, "host program-list identity failed\n");
		return 1;
	}
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

int main(int argc, char** argv)
{
	if (argc == 2 && std::string(argv[1]) == "--active") return activeAudioTest();
#if defined(_WIN32)
	_putenv_s("PROPHECY_FORCE_NO_ROM", "1");
#else
	setenv("PROPHECY_FORCE_NO_ROM", "1", 1);
#endif
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	ProphecyAudioProcessor processor;
	juce::MidiBuffer emptyMidi;
	const std::array<const char *, ProphecyAudioProcessor::kPerformanceParameterCount> expectedIds {{
		"perf.speed", "perf.knob1", "perf.knob2", "perf.knob3", "perf.knob4",
		"perf.knob5", "perf.wheel1", "perf.wheel2", "perf.ribbonX", "perf.logY",
		"perf.ribbonZ"
	}};
	const std::array<int, ProphecyAudioProcessor::kPerformanceParameterCount> expectedDefaults {{
		0x01, 0, 0, 0, 0, 0, 0x80, 0x80, 0x80, 0x74, 0
	}};
	const auto &parameters = processor.getParameters();
	if (parameters.size() != ProphecyAudioProcessor::kPerformanceParameterCount)
	{
		std::fprintf(stderr, "host parameter count is %d, expected %d\n", parameters.size(),
			ProphecyAudioProcessor::kPerformanceParameterCount);
		return 1;
	}
	for (int i = 0; i < ProphecyAudioProcessor::kPerformanceParameterCount; ++i)
	{
		auto *ranged = dynamic_cast<juce::RangedAudioParameter *>(parameters[i]);
		auto *integer = dynamic_cast<juce::AudioParameterInt *>(parameters[i]);
		if (ranged == nullptr || integer == nullptr || ranged->getParameterID() != expectedIds[(std::size_t)i]
				|| integer->get() != expectedDefaults[(std::size_t)i])
		{
			std::fprintf(stderr, "host parameter %d identity/default mismatch\n", i);
			return 1;
		}
	}

	// Editor writes must be host-visible and bracketed by automation gestures. Conversely,
	// a host parameter write must immediately reach the editor's controller snapshot.
	ParameterListener listener;
	parameters[8]->addListener(&listener);
	processor.beginAdinGesture(12);
	processor.setAdin(12, 37);
	processor.endAdinGesture(12);
	parameters[8]->removeListener(&listener);
	if (dynamic_cast<juce::AudioParameterInt *>(parameters[8])->get() != 37
			|| listener.values != 1 || listener.begins != 1 || listener.ends != 1)
	{
		std::fprintf(stderr, "editor/host automation gesture contract failed\n");
		return 1;
	}
	auto *hostY = dynamic_cast<juce::RangedAudioParameter *>(parameters[9]);
	hostY->setValue(hostY->getNormalisableRange().convertTo0to1(203.0f));
	std::uint8_t controllerValues[16] = {};
	processor.controllerDisplaySnapshot(controllerValues);
	if (controllerValues[12] != 37 || controllerValues[13] != 203)
	{
		std::fprintf(stderr, "host parameter change did not update controller display\n");
		return 1;
	}
	const std::uint8_t expectedControllerDefaults[16] = {
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa6,
		0x80, 0x80, 0x00, 0x00, 0x80, 0x74, 0x00, 0x00 };
	// Restore the two values changed above before checking the complete physical defaults.
	processor.setAdin(12, 0x80);
	hostY->setValue(hostY->getNormalisableRange().convertTo0to1(0x74));
	processor.controllerDisplaySnapshot(controllerValues);
	if (!std::equal(controllerValues, controllerValues + 16, expectedControllerDefaults))
	{
		std::fprintf(stderr, "controller display defaults are wrong\n");
		return 1;
	}

	// PRP4 round-trips all host controls plus the selected program. PRP1/2/3 and bare
	// SysEx retain their old layouts and defaults without scheduling a factory-program
	// load that could overwrite their authoritative edit-buffer dump.
	for (int i = 0; i < ProphecyAudioProcessor::kPerformanceParameterCount; ++i)
	{
		auto *ranged = dynamic_cast<juce::RangedAudioParameter *>(parameters[i]);
		ranged->setValue(ranged->getNormalisableRange().convertTo0to1((float)(i * 19 + 7)));
	}
	processor.setCcMap(74, (int)ProphecyAudioProcessor::CcTarget::PadX);
	processor.selectPatch(116);
	juce::MemoryBlock state;
	processor.getStateInformation(state);
	if (state.getSize() < 5 || std::memcmp(state.getData(), "PRP4", 4) != 0)
	{
		std::fprintf(stderr, "state writer did not emit PRP4\n");
		return 1;
	}
	ProphecyAudioProcessor restored;
	// A host is allowed to publish its default program before component state.
	// The state load must cancel that stale intent and restore the saved selection.
	restored.setCurrentProgram(17);
	restored.setStateInformation(state.getData(), (int)state.getSize());
	if (restored.ccMapTarget(74) != (int)ProphecyAudioProcessor::CcTarget::PadX
			|| restored.getCurrentProgram() != 116)
	{
		std::fprintf(stderr, "PRP4 CC mapping/program did not round-trip\n");
		return 1;
	}
	for (int i = 0; i < ProphecyAudioProcessor::kPerformanceParameterCount; ++i)
		if (dynamic_cast<juce::AudioParameterInt *>(restored.getParameters()[i])->get() != i * 19 + 7)
		{
			std::fprintf(stderr, "PRP4 parameter %d did not round-trip\n", i);
			return 1;
		}
	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
	ProphecyAudioProcessor standaloneRestore;
	juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);
	standaloneRestore.setStateInformation(state.getData(), (int)state.getSize());
	if (standaloneRestore.getCurrentProgram() != 0)
	{
		std::fprintf(stderr, "standalone incorrectly restored DAW program selection\n");
		return 1;
	}
	for (int i = 0; i < ProphecyAudioProcessor::kPerformanceParameterCount; ++i)
	{
		const int expected = i == 7 ? 7 * 19 + 7 : expectedDefaults[(std::size_t)i];
		if (dynamic_cast<juce::AudioParameterInt *>(standaloneRestore.getParameters()[i])->get() != expected)
		{
			std::fprintf(stderr, "standalone persistence policy failed at parameter %d\n", i);
			return 1;
		}
	}
	const std::uint8_t prp2State[] = {'P','R','P','2',0,211};
	restored.setStateInformation(prp2State, (int)sizeof(prp2State));
	for (int i = 0; i < ProphecyAudioProcessor::kPerformanceParameterCount; ++i)
	{
		const int expected = i == 7 ? 211 : expectedDefaults[(std::size_t)i];
		if (dynamic_cast<juce::AudioParameterInt *>(restored.getParameters()[i])->get() != expected)
		{
			std::fprintf(stderr, "PRP2 migration failed at parameter %d\n", i);
			return 1;
		}
	}
	const std::uint8_t prp3State[] = {'P','R','P','3',0,0};
	restored.setStateInformation(prp3State, (int)sizeof(prp3State));
	if (restored.getCurrentProgram() != 116)
	{
		std::fprintf(stderr, "PRP3 migration changed the selected program\n");
		return 1;
	}
	const std::uint8_t prp1State[] = {'P','R','P','1',0};
	restored.setStateInformation(prp1State, (int)sizeof(prp1State));
	for (int i = 0; i < ProphecyAudioProcessor::kPerformanceParameterCount; ++i)
		if (dynamic_cast<juce::AudioParameterInt *>(restored.getParameters()[i])->get()
				!= expectedDefaults[(std::size_t)i])
		{
			std::fprintf(stderr, "PRP1 migration failed at parameter %d\n", i);
			return 1;
		}
	dynamic_cast<juce::RangedAudioParameter *>(restored.getParameters()[0])->setValue(1.0f);
	const std::uint8_t bareState[] = {0xf0, 0x7d, 0xf7};
	restored.setStateInformation(bareState, (int)sizeof(bareState));
	for (int i = 0; i < ProphecyAudioProcessor::kPerformanceParameterCount; ++i)
		if (dynamic_cast<juce::AudioParameterInt *>(restored.getParameters()[i])->get()
				!= expectedDefaults[(std::size_t)i])
		{
			std::fprintf(stderr, "legacy state defaults failed at parameter %d\n", i);
			return 1;
		}

	constexpr double rates[] = { 44100.0, 48000.0, 96000.0 };
	constexpr int legalBlockSizes[] = { 1, 17, 64, 511, 512, 1024, 4096, 16384 };
	for (double rate : rates)
	{
		processor.prepareToPlay(rate, 16384);
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
