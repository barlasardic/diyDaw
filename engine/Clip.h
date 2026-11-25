#pragma once

#include "SampleData.h"

#include <memory>
#include <vector>

namespace daw
{

/** One note of a MIDI clip, in the clip's content time (beats from the content origin). */
struct MidiNote
{
    double startBeat = 0.0;
    double lengthBeats = 0.25;
    int pitch = 60;
    float velocity = 0.8f; // 0..1
};

/** Immutable note list of a MIDI clip, sorted by startBeat. */
struct MidiSequence
{
    std::vector<MidiNote> notes;
};

/** An immutable description of one clip on the timeline, as the audio thread sees it.

    Audio clip ('sample' set): plays at its native speed (no warping): one second of
    timeline = one second of audio.

    MIDI clip ('notes' set): timeline position t plays content beat
    offsetBeats + (t - startBeat); when looping, content wraps every loopLengthBeats
    (the loop is [0, loopLengthBeats) in content time and notes are cut at its end). */
struct Clip
{
    std::shared_ptr<const SampleData> sample;
    double startBeat = 0.0;
    double lengthBeats = 0.0;
    double offsetSeconds = 0.0; // where in the sample the clip's first sample comes from
    bool looping = false;       // repeat the sample to fill the clip
    float gain = 1.0f;
    double offsetBeats = 0.0;     // MIDI only
    double loopLengthBeats = 0.0; // MIDI only, used when looping
    std::shared_ptr<const MidiSequence> notes {};

    bool isMidi() const noexcept { return notes != nullptr; }
    double endBeat() const noexcept { return startBeat + lengthBeats; }
};

/** Snapshot of all clips on one track, sorted by startBeat.
    Built on the message thread and handed to the audio thread through RealtimeSwap;
    it owns references to its samples, so they are freed on the message thread too. */
struct ClipList
{
    std::vector<Clip> clips;
};

} // namespace daw
