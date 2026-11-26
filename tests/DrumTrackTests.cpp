#include "ProjectRenderer.h"
#include "BuiltinSamples.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>

namespace
{
using Project = daw::Project;
using Rack = daw::DrumRack;
using InstrumentType = daw::AudioTrack::InstrumentType;
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 1024;

/** Owns test files on the test thread; declared before fixtures so their pools die first. */
struct DrumDirectory
{
    juce::File directory = juce::File::createTempFile ("-drum-track");

    ~DrumDirectory() { directory.deleteRecursively(); }

    juce::File file (const juce::String& name) const { return directory.getChildFile (name); }
};

/** Model edits and simulated audio callbacks run sequentially on the test thread. */
struct DrumFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    Project project { engine, pool };

    DrumFixture()
    {
        // The application also wires sample arrival to the model's public resync hook.
        pool.onSampleAdded = [this] (const juce::String&) { project.syncAllTracks(); };
    }

    ~DrumFixture() { pool.onSampleAdded = nullptr; }

    juce::ValueTree addDrums() { return project.addTrack ({}, Project::TrackKind::drums); }

    juce::ValueTree addHit (const juce::ValueTree& track, int pitch = 36, float velocity = 1.0f)
    {
        const auto clip = project.addMidiClip (track, 0.0, 4.0);
        // An onset inside a block catches event-offset mistakes; short notes must still
        // play the whole one-shot sample after their note-off.
        return project.addNote (clip, pitch, 0.125, Project::minNoteLengthBeats, velocity);
    }

    juce::AudioBuffer<float> render (double seconds = 0.9, double startBeat = 0.0)
    {
        auto& transport = engine.getTransport();
        transport.stop();
        transport.setLoopEnabled (false);
        transport.setPositionBeats (startBeat);
        engine.prepare (sampleRate, blockSize);
        engine.getMetronome().setEnabled (false);
        juce::AudioBuffer<float> warmUp (2, blockSize);
        for (int done = 0; done < static_cast<int> (sampleRate * 0.05); done += blockSize)
            engine.render (warmUp, blockSize);
        transport.setPositionBeats (startBeat);
        transport.play();
        return daw::renderOffline (engine, seconds, blockSize);
    }
};

// Independent reference: read decoded PCM directly, applying pad and track centre-pan laws.
// No DrumRack, AudioTrack, clip rendering, or model snapshot is used here.
juce::AudioBuffer<float> sampleReference (const daw::SampleData& sample, int frames,
                                        float velocity = 1.0f, float gainDb = 0.0f)
{
    juce::AudioBuffer<float> result (2, frames);
    result.clear();
    const auto onset = static_cast<int> (sampleRate * 0.125 * 60.0 / 120.0);
    const auto centre = std::cos (juce::MathConstants<float>::pi / 4.0f);
    const auto gain = juce::Decibels::decibelsToGain (gainDb);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < std::min (sample.getNumFrames(), frames - onset); ++i)
            result.setSample (ch, onset + i,
                              ((sample.buffer.getSample (std::min (ch, sample.buffer.getNumChannels() - 1), i)
                                * velocity) * (gain * centre)) * centre);
    return result;
}

/** Shared assertions and WAV helpers; all file I/O runs outside simulated audio callbacks. */
struct DrumTest : juce::UnitTest
{
    explicit DrumTest (const juce::String& testName) : juce::UnitTest (testName, "diyDaw") {}

    bool expectSuccess (const juce::Result& result)
    {
        expect (result.wasOk(), result.getErrorMessage());
        return result.wasOk();
    }

    void expectAudio (const juce::AudioBuffer<float>& actual, const juce::AudioBuffer<float>& expected,
                      const juce::String& context, float tolerance = 2.0e-6f)
    {
        expectEquals (actual.getNumChannels(), expected.getNumChannels(), context);
        expectEquals (actual.getNumSamples(), expected.getNumSamples(), context);
        for (int ch = 0; ch < std::min (actual.getNumChannels(), expected.getNumChannels()); ++ch)
            for (int i = 0; i < std::min (actual.getNumSamples(), expected.getNumSamples()); ++i)
            {
                const auto a = actual.getSample (ch, i);
                const auto b = expected.getSample (ch, i);
                if (! std::isfinite (a) || ! std::isfinite (b) || std::abs (a - b) > tolerance)
                {
                    expect (false, context + ": channel " + juce::String (ch) + ", frame " + juce::String (i)
                                   + ", expected " + juce::String (b, 9) + ", actual " + juce::String (a, 9));
                    return;
                }
            }
    }

