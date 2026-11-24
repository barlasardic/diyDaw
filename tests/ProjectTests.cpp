#include "Project.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

/** Test-thread fixture: edits and offline audio callbacks run sequentially, with no file I/O. */
struct ProjectFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    daw::Project project { engine, pool };
    const juce::File kick { "/virtual/kick.wav" };
    const juce::File hit { "/virtual/hit.wav" };
    const juce::File longSample { "/virtual/long.wav" };

    ProjectFixture()
    {
        engine.prepare (sampleRate, blockSize);
        engine.getTransport().setBpm (120.0);
        engine.getMetronome().setEnabled (false);
        insert (kick, 2.0, 0.5f, "kick");       // four beats
        insert (hit, 0.5, -0.25f, "hit");       // one beat
        insert (longSample, 4.0, 0.75f, "long"); // eight beats
    }

    void insert (const juce::File& file, double seconds, float amplitude, const juce::String& name)
    {
        auto sample = std::make_shared<daw::SampleData>();
        sample->sampleRate = sampleRate;
        sample->name = name;
        sample->buffer.setSize (1, static_cast<int> (std::llround (seconds * sampleRate)));
        for (int i = 0; i < sample->getNumFrames(); ++i)
            sample->buffer.setSample (0, i, amplitude);
        pool.insert (daw::SamplePool::keyFor (file), sample);
    }

    juce::AudioBuffer<float> render (double beats = 10.0)
    {
        engine.getTransport().setPositionBeats (0.0);
        engine.getTransport().play();
        return daw::renderOffline (engine, beats * 0.5, blockSize);
    }
};

juce::ValueTree clipAt (const juce::ValueTree& track, double beat)
{
    for (const auto& clip : track)
        if (std::abs (daw::Project::getClipStart (clip) - beat) < 1.0e-9)
            return clip;
    return {};
}
}

/** Exercises public model edits, undo snapshots and the resulting immutable engine clip lists. */
struct ProjectTests : juce::UnitTest
{
    ProjectTests() : juce::UnitTest ("Project", "diyDaw") {}

    void expectClip (const juce::ValueTree& clip, double start, double length, double offset)
    {
        expect (clip.hasType (daw::ids::CLIP), "Expected an attached CLIP");
        if (! clip.isValid())
            return;
        expectWithinAbsoluteError (daw::Project::getClipStart (clip), start, 1.0e-9, "Clip start");
        expectWithinAbsoluteError (daw::Project::getClipLength (clip), length, 1.0e-9, "Clip length");
        expectWithinAbsoluteError (daw::Project::getClipOffset (clip), offset, 1.0e-9, "Source offset in seconds");
    }

    void expectState (daw::Project& project, const juce::ValueTree& expected, const juce::String& context)
    {
        expect (project.getState().isEquivalentTo (expected), context);
    }

    // Check every frame in a region away from clip edges and mixer gain ramps.
    void expectAudio (const juce::AudioBuffer<float>& audio, int channel, double startBeat,
                      double endBeat, float expected)
    {
        const auto start = static_cast<int> (std::llround (startBeat * 24000.0));
        const auto end = static_cast<int> (std::llround (endBeat * 24000.0));
        float error = 0.0f;
        for (int i = start; i < end; ++i)
        {
            const auto value = audio.getSample (channel, i);
            if (! std::isfinite (value))
            {
                expect (false, "Non-finite output at frame " + juce::String (i));
                return;
            }
            error = juce::jmax (error, std::abs (value - expected));
        }
        expectWithinAbsoluteError (error, 0.0f, 1.0e-6f,
                                   "Channel " + juce::String (channel) + ", beats " + juce::String (startBeat)
                                       + " to " + juce::String (endBeat));
    }

    void runTest() override
    {
        runTrackAndMoveTests();
        runOverwriteTests();
        runTrimAndSplitTests();
        runUndoTests();
        runEngineSyncTests();
    }

