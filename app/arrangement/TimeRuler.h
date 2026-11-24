#pragma once

#include "TimelineGeometry.h"
#include "Transport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** Bar/beat ruler above the arrangement lanes (x = 0 lines up with the lanes' left edge).

    - Draws bar numbers and grid ticks that adapt to the zoom level (geometry.gridBeats()).
    - Draws a playhead marker at transport.getPositionBeats().
    - Click / drag horizontally: moves the playhead (snapped to the grid; hold Alt for no snap).
    - Drag vertically (Ableton style): zooms around the mouse via onZoom.

    The owner repaints it when the geometry or playhead changes. Message thread only. */
class TimeRuler : public juce::Component
{
public:
    TimeRuler (const TimelineGeometry& geometry, Transport& transport);

    /** Requested zoom: factor > 1 zooms in, anchored at anchorX (ruler coordinates). */
    std::function<void (float anchorX, double factor)> onZoom;

    static constexpr int preferredHeight = 26;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    enum class DragMode
    {
        undecided,
        seek,
        zoom
    };

    void seekToX (int x, bool snapToGrid);

    const TimelineGeometry& geometry;
    Transport& transport;

    DragMode dragMode = DragMode::undecided;
    juce::Point<int> dragStart;
    int lastDragY = 0;
    double playheadBeforeDrag = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimeRuler)
};

} // namespace daw