    void expectSilent (const juce::AudioBuffer<float>& audio)
    {
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            expectEquals (audio.getMagnitude (ch, 0, audio.getNumSamples()), 0.0f, "Empty/unmapped pads are silent");
    }

    bool writeWav (const juce::File& file)
    {
        if (! expectSuccess (file.getParentDirectory().createDirectory()))
            return false;
        juce::AudioBuffer<float> data (2, 4096);
        for (int i = 0; i < data.getNumSamples(); ++i)
        {
            const auto value = 0.3f * static_cast<float> (std::sin (2.0 * juce::MathConstants<double>::pi * i / 97.0))
                               * static_cast<float> (std::sin (juce::MathConstants<double>::pi * i / 4095.0));
            data.setSample (0, i, value);
            data.setSample (1, i, -0.5f * value);
        }
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                          .withSampleRate (sampleRate)
                                                          .withNumChannels (2)
                                                          .withBitsPerSample (24));
        expect (writer != nullptr, "Create real WAV writer");
        if (writer == nullptr)
            return false;
        const auto ok = writer->writeFromAudioSampleBuffer (data, 0, data.getNumSamples());
        expect (ok, "Write real WAV frames");
        return ok;
    }

    daw::SamplePool::Ptr decodeWav (DrumFixture& f, const juce::File& file)
    {
        // Synchronous decode keeps this headless test independent of a GUI message loop.
        daw::SamplePool::Ptr sample = daw::loadSample (file, f.pool.getFormats());
        expect (sample != nullptr, "Decode " + file.getFullPathName());
        if (sample != nullptr)
            f.pool.insert (daw::SamplePool::keyFor (file), sample);
        return sample;
    }

    static int instrumentCount (const juce::ValueTree& track)
    {
        auto count = 0;
        for (const auto& child : track)
            if (child.hasType (daw::ids::DEVICE) && Project::isInstrumentType (Project::getDeviceType (child)))
                ++count;
        return count;
    }
};

/** Exercises drum track creation, pad edits, playback and automation on the test thread. */
struct DrumTrackModelTests : DrumTest
{
    DrumTrackModelTests() : DrumTest ("Drum track model and playback") {}

    void runTest() override
    {
        defaults();
        playback();
        padEdits();
        parameters();
        automation();
        coexistence();
        switching();
    }

    void defaults()
    {
        beginTest ("Drums are MIDI tracks with one first drumrack, ordered builtin pads and unique device uids");
        DrumFixture f;
        const auto audio = f.project.addTrack();
        const auto drums = f.addDrums();
        const auto synth = f.project.addTrack ({}, Project::TrackKind::midi);
        const auto otherDrums = f.addDrums();
        f.project.addEffect (audio, "eq");
        f.project.addEffect (f.project.getMaster(), "delay");
        expectEquals (drums[daw::ids::kind].toString(), juce::String ("midi"));
        expectEquals (drums[daw::ids::name].toString(), juce::String ("2 Drums"));
        expectEquals (otherDrums[daw::ids::name].toString(), juce::String ("4 Drums"));
        expect (Project::isMidiTrack (drums) && Project::isDrumTrack (drums));
        expect (! Project::isDrumTrack (audio) && ! Project::isDrumTrack (synth));
        const auto device = Project::getDevice (drums);
        expectEquals (drums.getNumChildren(), 1);
        expectEquals (instrumentCount (drums), 1);
        expect (drums.getChild (0) == device);
        expectEquals (Project::getDeviceType (device), juce::String ("drumrack"));
        auto& engineTrack = f.project.getEngineTrack (drums);
        expect (engineTrack.getInstrumentType() == InstrumentType::drumRack);
        expect (engineTrack.getInstrument() == &engineTrack.getDrumRack());
        expectEquals (engineTrack.getName(), juce::String ("2 Drums"));
        const auto& names = daw::getBuiltinSampleNames();
        expectEquals (names.size(), Rack::numPads);
        for (int pad = 0; pad < std::min (names.size(), Rack::numPads); ++pad)
        {
            expectEquals (Project::getPadSampleKey (device, pad), daw::builtinSampleKey (names[pad]));
            const auto choke = names[pad] == "closedHat" || names[pad] == "openHat" ? 1.0f : 0.0f;
            expectEquals (Project::getDeviceParam (device, Rack::paramIndex (pad, Rack::choke)), choke);
            expectEquals (engineTrack.getDrumRack().getParam (Rack::paramIndex (pad, Rack::choke)), choke);
        }
        std::set<int> uids;
        for (const auto& track : f.project.getState())
            for (const auto& child : track)
                if (child.hasType (daw::ids::DEVICE))
                {
                    const auto uid = static_cast<int> (child[daw::ids::uid]);
                    expect (uid > 0);
                    expect (uids.insert (uid).second, "Every instrument and effect has a unique uid");
                }
    }

