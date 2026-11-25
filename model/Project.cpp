#include "Project.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace daw
{

namespace
{
    const juce::Colour trackPalette[] = {
        juce::Colour (0xffff8a3d), juce::Colour (0xffffc23d), juce::Colour (0xff9be05a),
        juce::Colour (0xff3ddc97), juce::Colour (0xff3dc8ff), juce::Colour (0xff7a8cff),
        juce::Colour (0xffc77aff), juce::Colour (0xffff6fa8),
    };
}

Project::Project (Engine& e, SamplePool& p) : engine (e), pool (p)
{
    state.addListener (this);
    newDocument (0);
}

Project::~Project()
{
    state.removeListener (this);
}

void Project::beginTransaction (const juce::String& actionName)
{
    undoManager.beginNewTransaction (actionName);
}

bool Project::undo() { return undoManager.undo(); }
bool Project::redo() { return undoManager.redo(); }

double Project::secondsToBeats (double seconds) const { return seconds * engine.getTransport().getBpm() / 60.0; }
double Project::beatsToSeconds (double beats) const   { return beats * 60.0 / engine.getTransport().getBpm(); }

SamplePool::Ptr Project::getClipSample (const juce::ValueTree& clip) const
{
    return pool.get (clip[ids::file].toString());
}

// ---- document / files -------------------------------------------------------

namespace
{
    /** Shortest decimal text that parses back to exactly the same double (JUCE's own var
        serialisation drops the last bits, which would move clips after a save/load). */
    juce::String exactDoubleText (double value)
    {
        char buffer[64];
        const auto result = std::to_chars (buffer, buffer + sizeof (buffer), value);
        return juce::String (buffer, (size_t) (result.ptr - buffer));
    }

    double parseExactDouble (const juce::var& value)
    {
        if (! value.isString())
            return (double) value;

        const auto text = value.toString().trim().toStdString();
        double parsed = 0.0;
        const auto result = std::from_chars (text.data(), text.data() + text.size(), parsed);
        return result.ec == std::errc() ? parsed : (double) value;
    }

    /** Replaces every double property in the tree by its exact text form (saving only). */
    void writeDoublesExactly (juce::ValueTree tree)
    {
        for (int i = 0; i < tree.getNumProperties(); ++i)
        {
            const auto id = tree.getPropertyName (i);
            if (const auto& v = tree[id]; v.isDouble())
                tree.setProperty (id, exactDoubleText ((double) v), nullptr);
        }

        for (auto child : tree)
            writeDoublesExactly (child);
    }

    /** XML gives every property back as a string; convert the known ones to their real types
        (and fill in defaults) so comparisons and arithmetic behave the same as before saving. */
    void normaliseTypes (juce::ValueTree& tree)
    {
        auto setDouble = [] (juce::ValueTree& t, const juce::Identifier& id, double fallback)
        {
            t.setProperty (id, t.hasProperty (id) ? parseExactDouble (t[id]) : fallback, nullptr);
        };
        auto setBool = [] (juce::ValueTree& t, const juce::Identifier& id)
        {
            t.setProperty (id, t.hasProperty (id) && (bool) t[id], nullptr);
        };

        if (tree.hasType (ids::TRACK))
        {
            setDouble (tree, ids::gain, 0.0);
            setDouble (tree, ids::pan, 0.0);
            setBool (tree, ids::mute);
            setBool (tree, ids::solo);
            tree.setProperty (ids::gain, juce::jlimit ((double) AudioTrack::minGainDb, (double) AudioTrack::maxGainDb, (double) tree[ids::gain]), nullptr);
            tree.setProperty (ids::pan, juce::jlimit (-1.0, 1.0, (double) tree[ids::pan]), nullptr);
        }
        else if (tree.hasType (ids::CLIP))
        {
            setDouble (tree, ids::start, 0.0);
            setDouble (tree, ids::length, Project::minClipLengthBeats);
            setBool (tree, ids::loop);

            if (Project::isMidiClip (tree))
            {
                setDouble (tree, ids::offsetBeats, 0.0);
                setDouble (tree, ids::loopLength, (double) tree[ids::length]);
                tree.setProperty (ids::loopLength, juce::jmax (Project::minClipLengthBeats, (double) tree[ids::loopLength]), nullptr);
            }
            else
            {
                setDouble (tree, ids::offset, 0.0);
            }

            tree.setProperty (ids::start, juce::jmax (0.0, (double) tree[ids::start]), nullptr);
            tree.setProperty (ids::length, juce::jmax (Project::minClipLengthBeats, (double) tree[ids::length]), nullptr);
        }
        else if (tree.hasType (ids::NOTE))
        {
            setDouble (tree, ids::start, 0.0);
            setDouble (tree, ids::length, 0.25);
            setDouble (tree, ids::velocity, 0.8);
            tree.setProperty (ids::pitch, juce::jlimit (0, 127, (int) tree.getProperty (ids::pitch, 60)), nullptr);
            tree.setProperty (ids::start, juce::jmax (0.0, (double) tree[ids::start]), nullptr);
            tree.setProperty (ids::length, juce::jmax (Project::minNoteLengthBeats, (double) tree[ids::length]), nullptr);
            tree.setProperty (ids::velocity, juce::jlimit (0.0, 1.0, (double) tree[ids::velocity]), nullptr);
        }
        else if (tree.hasType (ids::DEVICE))
        {
            for (const auto& info : Project::getDeviceParamInfos (tree))
            {
                if (info.id == nullptr || ! tree.hasProperty (info.id))
                    continue;

                // Same clamping and stepping as setDeviceParam / the devices themselves.
                auto value = juce::jlimit ((double) info.minValue, (double) info.maxValue, parseExactDouble (tree[info.id]));
                if (info.stepped)
                    value = std::round (value);

                tree.setProperty (info.id, value, nullptr);
            }

            if (Project::isEffect (tree))
                tree.setProperty (ids::enabled, (bool) tree.getProperty (ids::enabled, true), nullptr);
        }

        for (auto child : tree)
            normaliseTypes (child);
    }
}

juce::ValueTree Project::makeDevice()
{
    juce::ValueTree device (ids::DEVICE);
    device.setProperty (ids::type, "synth", nullptr);

    for (const auto& info : Synth::getParamInfos())
        if (info.id != nullptr)
            device.setProperty (info.id, (double) info.defaultValue, nullptr);

    return device;
}

juce::ValueTree Project::makeTrack (int number, TrackKind trackKind)
{
    const auto midi = trackKind == TrackKind::midi;
    juce::ValueTree track (ids::TRACK);
    track.setProperty (ids::kind, midi ? "midi" : "audio", nullptr);
    track.setProperty (ids::name, juce::String (number) + (midi ? " MIDI" : " Audio"), nullptr);
    track.setProperty (ids::colour, trackPalette[(size_t) (number - 1) % std::size (trackPalette)].toString(), nullptr);
    track.setProperty (ids::gain, 0.0, nullptr);
    track.setProperty (ids::pan, 0.0, nullptr);
    track.setProperty (ids::mute, false, nullptr);
    track.setProperty (ids::solo, false, nullptr);

    if (midi)
        track.appendChild (makeDevice(), nullptr);

    return track;
}

juce::String Project::replaceContents (const juce::ValueTree& newState)
{
    undoManager.clearUndoHistory();
    state.removeAllChildren (nullptr);
    engine.resetTracks();
    effectInstances.clear();

    state.setProperty (ids::version, fileFormatVersion, nullptr);
    state.setProperty (ids::bpm, juce::jlimit (Transport::minBpm, Transport::maxBpm,
                                               parseExactDouble (newState.getProperty (ids::bpm, 120.0))), nullptr);
    state.setProperty (ids::beatsPerBar, juce::jlimit (1, 32, (int) newState.getProperty (ids::beatsPerBar, 4)), nullptr);
    state.setProperty (ids::masterGain, juce::jlimit ((double) AudioTrack::minGainDb, (double) AudioTrack::maxGainDb,
                                                      parseExactDouble (newState.getProperty (ids::masterGain, 0.0))), nullptr);
    syncProjectProperties();

    juce::String warning;

    for (const auto& t : newState)
    {
        if (! t.hasType (ids::TRACK))
            continue;

        const auto index = engine.addTrack();
        if (index < 0)
        {
            warning = "Only the first " + juce::String (Engine::maxTracks) + " tracks were loaded.";
            break;
        }

        auto track = t.createCopy();
        normaliseTypes (track);
        track.setProperty (ids::engineIndex, index, nullptr);

        if (track[ids::name].toString().isEmpty())
            track.setProperty (ids::name, juce::String (getNumTracks() + 1) + " Audio", nullptr);

        if (! track.hasProperty (ids::colour))
            track.setProperty (ids::colour, makeTrack (getNumTracks() + 1)[ids::colour], nullptr);

        // Enforce the structure the rest of the code trusts: clips of the track's kind,
        // exactly one synth DEVICE on MIDI tracks (first device), effects of known types.
        const auto midi = isMidiTrack (track);
        track.setProperty (ids::kind, midi ? "midi" : "audio", nullptr);
        auto haveDevice = false;

        for (int i = 0; i < track.getNumChildren(); ++i)
        {
            const auto child = track.getChild (i);
            const auto isSynth = child.hasType (ids::DEVICE) && getDeviceType (child) == "synth";
            const auto keep = (child.hasType (ids::CLIP) && isMidiClip (child) == midi)
                           || (midi && isSynth && ! haveDevice)
                           || (isEffect (child) && getEffectTypes().contains (getDeviceType (child)));
            haveDevice = haveDevice || (keep && isSynth);

            if (! keep)
            {
                track.removeChild (i--, nullptr);
                continue;
            }

            else if (isMidiClip (child))
                for (int n = child.getNumChildren(); --n >= 0;)
                    if (! child.getChild (n).hasType (ids::NOTE))
                        juce::ValueTree (child).removeChild (n, nullptr);
        }

        if (midi && ! haveDevice)
            track.addChild (makeDevice(), 0, nullptr);

        // The instrument comes first in the device chain.
        if (midi)
            if (const auto synthDevice = getDevice (track); track.indexOf (synthDevice) > 0)
            {
                track.removeChild (synthDevice, nullptr);
                track.addChild (synthDevice, 0, nullptr);
            }

        state.appendChild (track, nullptr);
    }

    undoManager.clearUndoHistory();
    unsavedChanges = false;
    return warning;
}

void Project::newDocument (int numAudioTracks, int numMidiTracks)
{
    juce::ValueTree fresh (ids::PROJECT);
    for (int i = 0; i < juce::jmin (numAudioTracks + numMidiTracks, Engine::maxTracks); ++i)
        fresh.appendChild (makeTrack (i + 1, i < numMidiTracks ? TrackKind::midi : TrackKind::audio), nullptr);

    replaceContents (fresh);
    documentFile = juce::File();
}

void Project::copyFrom (const Project& other)
{
    replaceContents (other.state.createCopy());
    documentFile = juce::File();
}

juce::Result Project::saveTo (const juce::File& file)
{
    const auto folder = file.getParentDirectory();
    auto copy = state.createCopy();

    for (auto track : copy)
    {
        track.removeProperty (ids::engineIndex, nullptr); // runtime only

        for (auto clip : track)
            if (isClip (clip) && ! isMidiClip (clip))
                clip.setProperty (ids::relativeFile, getClipFile (clip).getRelativePathFrom (folder), nullptr);
    }

    writeDoublesExactly (copy);
    const auto xml = copy.createXml();
    if (xml == nullptr)
        return juce::Result::fail ("Couldn't serialise the project.");

    if (! folder.createDirectory())
        return juce::Result::fail ("Couldn't create " + folder.getFullPathName());

    if (! xml->writeTo (file))
        return juce::Result::fail ("Couldn't write " + file.getFullPathName());

    documentFile = file;
    unsavedChanges = false;
    return juce::Result::ok();
}

juce::Result Project::loadFrom (const juce::File& file)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("File not found: " + file.getFullPathName());

    const auto xml = juce::parseXML (file);
    if (xml == nullptr)
        return juce::Result::fail (file.getFileName() + " is not a valid project file.");

    auto loaded = juce::ValueTree::fromXml (*xml);
    if (! loaded.hasType (ids::PROJECT))
        return juce::Result::fail (file.getFileName() + " is not a diyDaw project.");

    if ((int) loaded[ids::version] > fileFormatVersion)
        return juce::Result::fail (file.getFileName() + " was saved by a newer version of diyDaw.");

    // Resolve sample paths: saved absolute path first, then relative to the project file
    // (so a project folder can be moved or copied to another machine).
    juce::Array<juce::File> files;

    for (auto track : loaded)
    {
        for (auto clip : track)
        {
            if (! isClip (clip) || isMidiClip (clip))
                continue;

            auto sampleFile = getClipFile (clip);
            const auto relative = clip[ids::relativeFile].toString();

            if (! sampleFile.existsAsFile() && relative.isNotEmpty())
                if (const auto candidate = file.getParentDirectory().getChildFile (relative); candidate.existsAsFile())
                    sampleFile = candidate;

            clip.setProperty (ids::file, SamplePool::keyFor (sampleFile), nullptr);
            clip.removeProperty (ids::relativeFile, nullptr);
            files.addIfNotAlreadyThere (sampleFile);
        }
    }

    const auto warning = replaceContents (loaded);
    documentFile = file;

    if (warning.isNotEmpty())
        DBG (warning);

    // Clips are skipped by the engine until their sample arrives; SamplePool::onSampleAdded
    // then triggers syncAllTracks().
    for (const auto& f : files)
        if (pool.get (f) == nullptr && ! pool.isLoading (f) && f.existsAsFile())
            pool.loadAsync (f, [] (SamplePool::Ptr) {});

    return juce::Result::ok();
}

