#pragma once

#include "Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace daw
{

/** Device view of a drum track's DrumRack (Ableton Drum Rack style, basic).

    Left: a 4x4 grid of pads, pad 0 (C1, MIDI 36) bottom-left, pad 15 top-right. Each pad shows
    its note name ("C1") and sample name (Project::getPadName) or "Drop sample" when empty; the
    selected pad is highlighted; pads flash briefly when auditioned. Clicking a pad selects it and
    auditions it (onPreviewNote note-on on mouse down, note-off on mouse up). Dropping an audio
    file from the OS onto a pad loads it (SamplePool::loadAsync) and puts it on that pad
    (Project::setPadSample, one transaction); dropping several files fills consecutive pads.
    Right: controls of the selected pad - its name, knobs Gain / Pan / Tune / Decay
    (bindSliderToProject -> Project::setDeviceParam (device, DrumRack::paramIndex (pad, p), v)),
    a Choke combo ("None", "1".."8"), a "Sound" menu button listing the builtin sounds
    (getBuiltinSampleNames, shown by Project::getPadName style) to put one on the pad, and
    "Clear". Every edit is one undo step; refresh() pulls values back from the model.

    Message thread only. */
class DrumRackPanel : public juce::Component,
                      public juce::FileDragAndDropTarget
{
public:
    explicit DrumRackPanel (Project& project);
    ~DrumRackPanel() override;

    /** A drum track (its instrument DEVICE is a "drumrack"), or invalid for an empty panel. */
    void setTrack (juce::ValueTree track);
    juce::ValueTree getTrack() const { return track; }

    void refresh();

    std::function<void (int pitch, float velocity, bool isNoteOn)> onPreviewNote;

    static constexpr int preferredWidth = 760;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void selectPad (int pad);
    void stopPreview();
    void rebuildKnobs();
    void showSoundMenu();
    int padAt (int x, int y) const;
    void visibilityChanged() override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;

    Project& project;
    juce::ValueTree track;

    struct Controls;
    std::unique_ptr<Controls> controls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumRackPanel)
};

} // namespace daw