    void playback()
    {
        beginTest ("Notes 36, 38 and 51 play the kick, snare and final pad with linear velocity");
        DrumFixture f;
        const auto drums = f.addDrums();
        const auto note = f.addHit (drums);
        for (const auto pitch : { 36, 38, 51 })
        {
            const auto soundName = pitch == 36 ? "kick" : pitch == 38 ? "snare" : "ride";
            const auto sample = daw::createBuiltinSample (soundName, sampleRate);
            expect (sample != nullptr);
            if (sample == nullptr)
                continue;
            f.project.moveNote (note, 0.125, pitch);
            for (const auto velocity : { 0.0f, 0.25f, 0.5f, 1.0f })
            {
                f.project.setNoteVelocity (note, velocity);
                const auto actual = f.render();
                expectAudio (actual, sampleReference (*sample, actual.getNumSamples(), velocity),
                             juce::String (soundName) + " velocity " + juce::String (velocity));
                if (velocity > 0.0f)
                    expect (actual.getMagnitude (0, 0, actual.getNumSamples()) > 0.01f);
            }
        }

        beginTest ("All MIDI pitches outside 36..51 are silent");
        f.project.deleteNote (note);
        const auto clip = f.project.addMidiClip (drums, 0.0, 4.0);
        for (int pitch = 0; pitch < 128; ++pitch)
            if (pitch < Rack::firstNote || pitch >= Rack::firstNote + Rack::numPads)
                f.project.addNote (clip, pitch, 0.125, 0.25, 1.0f);
        expectSilent (f.render());
    }

    void padEdits()
    {
        beginTest ("Builtin assignment, clear, undo and redo republish the audible kit");
        DrumDirectory temp;
        DrumFixture f;
        const auto drums = f.addDrums();
        const auto device = Project::getDevice (drums);
        f.addHit (drums);
        const auto kick = f.render();
        f.project.getUndoManager().clearUndoHistory();
        f.project.beginTransaction ("Replace kick");
        f.project.setPadSample (device, 0, daw::builtinSampleKey ("snare"));
        expectEquals (Project::getPadSampleKey (device, 0), juce::String ("builtin:snare"));
        const auto snare = f.render();
        expectAudio (snare, sampleReference (*f.pool.get ("builtin:snare"), snare.getNumSamples()), "Assigned snare");
        expect (f.project.undo());
        expectAudio (f.render(), kick, "Undo restores builtin kick");
        expect (f.project.redo());
        expectAudio (f.render(), snare, "Redo restores assigned snare");
        expectEquals (Project::getPadName (device, 6), juce::String ("Closed Hat"));

        beginTest ("A real stereo WAV becomes audible when decoded into the pool");
        const auto file = temp.file ("my kick.wav");
        if (! writeWav (file))
            return;
        f.project.beginTransaction ("Assign WAV");
        f.project.setPadSample (device, 0, daw::SamplePool::keyFor (file));
        expectEquals (Project::getPadName (device, 0), juce::String ("my kick"));
        expectSilent (f.render());
        const auto sample = decodeWav (f, file);
        if (sample == nullptr)
            return;
        const auto wavAudio = f.render();
        expectAudio (wavAudio, sampleReference (*sample, wavAudio.getNumSamples()), "Loaded stereo WAV");
        expect (wavAudio.getMagnitude (0, 0, wavAudio.getNumSamples()) > 0.01f);
        expect (f.project.undo());
        expectAudio (f.render(), snare, "Undo WAV assignment");
        expect (f.project.redo());
        expectAudio (f.render(), wavAudio, "Redo WAV assignment");

        f.project.beginTransaction ("Clear pad");
        f.project.clearPad (device, 0);
        expectEquals (Project::getPadSampleKey (device, 0), juce::String());
        expectEquals (Project::getPadName (device, 0), juce::String());
        expectSilent (f.render());
        expect (f.project.undo());
        expectAudio (f.render(), wavAudio, "Undo clear");
        expect (f.project.redo());
        expectSilent (f.render());

        beginTest ("Invalid pad indices and non-drumrack devices ignore assignments and clears");
        const auto synth = Project::getDevice (f.project.addTrack ({}, Project::TrackKind::midi));
        const auto effect = f.project.addEffect (drums, "eq");
        const auto before = f.project.getState().createCopy();
        f.project.getUndoManager().clearUndoHistory();
        f.project.beginTransaction ("Invalid edits");
        for (const auto pad : { -1, Rack::numPads, 100 })
        {
            f.project.setPadSample (device, pad, "builtin:kick");
            f.project.clearPad (device, pad);
        }
        for (const auto& invalid : { synth, effect, drums, juce::ValueTree() })
        {
            f.project.setPadSample (invalid, 0, "builtin:kick");
            f.project.clearPad (invalid, 0);
        }
        expect (f.project.getState().isEquivalentTo (before));
        expect (! f.project.undo(), "Ignored edits create no undo step");
    }

