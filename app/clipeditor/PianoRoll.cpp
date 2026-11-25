#include "PianoRoll.h"
#include "DawLookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace daw
{

namespace
{
    bool commandDown (const juce::ModifierKeys& mods)
    {
        return mods.isCommandDown() || mods.isCtrlDown();
    }

    bool blackKey (int pitch)
    {
        const auto key = pitch % 12;
        return key == 1 || key == 3 || key == 6 || key == 8 || key == 10;
    }

    juce::String pitchName (int pitch)
    {
        static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (names[pitch % 12]) + juce::String (pitch / 12 - 2);
    }
}

PianoRoll::PianoRoll (Project& p) : project (p)
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
    setOpaque (true);
}

PianoRoll::~PianoRoll()
{
    stopPreview();
    clip.removeListener (this);
}

bool PianoRoll::hasClip() const
{
    return clip.hasType (ids::CLIP) && Project::isMidiClip (clip)
        && project.indexOfTrack (clip.getParent()) >= 0;
}

bool PianoRoll::hasNote (const juce::ValueTree& note) const
{
    return hasClip() && note.hasType (ids::NOTE) && note.getParent() == clip;
}

bool PianoRoll::isSelected (const juce::ValueTree& note) const
{
    return std::find (selection.begin(), selection.end(), note) != selection.end();
}

void PianoRoll::validateSelection()
{
    std::erase_if (selection, [this] (const auto& note) { return ! hasNote (note); });
    if (! hasClip() || std::any_of (drag.original.begin(), drag.original.end(),
                                   [this] (const auto& state) { return ! hasNote (state.note); }))
        cancelDrag();
}

void PianoRoll::setClip (juce::ValueTree newClip)
{
    if (newClip == clip)
    {
        validateSelection();
        return;
    }

    cancelDrag();
    clip.removeListener (this);
    clip = newClip.hasType (ids::CLIP) && Project::isMidiClip (newClip) ? newClip : juce::ValueTree();
    clip.addListener (this);
    selection.clear();
    playheadBeat = -1.0;
    needsFit = true;
    fitClip();
    refresh();
    repaint();
}

juce::Rectangle<float> PianoRoll::gridBounds() const
{
    return { (float) keyboardWidth, (float) rulerHeight,
             (float) juce::jmax (0, getWidth() - keyboardWidth),
             (float) juce::jmax (0, getHeight() - rulerHeight - velocityLaneHeight) };
}

juce::Rectangle<float> PianoRoll::velocityBounds() const
{
    const auto grid = gridBounds();
    return { grid.getX(), grid.getBottom(), grid.getWidth(),
             (float) juce::jmax (0, getHeight() - (int) grid.getBottom()) };
}

void PianoRoll::fitClip()
{
    const auto grid = gridBounds();
    if (! hasClip() || grid.getWidth() <= 24.0f || grid.getHeight() <= 0.0f)
        return;

    geometry.beatsPerBar = project.getEngine().getTransport().getBeatsPerBar();
    geometry.viewStartBeat = 0.0;
    const auto loop = juce::jmax (Project::minClipLengthBeats, Project::getClipLoopLength (clip));
    geometry.pixelsPerBeat = juce::jlimit (TimelineGeometry::minPixelsPerBeat,
                                          TimelineGeometry::maxPixelsPerBeat,
                                          (double) (grid.getWidth() - 24.0f) / loop);
    int lowest = 127, highest = 0;
    for (const auto& note : clip)
        if (note.hasType (ids::NOTE))
        {
            lowest = juce::jmin (lowest, Project::getNotePitch (note));
            highest = juce::jmax (highest, Project::getNotePitch (note));
        }

    const auto centre = lowest <= highest ? (float) (lowest + highest) * 0.5f : 60.0f;
    rowHeight = lowest <= highest ? juce::jlimit (6.0f, 14.0f, grid.getHeight() / (float) (highest - lowest + 5))
                                 : 14.0f;
    pitchScroll = (127.5f - centre) * rowHeight - grid.getHeight() * 0.5f;
    clampPitchScroll();
    needsFit = false;
}

