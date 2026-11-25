#pragma once

#include "Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace daw
{

/** Device view of a MIDI track's Synth (Ableton device style): a titled panel with sections
    "Oscillators" (osc1/osc2 wave selectors, osc2 semitones and detune, mix), "Filter"
    (cutoff, resonance, env amount, key track), "Filter Envelope" (A D S R), "Amp Envelope"
    (A D S R) and "Output" (velocity sensitivity, volume).

    Controls are generated from Synth::getParamInfos(): continuous params are rotary knobs
    (skewed so centreValue is at mid travel, value popup with unit), stepped wave params are
    combo boxes, osc2Semitones a stepped knob. Every control edits the model with
    Project::setDeviceParam through bindSliderToProject / an onChange that begins a transaction
    (so one gesture = one undo step); refresh() pulls model values back (undo, file load).

    Message thread only. */
class SynthPanel : public juce::Component
{
public:
    explicit SynthPanel (Project& project);
    ~SynthPanel() override;

    /** Width the device chain gives the instrument, wide enough for all knob columns. */
    static constexpr int preferredWidth = 880;

    /** The MIDI track whose synth to show, or invalid for an empty panel. */
    void setTrack (juce::ValueTree track);
    juce::ValueTree getTrack() const { return track; }

    /** Called ~30 Hz: resync controls from the model (never fight an active drag). */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Project& project;
    juce::ValueTree track;

    struct Controls;
    std::unique_ptr<Controls> controls;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthPanel)
};

} // namespace daw
