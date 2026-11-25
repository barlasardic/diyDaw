#pragma once

#include "AudioEffect.h"

#include <memory>
#include <vector>

namespace daw
{

/** One breakpoint of an automation envelope (value in the parameter's own units). */
struct AutomationPoint
{
    double beat = 0.0;
    float value = 0.0f;
};

/** An automation envelope bound to one engine parameter, as the audio thread sees it.
    Points are sorted by beat; the value is linear between points and holds the first / last
    point's value before / after them. */
struct AutomationLane
{
    enum class Target { trackGain, trackPan, synthParam, effectParam };

    Target target = Target::trackGain;
    int paramIndex = 0;                      // synth / effect parameter index
    std::shared_ptr<AudioEffect> effect;     // effectParam only (keeps the instance alive)
    std::vector<AutomationPoint> points;

    /** Value at 'beat' (points must be non-empty). Real-time safe. */
    float valueAt (double beat) const noexcept;
};

/** Immutable snapshot of a track's automation, handed to the audio thread via RealtimeSwap. */
struct AutomationList
{
    std::vector<AutomationLane> lanes;
};

} // namespace daw
