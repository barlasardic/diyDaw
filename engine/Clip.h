#pragma once

#include "SampleData.h"

#include <memory>
#include <vector>

namespace daw
{

/** An immutable description of one audio clip on the timeline, as the audio thread sees it.
    Audio plays at its native speed (no warping): one second of timeline = one second of audio. */
struct Clip
{
    std::shared_ptr<const SampleData> sample;
    double startBeat = 0.0;
    double lengthBeats = 0.0;
    double offsetSeconds = 0.0; // where in the sample the clip's first sample comes from
    bool looping = false;       // repeat the sample to fill the clip
    float gain = 1.0f;

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
