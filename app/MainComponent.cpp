#include "MainComponent.h"

namespace daw
{

MainComponent::MainComponent()
{
    setLookAndFeel (&lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // Clips whose sample was still decoding were skipped in the engine snapshot.
    samplePool.onSampleAdded = [this] (const juce::String&) { project.syncAllTracks(); };

    transportBar.onAddTrack = [this] { addTrack(); };
    transportBar.onAudioSettings = [this] { showAudioSettings(); };
    transportBar.onToggleView = [this] { setShowingMixer (! showingMixer); };
    addAndMakeVisible (transportBar);

    arrangement.onAddTrackRequested = [this] { addTrack(); };
    addAndMakeVisible (arrangement);

    stripViewport.setViewedComponent (&stripHolder, false);
    stripViewport.setScrollBarsShown (false, true);
    mixer.addAndMakeVisible (stripViewport);
    mixer.addAndMakeVisible (masterStrip);
    addChildComponent (mixer);

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

    setShowingMixer (false);
    setWantsKeyboardFocus (true);
    setSize (1280, 760);
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

    if (showingMixer)
    {
        for (auto& s : strips)
            s->refresh();

        masterStrip.refresh();
    }
    else
    {
        arrangement.refresh();
    }
}

void MainComponent::addTrack()
{
    const auto track = project.addTrack();
    if (! track.isValid())
        return;

    auto strip = std::make_unique<TrackStrip> (project.getEngineTrack (track));
    stripHolder.addAndMakeVisible (*strip);
    strips.push_back (std::move (strip));
    layoutStrips();
}

void MainComponent::loadFilesSynchronously (const juce::Array<juce::File>& files, double beatSpacing)
{
    for (int i = 0; i < files.size(); ++i)
    {
        while (project.getNumTracks() <= i)
            addTrack();

        if (auto sample = loadSample (files[i], samplePool.getFormats()))
        {
            samplePool.insert (SamplePool::keyFor (files[i]), std::move (sample));
            project.addClip (project.getTrack (i), files[i], i * beatSpacing);
        }
    }
}

void MainComponent::setShowingMixer (bool shouldShowMixer)
{
    showingMixer = shouldShowMixer;
    mixer.setVisible (showingMixer);
    arrangement.setVisible (! showingMixer);
    transportBar.setViewButtonText (showingMixer ? "Arrangement" : "Mixer");
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
    const auto mods = key.getModifiers();

    if (key == juce::KeyPress::spaceKey)
    {
        engine.getTransport().togglePlay();
        return true;
    }

    if (key == juce::KeyPress::tabKey)
    {
        setShowingMixer (! showingMixer);
        return true;
    }

    if (mods.isCommandDown() && key.getKeyCode() == 'Z')
    {
        mods.isShiftDown() ? project.redo() : project.undo();
        return true;
    }

    if (mods.isCommandDown() && key.getKeyCode() == 'Y')
    {
        project.redo();
        return true;
    }

    if (mods.isCommandDown() && key.getKeyCode() == 'T')
    {
        addTrack();
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

    arrangement.setBounds (area);
    mixer.setBounds (area);

    auto mixerArea = mixer.getLocalBounds().reduced (6, 6);
    masterStrip.setBounds (mixerArea.removeFromRight (MasterStrip::preferredWidth));
    mixerArea.removeFromRight (8);
    stripViewport.setBounds (mixerArea);
    layoutStrips();
}

} // namespace daw
