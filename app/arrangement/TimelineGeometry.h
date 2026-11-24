#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

namespace daw
{

/** Maps timeline beats <-> horizontal pixels for the arrangement view, plus grid/snap.
    x = 0 is the left edge of the lanes area (i.e. right of the track headers). */
struct TimelineGeometry
{
    static constexpr double minPixelsPerBeat = 1.0;
    static constexpr double maxPixelsPerBeat = 2048.0;

    double pixelsPerBeat = 40.0;
    double viewStartBeat = 0.0; // beat shown at x = 0 (never negative)
    int beatsPerBar = 4;

    float beatToX (double beat) const noexcept  { return (float) ((beat - viewStartBeat) * pixelsPerBeat); }
    double xToBeat (float x) const noexcept     { return viewStartBeat + (double) x / pixelsPerBeat; }
    double widthToBeats (float w) const noexcept { return (double) w / pixelsPerBeat; }

    /** Grid step in beats: the finest of 1/16, 1/8, 1/4, 1/2, 1 beat, then 1, 2, 4, 8, 16... bars
        whose on-screen spacing is at least minPixels. */
    double gridBeats (float minPixels = 12.0f) const noexcept
    {
        for (const auto step : { 0.0625, 0.125, 0.25, 0.5, 1.0 })
            if (step * pixelsPerBeat >= minPixels)
                return step;

        auto bars = 1.0;
        while (bars * beatsPerBar * pixelsPerBeat < minPixels && bars < 4096.0)
            bars *= 2.0;

        return bars * beatsPerBar;
    }

    double snap (double beat) const noexcept      { const auto g = gridBeats(); return std::round (beat / g) * g; }
    double snapFloor (double beat) const noexcept { const auto g = gridBeats(); return std::floor (beat / g) * g; }

    /** Zooms by 'factor' (>1 = in), keeping the beat under anchorX fixed on screen. */
    void zoomAround (float anchorX, double factor) noexcept
    {
        const auto anchorBeat = xToBeat (anchorX);
        pixelsPerBeat = juce::jlimit (minPixelsPerBeat, maxPixelsPerBeat, pixelsPerBeat * factor);
        viewStartBeat = juce::jmax (0.0, anchorBeat - (double) anchorX / pixelsPerBeat);
    }

    void scrollBy (float pixels) noexcept { viewStartBeat = juce::jmax (0.0, viewStartBeat + widthToBeats (pixels)); }
};

} // namespace daw