    void parameters()
    {
        beginTest ("All pad parameters push to the engine, clamp/step and undo as one gesture");
        DrumFixture f;
        const auto track = f.addDrums();
        const auto device = Project::getDevice (track);
        const auto before = f.project.getState().createCopy();
        auto& rack = f.project.getEngineTrack (track).getDrumRack();
        const auto& infos = Rack::getParamInfos();
        expectEquals (static_cast<int> (Project::getDeviceParamInfos (device).size()), Rack::numParams);
        f.project.getUndoManager().clearUndoHistory();
        f.project.beginTransaction ("Adjust pads");
        for (int pad = 0; pad < Rack::numPads; ++pad)
            for (int p = 0; p < Rack::numPadParams; ++p)
            {
                const auto index = Rack::paramIndex (pad, static_cast<Rack::PadParam> (p));
                const auto& info = infos[static_cast<size_t> (index)];
                const auto value = info.minValue + 0.371f * (info.maxValue - info.minValue);
                const auto expected = info.stepped ? std::round (value) : value;
                f.project.setDeviceParam (pad % 2 == 0 ? device : track, index, value);
                expectEquals (Project::getDeviceParam (device, index), expected, info.id);
                expectEquals (rack.getParam (index), expected, info.id);
            }
        const auto edited = f.project.getState().createCopy();
        expect (f.project.undo());
        expect (f.project.getState().isEquivalentTo (before));
        for (int i = 0; i < Rack::numParams; ++i)
            expectEquals (rack.getParam (i), Project::getDeviceParam (device, i));
        expect (f.project.redo());
        expect (f.project.getState().isEquivalentTo (edited));
        for (int i = 0; i < Rack::numParams; ++i)
        {
            const auto& info = infos[static_cast<size_t> (i)];
            expectEquals (rack.getParam (i), Project::getDeviceParam (device, i));
            f.project.setDeviceParam (device, i, info.maxValue + 100.0f);
            expectEquals (rack.getParam (i), info.maxValue);
            f.project.setDeviceParam (device, i, info.minValue - 100.0f);
            expectEquals (rack.getParam (i), info.minValue);
        }
    }

