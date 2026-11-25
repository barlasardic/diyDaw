#include "AutomationLane.h"
#include "DawLookAndFeel.h"
#include "TrackHeader.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace daw
{
namespace
{
    constexpr float verticalPadding = 8.0f;

    // Keep the device panel's units and precision for automation readouts.
    juce::String formatValue (const ParamInfo& info, double value)
    {
        const auto unit = juce::String (info.unit != nullptr ? info.unit : "");
        if (unit == "Hz")
            return value >= 1000.0 ? juce::String::formatted ("%.2f kHz", value / 1000.0)
                                   : juce::String::formatted ("%.0f Hz", value);
        if (unit == "s")
            return value < 1.0 ? juce::String::formatted ("%.0f ms", value * 1000.0)
                               : juce::String::formatted ("%.2f s", value);
        if (unit == "dB")
            return juce::String::formatted ("%.1f dB", value);
        if (unit == "st" || unit == "ct")
        {
            const auto number = juce::roundToInt (value);
            return juce::String (number > 0 ? "+" : "") + juce::String (number) + " " + unit;
        }
        if (unit == "%")
            return juce::String::formatted ("%.0f %%", value * 100.0);
        if (info.minValue < 0.0f && info.maxValue > 0.0f)
            return juce::String::formatted ("%+.2f", value);
        return juce::String::formatted ("%.2f", value);
    }
}

AutomationLaneView::AutomationLaneView (Project& p, juce::ValueTree t, const TimelineGeometry& geo,
                                const double& position)
    : project (p), track (t), geometry (geo), playhead (position)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    selector.setTooltip (juce::String (juce::CharPointer_UTF8 ("Automation target (• = automated)")));
    selector.setMouseClickGrabsKeyboardFocus (false);
    selector.setColour (juce::ComboBox::backgroundColourId, colours::panelRaised);
    selector.setColour (juce::ComboBox::textColourId, colours::text);
    selector.setColour (juce::ComboBox::outlineColourId, colours::outline);
    selector.onChange = [this]
    {
        const auto index = selector.getSelectedId() - 1;
        if (index >= 0 && index < (int) targets.size())
        {
            clearSelection();
            targetId = targets[(size_t) index].id;
            refresh();
        }
    };
    addAndMakeVisible (selector);
    clearButton.setTooltip ("Clear target automation");
    clearButton.setMouseClickGrabsKeyboardFocus (false);
    clearButton.onClick = [this]
    {
        if (auto automation = lane(); automation.isValid())
        {
            clearSelection();
            project.beginTransaction ("Clear automation");
            project.removeAutomationLane (automation);
        }
    };
    addAndMakeVisible (clearButton);
    refresh();
}

juce::ValueTree AutomationLaneView::lane() const { return Project::getAutomationLane (track, targetId); }

const Project::AutomationTarget* AutomationLaneView::target() const
{
    for (const auto& item : targets)
        if (item.id == targetId)
            return &item;
    return nullptr;
}

void AutomationLaneView::refresh()
{
    targets = Project::getAutomationTargets (track);
    if (target() == nullptr)
    {
        clearSelection();
        targetId = "gain";
        for (const auto& item : targets)
            if (Project::getAutomationLane (track, item.id).getNumChildren() > 0)
            {
                targetId = item.id;
                break;
            }
        if (target() == nullptr && ! targets.empty())
            targetId = targets.front().id;
    }

    juce::StringArray labels;
    int selectedId = 0;
    for (size_t i = 0; i < targets.size(); ++i)
    {
        const auto& item = targets[i];
        const auto automated = Project::getAutomationLane (track, item.id).getNumChildren() > 0;
        labels.add ((automated ? juce::String (juce::CharPointer_UTF8 ("• ")) : juce::String()) + item.name);
        if (item.id == targetId)
            selectedId = (int) i + 1;
    }
    bool changed = selector.getNumItems() != labels.size();
    for (int i = 0; ! changed && i < labels.size(); ++i)
        changed = selector.getItemText (i) != labels[i];
    if (changed)
    {
        selector.clear (juce::dontSendNotification);
        selector.addItemList (labels, 1);
    }
    selector.setSelectedId (selectedId, juce::dontSendNotification);
    selector.setTooltip (target() != nullptr ? target()->name + juce::String (juce::CharPointer_UTF8 (" (• = automated)")) : "Automation target");
    clearButton.setEnabled (lane().isValid());
    if (selected.isValid() && selected.getParent() != lane())
        selected = {};
    if (hovered.isValid() && hovered.getParent() != lane())
        hovered = {};
    if (gesture.active && ! gesture.adding && gesture.point.getParent() != lane())
        cancelGesture();
    repaint();
}

