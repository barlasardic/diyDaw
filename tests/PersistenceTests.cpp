#include "Project.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

/** Test-thread fixture: model edits and offline callbacks run sequentially. */
struct PersistenceFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    daw::Project project { engine, pool };
    const juce::File firstSample { "/virtual/persistence-first.wav" };
    const juce::File secondSample { "/virtual/persistence-second.wav" };

    PersistenceFixture()
    {
        engine.prepare (sampleRate, blockSize);
        engine.getMetronome().setEnabled (false);
        insert (firstSample, 2.0, 0.4f);
        insert (secondSample, 1.0, -0.3f);
    }

    void insert (const juce::File& file, double seconds, float amplitude)
    {
        auto sample = std::make_shared<daw::SampleData>();
        sample->sampleRate = sampleRate;
        sample->name = file.getFileNameWithoutExtension();
        sample->buffer.setSize (2, static_cast<int> (std::llround (seconds * sampleRate)));
        for (int i = 0; i < sample->getNumFrames(); ++i)
        {
            // A varying stereo signal makes source offsets and channel routing observable.
            const auto value = amplitude * static_cast<float> (i % 257) / 256.0f;
            sample->buffer.setSample (0, i, value);
            sample->buffer.setSample (1, i, -0.5f * value);
        }
        pool.insert (daw::SamplePool::keyFor (file), sample);
    }

    juce::AudioBuffer<float> render (double seconds = 6.0)
    {
        engine.getTransport().setPositionBeats (0.0);
        engine.getTransport().play();
        return daw::renderOffline (engine, seconds, blockSize);
    }
};

/** Owns on-disk test artifacts; destroyed after fixtures and their decoder jobs. */
struct TemporaryDirectory
{
    juce::File directory = juce::File::createTempFile ("-persistence");

    ~TemporaryDirectory() { directory.deleteRecursively(); }

    juce::File file (const juce::String& name) const { return directory.getChildFile (name); }
};
}

/** Exercises persistence and model-to-engine synchronization on the test thread only. */
struct PersistenceTests : juce::UnitTest
{
    PersistenceTests() : juce::UnitTest ("Project persistence", "diyDaw") {}

    void runTest() override
    {
        runMixerTests();
        runProjectParameterTests();
        runRoundTripTests();
        runDocumentLifecycleTests();
        runReplacementTests();
        runRelativePathTests();
        runErrorTests();
        runTrackLimitTests();
        runTypeAndDefaultTests();
    }

    bool expectSuccess (const juce::Result& result)
    {
        expect (result.wasOk(), result.getErrorMessage());
        return result.wasOk();
    }

    void expectMixer (PersistenceFixture& f, const juce::ValueTree& track,
                      float gain, float pan, bool mute, bool solo)
    {
        expectEquals (daw::Project::getTrackGain (track), gain, "Model gain");
        expectEquals (daw::Project::getTrackPan (track), pan, "Model pan");
        expect (static_cast<bool> (track[daw::ids::mute]) == mute, "Model mute");
        expect (static_cast<bool> (track[daw::ids::solo]) == solo, "Model solo");
        auto& audio = f.project.getEngineTrack (track);
        expectEquals (audio.getGainDb(), gain, "Engine gain");
        expectEquals (audio.getPan(), pan, "Engine pan");
        expect (audio.isMuted() == mute, "Engine mute");
        expect (audio.isSoloed() == solo, "Engine solo");
    }

    void expectProjectParameters (PersistenceFixture& f, double bpm, float master)
    {
        expectEquals (f.project.getBpm(), bpm, "Model tempo");
        expectEquals (f.engine.getTransport().getBpm(), bpm, "Transport tempo");
        expectEquals (f.project.getMasterGain(), master, "Model master gain");
        expectEquals (f.engine.getMasterGainDb(), master, "Engine master gain");
    }