    void runTrackAndMoveTests()
    {
        beginTest ("Tracks receive distinct engine slots, default/custom names and rename sync");
        {
            ProjectFixture f;
            const auto first = f.project.addTrack();
            const auto second = f.project.addTrack ("Drums");
            expect (first.hasType (daw::ids::TRACK));
            expect (second.hasType (daw::ids::TRACK));
            expectEquals (f.project.getNumTracks(), 2);
            expectEquals (f.engine.getNumTracks(), 2);
            expectEquals (static_cast<int> (first[daw::ids::engineIndex]), 0);
            expectEquals (static_cast<int> (second[daw::ids::engineIndex]), 1);
            expectEquals (first[daw::ids::name].toString(), juce::String ("1 Audio"));
            expectEquals (second[daw::ids::name].toString(), juce::String ("Drums"));
            expectEquals (f.engine.getTrack (0).getName(), juce::String ("1 Audio"));
            expectEquals (f.engine.getTrack (1).getName(), juce::String ("Drums"));
            expect (&f.project.getEngineTrack (second) == &f.engine.getTrack (1));
            expect (f.project.getTrack (1) == second);
            expectEquals (f.project.indexOfTrack (second), 1);
            f.project.setTrackName (second, "Percussion");
            expectEquals (second[daw::ids::name].toString(), juce::String ("Percussion"));
            expectEquals (f.engine.getTrack (1).getName(), juce::String ("Percussion"));
            for (int i = 2; i < daw::Engine::maxTracks; ++i)
                expect (f.project.addTrack().isValid());
            expect (! f.project.addTrack().isValid(), "Full engine pool rejects another track");
            expectEquals (f.project.getNumTracks(), daw::Engine::maxTracks);
        }

        beginTest ("Adding cached samples uses seconds * BPM / 60; unloaded files do not edit the tree");
        for (const auto bpm : { 90.0, 120.0, 150.0 })
        {
            ProjectFixture f;
            f.engine.getTransport().setBpm (bpm);
            const auto track = f.project.addTrack();
            const auto clip = f.project.addClip (track, f.kick, 3.0);
            expectClip (clip, 3.0, 2.0 * bpm / 60.0, 0.0);
            expect (clip.getParent() == track);
            expect (f.project.getClipSample (clip) == f.pool.get (f.kick));
            expectEquals (daw::Project::getClipFile (clip).getFullPathName(), daw::SamplePool::keyFor (f.kick));
            expectEquals (daw::Project::getClipName (clip), juce::String ("kick"));
            expect (! daw::Project::isClipLooping (clip));
            const auto before = f.project.getState().createCopy();
            expect (! f.project.addClip (track, juce::File ("/virtual/unloaded.wav"), 3.0).isValid());
            expectState (f.project, before, "Unloaded sample must not overwrite an existing clip");
        }

        beginTest ("Move preserves clip length/offset and reparents the same ValueTree across tracks");
        {
            ProjectFixture f;
            const auto first = f.project.addTrack();
            const auto second = f.project.addTrack();
            const auto clip = f.project.addClip (first, f.kick, 2.0);
            f.project.trimClipStart (clip, 3.0);
            f.project.moveClip (clip, first, 7.0);
            expectClip (clip, 7.0, 3.0, 0.5);
            expect (clip.getParent() == first);
            expectEquals (first.getNumChildren(), 1);
            f.project.moveClip (clip, second, 1.0);
            expectClip (clip, 1.0, 3.0, 0.5);
            expect (clip.getParent() == second);
            expectEquals (first.getNumChildren(), 0);
            expectEquals (second.getNumChildren(), 1);
            expect (second.getChild (0) == clip, "Move retains the clip's identity");
        }
    }

    void runOverwriteTests()
    {
        beginTest ("Landing fully over a clip removes it");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto victim = f.project.addClip (track, f.kick, 4.0);
            const auto landing = f.project.addClip (track, f.longSample, 2.0);
            expect (! victim.getParent().isValid());
            expectEquals (track.getNumChildren(), 1);
            expectClip (landing, 2.0, 8.0, 0.0);
        }