    void automation()
    {
        beginTest ("Every pad parameter has a uid-qualified automation target");
        DrumFixture f;
        const auto track = f.addDrums();
        const auto device = Project::getDevice (track);
        const auto targets = Project::getAutomationTargets (track);
        expectEquals (static_cast<int> (targets.size()), 2 + Rack::numParams);
        std::set<juce::String> ids;
        for (const auto& target : targets)
            expect (ids.insert (target.id).second);
        for (int i = 0; i < Rack::numParams; ++i)
        {
            const auto& info = Rack::getParamInfos()[static_cast<size_t> (i)];
            const auto id = device[daw::ids::uid].toString() + ":" + info.id;
            expectEquals (Project::getDeviceParamTargetId (device, i), id);
            Project::AutomationTarget target {};
            const auto found = Project::findAutomationTarget (track, id, target);
            expect (found, id);
            if (found)
            {
                expectEquals (target.info.minValue, info.minValue);
                expectEquals (target.info.maxValue, info.maxValue);
            }
        }

        beginTest ("A pad1Gain automation lane scales the kick and removal restores the model gain");
        f.addHit (track);
        const auto baseline = f.render();
        const auto target = device[daw::ids::uid].toString() + ":pad1Gain";
        const auto lane = f.project.getOrCreateAutomationLane (track, target);
        expect (lane.isValid());
        const auto point = f.project.addAutomationPoint (lane, 0.0, -12.0f);
        expect (point.isValid());
        expect (Project::isAutomated (track, target));
        auto expected = baseline;
        expected.applyGain (juce::Decibels::decibelsToGain (-12.0f));
        expectAudio (f.render(), expected, "Automated -12 dB kick");
        expectEquals (f.project.getEngineValue (track, target), -12.0f);
        expectEquals (Project::getDeviceParam (device, Rack::paramIndex (0, Rack::gain)), 0.0f);
        f.project.moveAutomationPoint (point, 0.0, -6.0f);
        expected = baseline;
        expected.applyGain (juce::Decibels::decibelsToGain (-6.0f));
        expectAudio (f.render(), expected, "Edited automation -6 dB kick");
        f.project.beginTransaction ("Remove pad automation");
        f.project.removeAutomationLane (lane);
        expect (! Project::isAutomated (track, target));
        expectAudio (f.render(), baseline, "Removing lane restores pad gain");
        expect (f.project.undo());
        expectAudio (f.render(), expected, "Undo restores automation audio");
    }

    void coexistence()
    {
        beginTest ("A synth and drum rack coexist and mix their independently rendered audio");
        DrumFixture f;
        const auto drums = f.addDrums();
        const auto synth = f.project.addTrack ({}, Project::TrackKind::midi);
        f.addHit (drums);
        f.addHit (synth, 69, 0.7f);
        auto& synthTrack = f.project.getEngineTrack (synth);
        auto& drumTrack = f.project.getEngineTrack (drums);
        expect (synthTrack.getInstrumentType() == InstrumentType::synth);
        expect (synthTrack.getInstrument() == &synthTrack.getSynth());
        expect (drumTrack.getInstrument() == &drumTrack.getDrumRack());
        const auto combined = f.render();
        f.project.setTrackMute (synth, true);
        auto expected = f.render();
        expectAudio (expected, sampleReference (*f.pool.get ("builtin:kick"), expected.getNumSamples()), "Drums alone");
        f.project.setTrackMute (synth, false);
        f.project.setTrackMute (drums, true);
        const auto synthOnly = f.render();
        expect (synthOnly.getMagnitude (0, 0, synthOnly.getNumSamples()) > 0.001f);
        for (int ch = 0; ch < 2; ++ch)
            expected.addFrom (ch, 0, synthOnly, ch, 0, synthOnly.getNumSamples());
        expectAudio (combined, expected, "Synth plus drums");
    }

    void switching()
    {
        beginTest ("AudioTrack switches instrument pointers and clears the previous drum voices");
        daw::AudioTrack track;
        expect (track.getInstrumentType() == InstrumentType::none);
        expect (track.getInstrument() == nullptr);
        auto kit = std::make_unique<Rack::Kit>();
        kit->samples[0] = daw::createBuiltinSample ("kick");
        track.getDrumRack().setKit (std::move (kit));
        track.prepare (sampleRate, blockSize);
        track.setInstrumentType (InstrumentType::drumRack);
        expect (track.getInstrument() == &track.getDrumRack());
        track.pushLiveNote (36, 1.0f, true);
        juce::AudioBuffer<float> out (2, blockSize);
        out.clear();
        daw::Transport transport;
        transport.prepare (sampleRate);
        track.process (out, transport.advance (blockSize), false);
        expect (track.getDrumRack().getNumActiveVoices() > 0);
        track.setInstrumentType (InstrumentType::synth);
        expect (track.getInstrument() == &track.getSynth());
        out.clear();
        track.process (out, transport.advance (blockSize), false);
        expectEquals (track.getDrumRack().getNumActiveVoices(), 0);
        expectSilent (out);
        track.setInstrumentType (InstrumentType::none);
        expect (track.getInstrument() == nullptr);
        out.clear();
        track.process (out, transport.advance (blockSize), false);
        expectSilent (out);
        track.setInstrumentType (InstrumentType::drumRack);
        out.clear();
        track.process (out, transport.advance (blockSize), false);
        expectSilent (out);
    }
};

/** Verifies drum project persistence, import and private export copies on the test thread. */
struct DrumTrackFileTests : DrumTest
{
    DrumTrackFileTests() : DrumTest ("Drum track files and export") {}

