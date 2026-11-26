#include "Project.h"

#include "BuiltinSamples.h"

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
        else if (tree.hasType (ids::POINT))
        {
            setDouble (tree, ids::beat, 0.0);
            setDouble (tree, ids::value, 0.0);
            tree.setProperty (ids::beat, juce::jmax (0.0, (double) tree[ids::beat]), nullptr);
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

namespace
{
    juce::Identifier padFileId (int pad) { return juce::Identifier ("pad" + juce::String (pad + 1) + "File"); }
}

juce::ValueTree Project::makeDevice (const juce::String& type)
{
    juce::ValueTree device (ids::DEVICE);
    device.setProperty (ids::type, type, nullptr);

    for (const auto& info : getDeviceParamInfos (device))
        if (info.id != nullptr)
            device.setProperty (info.id, (double) info.defaultValue, nullptr);

    if (type == "drumrack")
    {
        // The builtin kit, General MIDI-like; the two hi-hats choke each other.
        const auto& names = getBuiltinSampleNames();
        for (int pad = 0; pad < juce::jmin (DrumRack::numPads, names.size()); ++pad)
            device.setProperty (padFileId (pad), builtinSampleKey (names[pad]), nullptr);

        for (const auto& hat : { "closedHat", "openHat" })
            if (const auto pad = names.indexOf (hat); pad >= 0)
                device.setProperty (DrumRack::getParamInfos()[(size_t) DrumRack::paramIndex (pad, DrumRack::choke)].id, 1.0, nullptr);
    }

    return device;
}

juce::String Project::getPadSampleKey (const juce::ValueTree& drumRack, int pad)
{
    return drumRack[padFileId (pad)].toString();
}

juce::String Project::getPadName (const juce::ValueTree& drumRack, int pad)
{
    const auto key = getPadSampleKey (drumRack, pad);
    if (key.isEmpty())
        return {};

    if (isBuiltinSampleKey (key))
    {
        // "closedHat" -> "Closed Hat"
        juce::String name;
        for (auto c : builtinSampleName (key))
        {
            if (juce::CharacterFunctions::isUpperCase (c))
                name << ' ';
            name << juce::String::charToString (c);
        }
        return name.substring (0, 1).toUpperCase() + name.substring (1);
    }

    return juce::File (key).getFileNameWithoutExtension();
}

void Project::setPadSample (juce::ValueTree drumRack, int pad, const juce::String& sampleKey)
{
    if (getDeviceType (drumRack) != "drumrack" || pad < 0 || pad >= DrumRack::numPads)
        return;

    if (sampleKey.isEmpty())
        drumRack.removeProperty (padFileId (pad), &undoManager);
    else
        drumRack.setProperty (padFileId (pad), sampleKey, &undoManager);
}

void Project::syncDrumKit (const juce::ValueTree& track)
{
    const auto device = getDevice (track);
    if (getDeviceType (device) != "drumrack" || indexOfTrack (track) < 0)
        return;

    auto kit = std::make_unique<DrumRack::Kit>();
    for (int pad = 0; pad < DrumRack::numPads; ++pad)
        if (const auto key = getPadSampleKey (device, pad); key.isNotEmpty())
            kit->samples[(size_t) pad] = pool.get (key); // builtins are created on demand

    getEngineTrack (track).getDrumRack().setKit (std::move (kit));
}

juce::ValueTree Project::makeTrack (int number, TrackKind trackKind)
{
    const auto midi = trackKind != TrackKind::audio;
    juce::ValueTree track (ids::TRACK);
    track.setProperty (ids::kind, midi ? "midi" : "audio", nullptr);
    track.setProperty (ids::name, juce::String (number) + (trackKind == TrackKind::drums ? " Drums" : midi ? " MIDI" : " Audio"), nullptr);
    track.setProperty (ids::colour, trackPalette[(size_t) (number - 1) % std::size (trackPalette)].toString(), nullptr);
    track.setProperty (ids::gain, 0.0, nullptr);
    track.setProperty (ids::pan, 0.0, nullptr);
    track.setProperty (ids::mute, false, nullptr);
    track.setProperty (ids::solo, false, nullptr);

    if (midi)
        track.appendChild (makeDevice (trackKind == TrackKind::drums ? "drumrack" : "synth"), nullptr);

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
            const auto isSynth = child.hasType (ids::DEVICE) && isInstrumentType (getDeviceType (child));
            const auto keep = (child.hasType (ids::CLIP) && isMidiClip (child) == midi)
                           || (midi && isSynth && ! haveDevice)
                           || (isEffect (child) && getEffectTypes().contains (getDeviceType (child)))
                           || (isAutomationLane (child) && getAutomationTargetId (child).isNotEmpty());
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

            if (isAutomationLane (child))
                for (int n = child.getNumChildren(); --n >= 0;)
                    if (! child.getChild (n).hasType (ids::POINT))
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

    // The MASTER (master insert effects) is always the last child.
    {
        auto master = newState.getChildWithName (ids::MASTER).createCopy();
        if (! master.isValid())
            master = juce::ValueTree (ids::MASTER);

        for (int i = master.getNumChildren(); --i >= 0;)
            if (const auto child = master.getChild (i); ! isEffect (child) || ! getEffectTypes().contains (getDeviceType (child)))
                master.removeChild (i, nullptr);

        normaliseTypes (master);
        state.appendChild (master, nullptr);
    }

    // Every DEVICE needs a unique uid (automation refers to it); repair missing / duplicate ones.
    {
        juce::Array<int> used;
        juce::Array<juce::ValueTree> needUid;

        for (const auto& track : state)
            for (const auto& child : track)
                if (child.hasType (ids::DEVICE))
                {
                    const auto id = (int) child.getProperty (ids::uid, 0);
                    if (id > 0 && ! used.contains (id))
                        used.add (id);
                    else
                        needUid.add (child);
                }

        auto next = 1;
        for (auto device : needUid)
        {
            while (used.contains (next))
                ++next;
            device.setProperty (ids::uid, next, nullptr);
            used.add (next);
        }

        // Automation targets refer to uids: rebuild the engine snapshots with the final ones.
        if (! needUid.isEmpty())
            for (const auto& track : state)
                syncAutomation (track);
    }

    syncEffects (getMaster());

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

        // Drum pads: like clips, also store the path relative to the project file.
        for (auto device : track)
            if (getDeviceType (device) == "drumrack")
                for (int pad = 0; pad < DrumRack::numPads; ++pad)
                    if (const auto key = getPadSampleKey (device, pad); key.isNotEmpty() && ! isBuiltinSampleKey (key))
                        device.setProperty ("pad" + juce::String (pad + 1) + "RelativeFile",
                                            juce::File (key).getRelativePathFrom (folder), nullptr);
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

        for (auto device : track)
        {
            if (getDeviceType (device) != "drumrack")
                continue;

            for (int pad = 0; pad < DrumRack::numPads; ++pad)
            {
                const juce::Identifier relativeId ("pad" + juce::String (pad + 1) + "RelativeFile");
                const auto key = getPadSampleKey (device, pad);
                const auto relative = device[relativeId].toString();
                device.removeProperty (relativeId, nullptr);

                if (key.isEmpty() || isBuiltinSampleKey (key) || ! juce::File::isAbsolutePath (key))
                    continue;

                auto sampleFile = juce::File (key);
                if (! sampleFile.existsAsFile() && relative.isNotEmpty())
                    if (const auto candidate = file.getParentDirectory().getChildFile (relative); candidate.existsAsFile())
                        sampleFile = candidate;

                device.setProperty (padFileId (pad), SamplePool::keyFor (sampleFile), nullptr);
                files.addIfNotAlreadyThere (sampleFile);
            }
        }
    }

    const auto warning = replaceContents (loaded);
    documentFile = file;

    if (warning.isNotEmpty())
    {
        DBG (warning);
    }

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

int Project::getNumTracks() const
{
    auto n = 0;
    for (const auto& child : state)
        n += child.hasType (ids::TRACK) ? 1 : 0;
    return n;
}

juce::ValueTree Project::getTrack (int index) const
{
    for (const auto& child : state)
        if (child.hasType (ids::TRACK) && index-- == 0)
            return child;
    return {};
}

int Project::indexOfTrack (const juce::ValueTree& track) const
{
    if (! track.hasType (ids::TRACK))
        return -1;

    auto index = 0;
    for (const auto& child : state)
    {
        if (child == track)
            return index;
        index += child.hasType (ids::TRACK) ? 1 : 0;
    }
    return -1;
}

juce::ValueTree Project::addTrack (const juce::String& name, TrackKind trackKind)
{
    const auto index = engine.addTrack();
    if (index < 0)
        return {};

    auto track = makeTrack (getNumTracks() + 1, trackKind);
    if (auto device = getDevice (track); device.isValid())
        device.setProperty (ids::uid, nextDeviceUid(), nullptr);

    if (name.isNotEmpty())
        track.setProperty (ids::name, name, nullptr);

    track.setProperty (ids::engineIndex, index, nullptr);
    state.addChild (track, state.indexOf (getMaster()), nullptr); // tracks stay before MASTER
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

void Project::setTrackHeight (juce::ValueTree track, int heightInPixels)
{
    if (track.hasType (ids::TRACK))
        track.setProperty (ids::height, juce::jlimit (minTrackHeight, maxTrackHeight, heightInPixels), nullptr);
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
        if (child.hasType (ids::DEVICE) && isInstrumentType (getDeviceType (child)))
            return child;

    return {};
}

const std::vector<ParamInfo>& Project::getDeviceParamInfos (const juce::ValueTree& device)
{
    static const std::vector<ParamInfo> synthInfos (Synth::getParamInfos().begin(), Synth::getParamInfos().end());

    if (getDeviceType (device) == "synth")
        return synthInfos;

    if (getDeviceType (device) == "drumrack")
        return DrumRack::getParamInfos();

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
    if (! (track.hasType (ids::TRACK) || isMaster (track)) || ! getEffectTypes().contains (effectType))
        return {};

    juce::ValueTree device (ids::DEVICE);
    device.setProperty (ids::type, effectType, nullptr);
    device.setProperty (ids::uid, nextDeviceUid(), nullptr);
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
    if (! isEffect (device) || ! track.isValid())
        return;

    // Its automation goes with it (and comes back with an undo).
    const auto prefix = device[ids::uid].toString() + ":";
    for (int i = track.getNumChildren(); --i >= 0;)
        if (const auto child = track.getChild (i); isAutomationLane (child) && getAutomationTargetId (child).startsWith (prefix))
            track.removeChild (i, &undoManager);

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
    if (! track.hasType (ids::TRACK) && ! isMaster (track))
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

    if (isMaster (track))
        engine.setMasterEffects (std::move (chain));
    else
    {
        getEngineTrack (track).setEffects (std::move (chain));
        syncAutomation (track);
    }

    // Forget instances whose DEVICE is no longer in the project (an undo that brings one back
    // gets a fresh instance). The engine keeps its own references until it's done with them.
    effectInstances.erase (std::remove_if (effectInstances.begin(), effectInstances.end(),
                                           [this] (const auto& entry)
                                           {
                                               const auto parent = entry.first.getParent();
                                               if (isMaster (parent))
                                                   return parent != getMaster();
                                               return ! parent.isValid() || indexOfTrack (parent) < 0;
                                           }),
                           effectInstances.end());
}

void Project::syncDevice (const juce::ValueTree& track)
{
    if (! track.hasType (ids::TRACK))
        return;

    auto& t = getEngineTrack (track);
    const auto instrumentDevice = getDevice (track);
    const auto drums = getDeviceType (instrumentDevice) == "drumrack";

    t.setInstrumentType (! isMidiTrack (track) ? AudioTrack::InstrumentType::none
                         : drums               ? AudioTrack::InstrumentType::drumRack
                                               : AudioTrack::InstrumentType::synth);

    if (auto* instrument = t.getInstrument())
        for (int i = 0; i < (int) getDeviceParamInfos (instrumentDevice).size(); ++i)
            instrument->setParam (i, getDeviceParam (instrumentDevice, i));

    syncDrumKit (track);
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

// ---- automation -------------------------------------------------------------------

int Project::nextDeviceUid() const
{
    auto highest = 0;
    for (const auto& track : state)
        for (const auto& child : track)
            if (child.hasType (ids::DEVICE))
                highest = juce::jmax (highest, (int) child.getProperty (ids::uid, 0));

    return highest + 1;
}

juce::ValueTree Project::findDeviceByUid (const juce::ValueTree& track, int deviceUid)
{
    for (const auto& child : track)
        if (child.hasType (ids::DEVICE) && (int) child.getProperty (ids::uid, 0) == deviceUid)
            return child;

    return {};
}

std::vector<Project::AutomationTarget> Project::getAutomationTargets (const juce::ValueTree& track)
{
    std::vector<AutomationTarget> targets;
    if (! track.hasType (ids::TRACK))
        return targets;

    targets.push_back ({ "gain", "Volume", { "gain", "Volume", AudioTrack::minGainDb, AudioTrack::maxGainDb, 0.0f, -12.0f, false, "dB" } });
    targets.push_back ({ "pan", "Pan", { "pan", "Pan", -1.0f, 1.0f, 0.0f, 0.0f, false, "" } });

    for (const auto& device : track)
    {
        if (! device.hasType (ids::DEVICE))
            continue;

        const auto deviceName = getEffectDisplayName (getDeviceType (device));
        for (const auto& info : getDeviceParamInfos (device))
            if (info.id != nullptr)
                targets.push_back ({ device[ids::uid].toString() + ":" + info.id,
                                     deviceName + ": " + juce::String (info.name), info });
    }

    return targets;
}

bool Project::findAutomationTarget (const juce::ValueTree& track, const juce::String& targetId, AutomationTarget& result)
{
    for (auto& t : getAutomationTargets (track))
        if (t.id == targetId)
        {
            result = t;
            return true;
        }

    return false;
}

juce::ValueTree Project::getAutomationLane (const juce::ValueTree& track, const juce::String& targetId)
{
    for (const auto& child : track)
        if (isAutomationLane (child) && getAutomationTargetId (child) == targetId)
            return child;

    return {};
}

juce::ValueTree Project::getOrCreateAutomationLane (juce::ValueTree track, const juce::String& targetId)
{
    if (auto lane = getAutomationLane (track, targetId); lane.isValid())
        return lane;

    AutomationTarget t;
    if (! findAutomationTarget (track, targetId, t))
        return {};

    juce::ValueTree lane (ids::AUTOMATION);
    lane.setProperty (ids::target, targetId, nullptr);
    track.appendChild (lane, &undoManager);
    return lane;
}

void Project::removeAutomationLane (juce::ValueTree lane)
{
    auto track = lane.getParent();
    if (isAutomationLane (lane) && track.isValid())
        track.removeChild (lane, &undoManager);
}

namespace
{
    float clampToTarget (const Project::AutomationTarget& t, float value)
    {
        value = juce::jlimit (t.info.minValue, t.info.maxValue, value);
        return t.info.stepped ? std::round (value) : value;
    }
}

juce::ValueTree Project::addAutomationPoint (juce::ValueTree lane, double pointBeat, float pointValue)
{
    AutomationTarget t;
    if (! isAutomationLane (lane) || ! findAutomationTarget (lane.getParent(), getAutomationTargetId (lane), t))
        return {};

    juce::ValueTree point (ids::POINT);
    point.setProperty (ids::beat, juce::jmax (0.0, pointBeat), nullptr);
    point.setProperty (ids::value, (double) clampToTarget (t, pointValue), nullptr);
    lane.appendChild (point, &undoManager);
    return point;
}

void Project::moveAutomationPoint (juce::ValueTree point, double newBeat, float newValue)
{
    const auto lane = point.getParent();
    AutomationTarget t;
    if (! point.hasType (ids::POINT) || ! findAutomationTarget (lane.getParent(), getAutomationTargetId (lane), t))
        return;

    point.setProperty (ids::beat, juce::jmax (0.0, newBeat), &undoManager);
    point.setProperty (ids::value, (double) clampToTarget (t, newValue), &undoManager);
}

void Project::deleteAutomationPoint (juce::ValueTree point)
{
    auto lane = point.getParent();
    if (point.hasType (ids::POINT) && lane.isValid())
        lane.removeChild (point, &undoManager);
}

float Project::getAutomationValueAt (const juce::ValueTree& lane, double atBeat, float fallback)
{
    AutomationLane envelope;
    for (const auto& p : lane)
        if (p.hasType (ids::POINT))
            envelope.points.push_back ({ getPointBeat (p), getPointValue (p) });

    if (envelope.points.empty())
        return fallback;

    std::stable_sort (envelope.points.begin(), envelope.points.end(),
                      [] (const AutomationPoint& a, const AutomationPoint& b) { return a.beat < b.beat; });
    return envelope.valueAt (atBeat);
}

bool Project::isAutomated (const juce::ValueTree& track, const juce::String& targetId)
{
    const auto lane = getAutomationLane (track, targetId);
    return lane.isValid() && lane.getChildWithName (ids::POINT).isValid();
}

juce::String Project::getDeviceParamTargetId (const juce::ValueTree& device, int paramIndex)
{
    const auto& infos = getDeviceParamInfos (device);
    if (paramIndex < 0 || paramIndex >= (int) infos.size() || infos[(size_t) paramIndex].id == nullptr)
        return {};

    return device[ids::uid].toString() + ":" + infos[(size_t) paramIndex].id;
}

float Project::getEngineValue (const juce::ValueTree& track, const juce::String& targetId)
{
    if (indexOfTrack (track) < 0)
        return 0.0f;

    auto& engineTrack = getEngineTrack (track);
    if (targetId == "gain") return engineTrack.getGainDb();
    if (targetId == "pan")  return engineTrack.getPan();

    const auto device = findDeviceByUid (track, targetId.upToFirstOccurrenceOf (":", false, false).getIntValue());
    const auto paramId = targetId.fromFirstOccurrenceOf (":", false, false);
    const auto& infos = getDeviceParamInfos (device);

    for (int i = 0; i < (int) infos.size(); ++i)
    {
        if (paramId != infos[(size_t) i].id)
            continue;

        if (! isEffect (device))
            return engineTrack.getInstrument() != nullptr ? engineTrack.getInstrument()->getParam (i) : 0.0f;

        if (const auto instance = getEffectInstance (device))
            return instance->getParam (i);
    }

    return getTargetModelValue (track, targetId);
}

float Project::getTargetModelValue (const juce::ValueTree& track, const juce::String& targetId)
{
    if (targetId == "gain") return getTrackGain (track);
    if (targetId == "pan")  return getTrackPan (track);

    const auto device = findDeviceByUid (track, targetId.upToFirstOccurrenceOf (":", false, false).getIntValue());
    const auto paramId = targetId.fromFirstOccurrenceOf (":", false, false);
    const auto& infos = getDeviceParamInfos (device);

    for (int i = 0; i < (int) infos.size(); ++i)
        if (paramId == infos[(size_t) i].id)
            return getDeviceParam (device, i);

    return 0.0f;
}

void Project::syncAutomation (const juce::ValueTree& track)
{
    if (! track.hasType (ids::TRACK) || indexOfTrack (track) < 0)
        return;

    auto& engineTrack = getEngineTrack (track);
    auto list = std::make_unique<AutomationList>();
    juce::StringArray automated;

    for (const auto& lane : track)
    {
        if (! isAutomationLane (lane) || lane.getNumChildren() == 0)
            continue;

        const auto targetId = getAutomationTargetId (lane);
        AutomationLane envelope;

        if (targetId == "gain")
            envelope.target = AutomationLane::Target::trackGain;
        else if (targetId == "pan")
            envelope.target = AutomationLane::Target::trackPan;
        else
        {
            const auto device = findDeviceByUid (track, targetId.upToFirstOccurrenceOf (":", false, false).getIntValue());
            const auto paramId = targetId.fromFirstOccurrenceOf (":", false, false);
            const auto& infos = getDeviceParamInfos (device);
            envelope.paramIndex = -1;

            for (int i = 0; i < (int) infos.size(); ++i)
                if (paramId == infos[(size_t) i].id)
                    envelope.paramIndex = i;

            if (envelope.paramIndex < 0)
                continue; // the device or parameter no longer exists

            if (isEffect (device))
            {
                envelope.target = AutomationLane::Target::effectParam;
                envelope.effect = getEffectInstance (device);
                if (envelope.effect == nullptr)
                    continue;
            }
            else
                envelope.target = AutomationLane::Target::synthParam;
        }

        for (const auto& p : lane)
            if (p.hasType (ids::POINT))
                envelope.points.push_back ({ getPointBeat (p), getPointValue (p) });

        std::stable_sort (envelope.points.begin(), envelope.points.end(),
                          [] (const AutomationPoint& a, const AutomationPoint& b) { return a.beat < b.beat; });

        automated.add (targetId);
        list->lanes.push_back (std::move (envelope));
    }

    // Parameters that are no longer automated go back to their model values.
    if (! automated.contains ("gain")) engineTrack.setGainDb (getTrackGain (track));
    if (! automated.contains ("pan"))  engineTrack.setPan (getTrackPan (track));

    for (const auto& device : track)
    {
        if (! device.hasType (ids::DEVICE))
            continue;

        const auto& infos = getDeviceParamInfos (device);
        const auto instance = isEffect (device) ? getEffectInstance (device) : nullptr;

        for (int i = 0; i < (int) infos.size(); ++i)
        {
            if (automated.contains (device[ids::uid].toString() + ":" + infos[(size_t) i].id))
                continue;

            if (instance != nullptr)
                instance->setParam (i, getDeviceParam (device, i));
            else if (! isEffect (device))
            {
                if (auto* instrument = engineTrack.getInstrument())
                    instrument->setParam (i, getDeviceParam (device, i));
            }
        }
    }

    engineTrack.setAutomation (std::move (list));
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
    {
        syncTrack (t);
        syncDrumKit (t);
    }
}

void Project::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    unsavedChanges = true;

    if (tree.hasType (ids::CLIP))
        syncTrack (tree.getParent());
    else if (tree.hasType (ids::NOTE))
        syncTrack (tree.getParent().getParent());
    else if (tree.hasType (ids::POINT))
        syncAutomation (tree.getParent().getParent());
    else if (tree.hasType (ids::AUTOMATION))
        syncAutomation (tree.getParent());
    else if (tree.hasType (ids::DEVICE))
    {
        const auto track = tree.getParent();
        if (! track.hasType (ids::TRACK) && ! isMaster (track))
            return;

        const auto& infos = getDeviceParamInfos (tree);
        const auto instance = isEffect (tree) ? getEffectInstance (tree) : nullptr;

        if (property == ids::enabled && instance != nullptr)
            instance->setEnabled (isEffectEnabled (tree));

        if (property.toString().startsWith ("pad") && property.toString().endsWith ("File"))
            syncDrumKit (track);

        for (int i = 0; i < (int) infos.size(); ++i)
        {
            if (infos[(size_t) i].id == nullptr || property != juce::Identifier (infos[(size_t) i].id))
                continue;

            if (instance != nullptr)
                instance->setParam (i, getDeviceParam (tree, i));
            else if (! isEffect (tree))
            {
                if (auto* instrument = getEngineTrack (track).getInstrument())
                    instrument->setParam (i, getDeviceParam (tree, i));
            }
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
        else if (child.hasType (ids::AUTOMATION))
            syncAutomation (parent);
        else
            syncTrack (parent);
    }
    else if (parent.hasType (ids::AUTOMATION))
        syncAutomation (parent.getParent());
    else if (parent.hasType (ids::CLIP))
        syncTrack (parent.getParent());
    else if (isMaster (parent))
        syncEffects (parent);
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

    if ((parent.hasType (ids::TRACK) || isMaster (parent)) && child.hasType (ids::DEVICE))
        syncEffects (parent);
    else if (parent.hasType (ids::TRACK) && child.hasType (ids::AUTOMATION))
        syncAutomation (parent);
    else if (parent.hasType (ids::AUTOMATION))
        syncAutomation (parent.getParent());
    else
        syncTrack (parent.hasType (ids::CLIP) ? parent.getParent() : parent);
}

void Project::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    unsavedChanges = true;

    if (parent.hasType (ids::TRACK) || isMaster (parent))
        syncEffects (parent);
}

} // namespace daw
