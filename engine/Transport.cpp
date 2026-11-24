#include "Transport.h"

#include <algorithm>

namespace daw
{

void Transport::setBpm (double newBpm) noexcept
{
    bpm.store (std::clamp (newBpm, minBpm, maxBpm));
}

void Transport::setBeatsPerBar (int n) noexcept
{
    beatsPerBar.store (std::clamp (n, 1, 32));
}

void Transport::setPositionBeats (double beats) noexcept
{
    seekTarget.store (std::max (0.0, beats), std::memory_order_relaxed);
    seekRequested.store (true, std::memory_order_release);
}

double Transport::getPositionBeats() const noexcept
{
    if (seekRequested.load (std::memory_order_acquire))
        return seekTarget.load (std::memory_order_relaxed);

    return positionBeats.load (std::memory_order_relaxed);
}

Transport::Block Transport::advance (int numSamples) noexcept
{
    if (seekRequested.exchange (false, std::memory_order_acq_rel))
        positionBeats.store (seekTarget.load (std::memory_order_relaxed), std::memory_order_relaxed);

    Block b;
    b.playing = playing.load (std::memory_order_relaxed);
    b.bpm = bpm.load (std::memory_order_relaxed);
    b.sampleRate = sampleRate;
    b.numSamples = numSamples;
    b.beatsPerBar = beatsPerBar.load (std::memory_order_relaxed);
    b.startBeat = positionBeats.load (std::memory_order_relaxed);
    b.beatsPerSample = b.bpm / (60.0 * sampleRate);

    // The next block's start is *exactly* this block's end, so beat-boundary
    // checks of the form [start, end) never miss or double-count a beat.
    if (b.playing)
        positionBeats.store (b.endBeat(), std::memory_order_relaxed);

    return b;
}

} // namespace daw
