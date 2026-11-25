#pragma once

#include "Engine.h"
#include "SamplePool.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

namespace daw
{

namespace ids
{
    #define DAW_DECLARE_ID(name) inline const juce::Identifier name { #name };
    DAW_DECLARE_ID (PROJECT)
    DAW_DECLARE_ID (TRACK)
    DAW_DECLARE_ID (CLIP)

    DAW_DECLARE_ID (name)        // TRACK, CLIP: display name
    DAW_DECLARE_ID (colour)      // TRACK: ARGB hex string
    DAW_DECLARE_ID (engineIndex) // TRACK: slot in Engine's track pool
    DAW_DECLARE_ID (file)        // CLIP: absolute path, also the SamplePool key
    DAW_DECLARE_ID (start)       // CLIP: timeline position, beats
    DAW_DECLARE_ID (length)      // CLIP: beats
    DAW_DECLARE_ID (offset)      // CLIP: seconds into the sample where the clip begins
    DAW_DECLARE_ID (loop)        // CLIP: bool
    DAW_DECLARE_ID (relativeFile)// CLIP, in saved files only: path relative to the project file
    DAW_DECLARE_ID (gain)        // TRACK: fader, dB
    DAW_DECLARE_ID (pan)         // TRACK: -1..1
    DAW_DECLARE_ID (mute)        // TRACK: bool
    DAW_DECLARE_ID (solo)        // TRACK: bool
    DAW_DECLARE_ID (version)     // PROJECT: file format version
    DAW_DECLARE_ID (bpm)         // PROJECT: tempo
    DAW_DECLARE_ID (beatsPerBar) // PROJECT: time signature numerator
    DAW_DECLARE_ID (masterGain)  // PROJECT: master fader, dB
    #undef DAW_DECLARE_ID
}

/** The arrangement document: tracks and their audio clips, stored in a juce::ValueTree.

        PROJECT { version, bpm, beatsPerBar, masterGain }
          TRACK { name, colour, engineIndex, gain, pan, mute, solo }
            CLIP { name, file, start, length, offset, loop }

    The tree is the single source of truth: every change (including undo/redo and loading a
    file) is pushed to the Engine immediately: clip edits as a fresh ClipList snapshot,
    mixer and tempo values as atomics. So what you see is what plays.

    Editing follows Ableton's rule that clips on a track never overlap: placing, moving,
    resizing or duplicating a clip trims, splits or deletes whatever it lands on.

    Undo grouping: call beginTransaction() once at the start of each user gesture; every
    edit after that (until the next beginTransaction) is undone as one step.

    Continuous controls (faders, knobs) should call beginTransaction() on drag start; repeated
    sets of the same property inside one transaction coalesce into a single undo step.

    Files: saveTo() writes the tree as XML (".diydaw"); loadFrom() replaces the whole
    document, clears the undo history and starts decoding the samples in the background.

    Message thread only.
*/
class Project : private juce::ValueTree::Listener
{
public:
    static constexpr double minClipLengthBeats = 1.0 / 16.0;

    Project (Engine& engine, SamplePool& pool);
    ~Project() override;

    juce::ValueTree getState() const noexcept { return state; }
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    Engine& getEngine() noexcept { return engine; }
    SamplePool& getSamplePool() noexcept { return pool; }

    void beginTransaction (const juce::String& actionName = {});
    bool undo();
    bool redo();

    // ---- document / files -----------------------------------------------------
    static constexpr int fileFormatVersion = 1;
    static constexpr const char* fileExtension = ".diydaw";

    /** Replaces the document with an empty one holding 'numTracks' fresh tracks, default
        tempo and mixer, no file and no undo history. */
    void newDocument (int numTracks);

    /** Writes the project. On success it becomes the document's file and is marked saved. */
    juce::Result saveTo (const juce::File& file);

    /** Replaces the document with the file's contents. On failure nothing changes.
        Samples that aren't in the pool yet are loaded in the background; a clip whose file
        is missing at its saved absolute path is looked up relative to the project file.
        Tracks beyond Engine::maxTracks are dropped (the Result says so, but is still ok). */
    juce::Result loadFrom (const juce::File& file);

    /** Replaces this document with a copy of another project's current state (no file,
        no undo history). Used for offline rendering. */
    void copyFrom (const Project& other);

    /** The file the document was last saved to / loaded from (may not exist). */
    juce::File getFile() const { return documentFile; }
    bool hasUnsavedChanges() const noexcept { return unsavedChanges; }

    // ---- tempo / master (undoable) ------------------------------------------
    void setBpm (double bpm);
    double getBpm() const { return state[ids::bpm]; }
    void setMasterGain (float db);
    float getMasterGain() const { return state[ids::masterGain]; }