    void runTest() override
    {
        builtinRoundTrip();
        relativeFiles();
        missingFile();
        demo();
        importedInstruments();
        exportAudio();
    }

    void builtinRoundTrip()
    {
        beginTest ("Builtin pad keys, cleared pads, parameters and rendered audio round-trip");
        DrumDirectory temp;
        DrumFixture source;
        const auto track = source.addDrums();
        const auto device = Project::getDevice (track);
        source.addHit (track);
        source.project.setPadSample (device, 0, "builtin:snare");
        source.project.clearPad (device, 15);
        source.project.setDeviceParam (device, Rack::paramIndex (0, Rack::gain), -7.25f);
        const auto before = source.render();
        const auto file = temp.file ("builtins.diydaw");
        if (! expectSuccess (source.project.saveTo (file)))
            return;
        const auto xml = juce::parseXML (file);
        expect (xml != nullptr);
        if (xml == nullptr)
            return;
        const auto savedDevice = Project::getDevice (juce::ValueTree::fromXml (*xml).getChildWithName (daw::ids::TRACK));
        DrumFixture loaded;
        if (! expectSuccess (loaded.project.loadFrom (file)))
            return;
        const auto loadedTrack = loaded.project.getTrack (0);
        const auto loadedDevice = Project::getDevice (loadedTrack);
        expect (Project::isDrumTrack (loadedTrack));
        for (int pad = 0; pad < Rack::numPads; ++pad)
        {
            expectEquals (Project::getPadSampleKey (loadedDevice, pad), Project::getPadSampleKey (device, pad));
            expect (! savedDevice.hasProperty ("pad" + juce::String (pad + 1) + "RelativeFile"));
        }
        for (int i = 0; i < Rack::numParams; ++i)
            expectEquals (Project::getDeviceParam (loadedDevice, i), Project::getDeviceParam (device, i));
        expectEquals (loadedDevice[daw::ids::uid].toString(), device[daw::ids::uid].toString());
        expectAudio (loaded.render(), before, "Builtin project round-trip");
        expect (! loaded.project.undo());
        expect (! loaded.project.hasUnsavedChanges());
    }

    void relativeFiles()
    {
        beginTest ("Pad 1 and 16 files save relative paths and resolve after moving the project folder");
        DrumDirectory temp;
        const auto original = temp.file ("original");
        const auto moved = temp.file ("moved");
        const auto wav = original.getChildFile ("samples/my kick.wav");
        if (! writeWav (wav))
            return;
        DrumFixture source;
        if (decodeWav (source, wav) == nullptr)
            return;
        const auto drums = source.addDrums();
        const auto device = Project::getDevice (drums);
        source.project.setPadSample (device, 0, daw::SamplePool::keyFor (wav));
        source.project.setPadSample (device, 15, daw::SamplePool::keyFor (wav));
        const auto note = source.addHit (drums);
        source.project.addNote (note.getParent(), 51, 0.5, 0.125, 0.4f);
        const auto audio = source.project.addTrack();
        source.project.addClip (audio, wav, 0.0);
        source.project.setTrackMute (audio, true);
        const auto before = source.render();
        const auto file = original.getChildFile ("song.diydaw");
        if (! expectSuccess (source.project.saveTo (file)))
            return;
        const auto xml = juce::parseXML (file);
        expect (xml != nullptr);
        if (xml == nullptr)
            return;
        const auto saved = Project::getDevice (juce::ValueTree::fromXml (*xml).getChildWithName (daw::ids::TRACK));
        for (const auto pad : { 1, 16 })
        {
            const auto id = "pad" + juce::String (pad) + "RelativeFile";
            expectEquals (saved[id].toString(), juce::String ("samples/my kick.wav"));
            expect (! device.hasProperty (id), "Saving adds relative paths only to the serialized copy");
        }
        if (! expectSuccess (original.copyDirectoryTo (moved) ? juce::Result::ok() : juce::Result::fail ("Copy project folder")))
            return;
        expect (original.deleteRecursively());
        expect (! wav.existsAsFile());
        const auto movedWav = moved.getChildFile ("samples/my kick.wav");
        DrumFixture loaded;
        // Decode the real relocated file in this fresh pool before loading; this isolates
        // path resolution and kit reconstruction from asynchronous message dispatch.
        const auto sample = decodeWav (loaded, movedWav);
        if (sample == nullptr || ! expectSuccess (loaded.project.loadFrom (moved.getChildFile ("song.diydaw"))))
            return;
        const auto loadedDevice = Project::getDevice (loaded.project.getTrack (0));
        for (const auto pad : { 0, 15 })
        {
            expectEquals (Project::getPadSampleKey (loadedDevice, pad), daw::SamplePool::keyFor (movedWav));
            expect (! loadedDevice.hasProperty ("pad" + juce::String (pad + 1) + "RelativeFile"));
        }
        expectEquals (Project::getClipFile (loaded.project.getTrack (1).getChildWithName (daw::ids::CLIP)).getFullPathName(),
                      movedWav.getFullPathName());
        expect (loaded.pool.get (movedWav) == sample);
        expectAudio (loaded.render(), before, "Relocated pad files play the same samples");
        expect (! loaded.project.hasUnsavedChanges());
    }

