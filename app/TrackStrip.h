#pragma once

#include "AudioTrack.h"
#include "Engine.h"
#include "LevelMeter.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace daw
{

/** One vertical mixer channel strip for an AudioTrack (Ableton session-mixer style).

    Top to bottom: track name header, sample slot (click to load / drop a file;
    shows the sample name), loop toggle, pan knob, mute ("track activator") + solo
    buttons, then fader with a level meter beside it and a dB readout.

    Message thread only.
*/
class TrackStrip : public juce::Component,
                   public juce::FileDragAndDropTarget
{
public:
    TrackStrip (AudioTrack& track, juce::AudioFormatManager& formats);
    ~TrackStrip() override;

    /** Called ~30 Hz by the owner: pulls meter levels and resyncs controls. */
    void refresh();

    static constexpr int preferredWidth = 112;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;

private:
    void loadFile (const juce::File& file);
    void openFileChooser();

    AudioTrack& track;
    juce::AudioFormatManager& formats;

    juce::Label nameLabel;
    juce::TextButton sampleButton;
    juce::TextButton loopButton { "Loop" };
    juce::Slider panKnob;
    juce::TextButton muteButton;   // lit = track active (Ableton "track activator")
    juce::TextButton soloButton { "S" };
    juce::Slider fader;
    juce::Label gainLabel;
    LevelMeter meter;

    std::unique_ptr<juce::FileChooser> chooser;
    bool dragHover = false;

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
