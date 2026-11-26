#pragma once

#include "Engine.h"
#include "PluginHost.h"
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
    DAW_DECLARE_ID (MASTER)
    DAW_DECLARE_ID (CLIP)
    DAW_DECLARE_ID (NOTE)
    DAW_DECLARE_ID (DEVICE)
    DAW_DECLARE_ID (AUTOMATION)
    DAW_DECLARE_ID (POINT)

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
    DAW_DECLARE_ID (type)        // DEVICE: "synth", "drumrack" (instruments), "eq", "compressor", "reverb", "delay"
    DAW_DECLARE_ID (enabled)     // DEVICE (effects): bool, false = bypassed
    DAW_DECLARE_ID (height)      // TRACK: arrangement lane height in pixels (UI layout, not undoable)
    DAW_DECLARE_ID (description) // DEVICE (plugin): juce::PluginDescription as XML text
    DAW_DECLARE_ID (pluginName)  // DEVICE (plugin): display name
    DAW_DECLARE_ID (state)       // DEVICE (plugin): plugin state, base64 (written on save)
    DAW_DECLARE_ID (uid)         // DEVICE: id unique within the project (automation targets refer to it)
    DAW_DECLARE_ID (target)      // AUTOMATION: "gain", "pan" or "<device uid>:<param id>"
    DAW_DECLARE_ID (beat)        // POINT: timeline position
    DAW_DECLARE_ID (value)       // POINT: value in the parameter's units
    #undef DAW_DECLARE_ID
}

