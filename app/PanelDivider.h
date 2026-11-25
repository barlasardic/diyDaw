#pragma once

#include "DawLookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** A thin horizontal bar between two stacked panels. Dragging it reports the vertical drag
    distance (from where the drag started); double-click toggles the lower panel collapsed.
    Message thread only. */
class PanelDivider : public juce::Component,
                     public juce::SettableTooltipClient
{
public:
    PanelDivider()
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setRepaintsOnMouseActivity (true);
        setTooltip ("Drag to resize, double-click to collapse / expand");
    }

    std::function<void()> onDragStart;
    std::function<void (int deltaY)> onDrag;
    std::function<void()> onDragEnd;
    std::function<void()> onDoubleClick;

    static constexpr int thickness = 5;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::outline);
        const auto hot = isMouseOverOrDragging();
        g.setColour (hot ? colours::accent.withAlpha (0.8f) : colours::panelRaised);
        g.fillRoundedRectangle (getLocalBounds().toFloat().withSizeKeepingCentre (36.0f, 2.0f), 1.0f);
    }

    void mouseDown (const juce::MouseEvent&) override         { if (onDragStart) onDragStart(); }
    void mouseDrag (const juce::MouseEvent& e) override       { if (onDrag) onDrag (e.getDistanceFromDragStartY()); }
    void mouseUp (const juce::MouseEvent&) override           { if (onDragEnd) onDragEnd(); }
    void mouseDoubleClick (const juce::MouseEvent&) override  { if (onDoubleClick) onDoubleClick(); }
};

} // namespace daw
