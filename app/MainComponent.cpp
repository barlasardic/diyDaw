#include "MainComponent.h"

#include <algorithm>

namespace daw
{

MainComponent::MainComponent()
{
    setLookAndFeel (&lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // Clips whose sample was still decoding were skipped in the engine snapshot.
    samplePool.onSampleAdded = [this] (const juce::String&)
    {
        project.syncAllTracks();
        arrangement.repaint();
    };
    samplePool.onLoadFailed = [this] (const juce::String&) { arrangement.repaint(); };

    transportBar.onFileMenu = [this] { showFileMenu(); };
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

    projectState.addListener (this);
    project.newDocument (4);
    syncStrips();

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
    exportJob.reset(); // stops the export thread before the project goes away
    if (exportWindow != nullptr)
        delete exportWindow.getComponent();
    projectState.removeListener (this);
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
    updateTitle();

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
    project.addTrack(); // syncStrips() runs from the ValueTree listener
}

void MainComponent::syncStrips()
{
    // Keep strips whose track still exists, in model order; make strips for new tracks.
    std::vector<std::unique_ptr<TrackStrip>> updated;

    for (const auto& track : project.getState())
    {
        auto existing = std::find_if (strips.begin(), strips.end(),
                                      [&track] (const auto& s) { return s != nullptr && s->getTrack() == track; });

        if (existing != strips.end())
            updated.push_back (std::move (*existing));
        else
        {
            auto strip = std::make_unique<TrackStrip> (project, track);
            stripHolder.addAndMakeVisible (*strip);
            updated.push_back (std::move (strip));
        }
    }

    strips = std::move (updated);
    layoutStrips();
}

void MainComponent::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == projectState)
        syncStrips();
}

void MainComponent::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == projectState)
        syncStrips();
}

void MainComponent::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == projectState)
        syncStrips();
}

// ---- files --------------------------------------------------------------------

namespace
{
    const juce::String fileWildcard = juce::String ("*") + Project::fileExtension;

    juce::File defaultProjectFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("diyDaw");
    }
}

juce::String MainComponent::getDocumentTitle() const
{
    const auto file = project.getFile();
    const auto name = file == juce::File() ? juce::String ("Untitled") : file.getFileNameWithoutExtension();
    return name + (project.hasUnsavedChanges() ? "*" : "") + " - diyDaw";
}

void MainComponent::updateTitle()
{
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        if (const auto title = getDocumentTitle(); window->getName() != title)
            window->setName (title);
}

void MainComponent::showError (const juce::String& title, const juce::String& message)
{
    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::WarningIcon)
                                      .withTitle (title)
                                      .withMessage (message)
                                      .withButton ("OK", 0)
                                      .withAssociatedComponent (this),
                                  nullptr);
}

void MainComponent::confirmDiscardingChanges (std::function<void()> action)
{
    if (! project.hasUnsavedChanges())
    {
        action();
        return;
    }

    const auto name = project.getFile() == juce::File() ? juce::String ("Untitled")
                                                        : project.getFile().getFileNameWithoutExtension();

    juce::Component::SafePointer<MainComponent> safeThis (this);

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("Unsaved changes")
                                      .withMessage ("Save changes to \"" + name + "\" before closing it?")
                                      .withButton ("Save", 1)
                                      .withButton ("Don't Save", 2)
                                      .withButton ("Cancel", 0)
                                      .withAssociatedComponent (this),
                                  [safeThis, action = std::move (action)] (int result)
                                  {
                                      if (safeThis == nullptr || result == 0)
                                          return;

                                      if (result == 2)
                                          action();
                                      else
                                          safeThis->saveProject ([action] (bool saved) { if (saved) action(); });
                                  });
}

void MainComponent::newProject()
{
    confirmDiscardingChanges ([this]
    {
        engine.getTransport().stop();
        engine.getTransport().setPositionBeats (0.0);
        project.newDocument (4);
    });
}

void MainComponent::openProject()
{
    confirmDiscardingChanges ([this]
    {
        const auto start = project.getFile() != juce::File() ? project.getFile().getParentDirectory()
                                                             : defaultProjectFolder();

        fileChooser = std::make_unique<juce::FileChooser> ("Open Project", start, fileWildcard);
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& chooser)
                                  {
                                      if (const auto file = chooser.getResult(); file != juce::File())
                                          openProject (file);
                                  });
    });
}

void MainComponent::openProject (const juce::File& file)
{
    engine.getTransport().stop();
    engine.getTransport().setPositionBeats (0.0);

    if (const auto result = project.loadFrom (file); result.failed())
        showError ("Couldn't open project", result.getErrorMessage());
}

