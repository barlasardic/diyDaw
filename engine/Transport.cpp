#include "Transport.h"

#include <algorithm>
#include <cmath>

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

void Transport::setLoop (double startBeat, double endBeat) noexcept
{
    startBeat = std::max (0.0, startBeat);
    loopStart.store (startBeat);
    loopEnd.store (std::max (startBeat + 1.0 / 64.0, endBeat));
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

    if (b.playing && loopEnabled.load (std::memory_order_relaxed))
    {
        const auto ls = loopStart.load (std::memory_order_relaxed);
        const auto le = loopEnd.load (std::memory_order_relaxed);

        if (le > ls && b.startBeat < le)
        {
            // Samples [0, k) lie before the loop end (same rounding as clip edges).
            auto k = (int) std::ceil ((le - b.startBeat) / b.beatsPerSample - 1.0e-3);

            if (k <= 0) // already at the loop end: wrap first
            {
                b.startBeat = ls;
                k = (int) std::ceil ((le - ls) / b.beatsPerSample - 1.0e-3);
            }

            if (k > 0 && k < numSamples)
            {
                b.numSamples = k;
                positionBeats.store (ls, std::memory_order_relaxed);
                return b;
            }

            if (k > 0 && k == numSamples)
            {
                positionBeats.store (ls, std::memory_order_relaxed);
                return b;
            }
        }
    }

    // The next block's start is *exactly* this block's end, so beat-boundary
    // checks of the form [start, end) never miss or double-count a beat.
    if (b.playing)
        positionBeats.store (b.endBeat(), std::memory_order_relaxed);

    return b;
}

} // namespace daw
