#include "Project.h"

#include <algorithm>
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

// ---- tracks -----------------------------------------------------------------

juce::ValueTree Project::addTrack (const juce::String& name)
{
    const auto index = engine.addTrack();
    if (index < 0)
        return {};

    const auto number = getNumTracks() + 1;
    juce::ValueTree track (ids::TRACK);
    track.setProperty (ids::name, name.isNotEmpty() ? name : juce::String (number) + " Audio", nullptr);
    track.setProperty (ids::colour, trackPalette[(size_t) (number - 1) % std::size (trackPalette)].toString(), nullptr);
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

void Project::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree.hasType (ids::CLIP))
        syncTrack (tree.getParent());
    else if (tree.hasType (ids::TRACK))
        syncTrack (tree);
}

void Project::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    syncTrack (parent.hasType (ids::TRACK) ? parent : child);
}

void Project::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    syncTrack (parent);
}

} // namespace daw
