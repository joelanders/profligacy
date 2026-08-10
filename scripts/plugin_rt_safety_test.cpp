// No-ROM regression test for the ProphecyAudioProcessor real-time callback contract.
#include "PluginProcessor.h"

#include <juce_events/juce_events.h>

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

int main()
{
#if defined(_WIN32)
	_putenv_s("PROPHECY_FORCE_NO_ROM", "1");
#else
	setenv("PROPHECY_FORCE_NO_ROM", "1", 1);
#endif
	juce::ScopedJuceInitialiser_GUI juceInitialiser;
	ProphecyAudioProcessor processor;
	juce::MidiBuffer emptyMidi;

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
	queueEngine.stop();

	std::fprintf(stderr,
		"PASS rates=3 legal_block_sizes=8 allocations=0 oversize_blocks=%llu immediate_midi_bytes_dropped=%llu "
		"audio_adin_events_dropped=%llu\n",
		(unsigned long long) processor.oversizedAudioBlocks(),
		(unsigned long long) processor.droppedImmediateMidiBytes(),
		(unsigned long long) processor.droppedAudioAdinEvents());
	return 0;
}