    // ---- tracks (adding a track is not undoable yet) ------------------------
    /** Returns the new TRACK, or an invalid tree if the engine's track pool is full. */
    juce::ValueTree addTrack (const juce::String& name = {});
    int getNumTracks() const { return state.getNumChildren(); }
    juce::ValueTree getTrack (int index) const { return state.getChild (index); }
    int indexOfTrack (const juce::ValueTree& track) const { return state.indexOf (track); }
    AudioTrack& getEngineTrack (const juce::ValueTree& track);
    void setTrackName (juce::ValueTree track, const juce::String& newName);
    static juce::Colour getTrackColour (const juce::ValueTree& track);

    // Mixer (undoable; values are clamped like AudioTrack's).
    void setTrackGain (juce::ValueTree track, float db);
    void setTrackPan (juce::ValueTree track, float pan);
    void setTrackMute (juce::ValueTree track, bool shouldMute);
    void setTrackSolo (juce::ValueTree track, bool shouldSolo);
    static float getTrackGain (const juce::ValueTree& track) { return track[ids::gain]; }
    static float getTrackPan (const juce::ValueTree& track)  { return track[ids::pan]; }
    static bool isTrackMuted (const juce::ValueTree& track)  { return track[ids::mute]; }
    static bool isTrackSoloed (const juce::ValueTree& track) { return track[ids::solo]; }

    // ---- clips (all undoable) -------------------------------------------------
    /** Places the whole sample at startBeat. The file must already be in the SamplePool
        (use SamplePool::loadAsync first). Returns the CLIP, or invalid if not loaded. */
    juce::ValueTree addClip (juce::ValueTree track, const juce::File& file, double startBeat);

    /** Moves a clip (possibly to another track); overwrites whatever it lands on. */
    void moveClip (juce::ValueTree clip, juce::ValueTree toTrack, double newStartBeat);

    /** Moves the left edge, keeping the right edge and the audio under it in place. */
    void trimClipStart (juce::ValueTree clip, double newStartBeat);

    /** Moves the right edge. Non-looping clips can't extend past the end of their sample. */
    void trimClipEnd (juce::ValueTree clip, double newEndBeat);

    void setClipLooping (juce::ValueTree clip, bool shouldLoop);
    void deleteClip (juce::ValueTree clip);

    /** Copies the clip to start right where it ends. Returns the copy. */
    juce::ValueTree duplicateClip (juce::ValueTree clip);

    /** Cuts a clip in two at 'atBeat' (must be strictly inside it). Returns the right half. */
    juce::ValueTree splitClip (juce::ValueTree clip, double atBeat);

    // ---- clip accessors -----------------------------------------------------
    static double getClipStart (const juce::ValueTree& clip)  { return clip[ids::start]; }
    static double getClipLength (const juce::ValueTree& clip) { return clip[ids::length]; }
    static double getClipEnd (const juce::ValueTree& clip)    { return getClipStart (clip) + getClipLength (clip); }
    static double getClipOffset (const juce::ValueTree& clip) { return clip[ids::offset]; }
    static bool isClipLooping (const juce::ValueTree& clip)   { return clip[ids::loop]; }
    static juce::File getClipFile (const juce::ValueTree& clip) { return juce::File (clip[ids::file].toString()); }
    static juce::String getClipName (const juce::ValueTree& clip) { return clip[ids::name].toString(); }

    /** The decoded sample behind a clip, or nullptr if it isn't loaded. */
    SamplePool::Ptr getClipSample (const juce::ValueTree& clip) const;

    /** Native-speed conversion at the current tempo. */
    double secondsToBeats (double seconds) const;
    double beatsToSeconds (double beats) const;

    /** Rebuilds every track's engine snapshot (e.g. after samples finish loading). */
    void syncAllTracks();

private:
    void clearRegion (juce::ValueTree track, double start, double end, const juce::ValueTree& except);
    juce::ValueTree splitInternal (juce::ValueTree clip, double atBeat);
    void setStartKeepingEnd (juce::ValueTree clip, double newStart);
    double normaliseOffset (const juce::ValueTree& clip, double offsetSeconds) const;
    void syncTrack (const juce::ValueTree& track);
    void syncTrackMixer (const juce::ValueTree& track);
    void syncProjectProperties();
    /** Swaps in a new set of tracks (and PROJECT properties) without undo; resets the engine. */
    juce::String replaceContents (const juce::ValueTree& newState);
    juce::ValueTree makeTrack (int number);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index) override;

    Engine& engine;
    SamplePool& pool;
    juce::ValueTree state { ids::PROJECT };
    juce::UndoManager undoManager;
    juce::File documentFile;
    bool unsavedChanges = false;

    JUCE_DECLARE_NON_COPYABLE (Project)
};

} // namespace daw
