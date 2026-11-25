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
    DAW_DECLARE_ID (NOTE)
    DAW_DECLARE_ID (DEVICE)

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
    DAW_DECLARE_ID (kind)        // TRACK, CLIP: "audio" or "midi" (missing = audio)
    DAW_DECLARE_ID (offsetBeats) // CLIP (midi): content beat at the clip's left edge
    DAW_DECLARE_ID (loopLength)  // CLIP (midi): loop length in beats (content wraps when 'loop')
    DAW_DECLARE_ID (pitch)       // NOTE: MIDI note number
    DAW_DECLARE_ID (velocity)    // NOTE: 0..1
    DAW_DECLARE_ID (type)        // DEVICE: "synth"
    #undef DAW_DECLARE_ID
}

/** The arrangement document: tracks and their audio clips, stored in a juce::ValueTree.

        PROJECT { version, bpm, beatsPerBar, masterGain }
          TRACK { kind, name, colour, engineIndex, gain, pan, mute, solo }
            CLIP { kind="audio", name, file, start, length, offset (seconds), loop }
            CLIP { kind="midi", name, start, length, offsetBeats, loop, loopLength }
              NOTE { pitch, start, length, velocity }      (start: content beats)
            DEVICE { type="synth", <Synth param id>=value ... }   (MIDI tracks only)

    A track's children are its CLIPs plus, on MIDI tracks, one DEVICE; use isClip() when
    iterating. Audio clips live on audio tracks and MIDI clips on MIDI tracks only.

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

    /** Replaces the document with an empty one holding fresh tracks (MIDI tracks first, like
        Ableton's default set), default tempo and mixer, no file and no undo history. */
    void newDocument (int numAudioTracks, int numMidiTracks = 0);

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
    enum class TrackKind { audio, midi };

    /** Returns the new TRACK, or an invalid tree if the engine's track pool is full.
        A MIDI track gets a synth DEVICE with default parameters. */
    juce::ValueTree addTrack (const juce::String& name = {}, TrackKind kind = TrackKind::audio);
    static bool isMidiTrack (const juce::ValueTree& track) { return track[ids::kind].toString() == "midi"; }
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

    // ---- instrument (MIDI tracks; undoable) -----------------------------------
    static juce::ValueTree getDevice (const juce::ValueTree& track) { return track.getChildWithName (ids::DEVICE); }

    /** Sets a Synth parameter (index into Synth::getParamInfos()); clamped like Synth does. */
    void setDeviceParam (juce::ValueTree track, int paramIndex, float value);
    static float getDeviceParam (const juce::ValueTree& track, int paramIndex);

    // ---- clips (all undoable) -------------------------------------------------
    static bool isClip (const juce::ValueTree& tree)     { return tree.hasType (ids::CLIP); }
    static bool isMidiClip (const juce::ValueTree& clip) { return clip.hasType (ids::CLIP) && clip[ids::kind].toString() == "midi"; }

    /** Creates an empty, looping MIDI clip on a MIDI track (loop length = clip length).
        Overwrites whatever it lands on. Returns the CLIP, or invalid on an audio track. */
    juce::ValueTree addMidiClip (juce::ValueTree track, double startBeat, double lengthBeats);

    /** Places the whole sample at startBeat. The file must already be in the SamplePool
        (use SamplePool::loadAsync first). Returns the CLIP, or invalid if not loaded. */
    juce::ValueTree addClip (juce::ValueTree track, const juce::File& file, double startBeat);

    /** Moves a clip (possibly to another track of the same kind); overwrites whatever it lands on. */
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
    static double getClipOffsetBeats (const juce::ValueTree& clip) { return clip[ids::offsetBeats]; }
    static double getClipLoopLength (const juce::ValueTree& clip)  { return clip[ids::loopLength]; }

    /** Sets a MIDI clip's loop length (content beats, >= minClipLengthBeats); the offset is
        re-wrapped into the new loop. */
    void setClipLoopLength (juce::ValueTree clip, double newLoopLengthBeats);

    // ---- notes (MIDI clips; all undoable) -------------------------------------
    static constexpr double minNoteLengthBeats = 1.0 / 64.0;

    /** Adds a note in content beats. Returns the NOTE, or invalid if 'clip' isn't a MIDI clip. */
    juce::ValueTree addNote (juce::ValueTree clip, int pitch, double startBeat, double lengthBeats, float velocity = 0.8f);
    void moveNote (juce::ValueTree note, double newStartBeat, int newPitch);
    void setNoteLength (juce::ValueTree note, double newLengthBeats);
    void setNoteVelocity (juce::ValueTree note, float newVelocity);
    void deleteNote (juce::ValueTree note);

    static double getNoteStart (const juce::ValueTree& note)   { return note[ids::start]; }
    static double getNoteLength (const juce::ValueTree& note)  { return note[ids::length]; }
    static int getNotePitch (const juce::ValueTree& note)      { return note[ids::pitch]; }
    static float getNoteVelocity (const juce::ValueTree& note) { return note[ids::velocity]; }

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
    double normaliseOffsetBeats (const juce::ValueTree& clip, double offsetBeats) const;
    /** Moves the clip's content by deltaBeats (audio: converted to seconds at the tempo). */
    void shiftContent (juce::ValueTree clip, double deltaBeats);
    void syncTrack (const juce::ValueTree& track);
    void syncTrackMixer (const juce::ValueTree& track);
    void syncDevice (const juce::ValueTree& track);
    static juce::ValueTree makeDevice();
    void syncProjectProperties();
    /** Swaps in a new set of tracks (and PROJECT properties) without undo; resets the engine. */
    juce::String replaceContents (const juce::ValueTree& newState);
    juce::ValueTree makeTrack (int number, TrackKind kind = TrackKind::audio);

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