// ---- tempo / master -----------------------------------------------------------

void Project::setBpm (double newBpm)
{
    state.setProperty (ids::bpm, juce::jlimit (Transport::minBpm, Transport::maxBpm, newBpm), &undoManager);
}

void Project::setMasterGain (float db)
{
    state.setProperty (ids::masterGain, juce::jlimit (AudioTrack::minGainDb, AudioTrack::maxGainDb, db), &undoManager);
}

void Project::syncProjectProperties()
{
    engine.getTransport().setBpm (getBpm());
    engine.getTransport().setBeatsPerBar ((int) state[ids::beatsPerBar]);
    engine.setMasterGainDb (getMasterGain());
}

// ---- tracks -----------------------------------------------------------------

juce::ValueTree Project::addTrack (const juce::String& name, TrackKind trackKind)
{
    const auto index = engine.addTrack();
    if (index < 0)
        return {};

    auto track = makeTrack (getNumTracks() + 1, trackKind);
    if (name.isNotEmpty())
        track.setProperty (ids::name, name, nullptr);

    track.setProperty (ids::engineIndex, index, nullptr);
    state.appendChild (track, nullptr);
    return track;
}

AudioTrack& Project::getEngineTrack (const juce::ValueTree& track)
{
    return engine.getTrack ((int) track[ids::engineIndex]);
}