std::vector<AutomationLaneView::Point> AutomationLaneView::points (bool preview) const
{
    std::vector<Point> result;
    for (const auto& point : lane())
        if (point.hasType (ids::POINT))
        {
            if (preview && gesture.active && gesture.point == point)
                result.push_back ({ point, gesture.beat, gesture.value });
            else
                result.push_back ({ point, Project::getPointBeat (point), Project::getPointValue (point) });
        }
    if (preview && gesture.active && gesture.adding)
        result.push_back ({ {}, gesture.beat, gesture.value });
    std::stable_sort (result.begin(), result.end(), [] (const auto& a, const auto& b) { return a.beat < b.beat; });
    return result;
}

juce::NormalisableRange<float> AutomationLaneView::range() const
{
    if (const auto* item = target())
    {
        const auto& info = item->info;
        juce::NormalisableRange<float> result (info.minValue, info.maxValue, info.stepped ? 1.0f : 0.0f);
        if (info.centreValue > info.minValue && info.centreValue < info.maxValue)
            result.setSkewForCentre (info.centreValue);
        return result;
    }
    return { 0.0f, 1.0f };
}

float AutomationLaneView::valueToY (float value) const
{
    const auto r = range();
    return verticalPadding + (1.0f - r.convertTo0to1 (juce::jlimit (r.start, r.end, value)))
                                * ((float) getHeight() - 2.0f * verticalPadding);
}

float AutomationLaneView::yToValue (float y) const
{
    const auto r = range();
    const auto normal = juce::jlimit (0.0f, 1.0f, 1.0f - (y - verticalPadding)
                                                       / ((float) getHeight() - 2.0f * verticalPadding));
    return r.snapToLegalValue (r.convertFrom0to1 (normal));
}

void AutomationLaneView::resized()
{
    selector.setBounds (10, 18, TrackHeader::preferredWidth - 42, 22);
    clearButton.setBounds (TrackHeader::preferredWidth - 27, 18, 19, 22);
}

