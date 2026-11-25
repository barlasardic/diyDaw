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
            setDouble (tree, ids::offset, 0.0);
            setBool (tree, ids::loop);
            tree.setProperty (ids::start, juce::jmax (0.0, (double) tree[ids::start]), nullptr);
            tree.setProperty (ids::length, juce::jmax (Project::minClipLengthBeats, (double) tree[ids::length]), nullptr);
        }

        for (auto child : tree)
            normaliseTypes (child);
    }
}

juce::ValueTree Project::makeTrack (int number)
{
    juce::ValueTree track (ids::TRACK);
    track.setProperty (ids::name, juce::String (number) + " Audio", nullptr);
    track.setProperty (ids::colour, trackPalette[(size_t) (number - 1) % std::size (trackPalette)].toString(), nullptr);
    track.setProperty (ids::gain, 0.0, nullptr);
    track.setProperty (ids::pan, 0.0, nullptr);
    track.setProperty (ids::mute, false, nullptr);
    track.setProperty (ids::solo, false, nullptr);
    return track;
}

juce::String Project::replaceContents (const juce::ValueTree& newState)
{
    undoManager.clearUndoHistory();
    state.removeAllChildren (nullptr);
    engine.resetTracks();

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

        // Drop anything that isn't a clip, so the rest of the code can trust the structure.
        for (int i = track.getNumChildren(); --i >= 0;)
            if (! track.getChild (i).hasType (ids::CLIP))
                track.removeChild (i, nullptr);

        state.appendChild (track, nullptr);
    }

    undoManager.clearUndoHistory();
    unsavedChanges = false;
    return warning;
}

void Project::newDocument (int numTracks)
{
    juce::ValueTree fresh (ids::PROJECT);
    for (int i = 0; i < juce::jmin (numTracks, Engine::maxTracks); ++i)
        fresh.appendChild (makeTrack (i + 1), nullptr);

    replaceContents (fresh);
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

juce::ValueTree Project::addTrack (const juce::String& name)
{
    const auto index = engine.addTrack();
    if (index < 0)
        return {};

    auto track = makeTrack (getNumTracks() + 1);
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
    if (sample == nullptr || ! track.hasType (ids::TRACK))
        return {};

    startBeat = juce::jmax (0.0, startBeat);
    const auto length = juce::jmax (minClipLengthBeats, secondsToBeats (sample->getLengthSeconds()));

    clearRegion (track, startBeat, startBeat + length, {});

    juce::ValueTree clip (ids::CLIP);
    clip.setProperty (ids::name, sample->name, nullptr);
    clip.setProperty (ids::file, SamplePool::keyFor (file), nullptr);
    clip.setProperty (ids::start, startBeat, nullptr);
    clip.setProperty (ids::length, length, nullptr);
    clip.setProperty (ids::offset, 0.0, nullptr);
    clip.setProperty (ids::loop, false, nullptr);
    track.appendChild (clip, &undoManager);
    return clip;
}

void Project::moveClip (juce::ValueTree clip, juce::ValueTree toTrack, double newStartBeat)
{
    if (! clip.hasType (ids::CLIP) || ! toTrack.hasType (ids::TRACK))
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

void Project::setStartKeepingEnd (juce::ValueTree clip, double newStart)
{
    const auto oldStart = getClipStart (clip);
    const auto end = getClipEnd (clip);
    const auto newOffset = normaliseOffset (clip, getClipOffset (clip) + beatsToSeconds (newStart - oldStart));

    clip.setProperty (ids::offset, newOffset, &undoManager);
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
    if (! isClipLooping (clip)) // can't reveal audio from before the sample's first frame
        earliest = juce::jmax (0.0, start - secondsToBeats (getClipOffset (clip)));

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

    if (! isClipLooping (clip))
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
        if (c != except)
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

// ---- engine sync ------------------------------------------------------------

void Project::syncTrack (const juce::ValueTree& track)
{
    if (! track.hasType (ids::TRACK))
        return;

    auto list = std::make_unique<ClipList>();

    for (const auto& c : track)
    {
        Clip clip;
        clip.sample = getClipSample (c);
        if (clip.sample == nullptr)
            continue; // not loaded yet; syncAllTracks() picks it up later

        clip.startBeat = getClipStart (c);
        clip.lengthBeats = getClipLength (c);
        clip.offsetSeconds = getClipOffset (c);
        clip.looping = isClipLooping (c);
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
        syncTrack (parent);
    else if (child.hasType (ids::TRACK))
    {
        syncTrack (child);
        syncTrackMixer (child);
    }
}

void Project::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    unsavedChanges = true;
    syncTrack (parent);
}

} // namespace daw
