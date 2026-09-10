// SPDX-License-Identifier: AGPL-3.0-only
#include "latency_sequence.h"
#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main()
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote(500);
    juce::MidiMessageSequence tempo, notes, earlierTrack;
    auto add = [](auto& track, juce::MidiMessage message, double ticks) {
        message.setTimeStamp(ticks);
        track.addEvent(message);
    };
    add(tempo, juce::MidiMessage::tempoMetaEvent(500000), 0);
    add(tempo, juce::MidiMessage::tempoMetaEvent(1000000), 500);
    add(notes, juce::MidiMessage::noteOn(1, 60, (juce::uint8) 64), 3);
    add(notes, juce::MidiMessage::noteOff(1, 60), 203);
    add(notes, juce::MidiMessage::noteOn(1, 62, (juce::uint8) 64), 750);
    add(notes, juce::MidiMessage::noteOff(1, 62), 850);
    add(earlierTrack, juce::MidiMessage::programChange(1, 2), 0);
    file.addTrack(tempo);
    file.addTrack(notes);
    file.addTrack(earlierTrack);
    juce::MemoryOutputStream encoded;
    require(file.writeTo(encoded, 1), "Could not encode the synthetic MIDI fixture");
    auto read = [&](double rate, double seconds, auto& events) {
        juce::MemoryInputStream input(encoded.getData(), encoded.getDataSize(), false);
        juce::String error;
        return readLatencySequence(input, rate, seconds, events, error);
    };
    std::vector<LatencyEvent> events;
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        require(read(rate, 2, events), "Synthetic MIDI sequence failed to load");
        require(events.size() == 5, "Tempo metadata leaked or note events changed");
        require(events[0].sample == 0 && events[0].message.isProgramChange(),
                "Tracks did not merge in timestamp order");
        require(events[1].sample == std::llround(.003 * rate), "Early note moved");
        require(events[2].sample == std::llround(.203 * rate), "Note-off moved");
        require(events[3].sample == (int64_t) rate, "Tempo change was not applied");
        require(events[4].sample == std::llround(1.2 * rate), "Release timing changed");
    }
    require(!read(48000, 1, events), "Out-of-recording MIDI was silently truncated");
    juce::MemoryInputStream invalid("invalid", 7, false);
    juce::String error;
    require(!readLatencySequence(invalid, 48000, 2, events, error), "Malformed MIDI accepted");
    juce::MemoryOutputStream asynchronous;
    require(file.writeTo(asynchronous, 2), "Could not encode type 2 MIDI");
    juce::MemoryInputStream type2(asynchronous.getData(), asynchronous.getDataSize(), false);
    require(!readLatencySequence(type2, 48000, 2, events, error), "Asynchronous tracks accepted");
    std::puts("PASS MIDI timing, tempo changes, track merging and invalid sequence rejection");
}
