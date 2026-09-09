// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <vector>

struct LatencyEvent { int64_t sample; juce::MidiMessage message; };

// Standard MIDI files carry the test recipe. Tempo metadata determines event
// times but is not sent to the instrument. Keep event rounding on the same
// absolute host-sample timeline used by the capture loop.
inline bool readLatencySequence(juce::InputStream& input, double rate, double seconds,
                               std::vector<LatencyEvent>& events, juce::String& error)
{
    juce::MidiFile file;
    int type = -1;
    if (!file.readFrom(input, false, &type) || (type != 0 && type != 1) || file.getTimeFormat() == 0) {
        error = "Expected a synchronous type 0 or type 1 Standard MIDI file";
        return false;
    }
    file.convertTimestampTicksToSeconds();
    events.clear();
    for (int track = 0; track < file.getNumTracks(); ++track) {
        const auto* sequence = file.getTrack(track);
        for (int i = 0; i < sequence->getNumEvents(); ++i) {
            const auto& message = sequence->getEventPointer(i)->message;
            if (message.isMetaEvent()) continue;
            const double time = message.getTimeStamp();
            if (!std::isfinite(time) || time < 0 || time >= seconds
                || std::llround(time * rate) >= std::llround(seconds * rate)) {
                error = "MIDI event falls outside the recording";
                return false;
            }
            events.push_back({std::llround(time * rate), message});
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.sample < b.sample;
    });
    if (events.empty()) {
        error = "MIDI file contains no instrument events";
        return false;
    }
    return true;
}