void MainComponent::saveProject (std::function<void (bool)> then)
{
    if (project.getFile() == juce::File())
    {
        saveProjectAs (std::move (then));
        return;
    }

    const auto result = project.saveTo (project.getFile());
    if (result.failed())
        showError ("Couldn't save project", result.getErrorMessage());

    if (then)
        then (result.wasOk());
}

void MainComponent::saveProjectAs (std::function<void (bool)> then)
{
    auto start = project.getFile();
    if (start == juce::File())
    {
        defaultProjectFolder().createDirectory();
        start = defaultProjectFolder().getChildFile (juce::String ("Untitled") + Project::fileExtension);
    }

    fileChooser = std::make_unique<juce::FileChooser> ("Save Project", start, fileWildcard);
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, then = std::move (then)] (const juce::FileChooser& chooser)
                              {
                                  auto file = chooser.getResult();
                                  if (file == juce::File())
                                  {
                                      if (then)
                                          then (false);
                                      return;
                                  }

                                  file = file.withFileExtension (Project::fileExtension);
                                  const auto result = project.saveTo (file);
                                  if (result.failed())
                                      showError ("Couldn't save project", result.getErrorMessage());

                                  if (then)
                                      then (result.wasOk());
                              });
}

void MainComponent::requestQuit (std::function<void()> quit)
{
    confirmDiscardingChanges (std::move (quit));
}

void MainComponent::showFileMenu()
{
    const auto cmd = juce::String ("Ctrl+");

    juce::PopupMenu menu;
    menu.addItem ("New Project\t" + cmd + "N", [this] { newProject(); });
    menu.addItem ("Open...\t" + cmd + "O", [this] { openProject(); });
    menu.addSeparator();
    menu.addItem ("Save\t" + cmd + "S", [this] { saveProject(); });
    menu.addItem ("Save As...\t" + cmd + "Shift+S", [this] { saveProjectAs(); });
    menu.addSeparator();
    menu.addItem ("Export Audio...\t" + cmd + "Shift+R", exportJob == nullptr, false, [this] { showExportDialog(); });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (transportBar.getFileButton()));
}

void MainComponent::showExportDialog()
{
    if (exportJob != nullptr)
        return; // one export at a time

    if (exportWindow != nullptr)
    {
        exportWindow->toFront (true);
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    auto dialog = std::make_unique<ExportDialog> (project, device != nullptr ? device->getCurrentSampleRate() : 44100.0);

    dialog->onCancel = [this] { if (exportWindow != nullptr) exportWindow->exitModalState (0); };
    dialog->onExport = [this] (const ExportSettings& settings, const juce::File& file)
    {
        if (exportWindow != nullptr)
            exportWindow->exitModalState (0);

        // The renderer copies the project now, so editing or playing during the export is safe.
        exportJob = std::make_unique<ExportJob> (project, settings, file);
        exportJob->onFinished = [this, file] (juce::Result result)
        {
            // Delete the job after its window callback has returned.
            juce::MessageManager::callAsync ([safeThis = juce::Component::SafePointer<MainComponent> (this)]
            {
                if (safeThis != nullptr)
                    safeThis->exportJob.reset();
            });

            if (result.failed())
                showError ("Export failed", result.getErrorMessage());
            else
                juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                                  .withIconType (juce::MessageBoxIconType::InfoIcon)
                                                  .withTitle ("Export finished")
                                                  .withMessage ("Saved " + file.getFullPathName())
                                                  .withButton ("OK", 0)
                                                  .withAssociatedComponent (this),
                                              nullptr);
        };
        exportJob->launchThread();
    };

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (dialog.release());
    options.dialogTitle = "Export Audio";
    options.dialogBackgroundColour = colours::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.componentToCentreAround = this;
    exportWindow = options.launchAsync();
}

juce::Result MainComponent::openProjectSynchronously (const juce::File& file)
{
    const auto result = project.loadFrom (file);
    if (result.failed())
        return result;

    for (const auto& track : project.getState())
        for (const auto& clip : track)
            if (const auto f = Project::getClipFile (clip); samplePool.get (f) == nullptr)
                if (auto sample = loadSample (f, samplePool.getFormats()))
                    samplePool.insert (SamplePool::keyFor (f), std::move (sample));

    transportBar.refresh();
    return result;
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

    if (mods.isCommandDown() && key.getKeyCode() == 'S')
    {
        mods.isShiftDown() ? saveProjectAs() : saveProject();
        return true;
    }

    if (mods.isCommandDown() && mods.isShiftDown() && key.getKeyCode() == 'R')
    {
        showExportDialog();
        return true;
    }

    if (mods.isCommandDown() && key.getKeyCode() == 'O')
    {
        openProject();
        return true;
    }

    if (mods.isCommandDown() && key.getKeyCode() == 'N')
    {
        newProject();
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