void Project::setTrackName (juce::ValueTree track, const juce::String& newName)
{
    if (newName.isNotEmpty())
        track.setProperty (ids::name, newName, &undoManager);
}

juce::Colour Project::getTrackColour (const juce::ValueTree& track)
{
    return juce::Colour::fromString (track[ids::colour].toString());
}

void Project::setTrackGain (juce::ValueTree track, float db)
{
    if (track.hasType (ids::TRACK))
        track.setProperty (ids::gain, juce::jlimit (AudioTrack::minGainDb, AudioTrack::maxGainDb, db), &undoManager);
}

void Project::setTrackPan (juce::ValueTree track, float newPan)
{
    if (track.hasType (ids::TRACK))
        track.setProperty (ids::pan, juce::jlimit (-1.0f, 1.0f, newPan), &undoManager);
}

void Project::setTrackMute (juce::ValueTree track, bool shouldMute)
{
    if (track.hasType (ids::TRACK))
        track.setProperty (ids::mute, shouldMute, &undoManager);
}

void Project::setTrackSolo (juce::ValueTree track, bool shouldSolo)
{
    if (track.hasType (ids::TRACK))
        track.setProperty (ids::solo, shouldSolo, &undoManager);
}

juce::ValueTree Project::getDevice (const juce::ValueTree& track)
{
    for (const auto& child : track)
        if (child.hasType (ids::DEVICE) && getDeviceType (child) == "synth")
            return child;

    return {};
}

