#pragma once

#include "ArrangementView.h"
#include "DawLookAndFeel.h"
#include "ExportDialog.h"
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
                      private juce::ValueTree::Listener,
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

    /** Opens a project and decodes its samples on this thread (--snapshot tooling). */
    juce::Result openProjectSynchronously (const juce::File& file);

    Project& getProject() noexcept { return project; }

    // ---- file actions (asynchronous: they may ask about unsaved changes / show choosers)
    void newProject();
    void openProject();                     // shows a file chooser
    void openProject (const juce::File& file);
    void saveProject (std::function<void (bool saved)> then = {});
    void saveProjectAs (std::function<void (bool saved)> then = {});
    void showExportDialog();

    /** Asks to save unsaved changes, then calls quit if the user didn't cancel. */
    void requestQuit (std::function<void()> quit);

    /** "Name* - diyDaw", for the window title. */
    juce::String getDocumentTitle() const;

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
    void showFileMenu();
    void setShowingMixer (bool shouldShowMixer);
    void syncStrips();
    void layoutStrips();
    void showError (const juce::String& title, const juce::String& message);

    /** Runs 'action' after the user saved or discarded unsaved changes (not if they cancel). */
    void confirmDiscardingChanges (std::function<void()> action);
    void updateTitle();

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;

    DawLookAndFeel lookAndFeel;

    Engine engine;
    SamplePool samplePool;
    Project project { engine, samplePool };
    juce::AudioDeviceManager deviceManager;

    juce::ValueTree projectState { project.getState() };
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::Component::SafePointer<juce::DialogWindow> exportWindow;
    std::unique_ptr<ExportJob> exportJob;

    TransportBar transportBar { project };
    ArrangementView arrangement { project };

    // Mixer view
    juce::Component mixer;
    juce::Component stripHolder;
    juce::Viewport stripViewport;
    std::vector<std::unique_ptr<TrackStrip>> strips;
    MasterStrip masterStrip { project };
    bool showingMixer = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace daw
