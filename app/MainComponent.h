#pragma once

#include "DawLookAndFeel.h"
#include "Engine.h"
#include "TrackStrip.h"
#include "TransportBar.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <vector>

namespace daw
{

/** Root component: owns the audio device, the engine and the mixer UI. */
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
    void layoutStrips();

    DawLookAndFeel lookAndFeel;

    Engine engine;
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formats;

    TransportBar transportBar { engine };
    juce::Component stripHolder;
    juce::Viewport stripViewport;
    std::vector<std::unique_ptr<TrackStrip>> strips;
    MasterStrip masterStrip { engine };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace daw