const std::vector<ParamInfo>& Project::getDeviceParamInfos (const juce::ValueTree& device)
{
    static const std::vector<ParamInfo> synthInfos (Synth::getParamInfos().begin(), Synth::getParamInfos().end());

    if (getDeviceType (device) == "synth")
        return synthInfos;

    return getEffectParamInfos (getDeviceType (device));
}

void Project::setDeviceParam (juce::ValueTree deviceOrTrack, int paramIndex, float value)
{
    auto device = deviceOrTrack.hasType (ids::TRACK) ? getDevice (deviceOrTrack) : deviceOrTrack;
    const auto& infos = getDeviceParamInfos (device);
    if (! device.hasType (ids::DEVICE) || paramIndex < 0 || paramIndex >= (int) infos.size())
        return;

    const auto& info = infos[(size_t) paramIndex];
    if (info.id == nullptr)
        return;

    value = juce::jlimit (info.minValue, info.maxValue, value);
    if (info.stepped)
        value = std::round (value);

    device.setProperty (info.id, (double) value, &undoManager);
}

float Project::getDeviceParam (const juce::ValueTree& deviceOrTrack, int paramIndex)
{
    const auto device = deviceOrTrack.hasType (ids::TRACK) ? getDevice (deviceOrTrack) : deviceOrTrack;
    const auto& infos = getDeviceParamInfos (device);
    if (paramIndex < 0 || paramIndex >= (int) infos.size())
        return 0.0f;

    const auto& info = infos[(size_t) paramIndex];
    if (info.id == nullptr)
        return 0.0f;

    return (float) device.getProperty (info.id, info.defaultValue);
}

// ---- effects ----------------------------------------------------------------------

juce::Array<juce::ValueTree> Project::getEffects (const juce::ValueTree& track) const
{
    juce::Array<juce::ValueTree> result;
    for (const auto& child : track)
        if (isEffect (child))
            result.add (child);
    return result;
}

juce::ValueTree Project::addEffect (juce::ValueTree track, const juce::String& effectType, int index)
{
    if (! track.hasType (ids::TRACK) || ! getEffectTypes().contains (effectType))
        return {};

    juce::ValueTree device (ids::DEVICE);
    device.setProperty (ids::type, effectType, nullptr);
    device.setProperty (ids::enabled, true, nullptr);

    for (const auto& info : getEffectParamInfos (effectType))
        device.setProperty (info.id, (double) info.defaultValue, nullptr);

    // Insert among the effects: before the index-th effect, or after the last device.
    const auto effects = getEffects (track);
    int childIndex = -1;

    if (index >= 0 && index < effects.size())
        childIndex = track.indexOf (effects[index]);
    else if (! effects.isEmpty())
        childIndex = track.indexOf (effects.getLast()) + 1;
    else if (const auto instrument = getDevice (track); instrument.isValid())
        childIndex = track.indexOf (instrument) + 1;

    track.addChild (device, childIndex, &undoManager);
    return device;
}