    void expectTree (const juce::ValueTree& actual, const juce::ValueTree& expected,
                     const juce::String& path = "PROJECT")
    {
        expect (actual.getType() == expected.getType(), path + ": node type");
        expectEquals (actual.getNumProperties(), expected.getNumProperties(), path + ": property count");
        for (int i = 0; i < expected.getNumProperties(); ++i)
        {
            const auto id = expected.getPropertyName (i);
            const auto value = expected[id];
            const auto context = path + "/" + id.toString();
            expect (actual.hasProperty (id), context + ": present");
            if (value.isDouble())
            {
                const auto a = static_cast<double> (actual[id]);
                const auto e = static_cast<double> (value);
                expect (juce::exactlyEqual (a, e), context + ": expected " + juce::String (e, 17)
                                                       + ", actual " + juce::String (a, 17));
            }
            else if (value.isBool())
                expect (static_cast<bool> (actual[id]) == static_cast<bool> (value), context);
            else if (value.isInt())
                expectEquals (static_cast<int> (actual[id]), static_cast<int> (value), context);
            else
                expectEquals (actual[id].toString(), value.toString(), context);
        }
        expectEquals (actual.getNumChildren(), expected.getNumChildren(), path + ": child count");
        for (int i = 0; i < juce::jmin (actual.getNumChildren(), expected.getNumChildren()); ++i)
            expectTree (actual.getChild (i), expected.getChild (i), path + "/" + juce::String (i));
    }

    void expectAudioEqual (const juce::AudioBuffer<float>& actual,
                           const juce::AudioBuffer<float>& expected)
    {
        expectEquals (actual.getNumChannels(), expected.getNumChannels());
        expectEquals (actual.getNumSamples(), expected.getNumSamples());
        for (int ch = 0; ch < juce::jmin (actual.getNumChannels(), expected.getNumChannels()); ++ch)
        {
            for (int i = 0; i < juce::jmin (actual.getNumSamples(), expected.getNumSamples()); ++i)
            {
                const auto a = actual.getSample (ch, i);
                const auto b = expected.getSample (ch, i);
                if (! std::isfinite (a) || ! std::isfinite (b) || ! juce::exactlyEqual (a, b))
                {
                    expect (false, "Audio differs at channel " + juce::String (ch) + ", frame " + juce::String (i));
                    return;
                }
            }
        }
        expect (true, "Every audio sample matches exactly");
    }