    void missingFile()
    {
        beginTest ("A missing pad WAV loads successfully and leaves that pad silent");
        DrumDirectory temp;
        const auto wav = temp.file ("missing.wav");
        if (! writeWav (wav))
            return;
        DrumFixture source;
        const auto track = source.addDrums();
        source.addHit (track);
        if (decodeWav (source, wav) == nullptr)
            return;
        source.project.setPadSample (Project::getDevice (track), 0, daw::SamplePool::keyFor (wav));
        expect (source.render().getMagnitude (0, 0, 10000) > 0.01f);
        const auto file = temp.file ("missing.diydaw");
        if (! expectSuccess (source.project.saveTo (file)))
            return;
        expect (wav.deleteFile());
        DrumFixture loaded;
        if (! expectSuccess (loaded.project.loadFrom (file)))
            return;
        expect (Project::isDrumTrack (loaded.project.getTrack (0)));
        expect (loaded.pool.get (wav) == nullptr);
        expectSilent (loaded.render());
    }

    void demo()
    {
        beginTest ("The hand-written drums demo loads a drum rack and renders non-silent audio");
        const auto file = juce::File (__FILE__).getParentDirectory().getParentDirectory()
                             .getChildFile ("examples/drums-demo.diydaw");
        DrumFixture f;
        if (! expectSuccess (f.project.loadFrom (file)))
            return;
        expectEquals (f.project.getNumTracks(), 1);
        const auto track = f.project.getTrack (0);
        expect (Project::isDrumTrack (track));
        expectEquals (Project::getClipLength (track.getChildWithName (daw::ids::CLIP)), 16.0);
        const auto audio = f.render (2.0);
        expect (audio.getMagnitude (0, 0, audio.getNumSamples()) > 0.01f);
        expect (audio.getMagnitude (1, 0, audio.getNumSamples()) > 0.01f);
    }

