#pragma once

#include "ArrangementView.h"
#include "DawLookAndFeel.h"
#include "DetailView.h"
#include "PanelDivider.h"
#include "ExportDialog.h"
#include "Engine.h"
#include "Project.h"
#include "SamplePool.h"
#include "SongPositionBar.h"
#include "TrackStrip.h"
#include "TransportBar.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace daw
{

/** Root component: owns the audio device, engine, project model and the two main views
    (arrangement and mixer, switched with Tab). */
class MainComponent : public juce::Component,
                      private juce::AudioIODeviceCallback,
                      private juce::ValueTree::Listener,
                      private juce::MidiInputCallback,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;

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
    void addMidiTrack();

    // FL-style pattern mode: loop only the MIDI clip open in the piano roll (or last selected).
    void setPatternMode (bool shouldBeOn);
    juce::ValueTree getPatternClip() const;
    void updatePlaybackRange();   // loop/isolation and the position bar's range, every tick
    void play();
    void stop();

    /** The track that receives live notes (computer keyboard, MIDI input, piano roll preview):
        the selected track, if it is a MIDI track. */
    void selectTrack (juce::ValueTree track);
    void playLiveNote (int pitch, float velocity, bool isNoteOn);
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override; // MIDI thread
    void enableMidiInputs();
    bool handleComputerKeyboard (const juce::KeyPress&);
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
    juce::TooltipWindow tooltipWindow { this, 700 };

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
    DetailView detail { project };
    PanelDivider detailDivider;

    // Layout remembered between sessions (app settings, not the project).
    juce::ApplicationProperties appSettings;
    int detailHeight = DetailView::preferredHeight;
    int detailHeightAtDragStart = 0;
    bool detailCollapsed = false;
    void saveLayout();
    SongPositionBar positionBar { project };
    bool patternMode = false;
    juce::ValueTree lastSelectedMidiClip;

    // Live input routing. liveTrackIndex is the engine slot of the selected MIDI track (-1 = none),
    // read by the MIDI input thread.
    juce::ValueTree selectedTrack;
    std::atomic<int> liveTrackIndex { -1 };

    // Computer MIDI keyboard (Ableton layout: A W S E D F T G Y H U J K = C..C, Z/X octave, C/V velocity).
    int keyboardOctave = 3;            // C3 = 60
    float keyboardVelocity = 0.8f;
    std::map<int, int> heldKeys;       // key code -> sounding pitch

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