    void expectSilent (const juce::AudioBuffer<float>& audio)
    {
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < audio.getNumSamples(); ++i)
                if (! juce::exactlyEqual (audio.getSample (ch, i), 0.0f))
                {
                    expect (false, "Expected silence at channel " + juce::String (ch) + ", frame " + juce::String (i));
                    return;
                }
        expect (true, "Every audio sample is silent");
    }

    void runMixerTests()
    {
        beginTest ("Mixer setters synchronize, clamp and undo/redo both model and engine");
        PersistenceFixture f;
        const auto track = f.project.addTrack();
        const auto other = f.project.addTrack();
        struct MixerState { float gain, pan; bool mute, solo; };
        std::vector<MixerState> states { { 0.0f, 0.0f, false, false } };
        auto save = [&] (float gain, float pan, bool mute, bool solo)
        {
            states.push_back ({ gain, pan, mute, solo });
            expectMixer (f, track, gain, pan, mute, solo);
            expectMixer (f, other, 0.0f, 0.0f, false, false);
        };
        expectMixer (f, track, 0.0f, 0.0f, false, false);
        f.project.beginTransaction ("Gain");
        f.project.setTrackGain (track, -12.5f);
        save (-12.5f, 0.0f, false, false);
        f.project.beginTransaction ("Pan");
        f.project.setTrackPan (track, 0.375f);
        save (-12.5f, 0.375f, false, false);
        f.project.beginTransaction ("Mute");
        f.project.setTrackMute (track, true);
        save (-12.5f, 0.375f, true, false);
        f.project.beginTransaction ("Solo");
        f.project.setTrackSolo (track, true);
        save (-12.5f, 0.375f, true, true);
        f.project.beginTransaction ("Upper limits");
        f.project.setTrackGain (track, 100.0f);
        f.project.setTrackPan (track, 9.0f);
        save (daw::AudioTrack::maxGainDb, 1.0f, true, true);
        f.project.beginTransaction ("Lower limits and flags off");
        f.project.setTrackGain (track, -100.0f);
        f.project.setTrackPan (track, -9.0f);
        f.project.setTrackMute (track, false);
        f.project.setTrackSolo (track, false);
        save (daw::AudioTrack::minGainDb, -1.0f, false, false);

        for (int i = static_cast<int> (states.size()) - 2; i >= 0; --i)
        {
            expect (f.project.undo());
            const auto& s = states[static_cast<size_t> (i)];
            expectMixer (f, track, s.gain, s.pan, s.mute, s.solo);
        }
        expect (! f.project.undo());
        for (size_t i = 1; i < states.size(); ++i)
        {
            expect (f.project.redo());
            const auto& s = states[i];
            expectMixer (f, track, s.gain, s.pan, s.mute, s.solo);
        }
        expect (! f.project.redo());

        beginTest ("A continuous gain gesture is one undo step");
        PersistenceFixture gesture;
        const auto t = gesture.project.addTrack();
        gesture.project.beginTransaction ("Pan before gesture");
        gesture.project.setTrackPan (t, -0.5f);
        gesture.project.beginTransaction ("Gain gesture");
        for (const auto db : { -2.0f, -5.0f, -8.0f, -13.0f })
        {
            gesture.project.setTrackGain (t, db);
            expectMixer (gesture, t, db, -0.5f, false, false);
        }
        expect (gesture.project.undo());
        expectMixer (gesture, t, 0.0f, -0.5f, false, false);
        expect (gesture.project.undo());
        expectMixer (gesture, t, 0.0f, 0.0f, false, false);
        expect (! gesture.project.undo());
        expect (gesture.project.redo());
        expect (gesture.project.redo());
        expectMixer (gesture, t, -13.0f, -0.5f, false, false);
        expect (! gesture.project.redo());
    }

    void runProjectParameterTests()
    {
        beginTest ("Tempo and master gain synchronize and undo independently");
        PersistenceFixture f;
        expectProjectParameters (f, 120.0, 0.0f);
        f.project.beginTransaction ("Tempo");
        f.project.setBpm (97.5);
        expectProjectParameters (f, 97.5, 0.0f);
        f.project.beginTransaction ("Master");
        f.project.setMasterGain (-7.25f);
        expectProjectParameters (f, 97.5, -7.25f);
        expect (f.project.undo());
        expectProjectParameters (f, 97.5, 0.0f);
        expect (f.project.undo());
        expectProjectParameters (f, 120.0, 0.0f);
        expect (! f.project.undo());
        expect (f.project.redo());
        expectProjectParameters (f, 97.5, 0.0f);
        expect (f.project.redo());
        expectProjectParameters (f, 97.5, -7.25f);

        beginTest ("Tempo and master gain clamp in the model as well as the engine");
        f.project.beginTransaction ("Upper limits");
        f.project.setBpm (10000.0);
        f.project.setMasterGain (100.0f);
        expectProjectParameters (f, daw::Transport::maxBpm, daw::AudioTrack::maxGainDb);
        f.project.beginTransaction ("Lower limits");
        f.project.setBpm (-100.0);
        f.project.setMasterGain (-100.0f);
        expectProjectParameters (f, daw::Transport::minBpm, daw::AudioTrack::minGainDb);
        expect (f.project.undo());
        expectProjectParameters (f, daw::Transport::maxBpm, daw::AudioTrack::maxGainDb);
        expect (f.project.redo());
        expectProjectParameters (f, daw::Transport::minBpm, daw::AudioTrack::minGainDb);
    }

    void runRoundTripTests()
    {
        beginTest ("A complete project round-trips every property and every offline audio sample");
        TemporaryDirectory temp;
        PersistenceFixture original;
        original.project.setBpm (97.5);
        original.project.setMasterGain (-4.25f);
        const auto first = original.project.addTrack ("Original name");
        const auto second = original.project.addTrack ("Muted");
        const auto third = original.project.addTrack ("Other solo");
        original.project.setTrackName (first, "Renamed & <stereo>");
        original.project.setTrackGain (first, -8.5f);
        original.project.setTrackPan (first, -0.375f);
        original.project.setTrackSolo (first, true);
        original.project.setTrackGain (second, 2.5f);
        original.project.setTrackPan (second, 0.625f);
        original.project.setTrackMute (second, true);
        original.project.setTrackGain (third, -3.25f);
        original.project.setTrackPan (third, 0.25f);
        original.project.setTrackSolo (third, true);

        const auto loop = original.project.addClip (first, original.firstSample, 0.125);
        original.project.trimClipStart (loop, 0.9375); // offset = 0.5 seconds at 97.5 bpm
        original.project.setClipLooping (loop, true);
        original.project.trimClipEnd (loop, 6.75);
        const auto tail = original.project.addClip (first, original.secondSample, 7.0);
        original.project.trimClipEnd (tail, 8.0);
        original.project.addClip (second, original.firstSample, 0.0);
        const auto trimmed = original.project.addClip (third, original.secondSample, 1.0);
        original.project.trimClipStart (trimmed, 1.8125);
        original.project.trimClipEnd (trimmed, 2.4375);

        const auto before = original.project.getState().createCopy();
        const auto file = temp.file ("complete.diydaw");
        if (! expectSuccess (original.project.saveTo (file)))
            return;
        expectTree (original.project.getState(), before); // Runtime state must not gain relativeFile.

        PersistenceFixture loaded;
        if (! expectSuccess (loaded.project.loadFrom (file)))
            return;
        expectTree (loaded.project.getState(), before);
        expectProjectParameters (loaded, 97.5, -4.25f);
        expectMixer (loaded, loaded.project.getTrack (0), -8.5f, -0.375f, false, true);
        expectMixer (loaded, loaded.project.getTrack (1), 2.5f, 0.625f, true, false);
        expectMixer (loaded, loaded.project.getTrack (2), -3.25f, 0.25f, false, true);
        for (int i = 0; i < loaded.project.getNumTracks(); ++i)
        {
            const auto track = loaded.project.getTrack (i);
            expectEquals (loaded.project.getEngineTrack (track).getName(), track[daw::ids::name].toString());
            for (const auto clip : track)
                expect (loaded.project.getClipSample (clip) != nullptr, "Virtual samples are available immediately");
        }
        // Start both smoothing histories from the same prepared state.
        original.engine.prepare (sampleRate, blockSize);
        loaded.engine.prepare (sampleRate, blockSize);
        const auto expectedAudio = original.render();
        expect (expectedAudio.getMagnitude (0, expectedAudio.getNumSamples()) > 0.01f,
                "Render comparison must contain audible clips");
        expectAudioEqual (loaded.render(), expectedAudio);

        beginTest ("Full-precision double clip properties survive XML exactly");
        // Regression: saveTo's ValueTree XML serialization rounds some valid doubles.
        // Keep the exact assertion so persistence cannot silently alter clip timing.
        PersistenceFixture precise;
        const auto t = precise.project.addTrack();
        const auto clip = precise.project.addClip (t, precise.firstSample, 1.2345678901234567);
        precise.project.trimClipStart (clip, 1.8765432109876543);
        precise.project.trimClipEnd (clip, 4.1234567890123457);
        const auto preciseState = precise.project.getState().createCopy();
        const auto preciseFile = temp.file ("precise.diydaw");
        if (! expectSuccess (precise.project.saveTo (preciseFile)))
            return;
        PersistenceFixture preciseLoaded;
        if (expectSuccess (preciseLoaded.project.loadFrom (preciseFile)))
            expectTree (preciseLoaded.project.getState(), preciseState);
    }

    void runDocumentLifecycleTests()
    {
        beginTest ("Load clears undo/redo and marks saved; edits and saving update document status");
        TemporaryDirectory temp;
        PersistenceFixture f;
        const auto track = f.project.addTrack();
        f.project.beginTransaction ("Saved edit");
        f.project.setBpm (97.5);
        f.project.setMasterGain (-6.0f);
        f.project.setTrackGain (track, -3.0f);
        f.project.addClip (track, f.firstSample, 0.0);
        expect (f.project.hasUnsavedChanges());
        const auto file = temp.file ("lifecycle.diydaw");
        if (! expectSuccess (f.project.saveTo (file)))
            return;
        expect (! f.project.hasUnsavedChanges());
        expect (f.project.getFile() == file);
        f.project.beginTransaction ("Unsaved edit");
        f.project.setTrackPan (track, -0.5f);
        expect (f.project.undo()); // Seed both undo and redo histories before loading.
        expect (f.project.getUndoManager().canUndo());
        expect (f.project.getUndoManager().canRedo());
        if (! expectSuccess (f.project.loadFrom (file)))
            return;
        expect (! f.project.getUndoManager().canUndo());
        expect (! f.project.getUndoManager().canRedo());
        expect (! f.project.undo());
        expect (! f.project.redo());
        expect (! f.project.hasUnsavedChanges());
        expect (f.project.getFile() == file);
        f.project.beginTransaction ("Rename after load");
        f.project.setTrackName (f.project.getTrack (0), "Changed");
        expect (f.project.hasUnsavedChanges());
        if (! expectSuccess (f.project.saveTo (file)))
            return;
        expect (! f.project.hasUnsavedChanges());

        beginTest ("newDocument resets tracks, parameters, file, dirty state and undo history");
        f.project.setBpm (180.0);
        f.project.setMasterGain (3.0f);
        f.project.newDocument (4);
        expectEquals (f.project.getNumTracks(), 4);
        expectEquals (f.engine.getNumTracks(), 4);
        expectProjectParameters (f, 120.0, 0.0f);
        expectEquals (f.engine.getTransport().getBeatsPerBar(), 4);
        expect (f.project.getFile() == juce::File());
        expect (! f.project.hasUnsavedChanges());
        expect (! f.project.undo());
        expect (! f.project.redo());
        for (int i = 0; i < 4; ++i)
        {
            const auto fresh = f.project.getTrack (i);
            expectEquals (fresh.getNumChildren(), 0);
            expectEquals (static_cast<int> (fresh[daw::ids::engineIndex]), i);
            expectMixer (f, fresh, 0.0f, 0.0f, false, false);
        }
        expectSilent (f.render (1.0));
    }

    void runReplacementTests()
    {
        beginTest ("Loading fewer empty tracks removes old active and pending engine clips");
        TemporaryDirectory temp;
        PersistenceFixture empty;
        empty.project.newDocument (1);
        const auto file = temp.file ("empty.diydaw");
        if (! expectSuccess (empty.project.saveTo (file)))
            return;
        PersistenceFixture f;
        for (int i = 0; i < 3; ++i)
        {
            const auto t = f.project.addTrack();
            f.project.addClip (t, f.firstSample, 0.0);
            f.project.setTrackGain (t, -2.0f);
            f.project.setTrackPan (t, 0.25f);
            f.project.setTrackSolo (t, true);
        }
        const auto audible = f.render (0.25); // Acquire the old snapshots on the audio thread.
        expect (audible.getMagnitude (0, audible.getNumSamples()) > 0.01f);
        f.project.addClip (f.project.getTrack (2), f.secondSample, 5.0); // Also leave a pending snapshot.
        if (! expectSuccess (f.project.loadFrom (file)))
            return;
        expectEquals (f.project.getNumTracks(), 1);
        expectEquals (f.engine.getNumTracks(), 1);
        expectMixer (f, f.project.getTrack (0), 0.0f, 0.0f, false, false);
        expectSilent (f.render());
        // Reusing every inactive slot must not bring old clips or mixer flags back.
        for (int i = 1; i < daw::Engine::maxTracks; ++i)
        {
            const auto t = f.project.addTrack();
            expectEquals (static_cast<int> (t[daw::ids::engineIndex]), i);
            expectMixer (f, t, 0.0f, 0.0f, false, false);
        }
        expectSilent (f.render());
        expect (f.project.addTrack().isValid() == false);

        beginTest ("Loading a zero-track document deactivates the whole engine pool");
        empty.project.newDocument (0);
        if (! expectSuccess (empty.project.saveTo (file)) || ! expectSuccess (f.project.loadFrom (file)))
            return;
        expectEquals (f.engine.getNumTracks(), 0);
        expectEquals (f.project.getNumTracks(), 0);
        expectSilent (f.render (1.0));
    }

    void runRelativePathTests()
    {
        beginTest ("A moved project folder resolves a real WAV using its saved relative path");
        TemporaryDirectory temp;
        const auto source = temp.file ("original");
        const auto copied = temp.file ("copied");
        if (! expectSuccess (source.createDirectory()))
            return;
        const auto wav = source.getChildFile ("sample.wav");
        PersistenceFixture original;
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::OutputStream> stream = wav.createOutputStream();
            auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                              .withSampleRate (sampleRate)
                                                              .withNumChannels (2)
                                                              .withBitsPerSample (24));
            expect (writer != nullptr, "Create a real WAV writer");
            if (writer == nullptr)
                return;
            const auto sample = original.pool.get (original.firstSample);
            expect (writer->writeFromAudioSampleBuffer (sample->buffer, 0, sample->getNumFrames()));
        }
        expect (wav.existsAsFile());
        original.pool.insert (daw::SamplePool::keyFor (wav), original.pool.get (original.firstSample));
        original.project.addClip (original.project.addTrack(), wav, 0.25);
        const auto file = source.getChildFile ("moved.diydaw");
        if (! expectSuccess (original.project.saveTo (file)))
            return;
        expect (source.copyDirectoryTo (copied));
        expect (source.deleteRecursively());
        expect (! wav.existsAsFile());
        const auto copiedWav = copied.getChildFile ("sample.wav");
        expect (copiedWav.existsAsFile());
        PersistenceFixture loaded;
        // Deliberately leave the copied WAV uncached; loadAsync need not finish in this test.
        if (! expectSuccess (loaded.project.loadFrom (copied.getChildFile ("moved.diydaw"))))
            return;
        const auto clip = loaded.project.getTrack (0).getChild (0);
        expectEquals (daw::Project::getClipFile (clip).getFullPathName(), copiedWav.getFullPathName());
        expect (! clip.hasProperty (daw::ids::relativeFile));
        expect (! loaded.project.hasUnsavedChanges());
    }

    void runErrorTests()
    {
        beginTest ("Missing, malformed, wrong-root and future-version files leave the document untouched");
        TemporaryDirectory temp;
        PersistenceFixture f;
        const auto track = f.project.addTrack ("Keep me");
        f.project.beginTransaction ("Initial edits");
        f.project.addClip (track, f.firstSample, 0.0);
        f.project.setBpm (97.5);
        f.project.setMasterGain (-4.0f);
        f.project.setTrackGain (track, -5.0f);
        const auto currentFile = temp.file ("current.diydaw");
        if (! expectSuccess (f.project.saveTo (currentFile)))
            return;
        f.project.beginTransaction ("Retained undo");
        f.project.setTrackName (track, "Unsaved name");
        f.project.beginTransaction ("Retained redo");
        f.project.setTrackPan (track, 0.5f);
        expect (f.project.undo());
        const auto state = f.project.getState().createCopy();
        const auto identity = f.project.getState();
        const auto undoDescription = f.project.getUndoManager().getUndoDescription();
        const auto redoDescription = f.project.getUndoManager().getRedoDescription();
        const auto dirty = f.project.hasUnsavedChanges();
        const auto missing = temp.file ("missing.diydaw");
        const auto garbage = temp.file ("garbage.diydaw");
        const auto wrongRoot = temp.file ("wrong-root.diydaw");
        const auto future = temp.file ("future.diydaw");
        expect (garbage.replaceWithText ("This is not XML."));
        expect (wrongRoot.replaceWithText ("<NOT_A_PROJECT><TRACK/></NOT_A_PROJECT>"));
        expect (future.replaceWithText ("<PROJECT version=\"" + juce::String (daw::Project::fileFormatVersion + 1)
                                            + "\" bpm=\"200\"><TRACK name=\"Do not load\"/></PROJECT>"));
        f.render (0.05); // Settle mixer smoothing before comparing repeated renders.
        const auto expectedAudio = f.render (0.5);
        for (const auto& file : { missing, garbage, wrongRoot, future })
        {
            const auto result = f.project.loadFrom (file);
            expect (result.failed(), file.getFileName());
            expect (result.getErrorMessage().isNotEmpty());
            expect (f.project.getState() == identity);
            expect (f.project.getTrack (0) == track);
            expectTree (f.project.getState(), state);
            expect (f.project.getFile() == currentFile);
            expect (f.project.hasUnsavedChanges() == dirty);
            expectEquals (f.engine.getNumTracks(), 1);
            expectProjectParameters (f, 97.5, -4.0f);
            expectMixer (f, track, -5.0f, 0.0f, false, false);
            expect (f.project.getUndoManager().canUndo());
            expect (f.project.getUndoManager().canRedo());
            expectEquals (f.project.getUndoManager().getUndoDescription(), undoDescription);
            expectEquals (f.project.getUndoManager().getRedoDescription(), redoDescription);
            expectAudioEqual (f.render (0.5), expectedAudio);
        }
        expect (f.project.redo());
        expectMixer (f, track, -5.0f, 0.5f, false, false);
        expect (f.project.undo());
        expect (f.project.undo());
        expectEquals (track[daw::ids::name].toString(), juce::String ("Keep me"));
    }

    void runTrackLimitTests()
    {
        beginTest ("Oversized XML loads only the first maxTracks in file order");
        TemporaryDirectory temp;
        expectSuccess (temp.directory.createDirectory());
        juce::XmlElement xml ("PROJECT");
        xml.setAttribute ("version", daw::Project::fileFormatVersion);
        for (int i = 0; i < daw::Engine::maxTracks + 3; ++i)
        {
            auto* track = xml.createNewChildElement ("TRACK");
            track->setAttribute ("name", "Track " + juce::String (i));
            track->setAttribute ("engineIndex", 999 - i); // Runtime indices must be reassigned.
            track->setAttribute ("gain", -0.25 * i);
        }
        const auto file = temp.file ("oversized.diydaw");
        expect (xml.writeTo (file));
        PersistenceFixture f;
        if (! expectSuccess (f.project.loadFrom (file)))
            return;
        expectEquals (f.project.getNumTracks(), daw::Engine::maxTracks);
        expectEquals (f.engine.getNumTracks(), daw::Engine::maxTracks);
        for (int i = 0; i < daw::Engine::maxTracks; ++i)
        {
            const auto track = f.project.getTrack (i);
            expectEquals (track[daw::ids::name].toString(), "Track " + juce::String (i));
            expectEquals (static_cast<int> (track[daw::ids::engineIndex]), i);
            expectMixer (f, track, -0.25f * static_cast<float> (i), 0.0f, false, false);
        }
        expect (! f.project.addTrack().isValid());
        expect (! f.project.hasUnsavedChanges());
    }

    void runTypeAndDefaultTests()
    {
        beginTest ("Hand-written XML normalizes numeric/bool types and supplies missing mixer defaults");
        TemporaryDirectory temp;
        if (! expectSuccess (temp.directory.createDirectory()))
            return;
        const auto file = temp.file ("types.diydaw");
        expect (file.replaceWithText (
            "<PROJECT version=\"1\" bpm=\"97.5\" beatsPerBar=\"7\" masterGain=\"-3.25\">"
            "<TRACK name=\"Defaults\"><CLIP name=\"Loop\" file=\"/virtual/persistence-first.wav\" "
            "start=\"1.25\" length=\"5.5\" offset=\"0.375\" loop=\"1\"/></TRACK>"
            "<TRACK name=\"Explicit\" gain=\"-9.5\" pan=\"0.25\" mute=\"1\" solo=\"1\">"
            "<CLIP name=\"One shot\" file=\"/virtual/persistence-second.wav\" "
            "start=\"0.5\" length=\"1.25\" offset=\"0.125\" loop=\"0\"/></TRACK></PROJECT>"));
        PersistenceFixture f;
        if (! expectSuccess (f.project.loadFrom (file)))
            return;
        const auto state = f.project.getState();
        expect (state[daw::ids::version].isInt());
        expect (state[daw::ids::bpm].isDouble());
        expect (state[daw::ids::beatsPerBar].isInt());
        expect (state[daw::ids::masterGain].isDouble());
        expectProjectParameters (f, 97.5, -3.25f);
        expectEquals (f.engine.getTransport().getBeatsPerBar(), 7);
        expectMixer (f, f.project.getTrack (0), 0.0f, 0.0f, false, false);
        expectMixer (f, f.project.getTrack (1), -9.5f, 0.25f, true, true);
        expect (state.getChildWithName (daw::ids::MASTER).isValid());
        for (int t = 0; t < f.project.getNumTracks(); ++t)
        {
            const auto track = f.project.getTrack (t);
            expect (track[daw::ids::engineIndex].isInt());
            expect (track[daw::ids::gain].isDouble());
            expect (track[daw::ids::pan].isDouble());
            expect (track[daw::ids::mute].isBool());
            expect (track[daw::ids::solo].isBool());
            const auto clip = track.getChild (0);
            expect (clip[daw::ids::start].isDouble());
            expect (clip[daw::ids::length].isDouble());
            expect (clip[daw::ids::offset].isDouble());
            expect (clip[daw::ids::loop].isBool());
        }
        const auto loop = f.project.getTrack (0).getChild (0);
        expectEquals (daw::Project::getClipStart (loop), 1.25);
        expectEquals (daw::Project::getClipLength (loop), 5.5);
        expectEquals (daw::Project::getClipOffset (loop), 0.375);
        expect (daw::Project::isClipLooping (loop));
        expect (! daw::Project::isClipLooping (f.project.getTrack (1).getChild (0)));
        f.project.newDocument (4);
        expectEquals (f.engine.getTransport().getBeatsPerBar(), 4);
    }
};

static PersistenceTests persistenceTests;
