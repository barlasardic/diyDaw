#pragma once

#include "AudioTrack.h"
#include "Engine.h"
#include "LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>


namespace daw
{

/** One vertical mixer channel strip for an AudioTrack (Ableton session-mixer style).

    Top to bottom: track name header, pan knob, mute ("track activator") + solo
    buttons, then fader with a level meter beside it and a dB readout.
    (Clips are placed in the arrangement view, not here.)

    Message thread only.
*/
class TrackStrip : public juce::Component
{
public:
    explicit TrackStrip (AudioTrack& track);

    /** Called ~30 Hz by the owner: pulls meter levels and resyncs controls. */
    void refresh();

    static constexpr int preferredWidth = 112;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AudioTrack& track;

    juce::Label nameLabel;
    juce::Slider panKnob;
    juce::TextButton muteButton;   // lit = track active (Ableton "track activator")
    juce::TextButton soloButton { "S" };
    juce::Slider fader;
    juce::Label gainLabel;
    LevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackStrip)
};

/** Master channel: master fader + meter, same visual language as TrackStrip. */
class MasterStrip : public juce::Component
{
public:
    explicit MasterStrip (Engine& engine);

    void refresh();

    static constexpr int preferredWidth = 112;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Engine& engine;

    juce::Label nameLabel;
    juce::Slider fader;
    juce::Label gainLabel;
    LevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterStrip)
};

} // namespace daw