        beginTest ("Landing in the middle splits the old clip, retaining both source offsets");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto old = f.project.addClip (track, f.longSample, 2.0);
            f.project.trimClipStart (old, 3.0); // [3, 10), source starts at 0.5 seconds
            const auto landing = f.project.addClip (track, f.hit, 5.0);
            expectEquals (track.getNumChildren(), 3);
            expectClip (old, 3.0, 2.0, 0.5);
            expectClip (landing, 5.0, 1.0, 0.0);
            const auto right = clipAt (track, 6.0);
            expectClip (right, 6.0, 4.0, 2.0);
            expect (f.project.getClipSample (right) == f.pool.get (f.longSample));
        }

        beginTest ("Overlap at the head advances the old offset by the trimmed seconds");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto old = f.project.addClip (track, f.kick, 4.0);
            f.project.trimClipStart (old, 5.0); // old offset = 0.5 seconds
            f.project.addClip (track, f.kick, 3.0); // [3, 7) trims two beats from old's head
            expectEquals (track.getNumChildren(), 2);
            expectClip (old, 7.0, 1.0, 1.5);
        }

        beginTest ("Overlap at the tail preserves start/offset; touching boundaries do not overlap");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto old = f.project.addClip (track, f.kick, 4.0);
            f.project.trimClipStart (old, 5.0);
            f.project.addClip (track, f.hit, 7.5);
            expectClip (old, 5.0, 2.5, 0.5);
            f.project.addClip (track, f.hit, 4.0); // ends exactly at old's start
            expectClip (old, 5.0, 2.5, 0.5);
            expectEquals (track.getNumChildren(), 3);
        }

        beginTest ("Moving across tracks applies overwrite rules on the destination track");
        {
            ProjectFixture f;
            const auto from = f.project.addTrack();
            const auto to = f.project.addTrack();
            const auto victim = f.project.addClip (to, f.hit, 4.0);
            const auto moving = f.project.addClip (from, f.kick, 0.0);
            f.project.moveClip (moving, to, 3.0);
            expect (! victim.getParent().isValid());
            expectEquals (from.getNumChildren(), 0);
            expectEquals (to.getNumChildren(), 1);
            expectClip (moving, 3.0, 4.0, 0.0);
        }
    }

    void runTrimAndSplitTests()
    {
        beginTest ("Left trim fixes the end, shifts offset and clamps at sample start/minimum length");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto clip = f.project.addClip (track, f.kick, 4.0);
            f.project.trimClipStart (clip, 6.0);
            expectClip (clip, 6.0, 2.0, 1.0);
            expectWithinAbsoluteError (daw::Project::getClipEnd (clip), 8.0, 1.0e-9);
            f.project.trimClipStart (clip, -10.0);
            expectClip (clip, 4.0, 4.0, 0.0);
            f.project.trimClipStart (clip, 20.0);
            const auto minimum = daw::Project::minClipLengthBeats;
            expectClip (clip, 8.0 - minimum, minimum, 2.0 - minimum * 0.5);
        }

        beginTest ("Right trim obeys remaining sample duration, looping extension and minimum length");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto clip = f.project.addClip (track, f.kick, 4.0);
            f.project.trimClipStart (clip, 5.0);
            f.project.trimClipEnd (clip, 6.0);
            expectClip (clip, 5.0, 1.0, 0.5);
            f.project.trimClipEnd (clip, 100.0);
            expectClip (clip, 5.0, 3.0, 0.5);
            f.project.setClipLooping (clip, true);
            f.project.trimClipEnd (clip, 12.0);
            expect (daw::Project::isClipLooping (clip));
            expectClip (clip, 5.0, 7.0, 0.5);
            f.project.setClipLooping (clip, false);
            expect (! daw::Project::isClipLooping (clip));
            expectClip (clip, 5.0, 3.0, 0.5);
            f.project.trimClipEnd (clip, 0.0);
            expectClip (clip, 5.0, daw::Project::minClipLengthBeats, 0.5);
        }

        beginTest ("Extending a looping left edge wraps the source offset and overwrites its neighbour");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto neighbour = f.project.addClip (track, f.hit, 2.0);
            const auto clip = f.project.addClip (track, f.kick, 4.0);
            f.project.setClipLooping (clip, true);
            f.project.trimClipStart (clip, 1.0);
            expectClip (clip, 1.0, 7.0, 0.5); // -1.5 seconds wraps around a 2 second sample
            expect (! neighbour.getParent().isValid());
        }

        beginTest ("Right extension overwrites neighbouring clips");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto clip = f.project.addClip (track, f.kick, 0.0);
            f.project.trimClipEnd (clip, 2.0);
            const auto neighbour = f.project.addClip (track, f.hit, 3.0);
            f.project.trimClipEnd (clip, 4.0);
            expectClip (clip, 0.0, 4.0, 0.0);
            expect (! neighbour.getParent().isValid());
        }

        beginTest ("Split preserves both halves, duration, file and accumulated offset; outside splits do nothing");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto left = f.project.addClip (track, f.kick, 4.0);
            f.project.trimClipStart (left, 5.0);
            const auto before = f.project.getState().createCopy();
            for (const auto position : { 4.0, 5.0, 8.0, 9.0 })
                expect (! f.project.splitClip (left, position).isValid());
            expectState (f.project, before, "Boundary/outside splits leave the complete tree unchanged");
            const auto right = f.project.splitClip (left, 6.0);
            expectEquals (track.getNumChildren(), 2);
            expectClip (left, 5.0, 1.0, 0.5);
            expectClip (right, 6.0, 2.0, 1.0);
            expect (right.getParent() == track);
            expect (right != left);
            expectEquals (daw::Project::getClipFile (right).getFullPathName(), f.kick.getFullPathName());
        }

        beginTest ("Duplicate starts at the original end, keeps offset/loop and overwrites its destination");
        {
            ProjectFixture f;
            const auto track = f.project.addTrack();
            const auto source = f.project.addClip (track, f.kick, 4.0);
            f.project.trimClipStart (source, 5.0);
            f.project.setClipLooping (source, true);
            const auto victim = f.project.addClip (track, f.hit, 8.0);
            const auto copy = f.project.duplicateClip (source);
            expectClip (source, 5.0, 3.0, 0.5);
            expectClip (copy, 8.0, 3.0, 0.5);
            expect (daw::Project::isClipLooping (copy));
            expect (copy != source);
            expect (! victim.getParent().isValid());
            expectEquals (track.getNumChildren(), 2);
            expect (f.project.getClipSample (copy) == f.project.getClipSample (source));
        }
    }

    void runUndoTests()
    {
        beginTest ("Undo restores every full ValueTree snapshot and redo reapplies a sequence of transactions");
        ProjectFixture f;
        const auto first = f.project.addTrack();
        const auto second = f.project.addTrack ("Destination");
        std::vector<juce::ValueTree> states { f.project.getState().createCopy() };
        const auto save = [&] { states.push_back (f.project.getState().createCopy()); };

        f.project.beginTransaction ("Add long clip");
        const auto original = f.project.addClip (first, f.longSample, 1.0);
        save();
        f.project.beginTransaction ("Overwrite middle");
        const auto landing = f.project.addClip (first, f.hit, 4.0);
        save();
        f.project.beginTransaction ("Move across tracks");
        f.project.moveClip (landing, second, 2.0);
        save();
        f.project.beginTransaction ("Trim start");
        f.project.trimClipStart (original, 2.0);
        save();
        f.project.beginTransaction ("Split");
        const auto right = f.project.splitClip (original, 3.0);
        expect (right.isValid());
        save();
        f.project.beginTransaction ("Duplicate");
        const auto copy = f.project.duplicateClip (landing);
        expect (copy.isValid());
        save();
        f.project.beginTransaction ("Loop and extend");
        f.project.setClipLooping (copy, true);
        f.project.trimClipEnd (copy, 8.0);
        save();
        f.project.beginTransaction ("Delete");
        f.project.deleteClip (right);
        save();
        f.project.beginTransaction ("Rename");
        f.project.setTrackName (second, "Edited");
        save();

        for (int i = static_cast<int> (states.size()) - 2; i >= 0; --i)
        {
            expect (f.project.undo(), "Undo transaction " + juce::String (i + 1));
            expectState (f.project, states[static_cast<size_t> (i)], "Undo snapshot " + juce::String (i));
        }
        expect (! f.project.undo(), "Track creation is outside undo history");
        for (size_t i = 1; i < states.size(); ++i)
        {
            expect (f.project.redo(), "Redo transaction " + juce::String (static_cast<int> (i)));
            expectState (f.project, states[i], "Redo snapshot " + juce::String (static_cast<int> (i)));
        }
        expect (! f.project.redo(), "Redo history exhausted");
    }

    void runEngineSyncTests()
    {
        beginTest ("Offline engine follows additions, moves, deletes and undo across both tracks");
        ProjectFixture f;
        const auto left = f.project.addTrack();
        const auto right = f.project.addTrack();
        f.project.getEngineTrack (left).setPan (-1.0f);
        f.project.getEngineTrack (right).setPan (1.0f);
        f.project.beginTransaction ("Add");
        const auto clip = f.project.addClip (left, f.kick, 1.0); // [1, 5)
        auto audio = f.render();
        expectAudio (audio, 0, 0.0, 1.0, 0.0f);
        expectAudio (audio, 0, 1.01, 4.99, 0.5f);
        expectAudio (audio, 0, 5.0, 10.0, 0.0f);
        expectAudio (audio, 1, 0.0, 10.0, 0.0f);

        f.project.beginTransaction ("Move on same track");
        f.project.moveClip (clip, left, 3.0); // [3, 7)
        audio = f.render();
        expectAudio (audio, 0, 0.0, 3.0, 0.0f);
        expectAudio (audio, 0, 3.01, 6.99, 0.5f);
        expectAudio (audio, 0, 7.0, 10.0, 0.0f);

        f.project.beginTransaction ("Move across tracks");
        f.project.moveClip (clip, right, 5.0); // [5, 9)
        audio = f.render();
        expectAudio (audio, 0, 0.0, 10.0, 0.0f);
        expectAudio (audio, 1, 0.0, 5.0, 0.0f);
        expectAudio (audio, 1, 5.01, 8.99, 0.5f);
        expectAudio (audio, 1, 9.0, 10.0, 0.0f);

        f.project.beginTransaction ("Delete");
        f.project.deleteClip (clip);
        audio = f.render();
        expectAudio (audio, 0, 0.0, 10.0, 0.0f);
        expectAudio (audio, 1, 0.0, 10.0, 0.0f);
        expect (f.project.undo());
        audio = f.render();
        expectAudio (audio, 0, 0.0, 10.0, 0.0f);
        expectAudio (audio, 1, 0.0, 5.0, 0.0f);
        expectAudio (audio, 1, 5.01, 8.99, 0.5f);
        expectAudio (audio, 1, 9.0, 10.0, 0.0f);
        expect (f.project.undo());
        audio = f.render();
        expectAudio (audio, 0, 0.0, 3.0, 0.0f);
        expectAudio (audio, 0, 3.01, 6.99, 0.5f);
        expectAudio (audio, 0, 7.0, 10.0, 0.0f);
        expectAudio (audio, 1, 0.0, 10.0, 0.0f);

        beginTest ("Engine snapshots sort clips by timeline position after overwrite and undo");
        ProjectFixture overwrite;
        const auto track = overwrite.project.addTrack();
        overwrite.project.getEngineTrack (track).setPan (-1.0f);
        overwrite.project.beginTransaction ("Long clip");
        overwrite.project.addClip (track, overwrite.longSample, 1.0); // [1, 9)
        overwrite.project.beginTransaction ("Overwrite middle");
        overwrite.project.addClip (track, overwrite.hit, 4.0); // [4, 5), appended after right remnant
        audio = overwrite.render();
        expectAudio (audio, 0, 0.0, 1.0, 0.0f);
        expectAudio (audio, 0, 1.01, 3.99, 0.75f);
        expectAudio (audio, 0, 4.01, 4.99, -0.25f);
        expectAudio (audio, 0, 5.01, 8.99, 0.75f);
        expectAudio (audio, 0, 9.0, 10.0, 0.0f);
        expect (overwrite.project.undo());
        audio = overwrite.render();
        expectAudio (audio, 0, 1.01, 8.99, 0.75f);
        expect (overwrite.project.redo());
        audio = overwrite.render();
        expectAudio (audio, 0, 4.01, 4.99, -0.25f);
        expectAudio (audio, 0, 5.01, 8.99, 0.75f);
    }
};

static ProjectTests projectTests;