void Project::removeEffect (juce::ValueTree device)
{
    auto track = device.getParent();
    if (isEffect (device) && track.isValid())
        track.removeChild (device, &undoManager);
}

void Project::moveEffect (juce::ValueTree device, int newIndex)
{
    auto track = device.getParent();
    if (! isEffect (device) || ! track.isValid())
        return;

    const auto effects = getEffects (track);
    newIndex = juce::jlimit (0, effects.size() - 1, newIndex);
    if (effects.indexOf (device) == newIndex)
        return;

    track.moveChild (track.indexOf (device), track.indexOf (effects[newIndex]), &undoManager);
}

void Project::setEffectEnabled (juce::ValueTree device, bool shouldBeEnabled)
{
    if (isEffect (device))
        device.setProperty (ids::enabled, shouldBeEnabled, &undoManager);
}

std::shared_ptr<AudioEffect> Project::getEffectInstance (const juce::ValueTree& device) const
{
    for (const auto& [tree, instance] : effectInstances)
        if (tree == device)
            return instance;

    return nullptr;
}

void Project::syncEffects (const juce::ValueTree& track)
{
    if (! track.hasType (ids::TRACK))
        return;

    auto chain = std::make_unique<EffectChain>();

    for (const auto& device : getEffects (track))
    {
        auto instance = getEffectInstance (device);

        if (instance == nullptr)
        {
            instance = createEffect (getDeviceType (device));
            if (instance == nullptr)
                continue;

            instance->prepare (engine.getPreparedSampleRate(), engine.getPreparedBlockSize());
            effectInstances.emplace_back (device, instance);
        }

        const auto& infos = instance->getParamInfos();
        for (int i = 0; i < (int) infos.size(); ++i)
            instance->setParam (i, getDeviceParam (device, i));

        instance->setEnabled (isEffectEnabled (device));
        chain->effects.push_back (std::move (instance));
    }

    getEngineTrack (track).setEffects (std::move (chain));

    // Forget instances whose DEVICE is no longer in the project (an undo that brings one back
    // gets a fresh instance). The engine keeps its own references until it's done with them.
    effectInstances.erase (std::remove_if (effectInstances.begin(), effectInstances.end(),
                                           [this] (const auto& entry)
                                           {
                                               const auto parent = entry.first.getParent();
                                               return ! parent.isValid() || indexOfTrack (parent) < 0;
                                           }),
                           effectInstances.end());
}

void Project::syncDevice (const juce::ValueTree& track)
{
    if (! track.hasType (ids::TRACK))
        return;

    auto& t = getEngineTrack (track);
    t.setInstrumentEnabled (isMidiTrack (track));

    if (isMidiTrack (track))
        for (int i = 0; i < Synth::numParams; ++i)
            t.getSynth().setParam (i, getDeviceParam (track, i));

    syncEffects (track);
}

void Project::syncTrackMixer (const juce::ValueTree& track)
{
    if (! track.hasType (ids::TRACK))
        return;

    auto& t = getEngineTrack (track);
    t.setGainDb (getTrackGain (track));
    t.setPan (getTrackPan (track));
    t.setMute (isTrackMuted (track));
    t.setSolo (isTrackSoloed (track));
}

// ---- clips ------------------------------------------------------------------

double Project::normaliseOffset (const juce::ValueTree& clip, double offsetSeconds) const
{
    const auto sample = getClipSample (clip);
    if (sample == nullptr)
        return juce::jmax (0.0, offsetSeconds);

    const auto len = sample->getLengthSeconds();

    if (isClipLooping (clip) && len > 0.0)
    {
        offsetSeconds = std::fmod (offsetSeconds, len);
        return offsetSeconds < 0.0 ? offsetSeconds + len : offsetSeconds;
    }

    return juce::jlimit (0.0, len, offsetSeconds);
}

juce::ValueTree Project::addClip (juce::ValueTree track, const juce::File& file, double startBeat)
{
    const auto sample = pool.get (file);
    if (sample == nullptr || ! track.hasType (ids::TRACK) || isMidiTrack (track))
        return {};

    startBeat = juce::jmax (0.0, startBeat);
    const auto length = juce::jmax (minClipLengthBeats, secondsToBeats (sample->getLengthSeconds()));

    clearRegion (track, startBeat, startBeat + length, {});

    juce::ValueTree clip (ids::CLIP);
    clip.setProperty (ids::kind, "audio", nullptr);
    clip.setProperty (ids::name, sample->name, nullptr);
    clip.setProperty (ids::file, SamplePool::keyFor (file), nullptr);
    clip.setProperty (ids::start, startBeat, nullptr);
    clip.setProperty (ids::length, length, nullptr);
    clip.setProperty (ids::offset, 0.0, nullptr);
    clip.setProperty (ids::loop, false, nullptr);
    track.appendChild (clip, &undoManager);
    return clip;
}

