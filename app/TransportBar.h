#pragma once

#include "Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** Top bar: file menu, tempo (undoable, via Project), metronome, play / stop, playhead
    position, global actions.
    Message thread only. */
class TransportBar : public juce::Component
{
public:
    explicit TransportBar (Project& project);

    /** Call from a UI timer to sync the display with the engine. */
    void refresh();

    std::function<void()> onFileMenu; // the owner shows the menu (it owns the file actions)
    std::function<void()> onAddTrack;
    std::function<void()> onAudioSettings;
    std::function<void()> onToggleView;

    /** Label of the view-switch button, e.g. "Mixer" while the arrangement is shown. */
    void setViewButtonText (const juce::String& text) { viewButton.setButtonText (text); }

    void paint (juce::Graphics&) override;
    void resized() override;

    /** "Bar.Beat.Sixteenth", 1-based, e.g. beat 0 -> "1.1.1". */
    static juce::String formatPosition (double beats, int beatsPerBar);

    juce::Component& getFileButton() noexcept { return fileButton; }

private:
    Project& project;
    Engine& engine;

    juce::TextButton fileButton { "File" };
    juce::ShapeButton playButton { "Play", {}, {}, {} };
    juce::ShapeButton stopButton { "Stop", {}, {}, {} };
    juce::Slider bpmSlider;
    juce::Label bpmCaption { {}, "BPM" };
    juce::TextButton metronomeButton { "Click" };
    juce::Label positionLabel;
    juce::TextButton addTrackButton { "+ Track" };
    juce::TextButton settingsButton { "Audio..." };
    juce::TextButton viewButton { "Mixer" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};

} // namespace daw
