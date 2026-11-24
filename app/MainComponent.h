#pragma once

#include "ArrangementView.h"
#include "DawLookAndFeel.h"
#include "Engine.h"
#include "Project.h"
#include "SamplePool.h"
#include "TrackStrip.h"
#include "TransportBar.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <vector>

namespace daw
{

/** Root component: owns the audio device, engine, project model and the two main views
    (arrangement and mixer, switched with Tab). */
class MainComponent : public juce::Component,
                      private juce::AudioIODeviceCallback,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Decodes files on this thread and places file i on track i at beat i * beatSpacing.
        For the --snapshot tooling; the UI path is the asynchronous drop in ArrangementView. */
    void loadFilesSynchronously (const juce::Array<juce::File>& files, double beatSpacing);
    Project& getProject() noexcept { return project; }

private:
    // AudioIODeviceCallback (audio thread)
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

    void timerCallback() override;

    void addTrack();
    void showAudioSettings();
    void setShowingMixer (bool shouldShowMixer);
    void layoutStrips();

    DawLookAndFeel lookAndFeel;

    Engine engine;
    SamplePool samplePool;
    Project project { engine, samplePool };
    juce::AudioDeviceManager deviceManager;

    TransportBar transportBar { engine };
    ArrangementView arrangement { project };

    // Mixer view
    juce::Component mixer;
    juce::Component stripHolder;
    juce::Viewport stripViewport;
    std::vector<std::unique_ptr<TrackStrip>> strips;
    MasterStrip masterStrip { engine };
    bool showingMixer = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace daw