void AutomationLaneView::paint (juce::Graphics& g)
{
    const auto left = (float) TrackHeader::preferredWidth;
    const auto width = juce::jmax (0.0f, (float) getWidth() - left);
    const auto colour = Project::getTrackColour (track);
    g.fillAll (colours::panel.darker (0.18f));
    g.setColour (colour.withAlpha (0.5f));
    g.fillRect (0, 0, 4, getHeight());
    g.setColour (colours::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine (TrackHeader::preferredWidth - 1, 0.0f, (float) getHeight());
    g.saveState();
    g.reduceClipRegion (TrackHeader::preferredWidth, 0, (int) width, getHeight() - 1);
    g.setOrigin (TrackHeader::preferredWidth, 0);
    g.setColour (colours::textDim.withAlpha (0.12f));
    for (const auto normal : { 0.0f, 0.5f, 1.0f })
        g.drawHorizontalLine ((int) (verticalPadding + normal * ((float) getHeight() - 2.0f * verticalPadding)), 0.0f, width);
    const auto step = geometry.gridBeats();
    const auto bar = (double) geometry.beatsPerBar;
    for (auto beat = geometry.snapFloor (geometry.viewStartBeat); beat <= geometry.xToBeat (width); beat += step)
    {
        const auto isBar = std::abs (beat / bar - std::round (beat / bar)) < 1.0e-7;
        g.setColour (colours::textDim.withAlpha (isBar ? 0.20f : 0.07f));
        g.drawVerticalLine ((int) geometry.beatToX (beat), 0.0f, (float) getHeight());
    }

    const auto envelope = points (true);
    if (envelope.empty())
    {
        const auto y = valueToY (Project::getTargetModelValue (track, targetId));
        g.setColour (colour.withAlpha (0.55f));
        const float dashes[] { 5.0f, 4.0f };
        g.drawDashedLine ({ 0.0f, y, width, y }, dashes, 2, 1.0f);
        g.setColour (colours::textDim.withAlpha (0.65f));
        g.setFont (juce::FontOptions (11.0f));
        const auto hintY = y < (float) getHeight() * 0.5f ? getHeight() - 21 : 4;
        g.drawText ("Click to add automation", 12, hintY, (int) width - 24, 16, juce::Justification::centredLeft);
    }
    else
    {
        // Interpolate in parameter units, then map through the knob's skew. Restrict
        // tessellation to the visible width even for very distant points.
        auto firstValue = envelope.front().value;
        if (geometry.viewStartBeat >= envelope.back().beat)
            firstValue = envelope.back().value;
        juce::Path path;
        path.startNewSubPath (0.0f, valueToY (firstValue));
        if (geometry.beatToX (envelope.front().beat) >= 0.0f)
            path.lineTo (juce::jmin (width, geometry.beatToX (envelope.front().beat)), valueToY (envelope.front().value));
        for (size_t i = 1; i < envelope.size(); ++i)
        {
            const auto& a = envelope[i - 1];
            const auto& b = envelope[i];
            const auto x1 = geometry.beatToX (a.beat), x2 = geometry.beatToX (b.beat);
            if (x2 < 0.0f || x1 > width)
                continue;
            const auto start = juce::jlimit (0.0f, width, x1);
            const auto end = juce::jlimit (0.0f, width, x2);
            const auto segments = juce::jmax (1, (int) std::ceil ((end - start) / 2.0f));
            for (int j = 0; j <= segments; ++j)
            {
                const auto x = start + (end - start) * (float) j / (float) segments;
                const auto t = x2 > x1 ? juce::jlimit (0.0f, 1.0f, (x - x1) / (x2 - x1)) : 1.0f;
                const auto y = valueToY (a.value + (b.value - a.value) * t);
                if (x <= 0.0f)
                    path.startNewSubPath (x, y);
                else
                    path.lineTo (x, y);
            }
        }
        if (geometry.beatToX (envelope.back().beat) <= width)
            path.lineTo (width, valueToY (envelope.back().value));
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (1.5f));

        for (const auto& point : envelope)
        {
            const auto x = geometry.beatToX (point.beat), y = valueToY (point.value);
            if (x < -5.0f || x > width + 5.0f)
                continue;
            const auto active = (gesture.active && (gesture.adding ? ! point.tree.isValid() : point.tree == gesture.point))
                                 || (point.tree.isValid() && (point.tree == hovered || point.tree == selected));
            const auto radius = active ? 3.5f : 2.5f;
            g.setColour (active ? colours::text : colour);
            g.fillEllipse (x - radius, y - radius, radius * 2.0f, radius * 2.0f);
            if (active && target() != nullptr && (gesture.active || point.tree == hovered))
            {
                const auto text = formatValue (target()->info, point.value);
                const auto labelX = juce::jlimit (2.0f, juce::jmax (2.0f, width - 96.0f), x + 8.0f);
                const auto labelY = juce::jlimit (1.0f, (float) getHeight() - 18.0f, y - 20.0f);
                g.setColour (colours::background.withAlpha (0.94f));
                g.fillRoundedRectangle (labelX, labelY, 92.0f, 16.0f, 3.0f);
                g.setColour (colours::text);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (text, (int) labelX + 5, (int) labelY, 82, 16, juce::Justification::centredLeft);
            }
        }
    }
    const auto x = geometry.beatToX (playhead);
    if (x >= 0.0f && x <= width)
    {
        g.setColour (colours::text);
        g.fillRect (x, 0.0f, 1.0f, (float) getHeight());
    }
    g.restoreState();
}

juce::ValueTree AutomationLaneView::pointAt (juce::Point<float> position) const
{
    position.x -= (float) TrackHeader::preferredWidth;
    juce::ValueTree result;
    auto distance = 7.0f;
    for (const auto& point : points (false))
    {
        const auto d = position.getDistanceFrom ({ geometry.beatToX (point.beat), valueToY (point.value) });
        if (d < distance)
        {
            distance = d;
            result = point.tree;
        }
    }
    return result;
}

