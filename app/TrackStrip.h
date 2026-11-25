#pragma once

#include "AudioTrack.h"
#include "Engine.h"
#include "LevelMeter.h"
#include "Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>


namespace daw
{

/** One vertical mixer channel strip for a project track (Ableton session-mixer style).
    Controls edit the Project (undoable); refresh() pulls the model back into the controls.

    Top to bottom: track name header, pan knob, mute ("track activator") + solo
    buttons, then fader with a level meter beside it and a dB readout.
    (Clips are placed in the arrangement view, not here.)

    Message thread only.
*/
class TrackStrip : public juce::Component
{
public:
    TrackStrip (Project& project, juce::ValueTree track);

    juce::ValueTree getTrack() const { return track; }

    /** Called ~30 Hz by the owner: pulls meter levels and resyncs controls. */
    void refresh();

    static constexpr int preferredWidth = 112;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Project& project;
    juce::ValueTree track;

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
class MasterStrip : public juce::Component,
                    public juce::SettableTooltipClient
{
public:
    explicit MasterStrip (Project& project);

    /** Clicking the strip (outside the fader) shows the master's effects. */
    std::function<void()> onSelected;
    void mouseDown (const juce::MouseEvent&) override { if (onSelected) onSelected(); }

    void refresh();

    static constexpr int preferredWidth = 112;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Project& project;

    juce::Label nameLabel;
    juce::Slider fader;
    juce::Label gainLabel;
    LevelMeter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterStrip)
};

} // namespace daw
