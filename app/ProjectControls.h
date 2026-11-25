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
