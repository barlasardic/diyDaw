#pragma once

#include "Project.h"
#include "TimelineGeometry.h"
#include "TrackHeader.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace daw
{

/** One automation sub-lane, including its target selector. Message thread only;
    point gestures are previews until mouseUp, and all edits go through Project. */
class AutomationLaneView : public juce::Component
{
public:
    AutomationLaneView (Project&, juce::ValueTree track, const TimelineGeometry&, const double& playhead);

    static constexpr int preferredHeight = 56;
    void refresh();
    void setHeaderWidth (int);
    void clearSelection();
    bool deleteSelectedPoint();
    bool cancelGesture();

    std::function<void()> onSelect;
    std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> onScroll;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    struct Point
    {
        juce::ValueTree tree;
        double beat;
        float value;
    };
    struct Gesture
    {
        bool active = false, adding = false, moved = false;
        juce::ValueTree point;
        double beat = 0.0, originalBeat = 0.0;
        double minimum = 0.0, maximum = 0.0;
        float value = 0.0f, originalValue = 0.0f;
        juce::Point<float> anchor;
    };

    juce::ValueTree lane() const;
    const Project::AutomationTarget* target() const;
    std::vector<Point> points (bool preview) const;
    juce::NormalisableRange<float> range() const;
    float valueToY (float) const;
    float yToValue (float) const;
    juce::ValueTree pointAt (juce::Point<float>) const;
    void updateGesture (const juce::MouseEvent&);
    void deletePoint (juce::ValueTree);
    void updateHover (juce::Point<float>);

    Project& project;
    juce::ValueTree track, selected, hovered;
    const TimelineGeometry& geometry;
    const double& playhead;
    std::vector<Project::AutomationTarget> targets;
    juce::String targetId;
    juce::ComboBox selector;
    juce::TextButton clearButton { "x" };
    Gesture gesture;
    int headerWidth = TrackHeader::preferredWidth;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomationLaneView)
};

} // namespace daw
