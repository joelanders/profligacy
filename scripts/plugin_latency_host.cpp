// SPDX-License-Identifier: AGPL-3.0-only
// Packaged AU/VST3 latency capture host for macOS; no firmware or plugin linkage.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "latency_sequence.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstaudioprocessor.h>
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/vst/ivstevents.h>

// Prefetch uses the native VST3 interface so the host
// actually sends kPrefetch (JUCE's ordinary hosting API exposes kOffline only).
class PrefetchEvents final : public Steinberg::Vst::IEventList
{
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
    {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, IEventList::iid)
            || Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid))
        { *object = static_cast<IEventList*>(this); return Steinberg::kResultOk; }
        *object = nullptr; return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
    Steinberg::int32 PLUGIN_API getEventCount() override { return count; }
    Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, Steinberg::Vst::Event& event) override
    {
        if (index < 0 || index >= count) return Steinberg::kInvalidArgument;
        event = events[index]; return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& event) override
    {
        if (count == 16) return Steinberg::kOutOfMemory;
        events[count++] = event; return Steinberg::kResultOk;
    }
    Steinberg::Vst::Event events[16]{};
    int count = 0;
};

struct BlockRecord { int64_t sample; int size; double lateMs; double renderMs; int latency; bool realtime; };

// AU lookup normally resolves a type/manufacturer/subtype through the system
// registry, even when JUCE was given a bundle path. Register the exact bundle's
// factory under a process-local test subtype to avoid testing an installed copy.
struct LocalAU
{
    ~LocalAU() { if (bundle) CFRelease(bundle); }
    bool open(const juce::String& path)
    {
        const auto* utf8 = path.toRawUTF8();
        auto url = CFURLCreateFromFileSystemRepresentation(nullptr,
            reinterpret_cast<const UInt8*>(utf8), (CFIndex) std::strlen(utf8), true);
        if (!url) return false;
        bundle = CFBundleCreate(nullptr, url);
        CFRelease(url);
        if (!bundle) return false;
        auto components = CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioComponents"));
        if (!components || CFGetTypeID(components) != CFArrayGetTypeID()
            || CFArrayGetCount((CFArrayRef) components) != 1) return false;
        auto info = (CFDictionaryRef) CFArrayGetValueAtIndex((CFArrayRef) components, 0);
        if (CFGetTypeID(info) != CFDictionaryGetTypeID()) return false;
        auto string = [&](CFStringRef key) -> CFStringRef {
            auto value = CFDictionaryGetValue(info, key);
            return value && CFGetTypeID(value) == CFStringGetTypeID() ? (CFStringRef) value : nullptr;
        };
        auto type = string(CFSTR("type")), manufacturer = string(CFSTR("manufacturer"));
        auto factoryName = string(CFSTR("factoryFunction"));
        if (!type || !manufacturer || !factoryName) return false;
        const auto typeString = juce::String::fromCFString(type);
        const auto manufacturerString = juce::String::fromCFString(manufacturer);
        auto fourcc = [](const juce::String& s) {
            return (UInt32(s[0]) << 24) | (UInt32(s[1]) << 16) | (UInt32(s[2]) << 8) | UInt32(s[3]);
        };
        if (typeString.length() != 4 || manufacturerString.length() != 4) return false;
        AudioComponentDescription description{};
        description.componentType = fourcc(typeString);
        description.componentManufacturer = fourcc(manufacturerString);
        description.componentSubType = fourcc("Ptl1");
        if (AudioComponentFindNext(nullptr, &description)) return false;
        auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
            CFBundleGetFunctionPointerForName(bundle, factoryName));
        if (!factory) return false;
        Dl_info loaded{};
        if (!dladdr(reinterpret_cast<void*>(factory), &loaded) || !loaded.dli_fname) return false;
        binaryPath = juce::File(loaded.dli_fname).getFullPathName();
        auto executableURL = CFBundleCopyExecutableURL(bundle);
        if (!executableURL) return false;
        auto executablePath = CFURLCopyFileSystemPath(executableURL, kCFURLPOSIXPathStyle);
        CFRelease(executableURL);
        if (!executablePath) return false;
        const bool exact = juce::File(binaryPath) == juce::File(juce::String::fromCFString(executablePath));
        CFRelease(executablePath);
        if (!exact) return false;
        component = AudioComponentRegister(&description, CFSTR("Profligacy: Latency capture"), 65536, factory);
        identifier = "AudioUnit:" + typeString + ",Ptl1," + manufacturerString;
        return component != nullptr;
    }
    CFBundleRef bundle = nullptr;
    AudioComponent component = nullptr;
    juce::String identifier, binaryPath;
};