/** The arrangement document: tracks and their audio clips, stored in a juce::ValueTree.

        PROJECT { version, bpm, beatsPerBar, masterGain }
          TRACK { kind, name, colour, engineIndex, gain, pan, mute, solo, height }
            CLIP { kind="audio", name, file, start, length, offset (seconds), loop }
            CLIP { kind="midi", name, start, length, offsetBeats, loop, loopLength }
              NOTE { pitch, start, length, velocity }      (start: content beats)
            DEVICE { type="synth", <Synth param id>=value ... }   (MIDI tracks only, first)
            DEVICE { type="drumrack", pad1File..pad16File (SamplePool keys), <DrumRack param id>=value ... }
                                                         (instead of the synth: a drum track)
            DEVICE { type="eq"|"compressor"|"reverb"|"delay", uid, enabled, <param id>=value ... }
            DEVICE { type="plugin", uid, enabled, description, pluginName, state }    (VST3/LV2 effect)
            DEVICE { type="pluginInstrument", uid, description, pluginName, state } (instead of the synth)
            AUTOMATION { target }
              POINT { beat, value }

          MASTER                                       (always the last child of PROJECT)
            DEVICE { type="eq"|..., uid, enabled, ... }  (master insert effects)

    The PROJECT's children are its TRACKs plus one MASTER: use getNumTracks() / getTrack() /
    indexOfTrack(), which only count TRACKs. The MASTER works like a track for the effect API
    (getEffects, addEffect, ... take it in place of a track); it has no clips or automation.

    A track's children are its CLIPs, its DEVICEs (the device chain, in order: the instrument
    on MIDI tracks, then insert effects) and its AUTOMATION lanes; use isClip() / isEffect() /
    isAutomationLane() when iterating.
    Audio clips live on audio tracks and MIDI clips on MIDI tracks only.

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
    bool hasUnsavedChanges() const noexcept { return unsavedChanges || pluginsChanged.load(); }

    // ---- plugins (VST3 / LV2) --------------------------------------------------------
    /** Where plugin devices are created from. Without a host (or when a plugin can't be
        loaded) plugin devices stay in the document but are silent ("missing"). Set it before
        loading documents; it must outlive the Project. */
    void setPluginHost (PluginHost* host) noexcept { pluginHost = host; }
    PluginHost* getPluginHost() const noexcept { return pluginHost; }

    /** Adds a plugin effect (like addEffect). Returns the DEVICE (even if the plugin failed to
        load: check getPluginInstance / getPluginError). */
    juce::ValueTree addPluginEffect (juce::ValueTree trackOrMaster, const juce::PluginDescription& description, int index = -1);

    /** Replaces a MIDI track's instrument: "synth", "drumrack" (undoable). The old instrument's
        automation goes with it. */
    void setInstrument (juce::ValueTree track, const juce::String& instrumentType);
    /** Replaces a MIDI track's instrument with a plugin instrument (undoable). */
    void setPluginInstrument (juce::ValueTree track, const juce::PluginDescription& description);

    static bool isPluginDevice (const juce::ValueTree& device)
    {
        return getDeviceType (device) == "plugin" || getDeviceType (device) == "pluginInstrument";
    }
    /** "Synth", "Drum Rack", "EQ Three", ... or the plugin's name. */
    static juce::String getDeviceDisplayName (const juce::ValueTree& device);

    /** The loaded plugin behind a plugin DEVICE, or nullptr if it is missing. */
    juce::AudioPluginInstance* getPluginInstance (const juce::ValueTree& device) const;
    /** Why a plugin DEVICE is missing ("" if it loaded). */
    juce::String getPluginError (const juce::ValueTree& device) const;

    /** A copy of the document with every plugin's current state captured (what gets saved). */
    juce::ValueTree createSnapshot() const;

    // ---- tempo / master (undoable) ------------------------------------------
    void setBpm (double bpm);
    double getBpm() const { return state[ids::bpm]; }
    void setMasterGain (float db);
    float getMasterGain() const { return state[ids::masterGain]; }

    // ---- tracks (adding a track is not undoable yet) ------------------------
    /** drums = a MIDI track whose instrument is a drum rack (with the builtin kit). */
    enum class TrackKind { audio, midi, drums };

    /** Returns the new TRACK, or an invalid tree if the engine's track pool is full.
        A MIDI track gets a synth DEVICE with default parameters. */
    juce::ValueTree addTrack (const juce::String& name = {}, TrackKind kind = TrackKind::audio);
    static bool isMidiTrack (const juce::ValueTree& track) { return track[ids::kind].toString() == "midi"; }
    int getNumTracks() const;
    juce::ValueTree getTrack (int index) const;
    int indexOfTrack (const juce::ValueTree& track) const;

    /** The MASTER node (master insert effects; pass it where a track is expected for effects). */
    juce::ValueTree getMaster() const { return state.getChildWithName (ids::MASTER); }
    static bool isMaster (const juce::ValueTree& tree) { return tree.hasType (ids::MASTER); }
    AudioTrack& getEngineTrack (const juce::ValueTree& track);
    void setTrackName (juce::ValueTree track, const juce::String& newName);
    static juce::Colour getTrackColour (const juce::ValueTree& track);

    // Arrangement lane height (layout state saved with the project; not undoable, like Ableton).
    static constexpr int minTrackHeight = 40, defaultTrackHeight = 64, maxTrackHeight = 320;
    void setTrackHeight (juce::ValueTree track, int heightInPixels);
    static int getTrackHeight (const juce::ValueTree& track)
    {
        return juce::jlimit (minTrackHeight, maxTrackHeight, (int) track.getProperty (ids::height, defaultTrackHeight));
    }

    // Mixer (undoable; values are clamped like AudioTrack's).
    void setTrackGain (juce::ValueTree track, float db);
    void setTrackPan (juce::ValueTree track, float pan);
    void setTrackMute (juce::ValueTree track, bool shouldMute);
    void setTrackSolo (juce::ValueTree track, bool shouldSolo);
    static float getTrackGain (const juce::ValueTree& track) { return track[ids::gain]; }
    static float getTrackPan (const juce::ValueTree& track)  { return track[ids::pan]; }
    static bool isTrackMuted (const juce::ValueTree& track)  { return track[ids::mute]; }
    static bool isTrackSoloed (const juce::ValueTree& track) { return track[ids::solo]; }

    // ---- devices (undoable) ---------------------------------------------------
    /** The instrument DEVICE of a MIDI track (invalid for audio tracks). */
    static juce::ValueTree getDevice (const juce::ValueTree& track);

    /** Sets a parameter of a DEVICE (or, given a TRACK, of its instrument); clamped/stepped
        like the device does. 'paramIndex' indexes getDeviceParamInfos(). */
    void setDeviceParam (juce::ValueTree deviceOrTrack, int paramIndex, float value);
    static float getDeviceParam (const juce::ValueTree& deviceOrTrack, int paramIndex);
    static const std::vector<ParamInfo>& getDeviceParamInfos (const juce::ValueTree& device);
    static juce::String getDeviceType (const juce::ValueTree& device) { return device[ids::type].toString(); }

    // Insert effects: any track; processed in order after the clips / instrument, before the fader.
    static bool isInstrumentType (const juce::String& type) { return type == "synth" || type == "drumrack" || type == "pluginInstrument"; }
    static bool isEffect (const juce::ValueTree& tree) { return tree.hasType (ids::DEVICE) && ! isInstrumentType (getDeviceType (tree)); }
    static bool isDrumTrack (const juce::ValueTree& track) { return getDeviceType (getDevice (track)) == "drumrack"; }

    // ---- drum rack pads (undoable) ----------------------------------------------
    /** SamplePool key of a pad's sample ("" = empty pad; "builtin:<name>" for builtin sounds). */
    static juce::String getPadSampleKey (const juce::ValueTree& drumRack, int pad);
    /** Display name: the builtin sound's name or the file name without extension. */
    static juce::String getPadName (const juce::ValueTree& drumRack, int pad);
    /** Puts a sample on a pad. The key is SamplePool::keyFor (file) or a builtin key; the pad
        stays silent until the sample is in the pool (load it with SamplePool::loadAsync). */
    void setPadSample (juce::ValueTree drumRack, int pad, const juce::String& sampleKey);
    void clearPad (juce::ValueTree drumRack, int pad) { setPadSample (drumRack, pad, {}); }
    juce::Array<juce::ValueTree> getEffects (const juce::ValueTree& track) const;

    /** Adds an effect of a type from getEffectTypes() at position 'index' among the track's
        effects (-1 = end). Returns the DEVICE, or invalid for an unknown type. */
    juce::ValueTree addEffect (juce::ValueTree track, const juce::String& type, int index = -1);
    void removeEffect (juce::ValueTree device);
    void moveEffect (juce::ValueTree device, int newIndex);  // index among the track's effects
    void setEffectEnabled (juce::ValueTree device, bool shouldBeEnabled);
    static bool isEffectEnabled (const juce::ValueTree& device) { return device.getProperty (ids::enabled, true); }

    /** The live engine instance behind an effect DEVICE (for meters such as gain reduction). */
    std::shared_ptr<AudioEffect> getEffectInstance (const juce::ValueTree& device) const;

    // ---- automation (all undoable) ---------------------------------------------
    /** Something on a track that can be automated. */
    struct AutomationTarget
    {
        juce::String id;     // "gain", "pan" or "<device uid>:<param id>"
        juce::String name;   // "Volume", "Pan", "Synth: Filter cutoff", "Reverb: Mix", ...
        ParamInfo info;      // range / default / unit (info.id and info.name are not used)
    };

    /** Everything on this track that can be automated: Volume, Pan, then every parameter of
        every device in chain order. */
    static std::vector<AutomationTarget> getAutomationTargets (const juce::ValueTree& track);
    static bool findAutomationTarget (const juce::ValueTree& track, const juce::String& targetId, AutomationTarget& result);

    static bool isAutomationLane (const juce::ValueTree& tree) { return tree.hasType (ids::AUTOMATION); }
    static juce::String getAutomationTargetId (const juce::ValueTree& lane) { return lane[ids::target].toString(); }

    /** The lane automating 'targetId' on this track, or invalid. */
    static juce::ValueTree getAutomationLane (const juce::ValueTree& track, const juce::String& targetId);

    /** Existing lane, or a new empty one (an empty lane doesn't affect playback). */
    juce::ValueTree getOrCreateAutomationLane (juce::ValueTree track, const juce::String& targetId);
    void removeAutomationLane (juce::ValueTree lane);

    /** Points are clamped to the target's range (beat >= 0; stepped targets rounded). */
    juce::ValueTree addAutomationPoint (juce::ValueTree lane, double beat, float value);
    void moveAutomationPoint (juce::ValueTree point, double newBeat, float newValue);
    void deleteAutomationPoint (juce::ValueTree point);

    static double getPointBeat (const juce::ValueTree& point) { return point[ids::beat]; }
    static float getPointValue (const juce::ValueTree& point) { return point[ids::value]; }

    /** The envelope's value at 'beat' (same rule as the engine: linear between points, holding
        the first / last value outside them). Returns 'fallback' for a lane without points. */
    static float getAutomationValueAt (const juce::ValueTree& lane, double beat, float fallback);

    /** True when the target has automation points (the engine then follows the envelope). */
    static bool isAutomated (const juce::ValueTree& track, const juce::String& targetId);

    /** Automation target id of a device parameter ("<uid>:<paramId>"). */
    static juce::String getDeviceParamTargetId (const juce::ValueTree& device, int paramIndex);

    /** The value the engine is using right now (differs from the model while automated). */
    float getEngineValue (const juce::ValueTree& track, const juce::String& targetId);

    /** Current (non-automated) value of a target as stored in the model. */
    static float getTargetModelValue (const juce::ValueTree& track, const juce::String& targetId);

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
    void syncEffects (const juce::ValueTree& track);
    void syncAutomation (const juce::ValueTree& track);
    int nextDeviceUid() const;
    static juce::ValueTree findDeviceByUid (const juce::ValueTree& track, int deviceUid);
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int oldIndex, int newIndex) override;
    static juce::ValueTree makeDevice (const juce::String& type = "synth");
    void syncDrumKit (const juce::ValueTree& track);
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

    // Plugin instruments (by DEVICE) and load errors. Message thread only.
    std::vector<std::pair<juce::ValueTree, std::shared_ptr<class PluginInstrument>>> pluginInstruments;
    std::vector<std::pair<juce::ValueTree, juce::String>> pluginErrors;
    PluginHost* pluginHost = nullptr;
    std::atomic<bool> pluginsChanged { false }; // a plugin's own parameters changed (any thread)
    struct PluginChangeListener;
    std::unique_ptr<PluginChangeListener> pluginListener;

    std::unique_ptr<juce::AudioPluginInstance> createPluginFor (const juce::ValueTree& device, double sampleRate, int blockSize);
    void replaceInstrument (juce::ValueTree track, juce::ValueTree newDevice);
    void forgetPluginInstance (juce::AudioPluginInstance*);
    /** Stores a plugin DEVICE's current state in the tree (not undoable), so that an undo
        bringing the DEVICE back restores the plugin as it was. */
    void captureStateBeforeRemoval (juce::ValueTree device);

    // Engine instances of effect DEVICEs (kept so an effect keeps its state, e.g. a reverb
    // tail, when the chain around it changes). Message thread only.
    std::vector<std::pair<juce::ValueTree, std::shared_ptr<AudioEffect>>> effectInstances;
    bool unsavedChanges = false;

    JUCE_DECLARE_NON_COPYABLE (Project)
};

} // namespace daw