juce::ValueTree Project::addMidiClip (juce::ValueTree track, double startBeat, double lengthBeats)
{
    if (! track.hasType (ids::TRACK) || ! isMidiTrack (track))
        return {};

    startBeat = juce::jmax (0.0, startBeat);
    lengthBeats = juce::jmax (minClipLengthBeats, lengthBeats);

    clearRegion (track, startBeat, startBeat + lengthBeats, {});

    juce::ValueTree clip (ids::CLIP);
    clip.setProperty (ids::kind, "midi", nullptr);
    clip.setProperty (ids::name, "MIDI Clip", nullptr);
    clip.setProperty (ids::start, startBeat, nullptr);
    clip.setProperty (ids::length, lengthBeats, nullptr);
    clip.setProperty (ids::offsetBeats, 0.0, nullptr);
    clip.setProperty (ids::loop, true, nullptr);
    clip.setProperty (ids::loopLength, lengthBeats, nullptr);
    track.appendChild (clip, &undoManager);
    return clip;
}

void Project::moveClip (juce::ValueTree clip, juce::ValueTree toTrack, double newStartBeat)
{
    if (! clip.hasType (ids::CLIP) || ! toTrack.hasType (ids::TRACK) || isMidiClip (clip) != isMidiTrack (toTrack))
        return;

    newStartBeat = juce::jmax (0.0, newStartBeat);
    const auto length = getClipLength (clip);

    auto fromTrack = clip.getParent();
    if (fromTrack != toTrack)
    {
        fromTrack.removeChild (clip, &undoManager);
        toTrack.appendChild (clip, &undoManager);
    }

    clip.setProperty (ids::start, newStartBeat, &undoManager);
    clearRegion (toTrack, newStartBeat, newStartBeat + length, clip);
}

double Project::normaliseOffsetBeats (const juce::ValueTree& clip, double offsetBeats) const
{
    const auto len = getClipLoopLength (clip);

    if (isClipLooping (clip) && len > 0.0)
    {
        offsetBeats = std::fmod (offsetBeats, len);
        return offsetBeats < 0.0 ? offsetBeats + len : offsetBeats;
    }

    return juce::jmax (0.0, offsetBeats);
}

void Project::shiftContent (juce::ValueTree clip, double deltaBeats)
{
    if (isMidiClip (clip))
        clip.setProperty (ids::offsetBeats, normaliseOffsetBeats (clip, getClipOffsetBeats (clip) + deltaBeats), &undoManager);
    else
        clip.setProperty (ids::offset, normaliseOffset (clip, getClipOffset (clip) + beatsToSeconds (deltaBeats)), &undoManager);
}

void Project::setStartKeepingEnd (juce::ValueTree clip, double newStart)
{
    const auto oldStart = getClipStart (clip);
    const auto end = getClipEnd (clip);

    shiftContent (clip, newStart - oldStart);
    clip.setProperty (ids::start, newStart, &undoManager);
    clip.setProperty (ids::length, end - newStart, &undoManager);
}

void Project::trimClipStart (juce::ValueTree clip, double newStartBeat)
{
    if (! clip.hasType (ids::CLIP))
        return;

    const auto start = getClipStart (clip);
    const auto end = getClipEnd (clip);

    auto earliest = 0.0;
    if (! isClipLooping (clip)) // can't reveal content from before its beginning
        earliest = juce::jmax (0.0, start - (isMidiClip (clip) ? getClipOffsetBeats (clip)
                                                               : secondsToBeats (getClipOffset (clip))));

    newStartBeat = juce::jlimit (earliest, end - minClipLengthBeats, newStartBeat);
    if (juce::exactlyEqual (newStartBeat, start))
        return;

    setStartKeepingEnd (clip, newStartBeat);
    clearRegion (clip.getParent(), newStartBeat, end, clip);
}

void Project::trimClipEnd (juce::ValueTree clip, double newEndBeat)
{
    if (! clip.hasType (ids::CLIP))
        return;

    const auto start = getClipStart (clip);
    auto latest = std::numeric_limits<double>::max();

    if (! isClipLooping (clip) && ! isMidiClip (clip))
        if (const auto sample = getClipSample (clip))
            latest = start + secondsToBeats (sample->getLengthSeconds() - getClipOffset (clip));

    newEndBeat = juce::jlimit (start + minClipLengthBeats, juce::jmax (start + minClipLengthBeats, latest), newEndBeat);

    clip.setProperty (ids::length, newEndBeat - start, &undoManager);
    clearRegion (clip.getParent(), start, newEndBeat, clip);
}

void Project::setClipLooping (juce::ValueTree clip, bool shouldLoop)
{
    if (! clip.hasType (ids::CLIP) || isClipLooping (clip) == shouldLoop)
        return;

    clip.setProperty (ids::loop, shouldLoop, &undoManager);

    if (isMidiClip (clip))
    {
        // Keep the offset inside the loop when looping is switched back on.
        clip.setProperty (ids::offsetBeats, normaliseOffsetBeats (clip, getClipOffsetBeats (clip)), &undoManager);
        return;
    }

    // A one-shot can't be longer than the audio left after its offset.
    if (! shouldLoop)
        trimClipEnd (clip, getClipEnd (clip));
}