int main(int argc, char** argv)
{
    if (argc < 12 || argc > 14) {
        std::fprintf(stderr, "usage: latency_host PLUGIN OUTPUT_PREFIX RATE BLOCK SECONDS REPREPARE_SECONDS fixed|variable OFFLINE_AFTER_SECONDS fast|paced|prefetch REALTIME_AFTER_SECONDS MIDI_FILE [INITIAL_STATE_FILE|- [SAVE_INTERVAL_BLOCKS]]\n");
        return 2;
    }
    const juce::String pluginPath(argv[1]), prefix(argv[2]);
    const double rate = std::atof(argv[3]), seconds = std::atof(argv[5]);
    const int block = std::atoi(argv[4]);
    const double reprepare = argc > 6 ? std::atof(argv[6]) : -1;
    const bool variable = argc > 7 && std::string(argv[7]) == "variable";
    const double offlineAfter = argc > 8 ? std::atof(argv[8]) : -1;
    const bool paceOffline = argc > 9 && std::string(argv[9]) == "paced";
    const bool prefetch = argc > 9 && std::string(argv[9]) == "prefetch";
    const double realtimeAfter = argc > 10 ? std::atof(argv[10]) : -1;
    if (!std::isfinite(reprepare) || !std::isfinite(offlineAfter) || !std::isfinite(realtimeAfter)) return 2;
    if (realtimeAfter >= 0 && (!prefetch || realtimeAfter <= offlineAfter || realtimeAfter >= seconds)) return 2;
    if (!std::isfinite(rate) || !std::isfinite(seconds) || rate < 8000 || block < 16
        || seconds <= 0 || seconds * rate > std::numeric_limits<int>::max()) return 2;
    const bool initialStateRequested = argc > 12 && std::string(argv[12]) != "-";
    const int saveInterval = argc > 13 ? std::atoi(argv[13]) : 0;
    // Stop between offline blocks and save on the message thread, as a host may
    // do. These captures test PCM preservation, not realtime deadline timing.
    if (saveInterval < 0 || (saveInterval > 0 && (offlineAfter != 0 || paceOffline || prefetch || realtimeAfter >= 0))) return 2;
    const bool isAU = pluginPath.endsWith(".component");
    if (prefetch && (isAU || offlineAfter < 0 || offlineAfter >= seconds)) return 2;
    std::vector<LatencyEvent> events;
    juce::String sequenceError;
    auto midiInput = juce::File(argv[11]).createInputStream();
    if (!midiInput || !readLatencySequence(*midiInput, rate, seconds, events, sequenceError)) {
        std::fprintf(stderr, "Could not read MIDI sequence: %s\n", sequenceError.toRawUTF8());
        return 2;
    }
    juce::Array<juce::var> noteRecords;
    for (const auto& event : events) {
        // The direct VST3 probe currently implements note events only. Reject
        // unsupported messages before loading a plugin or changing its state.
        if (prefetch && event.sample >= std::llround(offlineAfter * rate)
            && !event.message.isNoteOnOrOff()) {
            std::fprintf(stderr, "Only note events are supported after entering native VST3 prefetch\n");
            return 2;
        }
        if (event.message.isNoteOn()) {
            auto* note = new juce::DynamicObject();
            note->setProperty("sample", (juce::int64) event.sample);
            note->setProperty("seconds", event.message.getTimeStamp());
            noteRecords.add(juce::var(note));
        }
    }
    for (const auto* extension : { ".wav", ".json", ".blocks.csv" })
        if (juce::File(prefix + extension).exists()) {
            std::fprintf(stderr, "Refusing to replace an existing capture\n"); return 2;
        }
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    LocalAU localAU;
    juce::AudioPluginFormatManager formats;
    if (isAU) {
        if (!localAU.open(pluginPath)) {
            std::fprintf(stderr, "Could not register the exact AU bundle for this process\n"); return 1;
        }
        formats.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
    }
    else formats.addFormat(std::make_unique<juce::VST3PluginFormat>());
    juce::OwnedArray<juce::PluginDescription> descriptions;
    formats.getFormat(0)->findAllTypesForFile(descriptions, isAU ? localAU.identifier : pluginPath);
    if (descriptions.size() != 1) {
        std::fprintf(stderr, "Expected one description, got %d\n", descriptions.size());
        return 1;
    }
    juce::String error;
    auto instance = formats.createPluginInstance(*descriptions[0], rate, block, error);
    if (!instance) { std::fprintf(stderr, "%s\n", error.toRawUTF8()); return 1; }
    if (isAU && (!instance->getAudioUnitClient() || AudioComponentInstanceGetComponent(
        instance->getAudioUnitClient()->getAudioUnitHandle()) != localAU.component)) {
        std::fprintf(stderr, "AU instance did not use the requested bundle's factory\n"); return 1;
    }
    instance->setPlayConfigDetails(0, 2, rate, block);
    instance->setNonRealtime(false);
    instance->prepareToPlay(rate, block);
    bool initialStateVerified = false;
    if (initialStateRequested) {
        juce::MemoryBlock state, confirmed;
        if (!juce::File(argv[12]).loadFileAsData(state) || state.isEmpty()) return 2;
        // Hosting wrappers serialize their own containers. Passing raw processor
        // bytes to their setStateInformation would silently do nothing.
        if (isAU) {
            const auto unit = instance->getAudioUnitClient()->getAudioUnitHandle();
            auto read = [&]() -> CFDictionaryRef {
                CFPropertyListRef value = nullptr;
                UInt32 size = sizeof(value);
                if (AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo,
                    kAudioUnitScope_Global, 0, &value, &size) != noErr) return nullptr;
                if (!value || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
                    if (value) CFRelease(value);
                    return nullptr;
                }
                return (CFDictionaryRef) value;
            };
            auto original = read();
            if (!original) return 1;
            auto replacement = CFDictionaryCreateMutableCopy(nullptr, 0, original);
            CFRelease(original);
            auto data = CFDataCreate(nullptr, (const UInt8*) state.getData(), (CFIndex) state.getSize());
            CFDictionarySetValue(replacement, CFSTR("jucePluginState"), data);
            CFRelease(data);
            const auto status = AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo,
                kAudioUnitScope_Global, 0, &replacement, sizeof(replacement));
            CFRelease(replacement);
            if (status != noErr) return 1;
            auto saved = read();
            if (!saved) return 1;
            auto value = CFDictionaryGetValue(saved, CFSTR("jucePluginState"));
            if (value && CFGetTypeID(value) == CFDataGetTypeID())
                confirmed.append(CFDataGetBytePtr((CFDataRef) value), (size_t) CFDataGetLength((CFDataRef) value));
            CFRelease(saved);
        } else {
            juce::XmlElement xml("VST3PluginState");
            xml.createNewChildElement("IComponent")->addTextElement(state.toBase64Encoding());
            juce::MemoryBlock wrapped;
            juce::AudioProcessor::copyXmlToBinary(xml, wrapped);
            instance->setStateInformation(wrapped.getData(), (int) wrapped.getSize());
            wrapped.reset();
            instance->getStateInformation(wrapped);
            auto saved = juce::AudioProcessor::getXmlFromBinary(wrapped.getData(), (int) wrapped.getSize());
            if (saved)
                if (auto* component = saved->getChildByName("IComponent"))
                    confirmed.fromBase64Encoding(component->getAllSubText());
        }
        const auto* first = static_cast<const std::uint8_t*>(confirmed.getData());
        const auto* wanted = static_cast<const std::uint8_t*>(state.getData());
        initialStateVerified = confirmed.getSize() >= state.getSize()
            && std::search(first, first + confirmed.getSize(), wanted, wanted + state.getSize()) != first + confirmed.getSize();
        if (!initialStateVerified) {
            std::fprintf(stderr, "Initial native state did not survive immediate readback\n");
            return 1;
        }
    }
    const int initialLatency = instance->getLatencySamples();
    std::printf("Loaded %s %s; rate=%.0f block=%d reported=%d (%.6f ms)\n",
        descriptions[0]->name.toRawUTF8(), descriptions[0]->pluginFormatName.toRawUTF8(),
        rate, block, initialLatency, initialLatency * 1000.0 / rate);
    std::fflush(stdout);

    const int64_t total = std::llround(seconds * rate);
    juce::AudioBuffer<float> capture(2, (int)total);
    capture.clear();
    juce::AudioBuffer<float> audio(2, block);
    juce::MidiBuffer midi;
    midi.ensureSize(4096);
    std::vector<BlockRecord> blocks;
    blocks.reserve((size_t)(total / std::max(1, block/2) + 100));
    std::atomic<bool> done{false};
    std::atomic<bool> processFailed{false};
    std::atomic<bool> saveRequested{false};
    int stateSaves = 0;
    std::thread render([&] {
        Steinberg::Vst::IAudioProcessor* nativeProcessor = nullptr;
        using Clock = std::chrono::steady_clock;
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        const double ticksPerSecond = 1e9 * tb.denom / tb.numer;
        thread_time_constraint_policy_data_t policy{};
        policy.period = (uint32_t)(ticksPerSecond * block / rate);
        policy.computation = std::min(policy.period/2, (uint32_t)(ticksPerSecond*.001));
        policy.constraint = policy.period;
        policy.preemptible = true;
        const auto policyResult = thread_policy_set(pthread_mach_thread_np(pthread_self()),
            THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy),
            THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        std::printf("Host realtime scheduling result=%d\n", policyResult);
        std::fflush(stdout);
        uint64_t machEpoch = mach_absolute_time();
        auto epoch = Clock::now();
        size_t eventIndex = 0, blockIndex = 0;
        bool reprepared = false;
        bool offline = false;
        bool enteredPrefetch = false;
        for (int64_t start = 0; start < total; ++blockIndex) {
            const int sizes[] = {block, std::max(1, block/2), block-1, std::max(1, block/3)};
            const int count = (int)std::min<int64_t>(variable ? sizes[blockIndex % 4] : block, total - start);
            if (prefetch && offline && realtimeAfter >= 0 && start >= std::llround(realtimeAfter*rate)) {
                offline = false;
                machEpoch = mach_absolute_time() - (uint64_t)std::llround(start/rate*ticksPerSecond);
                epoch = Clock::now() - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(start/rate));
            }
            const auto due = epoch + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(start/rate));
            if (!prefetch && offlineAfter >= 0 && !offline && start >= std::llround(offlineAfter*rate)) {
                instance->releaseResources();
                instance->setNonRealtime(true);
                if (isAU) {
                    // JUCE's AU hosting instance does not forward its own
                    // AudioProcessor non-realtime flag to the native unit.
                    const auto unit = instance->getAudioUnitClient()->getAudioUnitHandle();
                    UInt32 enabled = 1, confirmed = 0, size = sizeof(confirmed);
                    if (AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender,
                        kAudioUnitScope_Global, 0, &enabled, sizeof(enabled)) != noErr
                        || AudioUnitGetProperty(unit, kAudioUnitProperty_OfflineRender,
                            kAudioUnitScope_Global, 0, &confirmed, &size) != noErr || confirmed != 1) {
                        processFailed.store(true); break;
                    }
                }
                instance->prepareToPlay(rate, block);
                offline = true;
            }
            if (prefetch && !enteredPrefetch && start >= std::llround(offlineAfter*rate)) {
                // VST3 allows realtime/prefetch changes without setupProcessing.
                // Exercise that contract directly, preserving the current epoch.
                auto* component = instance->getVST3Client()->getIComponentPtr();
                if (component->queryInterface(Steinberg::Vst::IAudioProcessor::iid,
                    reinterpret_cast<void**>(&nativeProcessor)) != Steinberg::kResultOk) {
                    processFailed.store(true); break;
                }
                enteredPrefetch = true;
                offline = true;
            }
            if (!offline || paceOffline) mach_wait_until(machEpoch + (uint64_t)std::llround(start/rate*ticksPerSecond));
            const auto arrived = Clock::now();
            if (reprepare >= 0 && !reprepared && start >= std::llround(reprepare*rate)) {
                instance->releaseResources();
                instance->prepareToPlay(rate, block);
                reprepared = true;
            }
            midi.clear();
            while (eventIndex < events.size() && events[eventIndex].sample < start + count) {
                midi.addEvent(events[eventIndex].message, (int)(events[eventIndex].sample-start));
                ++eventIndex;
            }
            audio.setSize(2, count, false, false, true);
            audio.clear();
            const auto before = Clock::now();
            if (nativeProcessor) {
                PrefetchEvents events;
                for (const auto metadata : midi) {
                    const auto message = metadata.getMessage();
                    Steinberg::Vst::Event event{};
                    event.sampleOffset = metadata.samplePosition;
                    if (message.isNoteOn()) {
                        event.type = Steinberg::Vst::Event::kNoteOnEvent;
                        event.noteOn.channel = (Steinberg::int16) (message.getChannel() - 1);
                        event.noteOn.pitch = (Steinberg::int16) message.getNoteNumber();
                        event.noteOn.velocity = message.getFloatVelocity();
                        event.noteOn.noteId = -1;
                    } else if (message.isNoteOff()) {
                        event.type = Steinberg::Vst::Event::kNoteOffEvent;
                        event.noteOff.channel = (Steinberg::int16) (message.getChannel() - 1);
                        event.noteOff.pitch = (Steinberg::int16) message.getNoteNumber();
                        event.noteOff.noteId = -1;
                    } else { processFailed.store(true); continue; }
                    if (events.addEvent(event) != Steinberg::kResultOk) processFailed.store(true);
                }
                float* channels[] = { audio.getWritePointer(0), audio.getWritePointer(1) };
                Steinberg::Vst::AudioBusBuffers output{};
                output.numChannels = 2; output.channelBuffers32 = channels;
                Steinberg::Vst::ProcessData data{};
                data.processMode = offline ? Steinberg::Vst::kPrefetch : Steinberg::Vst::kRealtime;
                data.symbolicSampleSize = Steinberg::Vst::kSample32;
                data.numSamples = count; data.numOutputs = 1; data.outputs = &output;
                data.inputEvents = &events;
                if (nativeProcessor->process(data) != Steinberg::kResultOk) processFailed.store(true);
            } else instance->processBlock(audio, midi);
            const auto after = Clock::now();
            for (int ch = 0; ch < 2; ++ch) capture.copyFrom(ch, (int)start, audio, ch, 0, count);
            blocks.push_back({start, count,
                std::chrono::duration<double, std::milli>(arrived-due).count(),
                std::chrono::duration<double, std::milli>(after-before).count(),
                instance->getLatencySamples(), !offline || paceOffline});
            start += count;
            if (saveInterval > 0 && blocks.size() % (std::size_t)saveInterval == 0) {
                saveRequested.store(true, std::memory_order_release);
                while (saveRequested.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        if (nativeProcessor) nativeProcessor->release();
        done.store(true);
    });
    while (!done.load()) {
        if (saveRequested.load(std::memory_order_acquire)) {
            juce::MemoryBlock state;
            instance->getStateInformation(state);
            if (state.isEmpty()) processFailed.store(true);
            ++stateSaves;
            saveRequested.store(false, std::memory_order_release);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
    render.join();
    const int finalLatency = instance->getLatencySamples();
    instance->releaseResources();
    instance.reset();

    juce::File wav(prefix + ".wav");
    if (wav.existsAsFile()) { std::fprintf(stderr, "Refusing to replace existing recording\n"); return 1; }
    std::unique_ptr<juce::OutputStream> stream = wav.createOutputStream();
    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(24);
    auto writer = format.createWriterFor(stream, options);
    if (!writer || !writer->writeFromAudioSampleBuffer(capture, 0, (int)total)) return 1;
    writer.reset();
    juce::FileOutputStream csv(juce::File(prefix + ".blocks.csv"));
    csv.writeText("sample,count,late_ms,render_ms,reported_latency_samples,realtime\n", false, false, nullptr);
    for (const auto& b : blocks) {
        auto row = juce::String((juce::int64)b.sample) + "," + juce::String(b.size) + ","
            + juce::String(b.lateMs, 6) + "," + juce::String(b.renderMs, 6) + "," + juce::String(b.latency) + "," + juce::String(b.realtime ? 1 : 0) + "\n";
        csv.writeText(row, false, false, nullptr);
    }
    juce::DynamicObject::Ptr receipt = new juce::DynamicObject();
    receipt->setProperty("plugin", pluginPath);
    receipt->setProperty("format", isAU ? "AU" : "VST3");
    if (isAU) {
        receipt->setProperty("au_local_identifier", localAU.identifier);
        receipt->setProperty("au_loaded_binary", localAU.binaryPath);
    }
    receipt->setProperty("sample_rate", rate);
    receipt->setProperty("block_size", block);
    receipt->setProperty("seconds", seconds);
    receipt->setProperty("variable_blocks", variable);
    receipt->setProperty("reprepare_seconds", reprepare);
    receipt->setProperty("offline_after_seconds", offlineAfter);
    receipt->setProperty("pace_offline", paceOffline);
    receipt->setProperty("prefetch", prefetch);
    receipt->setProperty("realtime_after_seconds", realtimeAfter);
    receipt->setProperty("initial_state_requested", initialStateRequested);
    receipt->setProperty("state_save_interval_blocks", saveInterval);
    receipt->setProperty("state_saves", stateSaves);
    receipt->setProperty("initial_state_verified", initialStateVerified);
    receipt->setProperty("process_failed", processFailed.load());
    receipt->setProperty("initial_latency_samples", initialLatency);
    receipt->setProperty("final_latency_samples", finalLatency);
    receipt->setProperty("notes", noteRecords);
    receipt->setProperty("midi_sequence", juce::File(argv[11]).getFullPathName());
    juce::File(prefix + ".json").replaceWithText(juce::JSON::toString(juce::var(receipt.get()), true));
    std::printf("Recorded %s\n", wav.getFullPathName().toRawUTF8());
    return processFailed.load() ? 1 : 0;
}
