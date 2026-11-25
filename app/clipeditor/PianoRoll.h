#pragma once

#include "Project.h"
#include "TimelineGeometry.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace daw
{

/** Ableton-style MIDI note editor for one MIDI clip (shown in the detail panel).

    Layout: a piano keyboard down the left (PianoRoll::keyboardWidth px), a beat ruler across
    the top (rulerHeight), the note grid, and a velocity lane along the bottom (velocityLaneHeight).
    Time is the clip's *content* time in beats (NOTE start), shown from 0; the loop region
    [0, loopLength) is highlighted and the loop end marker in the ruler can be dragged
    (Project::setClipLoopLength). Rows are pitches 0..127, C notes labelled (C3 = 60).

    Editing (each gesture = one Project::beginTransaction + undoable Project calls; while
    dragging show a preview, commit on mouseUp; Escape cancels a drag):
    - Double-click empty grid: add a note of the current grid length at the snapped position.
    - Click a note: select it (Shift/Ctrl-click toggles); click empty grid + drag: rubber-band select.
    - Drag selected notes: move in time (snapped; Alt = free) and pitch (by rows).
    - Drag a note's right edge (~5 px): change length of all selected notes.
    - Delete/Backspace: delete selection; Ctrl+A: select all; Ctrl+D: duplicate selection right
      after itself; Up/Down: transpose 1 semitone (Shift: octave); Left/Right: move by one grid step.
    - Velocity lane: drag a note's bar vertically to change its velocity (selected notes move together).
    - Ctrl+wheel zooms time around the mouse; wheel scrolls pitch; Shift+wheel scrolls time.
    - Clicking a key or adding/moving a note auditions it through onPreviewNote (note-on, then
      note-off on mouseUp).
    Keys handled by MainComponent (Space, Tab, Ctrl+Z/Y/T/S/O/N) must not be consumed.

    Message thread only. Listens to the clip's ValueTree to repaint on undo/redo. */
class PianoRoll : public juce::Component,
                  private juce::ValueTree::Listener
{
public:
    explicit PianoRoll (Project& project);
    ~PianoRoll() override;

    /** The MIDI clip to edit, or an invalid tree to show an empty-state hint. */
    void setClip (juce::ValueTree clip);
    juce::ValueTree getClip() const { return clip; }

    /** Called ~30 Hz: updates the playhead (transport position mapped into content time,
        when the playhead is inside the clip). */
    void refresh();

    /** Audition a note on the clip's track (pitch, velocity 0..1, isNoteOn). */
    std::function<void (int, float, bool)> onPreviewNote;

    static constexpr int keyboardWidth = 56;
    static constexpr int rulerHeight = 20;
    static constexpr int velocityLaneHeight = 56;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    enum class DragMode { none, move, length, velocity, marquee, loopEnd, keyboard, add };

    struct NoteState
    {
        juce::ValueTree note;
        double start = 0.0, length = 1.0;
        int pitch = 60;
        float velocity = 0.8f;
    };

    struct Drag
    {
        DragMode mode = DragMode::none;
        juce::Point<float> anchor, current;
        std::vector<NoteState> original, preview;
        std::vector<juce::ValueTree> initialSelection;
        juce::ValueTree primary;
        double originalLoop = 0.0, loop = 0.0;
        bool moved = false, additive = false;
    };

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusLost (FocusChangeType) override;
    void visibilityChanged() override;

    bool hasClip() const;
    bool hasNote (const juce::ValueTree&) const;
    bool isSelected (const juce::ValueTree&) const;
    void validateSelection();
    void fitClip();
    void clampPitchScroll();
    juce::Rectangle<float> gridBounds() const;
    juce::Rectangle<float> velocityBounds() const;
    float pitchToY (int pitch) const;
    int pitchAt (float y) const;
    double beatAt (float x) const;
    float xAt (double beat) const;
    static NoteState readNote (const juce::ValueTree&);
    NoteState displayedNote (const juce::ValueTree&) const;
    juce::Rectangle<float> noteBounds (const NoteState&) const;
    juce::Rectangle<float> velocityBar (const NoteState&) const;
    juce::ValueTree noteAt (juce::Point<float>, bool velocity) const;
    bool atRightEdge (const juce::ValueTree&, float x) const;
    bool atLoopEnd (juce::Point<float>) const;
    void updateCursor (juce::Point<float>);
    void updateDrag (const juce::MouseEvent&);
    void cancelDrag (bool restoreSelection = false);
    void previewNote (int pitch, float velocity);
    void stopPreview();

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override;

    Project& project;
    juce::ValueTree clip;
    TimelineGeometry geometry;
    std::vector<juce::ValueTree> selection;
    Drag drag;
    float rowHeight = 12.0f;
    float pitchScroll = 0.0f;
    double playheadBeat = -1.0;
    int previewPitch = -1;
    bool needsFit = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRoll)
};

} // namespace daw