void Project::deleteClip (juce::ValueTree clip)
{
    auto parent = clip.getParent();
    if (clip.hasType (ids::CLIP) && parent.isValid())
        parent.removeChild (clip, &undoManager);
}

juce::ValueTree Project::duplicateClip (juce::ValueTree clip)
{
    auto track = clip.getParent();
    if (! clip.hasType (ids::CLIP) || ! track.isValid())
        return {};

    auto copy = clip.createCopy();
    const auto start = getClipEnd (clip);
    copy.setProperty (ids::start, start, nullptr);

    clearRegion (track, start, start + getClipLength (clip), {});
    track.appendChild (copy, &undoManager);
    return copy;
}

juce::ValueTree Project::splitInternal (juce::ValueTree clip, double atBeat)
{
    auto right = clip.createCopy();
    const auto start = getClipStart (clip);
    const auto end = getClipEnd (clip);

    right.setProperty (ids::start, atBeat, nullptr);
    right.setProperty (ids::length, end - atBeat, nullptr);

    if (isMidiClip (clip))
        right.setProperty (ids::offsetBeats, normaliseOffsetBeats (clip, getClipOffsetBeats (clip) + (atBeat - start)), nullptr);
    else
        right.setProperty (ids::offset, normaliseOffset (clip, getClipOffset (clip) + beatsToSeconds (atBeat - start)), nullptr);

    clip.setProperty (ids::length, atBeat - start, &undoManager);
    clip.getParent().appendChild (right, &undoManager);
    return right;
}

juce::ValueTree Project::splitClip (juce::ValueTree clip, double atBeat)
{
    if (! clip.hasType (ids::CLIP) || ! clip.getParent().isValid())
        return {};

    if (atBeat <= getClipStart (clip) + 1.0e-9 || atBeat >= getClipEnd (clip) - 1.0e-9)
        return {};

    return splitInternal (clip, atBeat);
}

void Project::clearRegion (juce::ValueTree track, double start, double end, const juce::ValueTree& except)
{
    if (end <= start)
        return;

    // Snapshot the children first: the loop adds and removes clips.
    juce::Array<juce::ValueTree> clips;
    for (const auto& c : track)
        if (isClip (c) && c != except)
            clips.add (c);

    for (auto c : clips)
    {
        const auto cs = getClipStart (c);
        const auto ce = getClipEnd (c);

        if (ce <= start || cs >= end)
            continue;                              // no overlap

        if (cs >= start && ce <= end)
            track.removeChild (c, &undoManager);   // fully covered
        else if (cs < start && ce > end)
        {
            splitInternal (c, end);                // keep the part after the region...
            c.setProperty (ids::length, start - cs, &undoManager); // ...and the part before it
        }
        else if (cs < start)
            c.setProperty (ids::length, start - cs, &undoManager); // tail overlaps
        else
            setStartKeepingEnd (c, end);           // head overlaps
    }
}

// ---- notes --------------------------------------------------------------------

void Project::setClipLoopLength (juce::ValueTree clip, double newLoopLengthBeats)
{
    if (! isMidiClip (clip))
        return;

    clip.setProperty (ids::loopLength, juce::jmax (minClipLengthBeats, newLoopLengthBeats), &undoManager);
    clip.setProperty (ids::offsetBeats, normaliseOffsetBeats (clip, getClipOffsetBeats (clip)), &undoManager);
}

juce::ValueTree Project::addNote (juce::ValueTree clip, int notePitch, double startBeat, double lengthBeats, float noteVelocity)
{
    if (! isMidiClip (clip))
        return {};

    juce::ValueTree note (ids::NOTE);
    note.setProperty (ids::pitch, juce::jlimit (0, 127, notePitch), nullptr);
    note.setProperty (ids::start, juce::jmax (0.0, startBeat), nullptr);
    note.setProperty (ids::length, juce::jmax (minNoteLengthBeats, lengthBeats), nullptr);
    note.setProperty (ids::velocity, (double) juce::jlimit (0.0f, 1.0f, noteVelocity), nullptr);
    clip.appendChild (note, &undoManager);
    return note;
}

void Project::moveNote (juce::ValueTree note, double newStartBeat, int newPitch)
{
    if (! note.hasType (ids::NOTE))
        return;

    note.setProperty (ids::start, juce::jmax (0.0, newStartBeat), &undoManager);
    note.setProperty (ids::pitch, juce::jlimit (0, 127, newPitch), &undoManager);
}

void Project::setNoteLength (juce::ValueTree note, double newLengthBeats)
{
    if (note.hasType (ids::NOTE))
        note.setProperty (ids::length, juce::jmax (minNoteLengthBeats, newLengthBeats), &undoManager);
}

