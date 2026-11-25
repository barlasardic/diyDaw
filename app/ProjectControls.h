#pragma once

#include "Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** Binds a slider to an undoable Project setter so that each gesture (a drag, a click that
    jumps, a double-click reset) becomes exactly one undo step. 'apply' receives the new value.
    Message thread only. */
inline void bindSliderToProject (juce::Slider& slider, Project& project, const juce::String& actionName,
                                 std::function<void (double)> apply)
{
    slider.onDragStart = [&project, actionName] { project.beginTransaction (actionName); };
    slider.onValueChange = [&slider, &project, actionName, apply = std::move (apply)]
    {
        if (! slider.isMouseButtonDown()) // e.g. keyboard or programmatic changes
            project.beginTransaction (actionName);

        apply (slider.getValue());
    };
}

/** Shows whether a parameter is automated: the slider's accent turns red and it gets a tooltip.
    While automated the engine follows the envelope, so the caller shows the engine's value. */
inline void showAutomationState (juce::Slider& slider, bool automated)
{
    const auto wasAutomated = slider.isColourSpecified (juce::Slider::rotarySliderFillColourId);
    if (automated == wasAutomated)
        return;

    if (automated)
    {
        slider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffff5f57));
        slider.setColour (juce::Slider::trackColourId, juce::Colour (0xffff5f57));
        slider.setTooltip ("Automated - edit the envelope in the arrangement (A button on the track)");
    }
    else
    {
        slider.removeColour (juce::Slider::rotarySliderFillColourId);
        slider.removeColour (juce::Slider::trackColourId);
        slider.setTooltip ({});
    }

    slider.repaint();
}

/** Same for a toggle button: every click is its own undo step. */
inline void bindToggleToProject (juce::Button& button, Project& project, const juce::String& actionName,
                                 std::function<void (bool)> apply)
{
    button.onClick = [&button, &project, actionName, apply = std::move (apply)]
    {
        project.beginTransaction (actionName);
        apply (button.getToggleState());
    };
}

} // namespace daw