void PianoRoll::clampPitchScroll()
{
    pitchScroll = juce::jlimit (0.0f, juce::jmax (0.0f, 128.0f * rowHeight - gridBounds().getHeight()), pitchScroll);
}

void PianoRoll::resized()
{
    if (needsFit)
        fitClip();
    clampPitchScroll();
    repaint();
}

float PianoRoll::pitchToY (int pitch) const
{
    return gridBounds().getY() + (float) (127 - pitch) * rowHeight - pitchScroll;
}

int PianoRoll::pitchAt (float y) const
{
    return juce::jlimit (0, 127, 127 - (int) std::floor ((y - gridBounds().getY() + pitchScroll) / rowHeight));
}

double PianoRoll::beatAt (float x) const { return geometry.xToBeat (x - (float) keyboardWidth); }
float PianoRoll::xAt (double beat) const { return (float) keyboardWidth + geometry.beatToX (beat); }

PianoRoll::NoteState PianoRoll::readNote (const juce::ValueTree& note)
{
    return { note, Project::getNoteStart (note), Project::getNoteLength (note),
             Project::getNotePitch (note), Project::getNoteVelocity (note) };
}

PianoRoll::NoteState PianoRoll::displayedNote (const juce::ValueTree& note) const
{
    if (drag.moved)
        for (const auto& state : drag.preview)
            if (state.note == note)
                return state;
    return readNote (note);
}

juce::Rectangle<float> PianoRoll::noteBounds (const NoteState& state) const
{
    return { xAt (state.start), pitchToY (state.pitch) + 1.0f,
             juce::jmax (2.0f, (float) (state.length * geometry.pixelsPerBeat)),
             juce::jmax (2.0f, rowHeight - 2.0f) };
}

juce::Rectangle<float> PianoRoll::velocityBar (const NoteState& state) const
{
    const auto lane = velocityBounds().reduced (0.0f, 7.0f);
    const auto height = juce::jmax (2.0f, lane.getHeight() * state.velocity);
    return { xAt (state.start) - 2.0f, lane.getBottom() - height, 4.0f, height };
}