void Project::setNoteVelocity (juce::ValueTree note, float newVelocity)
{
    if (note.hasType (ids::NOTE))
        note.setProperty (ids::velocity, (double) juce::jlimit (0.0f, 1.0f, newVelocity), &undoManager);
}

void Project::deleteNote (juce::ValueTree note)
{
    auto clip = note.getParent();
    if (note.hasType (ids::NOTE) && clip.isValid())
        clip.removeChild (note, &undoManager);
}

// ---- engine sync ------------------------------------------------------------

void Project::syncTrack (const juce::ValueTree& track)
{
    if (! track.hasType (ids::TRACK))
        return;

    auto list = std::make_unique<ClipList>();

    for (const auto& c : track)
    {
        if (! isClip (c))
            continue;

        Clip clip;
        clip.startBeat = getClipStart (c);
        clip.lengthBeats = getClipLength (c);
        clip.looping = isClipLooping (c);

        if (isMidiClip (c))
        {
            auto sequence = std::make_shared<MidiSequence>();
            for (const auto& n : c)
                if (n.hasType (ids::NOTE))
                    sequence->notes.push_back ({ getNoteStart (n), getNoteLength (n), getNotePitch (n), getNoteVelocity (n) });

            std::sort (sequence->notes.begin(), sequence->notes.end(),
                       [] (const MidiNote& a, const MidiNote& b) { return a.startBeat < b.startBeat; });

            clip.notes = std::move (sequence);
            clip.offsetBeats = getClipOffsetBeats (c);
            clip.loopLengthBeats = getClipLoopLength (c);
        }
        else
        {
            clip.sample = getClipSample (c);
            if (clip.sample == nullptr)
                continue; // not loaded yet; syncAllTracks() picks it up later

            clip.offsetSeconds = getClipOffset (c);
        }

        list->clips.push_back (std::move (clip));
    }

    std::sort (list->clips.begin(), list->clips.end(),
               [] (const Clip& a, const Clip& b) { return a.startBeat < b.startBeat; });

    auto& engineTrack = getEngineTrack (track);
    engineTrack.setName (track[ids::name].toString());
    engineTrack.setClips (std::move (list));
}

void Project::syncAllTracks()
{
    for (const auto& t : state)
        syncTrack (t);
}

void Project::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    unsavedChanges = true;

    if (tree.hasType (ids::CLIP))
        syncTrack (tree.getParent());
    else if (tree.hasType (ids::NOTE))
        syncTrack (tree.getParent().getParent());
    else if (tree.hasType (ids::DEVICE))
    {
        const auto track = tree.getParent();
        if (! track.hasType (ids::TRACK))
            return;

        const auto& infos = getDeviceParamInfos (tree);
        const auto instance = isEffect (tree) ? getEffectInstance (tree) : nullptr;

        if (property == ids::enabled && instance != nullptr)
            instance->setEnabled (isEffectEnabled (tree));

        for (int i = 0; i < (int) infos.size(); ++i)
        {
            if (infos[(size_t) i].id == nullptr || property != juce::Identifier (infos[(size_t) i].id))
                continue;

            if (instance != nullptr)
                instance->setParam (i, getDeviceParam (tree, i));
            else if (! isEffect (tree))
                getEngineTrack (track).getSynth().setParam (i, getDeviceParam (tree, i));
        }
    }
    else if (tree.hasType (ids::TRACK))
    {
        if (property == ids::gain || property == ids::pan || property == ids::mute || property == ids::solo)
            syncTrackMixer (tree);
        else if (property == ids::name)
            getEngineTrack (tree).setName (tree[ids::name].toString());
        else if (property == ids::engineIndex)
        {
            syncTrack (tree);
            syncTrackMixer (tree);
            syncDevice (tree);
        }
    }
    else if (tree == state)
    {
        if (property == ids::bpm)
            engine.getTransport().setBpm (getBpm());
        else if (property == ids::beatsPerBar)
            engine.getTransport().setBeatsPerBar ((int) state[ids::beatsPerBar]);
        else if (property == ids::masterGain)
            engine.setMasterGainDb (getMasterGain());
    }
}

void Project::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    unsavedChanges = true;

    if (parent.hasType (ids::TRACK))
    {
        if (child.hasType (ids::DEVICE))
            syncDevice (parent);
        else
            syncTrack (parent);
    }
    else if (parent.hasType (ids::CLIP))
        syncTrack (parent.getParent());
    else if (child.hasType (ids::TRACK))
    {
        syncTrack (child);
        syncTrackMixer (child);
        syncDevice (child);
    }
}

void Project::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    unsavedChanges = true;

    if (parent.hasType (ids::TRACK) && child.hasType (ids::DEVICE))
        syncEffects (parent);
    else
        syncTrack (parent.hasType (ids::CLIP) ? parent.getParent() : parent);
}

void Project::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    unsavedChanges = true;

    if (parent.hasType (ids::TRACK))
        syncEffects (parent);
}

} // namespace daw
