#include "ArrangementView.h"
#include "AutomationLane.h"
#include "DawLookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace daw
{

namespace
{
    constexpr float trimHandleWidth = 6.0f;

    /** Message-thread hit surface. Screen deltas stay stable when its bounds move. */
    class ResizeHandle final : public juce::Component
    {
    public:
        explicit ResizeHandle (bool horizontal) : horizontalAxis (horizontal)
        {
            setMouseCursor (horizontal ? juce::MouseCursor::LeftRightResizeCursor
                                       : juce::MouseCursor::UpDownResizeCursor);
            setMouseClickGrabsKeyboardFocus (false);
        }

        std::function<int()> getValue;
        std::function<void (int)> onResize;
        std::function<void()> onFinish, onReset;
        std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> onScroll;

        void mouseDown (const juce::MouseEvent& event) override
        {
            active = event.mods.isLeftButtonDown();
            if (active)
            {
                anchor = event.getScreenPosition();
                original = getValue();
            }
        }

        void mouseDrag (const juce::MouseEvent& event) override
        {
            if (active)
            {
                const auto delta = event.getScreenPosition() - anchor;
                onResize (original + (horizontalAxis ? delta.x : delta.y));
            }
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            if (! active)
                return;
            if (event.mouseWasDraggedSinceMouseDown())
                mouseDrag (event);
            active = false;
            if (getValue() != original && onFinish)
                onFinish();
        }

        void mouseDoubleClick (const juce::MouseEvent& event) override
        {
            if (event.mods.isLeftButtonDown() && onReset)
            {
                active = false;
                onReset();
            }
        }

        void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
        {
            if (onScroll)
                onScroll (event, wheel);
        }

    private:
        bool horizontalAxis, active = false;
        int original = 0;
        juce::Point<int> anchor;
    };

    bool commandDown (const juce::ModifierKeys& mods)
    {
        return mods.isCommandDown() || mods.isCtrlDown();
    }
}

/** Scrolling content, including headers. All drawing, gestures and previews are
    message-thread only; clip edits commit when a gesture completes, layout changes live. */
class ArrangementView::Lanes : public juce::Component
{
public:
    explicit Lanes (ArrangementView& view) : owner (view)
    {
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (true);
        setOpaque (true);
        syncHeaders();
    }

    int timelineWidth() const { return juce::jmax (0, getWidth() - owner.getHeaderWidth()); }

    // All vertical geometry comes from this layout, including automation sub-lanes.
    struct RowLayout
    {
        int top = 0, automationTop = 0, bottom = 0;
        bool expanded = false;
    };

    int tracksHeight() const { return layout.empty() ? 0 : layout.back().bottom; }

    int rowAtY (float y) const
    {
        for (size_t i = 0; i < layout.size(); ++i)
            if (y >= (float) layout[i].top && y < (float) layout[i].bottom)
                return (int) i;
        return -1;
    }

    bool isAutomationY (float y) const
    {
        const auto row = rowAtY (y);
        return row >= 0 && layout[(size_t) row].expanded && y >= (float) layout[(size_t) row].automationTop;
    }

    void syncHeaders()
    {
        std::vector<Row> updated;
        for (int i = 0; i < owner.project.getNumTracks(); ++i)
        {
            const auto track = owner.project.getTrack (i);
            auto existing = std::find_if (rows.begin(), rows.end(), [&track] (const auto& row)
            {
                return row.header != nullptr && row.header->getTrack() == track;
            });

            if (existing != rows.end())
                updated.push_back (std::move (*existing));
            else
            {
                Row row;
                row.header = std::make_unique<TrackHeader> (owner.project, track);
                row.automation = std::make_unique<AutomationLaneView> (owner.project, track, owner.geometry, owner.playheadBeat);
                row.resizeHandle = std::make_unique<ResizeHandle> (false);
                row.resizeHandle->getValue = [track] { return Project::getTrackHeight (track); };
                row.resizeHandle->onResize = [this, track] (int height)
                {
                    if (owner.project.indexOfTrack (track) >= 0)
                        owner.project.setTrackHeight (track, height);
                };
                row.resizeHandle->onReset = [this, track]
                {
                    owner.project.setTrackHeight (track, Project::defaultTrackHeight);
                };
                row.resizeHandle->onScroll = [this] (const auto& event, const auto& wheel)
                {
                    owner.scrollOrZoom (event, wheel, true);
                };
                row.header->onAutomationToggled = [this, track] (bool expanded)
                {
                    for (auto& item : rows)
                        if (item.header->getTrack() == track)
                        {
                            item.expanded = expanded;
                            if (! expanded)
                                item.automation->clearSelection();
                        }
                    resized();
                    owner.resized();
                };
                const auto* automation = row.automation.get();
                row.automation->onSelect = [this, track, automation]
                {
                    cancelDrag();
                    selected = {};
                    for (auto& item : rows)
                        if (item.automation.get() != automation)
                            item.automation->clearSelection();
                    if (isShowing())
                        grabKeyboardFocus();
                    if (owner.onTrackSelected)
                        owner.onTrackSelected (track);
                    repaint();
                };
                row.automation->onScroll = [this] (const auto& event, const auto& wheel)
                {
                    owner.scrollOrZoom (event, wheel, true);
                };
                addAndMakeVisible (*row.header);
                addChildComponent (*row.automation);
                addAndMakeVisible (*row.resizeHandle);
                updated.push_back (std::move (row));
            }
        }

        rows = std::move (updated);
        resized();
    }

    void refreshHeaders()
    {
        for (auto& row : rows)
            row.header->refresh();
    }

    void refreshAutomation()
    {
        for (auto& row : rows)
            row.automation->refresh();
    }

    void showAutomationForTracksWithAutomation()
    {
        for (auto& row : rows)
            for (const auto& child : row.header->getTrack())
                if (Project::isAutomationLane (child))
                {
                    row.expanded = true;
                    break;
                }
        refreshAutomation();
        resized();
        owner.resized();
    }

    void validateSelection()
    {
        if (selected.isValid() && ! belongsToProject (selected))
            selected = {};
        if (drag.clip.isValid() && ! belongsToProject (drag.clip))
            cancelDrag();
    }

    void select (juce::ValueTree clip)
    {
        selected = clip;
        for (auto& row : rows)
            row.automation->clearSelection();
        repaint();
    }

    void repaintPlayhead (double beat)
    {
        const auto x = owner.geometry.beatToX (beat);
        if (x >= -2.0f && x <= (float) timelineWidth() + 2.0f)
        {
            const auto visible = owner.viewport.getViewArea();
            repaint (owner.getHeaderWidth() + (int) std::floor (x) - 2,
                     visible.getY(), 5, visible.getHeight());
        }
    }

    void resized() override
    {
        layout.clear();
        int y = 0;
        for (auto& row : rows)
        {
            const auto laneHeight = Project::getTrackHeight (row.header->getTrack());
            const auto automationTop = y + laneHeight;
            const auto bottom = automationTop + (row.expanded ? AutomationLaneView::preferredHeight : 0);
            layout.push_back ({ y, automationTop, bottom, row.expanded });
            row.header->setBounds (0, y, owner.getHeaderWidth(), laneHeight);
            row.header->setAutomationShown (row.expanded);
            row.automation->setHeaderWidth (owner.getHeaderWidth());
            row.automation->setBounds (0, automationTop, getWidth(), AutomationLaneView::preferredHeight);
            row.automation->setVisible (row.expanded);
            row.resizeHandle->setBounds (0, automationTop - 4, getWidth(), 4);
            row.resizeHandle->toFront (false);
            y = bottom;
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::background);
        g.saveState();
        g.reduceClipRegion (owner.getHeaderWidth(), 0, timelineWidth(), getHeight());
        g.setOrigin (owner.getHeaderWidth(), 0);

        const auto trackCount = owner.project.getNumTracks();
        const auto contentHeight = tracksHeight();
        const auto width = (float) timelineWidth();
        const auto& geometry = owner.geometry;
        const auto clipArea = g.getClipBounds();

        g.setColour (colours::panel);
        g.fillRect (0, 0, timelineWidth(), contentHeight);

        // Restrict bar/grid traversal even when repainting a narrow playhead strip.
        const auto firstBeat = geometry.xToBeat ((float) clipArea.getX());
        const auto lastBeat = geometry.xToBeat ((float) clipArea.getRight());
        const auto bar = (double) geometry.beatsPerBar;
        for (auto beat = std::floor (firstBeat / bar) * bar; beat <= lastBeat; beat += bar)
        {
            if (std::abs (std::fmod (std::floor (beat / bar), 2.0)) < 0.5)
            {
                g.setColour (colours::panel.brighter (0.035f));
                g.fillRect (geometry.beatToX (beat), 0.0f,
                            (float) (bar * geometry.pixelsPerBeat), (float) contentHeight);
            }
        }

        const auto grid = geometry.gridBeats();
        for (auto beat = std::floor (firstBeat / grid) * grid; beat <= lastBeat; beat += grid)
        {
            const auto isBar = std::abs (beat / bar - std::round (beat / bar)) < 1.0e-7;
            g.setColour (isBar ? colours::textDim.withAlpha (0.32f) : colours::outline.withAlpha (0.55f));
            g.drawVerticalLine ((int) std::floor (geometry.beatToX (beat)), 0.0f, (float) contentHeight);
        }

        for (int row = 0; row < trackCount; ++row)
        {
            const auto& rowBounds = layout[(size_t) row];
            if (rowBounds.automationTop <= clipArea.getY() || rowBounds.top >= clipArea.getBottom())
                continue;
            const auto track = owner.project.getTrack (row);
            for (const auto& clip : track)
            {
                if (! clip.hasType (ids::CLIP))
                    continue;
                const auto bounds = clipBounds (row, Project::getClipStart (clip), Project::getClipLength (clip));
                if (! bounds.intersects (clipArea.toFloat()))
                    continue;

                const auto isSource = drag.hasMoved && drag.clip == clip;
                if (isSource)
                    g.beginTransparencyLayer (0.25f);
                paintClip (g, bounds, drawInfo (clip, track, contentOffset (clip), Project::getClipLength (clip)));
                if (isSource)
                    g.endTransparencyLayer();
            }
            g.setColour (colours::outline);
            g.drawHorizontalLine (layout[(size_t) row].automationTop - 1, 0.0f, width);
        }

        if (drag.hasMoved && drag.track.isValid())
        {
            const auto row = owner.project.indexOfTrack (drag.track);
            if (row >= 0)
            {
                const auto bounds = clipBounds (row, drag.start, drag.length);
                g.beginTransparencyLayer (0.72f);
                paintClip (g, bounds, drawInfo (drag.clip, drag.track, drag.offset, drag.length));
                g.endTransparencyLayer();
                g.setColour (colours::accent);
                g.drawRect (bounds, 1.0f);
            }
        }

        g.setColour (colours::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Drop audio files here to create a track", 12, contentHeight + 16,
                    juce::jmax (0, timelineWidth() - 24), 24, juce::Justification::centredLeft, true);

        const auto playheadX = geometry.beatToX (owner.playheadBeat);
        if (playheadX >= 0.0f && playheadX <= width)
        {
            g.setColour (colours::text);
            g.fillRect (playheadX, 0.0f, 1.0f, (float) getHeight());
        }
        g.restoreState();
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! event.mods.isLeftButtonDown() || event.x < owner.getHeaderWidth())
            return;

        if (isShowing())
            grabKeyboardFocus();
        cancelDrag();
        const auto point = timelinePoint (event);
        const auto clip = clipAt (point);
        select (clip);

        if (const auto row = rowAtY (point.y); row >= 0 && owner.onTrackSelected)
            owner.onTrackSelected (owner.project.getTrack (row));

        if (clip.isValid() && owner.onClipSelected)
            owner.onClipSelected (clip);

        if (! clip.isValid())
        {
            auto beat = owner.geometry.xToBeat (point.x);
            if (! event.mods.isAltDown())
                beat = owner.geometry.snap (beat);
            owner.project.getEngine().getTransport().setPositionBeats (juce::jmax (0.0, beat));
            owner.refresh();
            return;
        }

        drag.clip = clip;
        drag.track = clip.getParent();
        drag.start = drag.originalStart = Project::getClipStart (clip);
        drag.length = drag.originalLength = Project::getClipLength (clip);
        drag.offset = contentOffset (clip);
        drag.anchorBeat = owner.geometry.xToBeat (point.x);
        drag.mode = modeAt (clip, point.x);
        updateCursor (point);
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (! drag.clip.isValid() || (! drag.hasMoved && event.getDistanceFromDragStart() < 3))
            return;
        drag.hasMoved = true;
        updatePreview (event);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& event) override
    {
        if (! drag.clip.isValid())
            return;
        if (drag.hasMoved)
            updatePreview (event);

        const auto completed = drag;
        cancelDrag();
        if (completed.hasMoved && belongsToProject (completed.clip)
            && (! juce::exactlyEqual (completed.start, completed.originalStart)
                || ! juce::exactlyEqual (completed.length, completed.originalLength)
                || completed.track != completed.clip.getParent()))
        {
            // Tree notifications are synchronous, so commit from a copy of the preview.
            auto& project = owner.project;
            switch (completed.mode)
            {
                case DragMode::move:
                    project.beginTransaction ("Move clip");
                    project.moveClip (completed.clip, completed.track, completed.start);
                    break;
                case DragMode::trimStart:
                    project.beginTransaction ("Trim clip start");
                    project.trimClipStart (completed.clip, completed.start);
                    break;
                case DragMode::trimEnd:
                    project.beginTransaction ("Trim clip end");
                    project.trimClipEnd (completed.clip, completed.start + completed.length);
                    break;
            }
            if (belongsToProject (completed.clip))
                select (completed.clip);
        }
        updateCursor (timelinePoint (event));
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (! event.mods.isLeftButtonDown() || event.x < owner.getHeaderWidth())
            return;

        cancelDrag();
        const auto point = timelinePoint (event);
        const auto row = rowAtY (point.y);
        if (row < 0 || isAutomationY (point.y))
            return;

        auto clip = clipAt (point);

        // Ableton: double-clicking empty space on a MIDI track creates a one-bar clip.
        if (! clip.isValid())
        {
            const auto track = owner.project.getTrack (row);
            if (! Project::isMidiTrack (track))
                return;

            const auto barLength = (double) juce::jmax (1, owner.project.getEngine().getTransport().getBeatsPerBar());
            const auto start = std::floor (owner.geometry.xToBeat (point.x) / barLength) * barLength;
            owner.project.beginTransaction ("Insert MIDI clip");
            clip = owner.project.addMidiClip (track, juce::jmax (0.0, start), barLength);
        }

        if (clip.isValid())
        {
            select (clip);
            if (owner.onClipDoubleClicked)
                owner.onClipDoubleClicked (clip);
        }
    }

    void mouseMove (const juce::MouseEvent& event) override { updateCursor (timelinePoint (event)); }
    void mouseExit (const juce::MouseEvent&) override
    {
        if (! drag.clip.isValid())
            setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        owner.scrollOrZoom (event, wheel, true);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.getKeyCode() == juce::KeyPress::escapeKey && drag.clip.isValid())
        {
            cancelDrag();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::escapeKey)
        {
            bool cancelled = false;
            for (auto& row : rows)
                cancelled = row.automation->cancelGesture() || cancelled;
            if (cancelled)
                return true;
        }
        validateSelection();
        if (! selected.isValid())
        {
            if (key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
                for (auto& row : rows)
                    if (row.automation->deleteSelectedPoint())
                        return true;
            return false;
        }

        const auto code = key.getKeyCode();
        auto& project = owner.project;
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        {
            cancelDrag();
            project.beginTransaction ("Delete clip");
            project.deleteClip (selected);
            return true;
        }
        if (! commandDown (key.getModifiers()))
            return false;
        if (code == 'D')
        {
            cancelDrag();
            project.beginTransaction ("Duplicate clip");
            select (project.duplicateClip (selected));
            return true;
        }
        if (code == 'E')
        {
            cancelDrag();
            const auto beat = project.getEngine().getTransport().getPositionBeats();
            if (beat > Project::getClipStart (selected) && beat < Project::getClipEnd (selected))
            {
                project.beginTransaction ("Split clip");
                const auto right = project.splitClip (selected, beat);
                if (right.isValid())
                    select (right);
            }
            return true;
        }
        if (code == 'L')
        {
            cancelDrag();
            project.beginTransaction ("Toggle clip loop");
            project.setClipLooping (selected, ! Project::isClipLooping (selected));
            return true;
        }
        return false;
    }

private:
    /** Audio: offset in seconds; MIDI: offset in content beats. */
    static double contentOffset (const juce::ValueTree& clip)
    {
        return Project::isMidiClip (clip) ? Project::getClipOffsetBeats (clip) : Project::getClipOffset (clip);
    }

    enum class DragMode { move, trimStart, trimEnd };
    struct Preview
    {
        juce::ValueTree clip, track;
        DragMode mode = DragMode::move;
        double start = 0.0, length = 0.0, offset = 0.0;
        double originalStart = 0.0, originalLength = 0.0, anchorBeat = 0.0;
        bool hasMoved = false;
    };

    bool belongsToProject (const juce::ValueTree& clip) const
    {
        return clip.hasType (ids::CLIP) && owner.project.indexOfTrack (clip.getParent()) >= 0;
    }

    juce::Point<float> timelinePoint (const juce::MouseEvent& event) const
    {
        return event.position.translated (-(float) owner.getHeaderWidth(), 0.0f);
    }

    juce::ValueTree clipAt (juce::Point<float> point) const
    {
        if (point.x < 0.0f || point.x >= (float) timelineWidth() || point.y < 0.0f)
            return {};
        const auto row = rowAtY (point.y);
        if (row < 0 || isAutomationY (point.y))
            return {};
        const auto track = owner.project.getTrack (row);
        const auto beat = owner.geometry.xToBeat (point.x);
        for (const auto& clip : track)
            if (clip.hasType (ids::CLIP) && beat >= Project::getClipStart (clip) && beat < Project::getClipEnd (clip))
                return clip;
        return {};
    }

    DragMode modeAt (const juce::ValueTree& clip, float x) const
    {
        const auto left = std::abs (x - owner.geometry.beatToX (Project::getClipStart (clip)));
        const auto right = std::abs (x - owner.geometry.beatToX (Project::getClipEnd (clip)));
        if (left <= trimHandleWidth && left <= right)
            return DragMode::trimStart;
        if (right <= trimHandleWidth)
            return DragMode::trimEnd;
        return DragMode::move;
    }

    void updateCursor (juce::Point<float> point)
    {
        const auto clip = clipAt (point);
        auto cursorType = juce::MouseCursor::NormalCursor;
        if (clip.isValid())
            cursorType = modeAt (clip, point.x) == DragMode::move ? juce::MouseCursor::DraggingHandCursor
                                                               : juce::MouseCursor::LeftRightResizeCursor;
        setMouseCursor (cursorType);
    }

    juce::Rectangle<float> clipBounds (int row, double start, double length) const
    {
        return { owner.geometry.beatToX (start), (float) (layout[(size_t) row].top + 2),
                 (float) (length * owner.geometry.pixelsPerBeat), (float) (layout[(size_t) row].automationTop - layout[(size_t) row].top - 4) };
    }

    ClipDrawInfo drawInfo (const juce::ValueTree& clip, const juce::ValueTree& track,
                           double offset, double length) const
    {
        if (Project::isMidiClip (clip))
        {
            ClipDrawInfo info;
            info.name = Project::getClipName (clip);
            info.colour = Project::getTrackColour (track);
            info.looping = Project::isClipLooping (clip);
            info.selected = clip == selected;
            info.midiClip = clip;
            info.offsetBeats = offset;
            info.lengthBeats = length;
            info.loopLengthBeats = Project::getClipLoopLength (clip);
            return info;
        }

        auto sample = owner.project.getClipSample (clip);
        const auto missing = sample == nullptr && ! owner.project.getSamplePool().isLoading (Project::getClipFile (clip));

        return { Project::getClipName (clip), Project::getTrackColour (track), std::move (sample),
                 offset, owner.project.beatsToSeconds (length), Project::isClipLooping (clip), clip == selected, missing };
    }

    void cancelDrag()
    {
        if (drag.clip.isValid())
        {
            drag = {};
            setMouseCursor (juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void updatePreview (const juce::MouseEvent& event)
    {
        auto& project = owner.project;
        const auto delta = owner.geometry.xToBeat (timelinePoint (event).x) - drag.anchorBeat;
        const auto end = drag.originalStart + drag.originalLength;
        const auto snap = [this, &event] (double beat)
        {
            return event.mods.isAltDown() ? beat : owner.geometry.snap (beat);
        };
        switch (drag.mode)
        {
            case DragMode::move:
            {
                drag.start = juce::jmax (0.0, snap (drag.originalStart + delta));
                // An automation sub-lane maps to its parent track for clip moves.
                auto row = rowAtY (event.position.y);
                if (row < 0)
                    row = event.position.y < 0.0f ? 0 : project.getNumTracks() - 1;
                // Clips only move between tracks of the same kind (audio <-> audio, MIDI <-> MIDI).
                if (const auto target = project.getTrack (row); Project::isMidiTrack (target) == Project::isMidiClip (drag.clip))
                    drag.track = target;
                break;
            }
            case DragMode::trimStart:
            {
                if (Project::isMidiClip (drag.clip))
                {
                    const auto originalOffset = Project::getClipOffsetBeats (drag.clip);
                    const auto loopLength = Project::getClipLoopLength (drag.clip);
                    const auto looping = Project::isClipLooping (drag.clip) && loopLength > 0.0;
                    const auto earliest = looping ? 0.0 : juce::jmax (0.0, drag.originalStart - originalOffset);
                    drag.start = juce::jlimit (earliest, end - Project::minClipLengthBeats, snap (drag.originalStart + delta));
                    drag.length = end - drag.start;
                    drag.offset = originalOffset + (drag.start - drag.originalStart);
                    if (looping)
                    {
                        drag.offset = std::fmod (drag.offset, loopLength);
                        if (drag.offset < 0.0)
                            drag.offset += loopLength;
                    }
                    break;
                }

                const auto originalOffset = Project::getClipOffset (drag.clip);
                const auto earliest = Project::isClipLooping (drag.clip) ? 0.0
                    : juce::jmax (0.0, drag.originalStart - project.secondsToBeats (originalOffset));
                drag.start = juce::jlimit (earliest, end - Project::minClipLengthBeats,
                                          snap (drag.originalStart + delta));
                drag.length = end - drag.start;
                drag.offset = originalOffset + project.beatsToSeconds (drag.start - drag.originalStart);
                if (const auto sample = project.getClipSample (drag.clip))
                {
                    const auto seconds = sample->getLengthSeconds();
                    if (Project::isClipLooping (drag.clip) && seconds > 0.0)
                    {
                        drag.offset = std::fmod (drag.offset, seconds);
                        if (drag.offset < 0.0)
                            drag.offset += seconds;
                    }
                    else
                        drag.offset = juce::jlimit (0.0, seconds, drag.offset);
                }
                else
                    drag.offset = juce::jmax (0.0, drag.offset);
                break;
            }
            case DragMode::trimEnd:
            {
                auto latest = std::numeric_limits<double>::max();
                if (! Project::isClipLooping (drag.clip) && ! Project::isMidiClip (drag.clip))
                    if (const auto sample = project.getClipSample (drag.clip))
                        latest = drag.originalStart + project.secondsToBeats (sample->getLengthSeconds() - drag.offset);
                const auto earliest = drag.originalStart + Project::minClipLengthBeats;
                const auto newEnd = juce::jlimit (earliest, juce::jmax (earliest, latest), snap (end + delta));
                drag.length = newEnd - drag.originalStart;
                break;
            }
        }
    }

    ArrangementView& owner;
    struct Row
    {
        std::unique_ptr<TrackHeader> header;
        std::unique_ptr<AutomationLaneView> automation;
        std::unique_ptr<ResizeHandle> resizeHandle;
        bool expanded = false;
    };
    std::vector<Row> rows;
    std::vector<RowLayout> layout;
    juce::ValueTree selected;
    Preview drag;
};

/** Retains a drop's results until every decode finishes, preserving file order and
    one undo transaction when background jobs complete out of order. */
struct ArrangementView::ImportBatch
{
    juce::StringArray files;
    std::vector<SamplePool::Ptr> samples;
    juce::ValueTree track;
    double startBeat = 0.0;
    int remaining = 0;
};

ArrangementView::ArrangementView (Project& p)
    : project (p), state (project.getState()), ruler (geometry, project.getEngine().getTransport()),
      lanes (std::make_unique<Lanes> (*this))
{
    auto& transport = project.getEngine().getTransport();
    geometry.beatsPerBar = transport.getBeatsPerBar();
    playheadBeat = transport.getPositionBeats();
    lastBpm = transport.getBpm();

    addAndMakeVisible (ruler);
    ruler.onZoom = [this] (float anchorX, double factor)
    {
        geometry.zoomAround (anchorX, factor);
        geometryChanged();
    };
    viewport.setViewedComponent (lanes.get(), false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
    viewport.setWantsKeyboardFocus (false);
    viewport.getVerticalScrollBar().setMouseClickGrabsKeyboardFocus (false);
    addAndMakeVisible (viewport);
    auto handle = std::make_unique<ResizeHandle> (true);
    handle->getValue = [this] { return getHeaderWidth(); };
    handle->onResize = [this] (int width) { setHeaderWidth (width); };
    handle->onFinish = [this]
    {
        if (onHeaderWidthChanged)
            onHeaderWidthChanged (headerWidth);
    };
    handle->onScroll = [this] (const auto& event, const auto& wheel)
    {
        scrollOrZoom (event, wheel, getLocalPoint (event.eventComponent, event.position).y
                                     >= (float) TimeRuler::preferredHeight);
    };
    addAndMakeVisible (*handle);
    headerResizeHandle = std::move (handle);
    state.addListener (this);
}

ArrangementView::~ArrangementView()
{
    state.removeListener (this);
    viewport.setViewedComponent (nullptr, false);
}

void ArrangementView::setHeaderWidth (int width)
{
    width = juce::jlimit (TrackHeader::minWidth, TrackHeader::maxWidth, width);
    if (headerWidth == width)
        return;
    headerWidth = width;
    resized();
    repaint();
}

void ArrangementView::showAutomationForTracksWithAutomation()
{
    lanes->showAutomationForTracksWithAutomation();
}

void ArrangementView::refresh()
{
    lanes->refreshHeaders();
    auto& transport = project.getEngine().getTransport();
    const auto oldBeat = playheadBeat;
    playheadBeat = transport.getPositionBeats();
    bool changed = false;
    if (geometry.beatsPerBar != transport.getBeatsPerBar() || ! juce::exactlyEqual (lastBpm, transport.getBpm()))
    {
        geometry.beatsPerBar = transport.getBeatsPerBar();
        lastBpm = transport.getBpm();
        changed = true;
    }

    const auto visibleBeats = geometry.widthToBeats ((float) lanes->timelineWidth());
    if (transport.isPlaying() && visibleBeats > 0.0
        && (playheadBeat < geometry.viewStartBeat || playheadBeat >= geometry.viewStartBeat + visibleBeats))
    {
        geometry.viewStartBeat = juce::jmax (0.0, std::floor (playheadBeat / visibleBeats) * visibleBeats);
        changed = true;
    }
    if (changed)
        geometryChanged();
    else if (! juce::exactlyEqual (oldBeat, playheadBeat))
    {
        ruler.repaint();
        lanes->repaintPlayhead (oldBeat);
        lanes->repaintPlayhead (playheadBeat);
    }
}

void ArrangementView::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    g.setColour (colours::panelRaised);
    g.fillRect (0, 0, headerWidth, TimeRuler::preferredHeight);
    g.setColour (colours::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("TRACKS", 10, 0, headerWidth - 20, TimeRuler::preferredHeight,
                juce::Justification::centredLeft);
}

void ArrangementView::resized()
{
    viewport.setBounds (getLocalBounds().withTrimmedTop (TimeRuler::preferredHeight));
    lanes->resized();
    const auto height = juce::jmax (viewport.getMaximumVisibleHeight(),
                                   lanes->tracksHeight() + Project::defaultTrackHeight);
    // Showing/hiding the scrollbar changes the available width.
    lanes->setSize (viewport.getWidth(), height);
    lanes->setSize (viewport.getMaximumVisibleWidth(), height);
    ruler.setBounds (headerWidth, 0, lanes->timelineWidth(), TimeRuler::preferredHeight);
    headerResizeHandle->setBounds (headerWidth - 2, 0, 5, getHeight());
    headerResizeHandle->toFront (false);
    geometryChanged();
}

void ArrangementView::geometryChanged()
{
    ruler.repaint();
    lanes->repaint();
}

void ArrangementView::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    scrollOrZoom (event, wheel, false);
}

void ArrangementView::scrollOrZoom (const juce::MouseEvent& event,
                                    const juce::MouseWheelDetails& wheel, bool overLanes)
{
    const auto point = ruler.getLocalPoint (event.eventComponent, event.position);
    if (commandDown (event.mods))
    {
        const auto delta = std::abs (wheel.deltaY) > 0.0f ? wheel.deltaY : wheel.deltaX;
        geometry.zoomAround (juce::jlimit (0.0f, (float) lanes->timelineWidth(), point.x),
                             std::exp ((double) delta * 2.0));
        geometryChanged();
        return;
    }
    const auto vertical = overLanes && ! event.mods.isShiftDown()
        && std::abs (wheel.deltaY) > std::abs (wheel.deltaX)
        && lanes->tracksHeight() > viewport.getMaximumVisibleHeight();
    if (vertical)
    {
        viewport.setViewPosition (0, viewport.getViewPositionY() - juce::roundToInt (wheel.deltaY * 180.0f));
        return;
    }
    const auto delta = std::abs (wheel.deltaX) > 0.0f ? wheel.deltaX : wheel.deltaY;
    geometry.scrollBy (-delta * 240.0f);
    geometryChanged();
}

bool ArrangementView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (project.getSamplePool().getFormats().findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
            return true;
    return false;
}

void ArrangementView::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (! viewport.getBounds().contains (x, y))
        return;
    const auto point = lanes->getLocalPoint (this, juce::Point<int> (x, y));
    const auto timelineX = point.x - headerWidth;
    if (timelineX < 0 || timelineX >= lanes->timelineWidth() || point.y < 0)
        return;

    auto batch = std::make_shared<ImportBatch>();
    for (const auto& path : files)
        if (project.getSamplePool().getFormats().findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
            batch->files.add (path);
    if (batch->files.isEmpty())
        return;

    // File drops on an automation sub-lane use its parent track too.
    const auto row = lanes->rowAtY ((float) point.y);
    if (row < 0 || Project::isMidiTrack (project.getTrack (row)))
    {
        const auto oldCount = project.getNumTracks();
        if (onAddTrackRequested)
            onAddTrackRequested();
        if (project.getNumTracks() <= oldCount)
            return;
        batch->track = project.getTrack (project.getNumTracks() - 1);
    }
    else
        batch->track = project.getTrack (row);

    batch->startBeat = juce::jmax (0.0, geometry.snap (geometry.xToBeat ((float) timelineX)));
    batch->remaining = batch->files.size();
    batch->samples.resize ((size_t) batch->remaining);
    const juce::Component::SafePointer<ArrangementView> safeThis (this);
    for (int i = 0; i < batch->files.size(); ++i)
        project.getSamplePool().loadAsync (juce::File (batch->files[i]), [safeThis, batch, i] (SamplePool::Ptr sample)
        {
            if (safeThis == nullptr)
                return;
            batch->samples[(size_t) i] = std::move (sample);
            if (--batch->remaining == 0)
                safeThis->finishImport (*batch);
        });
}

void ArrangementView::finishImport (const ImportBatch& batch)
{
    if (project.indexOfTrack (batch.track) < 0)
        return;
    auto beat = batch.startBeat;
    bool beganTransaction = false;
    juce::ValueTree lastClip;
    for (int i = 0; i < batch.files.size(); ++i)
    {
        if (batch.samples[(size_t) i] == nullptr)
            continue;
        if (! beganTransaction)
        {
            project.beginTransaction ("Import audio files");
            beganTransaction = true;
        }
        const auto clip = project.addClip (batch.track, juce::File (batch.files[i]), beat);
        if (clip.isValid())
        {
            beat = Project::getClipEnd (clip);
            lastClip = clip;
        }
    }
    if (lastClip.isValid())
        lanes->select (lastClip);
}

void ArrangementView::modelChanged (bool tracksChanged)
{
    lanes->validateSelection();
    if (tracksChanged)
    {
        lanes->syncHeaders();
        resized();
    }
    lanes->refreshAutomation();
    lanes->repaint();
}

void ArrangementView::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (ids::TRACK) && property == ids::height)
    {
        // Layout-only state: avoid rebuilding automation target lists on every drag pixel.
        resized();
        return;
    }
    modelChanged (false);
}

void ArrangementView::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    modelChanged (parent == state);
}

void ArrangementView::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    modelChanged (parent == state);
}

void ArrangementView::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    modelChanged (parent == state);
}

} // namespace daw