void PianoRoll::refresh()
{
    validateSelection();
    auto& transport = project.getEngine().getTransport();
    const auto oldBeat = playheadBeat;
    const auto oldMeter = geometry.beatsPerBar;
    geometry.beatsPerBar = transport.getBeatsPerBar();
    playheadBeat = -1.0;
    if (hasClip())
    {
        const auto position = transport.getPositionBeats();
        if (position >= Project::getClipStart (clip) && position < Project::getClipEnd (clip))
        {
            playheadBeat = Project::getClipOffsetBeats (clip) + position - Project::getClipStart (clip);
            const auto loop = Project::getClipLoopLength (clip);
            if (Project::isClipLooping (clip) && loop > 0.0)
            {
                playheadBeat = std::fmod (playheadBeat, loop);
                if (playheadBeat < 0.0)
                    playheadBeat += loop;
            }
        }
    }

    if (oldMeter != geometry.beatsPerBar)
        repaint();
    else if (! juce::exactlyEqual (oldBeat, playheadBeat))
    {
        for (const auto beat : { oldBeat, playheadBeat })
            if (beat >= 0.0 && xAt (beat) >= (float) keyboardWidth && xAt (beat) <= (float) getWidth())
                repaint ((int) std::floor (xAt (beat)) - 1, 0, 4, getHeight());
    }
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    if (! hasClip())
    {
        g.setColour (colours::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Select a MIDI clip to edit its notes", getLocalBounds(), juce::Justification::centred);
        return;
    }

    const auto grid = gridBounds();
    const auto velocity = velocityBounds();
    const auto colour = Project::getTrackColour (clip.getParent());
    const auto loop = drag.mode == DragMode::loopEnd ? drag.loop : Project::getClipLoopLength (clip);
    const auto loopX = xAt (loop);
    const auto width = (float) getWidth();

    g.saveState();
    g.reduceClipRegion (juce::Rectangle<int> (0, rulerHeight, getWidth(), (int) grid.getHeight()));
    for (int pitch = 0; pitch < 128; ++pitch)
    {
        const auto y = pitchToY (pitch);
        if (y + rowHeight < grid.getY() || y > grid.getBottom())
            continue;

        g.setColour (blackKey (pitch) ? colours::panel.darker (0.16f) : colours::panel);
        g.fillRect (grid.getX(), y, grid.getWidth(), rowHeight);
        g.setColour (pitch % 12 == 0 ? colours::textDim.withAlpha (0.25f) : colours::outline.withAlpha (0.32f));
        g.drawHorizontalLine ((int) std::floor (y + rowHeight), grid.getX(), width);

        const auto sounding = pitch == previewPitch;
        g.setColour (sounding ? colours::accent : (blackKey (pitch) ? colours::background : colours::panelRaised.brighter (0.2f)));
        g.fillRect (0.0f, y, (float) keyboardWidth - 1.0f, rowHeight);
        if (blackKey (pitch) && ! sounding)
        {
            g.setColour (colours::panelRaised);
            g.fillRect ((float) keyboardWidth - 13.0f, y, 12.0f, rowHeight);
        }
        g.setColour (colours::outline.withAlpha (0.65f));
        g.drawHorizontalLine ((int) std::floor (y + rowHeight), 0.0f, (float) keyboardWidth);
        if (pitch % 12 == 0)
        {
            g.setColour (sounding ? colours::background : colours::text);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (pitchName (pitch), 5, (int) std::floor (y - 1.0f), keyboardWidth - 9,
                        juce::jmax (10, (int) rowHeight + 2), juce::Justification::centredRight);
        }
    }
    g.restoreState();

    g.setColour (colours::panelRaised);
    g.fillRect (keyboardWidth, 0, juce::jmax (0, getWidth() - keyboardWidth), rulerHeight);
    g.setColour (colours::background.brighter (0.06f));
    g.fillRect (velocity);

    // Share the same time grid across the ruler, notes and velocity lane.
    g.saveState();
    g.reduceClipRegion (keyboardWidth, 0, juce::jmax (0, getWidth() - keyboardWidth), getHeight());
    const auto step = geometry.gridBeats();
    const auto bar = (double) geometry.beatsPerBar;
    const auto lastBeat = beatAt (width);
    const auto labelStep = geometry.gridBeats (48.0f);
    g.setFont (juce::FontOptions (10.0f));
    for (auto beat = std::floor (geometry.viewStartBeat / step) * step; beat <= lastBeat; beat += step)
    {
        const auto isBar = std::abs (beat / bar - std::round (beat / bar)) < 1.0e-7;
        const auto isBeat = std::abs (beat - std::round (beat)) < 1.0e-7;
        const auto x = xAt (beat);
        g.setColour (isBar ? colours::textDim.withAlpha (0.38f)
                          : colours::outline.withAlpha (isBeat ? 0.7f : 0.4f));
        g.drawVerticalLine ((int) std::floor (x), grid.getY(), velocity.getBottom());
        g.setColour (colours::textDim);
        g.drawVerticalLine ((int) std::floor (x), isBar ? 5.0f : 15.0f, (float) rulerHeight);
        if (std::abs (beat / labelStep - std::round (beat / labelStep)) < 1.0e-7 && isBeat)
        {
            auto label = juce::String ((int) std::floor (beat / bar) + 1);
            if (! isBar)
                label += "." + juce::String ((int) std::fmod (beat, bar) + 1);
            g.setColour (isBar ? colours::text : colours::textDim);
            g.drawText (label, (int) x + 4, 5, 44, rulerHeight - 5, juce::Justification::centredLeft);
        }
    }

    g.setColour (colours::background.withAlpha (0.58f));
    if (loopX < width)
        g.fillRect (juce::jmax (grid.getX(), loopX), 0.0f, width - juce::jmax (grid.getX(), loopX), (float) getHeight());
    g.setColour (colour.withAlpha (0.65f));
    g.fillRect (xAt (0.0), 0.0f, juce::jmax (0.0f, loopX - xAt (0.0)), 3.0f);
    if (loopX >= grid.getX() && loopX <= width)
    {
        g.setColour (colours::accent);
        g.fillRect (loopX - 1.0f, 0.0f, 1.0f, (float) rulerHeight);
        juce::Path marker;
        marker.addTriangle (loopX - 9.0f, 0.0f, loopX, 0.0f, loopX, 9.0f);
        g.fillPath (marker);
        g.setColour (colours::textDim.withAlpha (0.5f));
        g.drawVerticalLine ((int) loopX, grid.getY(), velocity.getBottom());
    }
    g.restoreState();

    const auto drawNote = [&] (const NoteState& state, bool selected, bool ghost, float alpha)
    {
        const auto bounds = noteBounds (state);
        const auto noteColour = selected ? colour.brighter (0.35f) : colour;
        g.saveState();
        g.reduceClipRegion (grid.toNearestInt());
        g.setColour (noteColour.withAlpha ((0.30f + 0.70f * state.velocity) * alpha));
        g.fillRect (bounds);
        g.setColour ((selected || ghost ? colours::text : colour.darker (0.35f)).withAlpha (alpha));
        g.drawRect (bounds, 1.0f);
        if (rowHeight >= 11.0f && bounds.getWidth() >= 36.0f)
        {
            g.setColour (colours::background.withAlpha (alpha));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (pitchName (state.pitch), bounds.reduced (4.0f, 0.0f), juce::Justification::centredLeft);
        }
        g.restoreState();

        g.saveState();
        g.reduceClipRegion (velocity.toNearestInt());
        const auto stem = velocityBar (state);
        g.setColour (noteColour.withAlpha (alpha * (selected ? 1.0f : 0.7f)));
        g.fillRect (stem);
        g.fillEllipse (stem.getCentreX() - 3.0f, stem.getY() - 2.0f, 6.0f, 4.0f);
        if (selected)
        {
            g.setColour (colours::text.withAlpha (alpha));
            g.drawHorizontalLine ((int) stem.getY(), stem.getX() - 1.0f, stem.getRight() + 1.0f);
        }
        g.restoreState();
    };

    // Leave the originals faintly visible behind the uncommitted drag preview.
    for (const auto& note : clip)
        if (note.hasType (ids::NOTE))
        {
            const auto source = drag.moved && std::any_of (drag.original.begin(), drag.original.end(),
                                                          [&note] (const auto& state) { return state.note == note; });
            drawNote (readNote (note), isSelected (note), false, source ? 0.18f : 1.0f);
        }
    if (drag.moved || drag.mode == DragMode::add)
        for (const auto& state : drag.preview)
            drawNote (state, true, true, 0.88f);

    if (drag.mode == DragMode::marquee && drag.moved)
    {
        const auto bounds = juce::Rectangle<float> (drag.anchor, drag.current).getIntersection (grid);
        g.setColour (colours::accent.withAlpha (0.12f));
        g.fillRect (bounds);
        g.setColour (colours::accent);
        g.drawRect (bounds, 1.0f);
    }

    if (playheadBeat >= 0.0)
    {
        const auto x = xAt (playheadBeat);
        if (x >= grid.getX() && x < width)
        {
            g.setColour (colours::text);
            g.fillRect (x, (float) rulerHeight, 1.0f, (float) getHeight() - (float) rulerHeight);
        }
    }

    g.setColour (colours::panelRaised);
    g.fillRect (0, 0, keyboardWidth, rulerHeight);
    g.fillRect (0.0f, velocity.getY(), (float) keyboardWidth, velocity.getHeight());
    g.setFont (juce::FontOptions (10.0f));
    g.setColour (colours::textDim);
    g.drawText ("NOTES", 0, 0, keyboardWidth, rulerHeight, juce::Justification::centred);
    g.drawText ("Velocity", 0, (int) velocity.getY() + 6, keyboardWidth, 16, juce::Justification::centred);
    const auto gridLabel = step <= 4.0 ? "1/" + juce::String ((int) std::round (4.0 / step))
                                      : juce::String (step / bar, 0) + " bars";
    g.drawText (gridLabel,
                0, (int) velocity.getY() + 25, keyboardWidth, 16, juce::Justification::centred);
    g.setColour (colours::outline);
    g.drawVerticalLine (keyboardWidth - 1, 0.0f, (float) getHeight());
    g.drawHorizontalLine (rulerHeight - 1, 0.0f, width);
    g.drawHorizontalLine ((int) grid.getBottom(), 0.0f, width);

    if (clip.getNumChildren() == 0)
    {
        g.setColour (colours::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Double-click to add notes", grid.toNearestInt(), juce::Justification::centred);
    }
}

juce::ValueTree PianoRoll::noteAt (juce::Point<float> point, bool velocity) const
{
    if (! hasClip() || ! (velocity ? velocityBounds() : gridBounds()).contains (point))
        return {};

    juce::ValueTree best;
    auto bestDistance = std::numeric_limits<float>::max();
    for (int i = clip.getNumChildren(); --i >= 0;)
    {
        const auto note = clip.getChild (i);
        if (! note.hasType (ids::NOTE))
            continue;
        const auto state = displayedNote (note);
        const auto bounds = velocity ? velocityBar (state).expanded (4.0f, 3.0f) : noteBounds (state);
        if (! bounds.contains (point))
            continue;
        if (! velocity)
            return note;

        // Nearby caps distinguish velocities of notes that start at the same beat.
        const auto distance = std::abs (point.y - velocityBar (state).getY()) + (isSelected (note) ? 0.0f : 0.5f);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = note;
        }
    }
    return best;
}

bool PianoRoll::atRightEdge (const juce::ValueTree& note, float x) const
{
    const auto bounds = noteBounds (readNote (note));
    return bounds.getRight() - x <= juce::jmin (5.0f, bounds.getWidth() * 0.4f);
}

bool PianoRoll::atLoopEnd (juce::Point<float> point) const
{
    return point.x >= (float) keyboardWidth && point.y >= 0.0f && point.y < (float) rulerHeight
        && std::abs (point.x - xAt (Project::getClipLoopLength (clip))) <= 9.0f;
}

void PianoRoll::updateCursor (juce::Point<float> point)
{
    auto cursorType = juce::MouseCursor::NormalCursor;
    if (hasClip())
    {
        const auto note = noteAt (point, false);
        if (atLoopEnd (point) || (note.isValid() && atRightEdge (note, point.x)))
            cursorType = juce::MouseCursor::LeftRightResizeCursor;
        else if (note.isValid())
            cursorType = juce::MouseCursor::DraggingHandCursor;
        else if (noteAt (point, true).isValid())
            cursorType = juce::MouseCursor::UpDownResizeCursor;
        else if (gridBounds().contains (point))
            cursorType = juce::MouseCursor::CrosshairCursor;
    }
    setMouseCursor (cursorType);
}

void PianoRoll::previewNote (int pitch, float velocity)
{
    if (pitch == previewPitch)
        return;
    stopPreview();
    previewPitch = pitch;
    if (onPreviewNote)
        onPreviewNote (pitch, velocity, true);
    repaint (0, rulerHeight, keyboardWidth, (int) gridBounds().getHeight());
}

void PianoRoll::stopPreview()
{
    if (previewPitch < 0)
        return;
    const auto pitch = previewPitch;
    previewPitch = -1;
    if (onPreviewNote)
        onPreviewNote (pitch, 0.0f, false);
    repaint (0, rulerHeight, keyboardWidth, (int) gridBounds().getHeight());
}

void PianoRoll::cancelDrag (bool restoreSelection)
{
    if (restoreSelection && drag.mode == DragMode::marquee)
    {
        selection = drag.initialSelection;
        std::erase_if (selection, [this] (const auto& note) { return ! hasNote (note); });
    }
    drag = {};
    stopPreview();
    setMouseCursor (juce::MouseCursor::NormalCursor);
    repaint();
}

void PianoRoll::mouseDown (const juce::MouseEvent& event)
{
    if (! event.mods.isLeftButtonDown())
        return;
    cancelDrag();
    validateSelection();
    if (! hasClip())
        return;
    if (isShowing())
        grabKeyboardFocus();

    drag.anchor = drag.current = event.position;
    if (event.position.x < (float) keyboardWidth
        && event.position.y >= gridBounds().getY() && event.position.y < gridBounds().getBottom())
    {
        drag.mode = DragMode::keyboard;
        previewNote (pitchAt (event.position.y), 0.8f);
        return;
    }
    if (atLoopEnd (event.position))
    {
        drag.mode = DragMode::loopEnd;
        drag.loop = drag.originalLoop = Project::getClipLoopLength (clip);
        return;
    }

    const auto inVelocity = velocityBounds().contains (event.position);
    if (! inVelocity && ! gridBounds().contains (event.position))
        return;
    const auto note = noteAt (event.position, inVelocity);
    const auto toggle = event.mods.isShiftDown() || commandDown (event.mods);
    if (! note.isValid())
    {
        if (inVelocity)
            return;
        // JUCE sends mouseDoubleClick after mouseUp. Start on the second down so
        // the new note is auditioned while held and committed with that mouseUp.
        if (event.getNumberOfClicks() == 2)
        {
            selection.clear();
            drag.mode = DragMode::add;
            const auto start = juce::jmax (0.0, event.mods.isAltDown() ? beatAt (event.position.x)
                                                                 : geometry.snap (beatAt (event.position.x)));
            drag.preview.push_back ({ {}, start, geometry.gridBeats(), pitchAt (event.position.y), 0.8f });
            previewNote (drag.preview.front().pitch, drag.preview.front().velocity);
        }
        else
        {
            drag.mode = DragMode::marquee;
            drag.initialSelection = selection;
            drag.additive = toggle;
            if (! toggle)
                selection.clear();
        }
        repaint();
        return;
    }

    if (toggle)
    {
        if (isSelected (note))
        {
            std::erase (selection, note);
            repaint();
            return;
        }
        selection.push_back (note);
    }
    else if (! isSelected (note))
        selection = { note };

    drag.primary = note;
    drag.mode = inVelocity ? DragMode::velocity : (atRightEdge (note, event.position.x) ? DragMode::length : DragMode::move);
    for (const auto& selected : selection)
        drag.original.push_back (readNote (selected));
    drag.preview = drag.original;
    if (drag.mode == DragMode::move)
        previewNote (Project::getNotePitch (note), Project::getNoteVelocity (note));
    updateCursor (event.position);
    repaint();
}

void PianoRoll::updateDrag (const juce::MouseEvent& event)
{
    drag.current = event.position;
    const auto beatDelta = beatAt (event.position.x) - beatAt (drag.anchor.x);
    const auto snap = [this, &event] (double beat)
    {
        return event.mods.isAltDown() ? beat : geometry.snap (beat);
    };

    if (drag.mode == DragMode::keyboard)
    {
        previewNote (pitchAt (event.position.y), 0.8f);
        return;
    }
    if (drag.mode == DragMode::marquee)
    {
        selection = drag.additive ? drag.initialSelection : std::vector<juce::ValueTree>();
        const auto bounds = juce::Rectangle<float> (drag.anchor, drag.current).getIntersection (gridBounds());
        for (const auto& note : clip)
            if (note.hasType (ids::NOTE) && bounds.intersects (noteBounds (readNote (note))) && ! isSelected (note))
                selection.push_back (note);
        return;
    }
    if (drag.mode == DragMode::loopEnd)
    {
        drag.loop = juce::jmax (Project::minClipLengthBeats, snap (drag.originalLoop + beatDelta));
        return;
    }
    if (drag.mode == DragMode::add || drag.original.empty())
        return;

    const auto primary = std::find_if (drag.original.begin(), drag.original.end(),
                                      [this] (const auto& state) { return state.note == drag.primary; });
    if (primary == drag.original.end())
        return;
    drag.preview = drag.original;

    if (drag.mode == DragMode::move)
    {
        auto delta = snap (primary->start + beatDelta) - primary->start;
        auto pitchDelta = (int) std::round ((drag.anchor.y - event.position.y) / rowHeight);
        double first = primary->start;
        int low = 127, high = 0;
        for (const auto& state : drag.original)
        {
            first = juce::jmin (first, state.start);
            low = juce::jmin (low, state.pitch);
            high = juce::jmax (high, state.pitch);
        }
        delta = juce::jmax (-first, delta);
        pitchDelta = juce::jlimit (-low, 127 - high, pitchDelta);
        for (auto& state : drag.preview)
        {
            state.start += delta;
            state.pitch += pitchDelta;
            if (state.note == drag.primary)
                previewNote (state.pitch, state.velocity);
        }
    }
    else if (drag.mode == DragMode::length)
    {
        const auto end = primary->start + primary->length;
        auto delta = snap (end + beatDelta) - end;
        for (const auto& state : drag.original)
            delta = juce::jmax (delta, Project::minNoteLengthBeats - state.length);
        for (auto& state : drag.preview)
            state.length += delta;
    }
    else if (drag.mode == DragMode::velocity)
    {
        auto delta = (drag.anchor.y - event.position.y) / juce::jmax (1.0f, velocityBounds().getHeight() - 14.0f);
        float low = 1.0f, high = 0.0f;
        for (const auto& state : drag.original)
        {
            low = juce::jmin (low, state.velocity);
            high = juce::jmax (high, state.velocity);
        }
        delta = juce::jlimit (-low, 1.0f - high, delta);
        for (auto& state : drag.preview)
            state.velocity += delta;
    }
}

void PianoRoll::mouseDrag (const juce::MouseEvent& event)
{
    if (drag.mode == DragMode::none || ! hasClip())
        return;
    if (! drag.moved && event.getDistanceFromDragStart() < 3)
        return;
    drag.moved = true;
    updateDrag (event);
    repaint();
}

void PianoRoll::mouseUp (const juce::MouseEvent& event)
{
    if (drag.mode == DragMode::none)
        return;
    validateSelection();
    if (drag.moved)
        updateDrag (event);
    const auto completed = drag;
    cancelDrag();
    if (! hasClip())
        return;

    if (completed.mode == DragMode::add && ! completed.preview.empty())
    {
        const auto& state = completed.preview.front();
        project.beginTransaction ("Add note");
        const auto note = project.addNote (clip, state.pitch, state.start, state.length, state.velocity);
        if (hasNote (note))
            selection = { note };
    }
    else if (completed.mode == DragMode::loopEnd && completed.moved
             && ! juce::exactlyEqual (completed.loop, completed.originalLoop))
    {
        project.beginTransaction ("Change MIDI loop length");
        project.setClipLoopLength (clip, completed.loop);
    }
    else if (completed.moved && ! completed.original.empty())
    {
        bool changed = false;
        for (size_t i = 0; i < completed.original.size(); ++i)
        {
            const auto& before = completed.original[i];
            const auto& after = completed.preview[i];
            changed = changed || ! juce::exactlyEqual (before.start, after.start) || before.pitch != after.pitch
                              || ! juce::exactlyEqual (before.length, after.length)
                              || ! juce::exactlyEqual (before.velocity, after.velocity);
        }
        if (changed)
        {
            project.beginTransaction (completed.mode == DragMode::move ? "Move notes"
                                      : completed.mode == DragMode::length ? "Resize notes" : "Change note velocity");
            // Notifications are synchronous; commit a copy, after clearing the drag.
            for (const auto& state : completed.preview)
                if (hasNote (state.note))
                {
                    if (completed.mode == DragMode::move)
                        project.moveNote (state.note, state.start, state.pitch);
                    else if (completed.mode == DragMode::length)
                        project.setNoteLength (state.note, state.length);
                    else if (completed.mode == DragMode::velocity)
                        project.setNoteVelocity (state.note, state.velocity);
                }
        }
    }
    updateCursor (event.position);
    repaint();
}

void PianoRoll::mouseMove (const juce::MouseEvent& event) { updateCursor (event.position); }
void PianoRoll::mouseExit (const juce::MouseEvent&)
{
    if (drag.mode == DragMode::none)
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel)
{
    if (! hasClip())
        return;
    cancelDrag (true);
    needsFit = false;
    if (commandDown (event.mods))
    {
        const auto delta = std::abs (wheel.deltaY) > 0.0f ? wheel.deltaY : wheel.deltaX;
        geometry.zoomAround (juce::jlimit (0.0f, gridBounds().getWidth(), event.position.x - (float) keyboardWidth),
                             std::exp ((double) delta * 2.0));
    }
    else if (event.mods.isShiftDown())
    {
        const auto delta = std::abs (wheel.deltaX) > 0.0f ? wheel.deltaX : wheel.deltaY;
        geometry.scrollBy (-delta * 240.0f);
    }
    else
    {
        geometry.scrollBy (-wheel.deltaX * 240.0f);
        pitchScroll -= wheel.deltaY * rowHeight * 12.0f;
        clampPitchScroll();
    }
    repaint();
}

bool PianoRoll::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::escapeKey && drag.mode != DragMode::none)
    {
        cancelDrag (true);
        return true;
    }
    validateSelection();
    if (! hasClip())
        return false;
    const auto command = commandDown (key.getModifiers());
    if (command && code == 'A')
    {
        cancelDrag();
        selection.clear();
        for (const auto& note : clip)
            if (note.hasType (ids::NOTE))
                selection.push_back (note);
        repaint();
        return true;
    }
    if (selection.empty())
        return false;

    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        cancelDrag();
        const auto notes = selection;
        project.beginTransaction ("Delete notes");
        for (const auto& note : notes)
            if (hasNote (note))
                project.deleteNote (note);
        selection.clear();
        repaint();
        return true;
    }
    if (command && code == 'D')
    {
        cancelDrag();
        double first = std::numeric_limits<double>::max(), end = 0.0;
        std::vector<NoteState> originals;
        for (const auto& note : selection)
        {
            originals.push_back (readNote (note));
            first = juce::jmin (first, Project::getNoteStart (note));
            end = juce::jmax (end, Project::getNoteStart (note) + Project::getNoteLength (note));
        }
        project.beginTransaction ("Duplicate notes");
        selection.clear();
        for (const auto& state : originals)
        {
            const auto note = project.addNote (clip, state.pitch, state.start + end - first, state.length, state.velocity);
            if (hasNote (note))
                selection.push_back (note);
        }
        repaint();
        return true;
    }
    // Leave transport, tab switching, undo and document shortcuts to MainComponent.
    if (command || key.getModifiers().isAltDown())
        return false;
    const auto vertical = code == juce::KeyPress::upKey || code == juce::KeyPress::downKey;
    const auto horizontal = code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey;
    if (! vertical && ! horizontal)
        return false;

    cancelDrag();
    int low = 127, high = 0;
    auto first = std::numeric_limits<double>::max();
    for (const auto& note : selection)
    {
        low = juce::jmin (low, Project::getNotePitch (note));
        high = juce::jmax (high, Project::getNotePitch (note));
        first = juce::jmin (first, Project::getNoteStart (note));
    }
    const auto semitones = key.getModifiers().isShiftDown() ? 12 : 1;
    const auto pitchDelta = vertical ? juce::jlimit (-low, 127 - high, code == juce::KeyPress::upKey ? semitones : -semitones) : 0;
    const auto beatDelta = horizontal ? juce::jmax (-first, code == juce::KeyPress::rightKey ? geometry.gridBeats() : -geometry.gridBeats()) : 0.0;
    if (pitchDelta != 0 || ! juce::exactlyEqual (beatDelta, 0.0))
    {
        const auto notes = selection;
        project.beginTransaction (vertical ? "Transpose notes" : "Move notes");
        for (const auto& note : notes)
            if (hasNote (note))
                project.moveNote (note, Project::getNoteStart (note) + beatDelta, Project::getNotePitch (note) + pitchDelta);
    }
    repaint();
    return true;
}

void PianoRoll::focusLost (FocusChangeType) { cancelDrag (true); }
void PianoRoll::visibilityChanged()
{
    if (! isShowing())
        cancelDrag (true);
}

void PianoRoll::valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&)
{
    cancelDrag();
    validateSelection();
    repaint();
}

void PianoRoll::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&)
{
    cancelDrag();
    repaint();
}

void PianoRoll::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int)
{
    cancelDrag();
    validateSelection();
    repaint();
}

void PianoRoll::valueTreeParentChanged (juce::ValueTree&)
{
    cancelDrag();
    validateSelection();
    repaint();
}

} // namespace daw
