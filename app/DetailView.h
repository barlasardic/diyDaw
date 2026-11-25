#pragma once

#include "PianoRoll.h"
#include "SynthPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** Ableton's bottom "detail view": a header with the selected clip / track name and two
    tabs, Clip (piano roll of a MIDI clip) and Device (the MIDI track's synth).
    Message thread only. */
class DetailView : public juce::Component
{
public:
    explicit DetailView (Project& project);

    /** Edits this clip (MIDI: piano roll; audio: a placeholder). Switches to the Clip tab. */
    void showClip (juce::ValueTree clip);

    /** Shows this track's device (without switching tabs). */
    void showTrack (juce::ValueTree track);

    enum class Tab { clip, device };
    void setTab (Tab);
    Tab getTab() const noexcept { return tab; }

    /** Track currently shown (owner of the clip, or the last showTrack). */
    juce::ValueTree getTrack() const { return track; }

    /** Called ~30 Hz. */
    void refresh();

    std::function<void (int pitch, float velocity, bool isNoteOn)> onPreviewNote;

    static constexpr int preferredHeight = 320;
    static constexpr int headerHeight = 26;

    PianoRoll& getPianoRoll() noexcept { return pianoRoll; }
    SynthPanel& getSynthPanel() noexcept { return synthPanel; }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void updateVisibility();

    Project& project;
    juce::ValueTree track, clip;
    Tab tab = Tab::clip;

    juce::TextButton clipTab { "Clip" }, deviceTab { "Device" };
    PianoRoll pianoRoll;
    SynthPanel synthPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DetailView)
};

} // namespace daw