void AutomationLaneView::mouseDown (const juce::MouseEvent& event)
{
    if (event.x < TrackHeader::preferredWidth || target() == nullptr)
        return;
    cancelGesture();
    if (onSelect)
        onSelect();
    selected = pointAt (event.position);
    if (event.mods.isPopupMenu())
    {
        deletePoint (selected);
        return;
    }
    if (! event.mods.isLeftButtonDown() || event.getNumberOfClicks() > 1)
        return;
    gesture.active = true;
    gesture.adding = ! selected.isValid();
    gesture.point = selected;
    gesture.anchor = event.position;
    gesture.originalBeat = selected.isValid() ? Project::getPointBeat (selected)
                                              : juce::jmax (0.0, geometry.xToBeat (event.position.x - (float) TrackHeader::preferredWidth));
    if (gesture.adding && ! event.mods.isAltDown())
        gesture.originalBeat = geometry.snap (gesture.originalBeat);
    gesture.originalValue = selected.isValid() ? Project::getPointValue (selected) : yToValue (event.position.y);
    gesture.minimum = 0.0;
    gesture.maximum = std::numeric_limits<double>::max();
    const auto sorted = points (false);
    for (size_t i = 0; i < sorted.size(); ++i)
        if (sorted[i].tree == selected)
        {
            if (i > 0)
                gesture.minimum = sorted[i - 1].beat;
            if (i + 1 < sorted.size())
                gesture.maximum = sorted[i + 1].beat;
        }
    gesture.beat = gesture.originalBeat;
    gesture.value = gesture.originalValue;
    if (gesture.adding)
        updateGesture (event);
    repaint();
}

void AutomationLaneView::updateGesture (const juce::MouseEvent& event)
{
    auto beat = gesture.originalBeat + (double) (event.position.x - gesture.anchor.x) / geometry.pixelsPerBeat;
    if (! event.mods.isAltDown())
        beat = geometry.snap (beat);
    gesture.beat = event.mods.isShiftDown() ? gesture.originalBeat
                                           : juce::jlimit (gesture.minimum, gesture.maximum, beat);
    gesture.value = gesture.adding ? yToValue (event.position.y)
                                  : yToValue (valueToY (gesture.originalValue) + event.position.y - gesture.anchor.y);
}

void AutomationLaneView::mouseDrag (const juce::MouseEvent& event)
{
    if (! gesture.active || (! gesture.adding && ! gesture.moved && event.getDistanceFromDragStart() < 3))
        return;
    gesture.moved = true;
    updateGesture (event);
    repaint();
}

void AutomationLaneView::mouseUp (const juce::MouseEvent& event)
{
    if (! gesture.active)
        return;
    if (gesture.adding || gesture.moved)
        updateGesture (event);
    const auto completed = gesture;
    cancelGesture();
    if (completed.adding)
    {
        project.beginTransaction ("Add automation point");
        const auto automation = project.getOrCreateAutomationLane (track, targetId);
        selected = project.addAutomationPoint (automation, completed.beat, completed.value);
    }
    else if (completed.moved && (! juce::exactlyEqual (completed.beat, completed.originalBeat)
                                || ! juce::exactlyEqual (completed.value, completed.originalValue)))
    {
        project.beginTransaction ("Move automation point");
        project.moveAutomationPoint (completed.point, completed.beat, completed.value);
    }
    updateHover (event.position);
}

void AutomationLaneView::mouseDoubleClick (const juce::MouseEvent& event)
{
    cancelGesture();
    if (event.mods.isLeftButtonDown() && event.x >= TrackHeader::preferredWidth)
        deletePoint (pointAt (event.position));
}

void AutomationLaneView::deletePoint (juce::ValueTree point)
{
    if (! point.isValid() || point.getParent() != lane())
        return;
    cancelGesture();
    project.beginTransaction ("Delete automation point");
    project.deleteAutomationPoint (point);
}

bool AutomationLaneView::deleteSelectedPoint()
{
    if (! selected.isValid())
        return false;
    deletePoint (selected);
    return true;
}

bool AutomationLaneView::cancelGesture()
{
    const auto active = gesture.active;
    gesture = {};
    repaint();
    return active;
}

void AutomationLaneView::clearSelection()
{
    cancelGesture();
    selected = hovered = {};
    repaint();
}

void AutomationLaneView::updateHover (juce::Point<float> position)
{
    hovered = position.x >= (float) TrackHeader::preferredWidth ? pointAt (position) : juce::ValueTree();
    setMouseCursor (hovered.isValid() ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor);
    repaint();
}

void AutomationLaneView::mouseMove (const juce::MouseEvent& event) { updateHover (event.position); }
void AutomationLaneView::mouseExit (const juce::MouseEvent&)
{
    hovered = {};
    repaint();
}
void AutomationLaneView::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (onScroll)
        onScroll (event, wheel);
}

} // namespace daw
