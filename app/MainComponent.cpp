#include "MainComponent.h"

namespace daw
{

MainComponent::MainComponent()
{
    setLookAndFeel (&lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    formats.registerBasicFormats();

    transportBar.onAddTrack = [this] { addTrack(); };
    transportBar.onAudioSettings = [this] { showAudioSettings(); };
    addAndMakeVisible (transportBar);

    stripViewport.setViewedComponent (&stripHolder, false);
    stripViewport.setScrollBarsShown (false, true);
    addAndMakeVisible (stripViewport);
    addAndMakeVisible (masterStrip);

    for (int i = 0; i < 4; ++i)
        addTrack();

    // Ask for stereo out, no inputs. A failure (e.g. no device) leaves the UI usable
    // and the user can pick a device from the Audio... dialog.
    const auto error = deviceManager.initialiseWithDefaultDevices (0, 2);
    if (error.isNotEmpty())
        DBG ("Audio device error: " << error);
    else if (auto* device = deviceManager.getCurrentAudioDevice())
        DBG ("Audio device: " << device->getTypeName() << " / " << device->getName() << " @ "
             << device->getCurrentSampleRate() << " Hz, " << device->getCurrentBufferSizeSamples() << " samples");

    deviceManager.addAudioCallback (this);

    setWantsKeyboardFocus (true);
    setSize (1100, 640);
    startTimerHz (30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

// ---- audio thread ---------------------------------------------------------

void MainComponent::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    engine.prepare (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void MainComponent::audioDeviceStopped() {}

void MainComponent::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                      float* const* outputChannelData, int numOutputChannels,
                                                      int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Wraps the device's channel pointers; no allocation for <= 32 channels.
    juce::AudioBuffer<float> out (outputChannelData, numOutputChannels, numSamples);
    engine.render (out, numSamples);
}

// ---- message thread -------------------------------------------------------

void MainComponent::timerCallback()
{
    engine.collectGarbage();
    transportBar.refresh();

    for (auto& s : strips)
        s->refresh();

    masterStrip.refresh();
}

void MainComponent::addTrack()
{
    const auto index = engine.addTrack();
    if (index < 0)
        return;

    auto strip = std::make_unique<TrackStrip> (engine.getTrack (index), formats);
    stripHolder.addAndMakeVisible (*strip);
    strips.push_back (std::move (strip));
    layoutStrips();
}

void MainComponent::showAudioSettings()
{
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent> (deviceManager, 0, 0, 2, 2,
                                                                         false, false, true, false);
    selector->setSize (520, 420);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (selector.release());
    options.dialogTitle = "Audio Settings";
    options.dialogBackgroundColour = colours::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        engine.getTransport().togglePlay();
        return true;
    }

    return false;
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}

void MainComponent::layoutStrips()
{
    const auto h = stripViewport.getMaximumVisibleHeight();
    stripHolder.setSize ((int) strips.size() * (TrackStrip::preferredWidth + 2), juce::jmax (1, h));

    int x = 0;
    for (auto& s : strips)
    {
        s->setBounds (x, 0, TrackStrip::preferredWidth, stripHolder.getHeight());
        x += TrackStrip::preferredWidth + 2;
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    transportBar.setBounds (area.removeFromTop (48));

    area.reduce (6, 6);
    masterStrip.setBounds (area.removeFromRight (MasterStrip::preferredWidth));
    area.removeFromRight (8);
    stripViewport.setBounds (area);
    layoutStrips();
}

} // namespace daw
