#pragma once

#include "Engine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** Top bar: play / stop, tempo, metronome, playhead position, global actions.
    Message thread only. */
class TransportBar : public juce::Component
{
public:
    explicit TransportBar (Engine& engine);

    /** Call from a UI timer to sync the display with the engine. */
    void refresh();

    std::function<void()> onAddTrack;
    std::function<void()> onAudioSettings;
    std::function<void()> onToggleView;

    /** Label of the view-switch button, e.g. "Mixer" while the arrangement is shown. */
    void setViewButtonText (const juce::String& text) { viewButton.setButtonText (text); }

    void paint (juce::Graphics&) override;
    void resized() override;

    /** "Bar.Beat.Sixteenth", 1-based, e.g. beat 0 -> "1.1.1". */
    static juce::String formatPosition (double beats, int beatsPerBar);

private:
    Engine& engine;

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