    void importedInstruments()
    {
        beginTest ("Imported mixed instrument chains retain the first instrument and drop drums on audio tracks");
        DrumDirectory temp;
        const auto file = temp.file ("instruments.diydaw");
        expect (file.getParentDirectory().createDirectory().wasOk());
        expect (file.replaceWithText (R"xml(<PROJECT version="1" bpm="120">
  <TRACK kind="midi" name="Drums first">
    <CLIP kind="midi" start="0" length="4"><NOTE pitch="36" start="0.125" length="0.1" velocity="1"/></CLIP>
    <DEVICE type="drumrack" uid="1" pad1File="builtin:kick"/>
    <DEVICE type="synth" uid="2"/>
    <DEVICE type="eq" uid="3"/>
  </TRACK>
  <TRACK kind="midi" name="Synth first">
    <DEVICE type="synth" uid="4"/>
    <DEVICE type="drumrack" uid="5" pad1File="builtin:kick"/>
  </TRACK>
  <TRACK kind="audio" name="Audio">
    <DEVICE type="drumrack" uid="6" pad1File="builtin:kick"/>
    <DEVICE type="delay" uid="7"/>
  </TRACK>
</PROJECT>)xml"));
        DrumFixture f;
        if (! expectSuccess (f.project.loadFrom (file)))
            return;
        expectEquals (f.project.getNumTracks(), 3);
        for (int i = 0; i < 3; ++i)
        {
            const auto track = f.project.getTrack (i);
            expectEquals (instrumentCount (track), i == 2 ? 0 : 1);
            auto& engineTrack = f.project.getEngineTrack (track);
            const auto expected = i == 0 ? InstrumentType::drumRack : i == 1 ? InstrumentType::synth : InstrumentType::none;
            expect (engineTrack.getInstrumentType() == expected);
            if (i < 2)
            {
                expect (track.getChild (0) == Project::getDevice (track));
                expectEquals (Project::getDeviceType (Project::getDevice (track)), juce::String (i == 0 ? "drumrack" : "synth"));
            }
            else
            {
                expect (! Project::getDevice (track).isValid());
                expect (engineTrack.getInstrument() == nullptr);
            }
        }
        expectEquals (f.project.getEffects (f.project.getTrack (0)).size(), 1);
        expectEquals (f.project.getEffects (f.project.getTrack (2)).size(), 1);
        const auto rendered = f.render();
        expectAudio (rendered, sampleReference (*f.pool.get ("builtin:kick"), rendered.getNumSamples()), "First drum instrument retained");
    }

    void exportAudio()
    {
        beginTest ("ProjectRenderer matches live drum audio and retains the original builtin and WAV kit snapshot");
        DrumDirectory temp;
        const auto wav = temp.file ("export pad.wav");
        if (! writeWav (wav))
            return;
        DrumFixture f;
        const auto sample = decodeWav (f, wav);
        if (sample == nullptr)
            return;
        const auto track = f.addDrums();
        const auto device = Project::getDevice (track);
        f.project.setPadSample (device, 15, daw::SamplePool::keyFor (wav));
        f.project.setDeviceParam (device, Rack::paramIndex (0, Rack::gain), -4.0f);
        f.project.setDeviceParam (device, Rack::paramIndex (2, Rack::pan), -0.25f);
        f.project.setDeviceParam (device, Rack::paramIndex (15, Rack::tune), 3.0f);
        const auto note = f.addHit (track);
        f.project.addNote (note.getParent(), 38, 0.75, 0.1, 0.7f);
        f.project.addNote (note.getParent(), 51, 1.5, 0.1, 0.6f);
        daw::ExportSettings settings;
        settings.sampleRate = sampleRate;
        settings.startBeat = 0.0;
        settings.endBeat = 4.0;
        daw::ProjectRenderer renderer (f.project, settings);
        expectEquals (renderer.getLengthInSamples(), static_cast<int64_t> (96000));
        const auto live = f.render (2.0);
        expect (live.getMagnitude (0, 0, live.getNumSamples()) > 0.01f);
        expectAudio (renderer.render(), live, "Export equals live render", 0.0f);

        // Replacing a pool entry and editing the source must not change the export's
        // already published kit. This checks sample ownership in the private copy.
        f.pool.insert (daw::SamplePool::keyFor (wav), f.pool.get ("builtin:snare"));
        for (const auto pad : { 0, 2, 15 })
            f.project.clearPad (device, pad);
        expectSilent (f.render (2.0));
        expectAudio (renderer.render(), live, "Private kit survives source and pool edits", 0.0f);
    }
};

/** Checks lazy builtin cache behavior on the test thread, without model prepopulation. */
struct DrumTrackSamplePoolTests : DrumTest
{
    DrumTrackSamplePoolTests() : DrumTest ("Drum track builtin sample pool") {}

    void runTest() override
    {
        beginTest ("Builtin keys are created on demand and repeated get calls share the same instance");
        daw::SamplePool pool;
        const auto& constPool = pool;
        for (const auto& soundName : daw::getBuiltinSampleNames())
        {
            const auto key = daw::builtinSampleKey (soundName);
            const auto first = constPool.get (key);
            expect (first != nullptr, key);
            expect (first == constPool.get (key), key + " is cached");
            if (first != nullptr)
            {
                expectEquals (first->name, soundName);
                expectEquals (first->sampleRate, sampleRate);
                expect (first->getNumFrames() > 0);
                expect (first->buffer.getMagnitude (0, 0, first->getNumFrames()) > 0.01f);
            }
        }

        beginTest ("Unknown builtin names and non-builtin keys return nullptr");
        for (const auto* key : { "builtin:no-such-drum", "builtin:", "builtin:Kick", "kick", "" })
        {
            expect (constPool.get (juce::String (key)) == nullptr, key);
            expect (constPool.get (juce::String (key)) == nullptr, "Repeated unknown lookup");
        }
    }
};

static DrumTrackModelTests drumTrackModelTests;
static DrumTrackFileTests drumTrackFileTests;
static DrumTrackSamplePoolTests drumTrackSamplePoolTests;
} // namespace
