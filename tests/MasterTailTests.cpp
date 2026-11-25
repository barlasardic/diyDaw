#include "ProjectRenderer.h"
#include "Effects.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace
{
using Project = daw::Project;
using Audio = juce::AudioBuffer<float>;
using EQ = daw::EqEffect;
using Compressor = daw::CompressorEffect;
using Delay = daw::DelayEffect;
using Reverb = daw::ReverbEffect;
constexpr double testRate = 48000.0;
constexpr int testBlock = 1024;

/** Model edits, simulated audio callbacks and file operations run on the test thread. */
struct MasterTailFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    Project project { engine, pool };
    const juce::File constantFile { "/virtual/master-tail-constant.wav" };
    const juce::File sineFile { "/virtual/master-tail-sine.wav" };

    MasterTailFixture()
    {
        engine.prepare (testRate, testBlock);
        engine.getMetronome().setEnabled (false);
        for (const auto& file : { constantFile, sineFile })
        {
            auto sample = std::make_shared<daw::SampleData>();
            sample->sampleRate = testRate;
            sample->buffer.setSize (2, static_cast<int> (testRate * 3.0));
            for (int i = 0; i < sample->getNumFrames(); ++i)
            {
                const auto value = file == constantFile ? 0.2f
                    : static_cast<float> (0.2 * std::sin (juce::MathConstants<double>::twoPi * 431.0 * i / testRate));
                sample->buffer.setSample (0, i, value);
                sample->buffer.setSample (1, i, value);
            }
            pool.insert (daw::SamplePool::keyFor (file), sample);
        }
    }

    juce::ValueTree addAudio (bool sine = false)
    {
        const auto track = project.addTrack ("Test signal");
        project.addClip (track, sine ? sineFile : constantFile, 0.0);
        return track;
    }

    daw::ExportSettings settings (int rangeSamples = 12288, int tailSamples = 0) const
    {
        daw::ExportSettings result;
        result.sampleRate = testRate;
        result.bitDepth = 32;
        result.endBeat = project.secondsToBeats (rangeSamples / testRate);
        result.tailSeconds = tailSamples / testRate;
        return result;
    }

    Audio live (int count = 16384)
    {
        auto& transport = engine.getTransport();
        transport.stop();
        engine.prepare (testRate, testBlock);
        Audio warmUp (2, testBlock);
        for (int done = 0; done < static_cast<int> (testRate * 0.05); done += testBlock)
            engine.render (warmUp, testBlock);
        transport.setPositionBeats (0.0);
        transport.play();
        return daw::renderOffline (engine, count / testRate, testBlock);
    }

    Audio bounce (const daw::ExportSettings& s)
    {
        daw::ProjectRenderer renderer (project, s);
        return renderer.render();
    }

    juce::ValueTree delay (const juce::ValueTree& parent, float milliseconds = 100.0f, float feedback = 0.5f)
    {
        const auto device = project.addEffect (parent, "delay");
        project.setDeviceParam (device, Delay::sync, 0.0f);
        project.setDeviceParam (device, Delay::timeMs, milliseconds);
        project.setDeviceParam (device, Delay::feedback, feedback);
        project.setDeviceParam (device, Delay::mix, 1.0f);
        project.setDeviceParam (device, Delay::lowCut, 20.0f);
        project.setDeviceParam (device, Delay::highCut, 20000.0f);
        return device;
    }

    juce::ValueTree reverb (const juce::ValueTree& parent)
    {
        const auto device = project.addEffect (parent, "reverb");
        project.setDeviceParam (device, Reverb::size, 0.9f);
        project.setDeviceParam (device, Reverb::damping, 0.1f);
        project.setDeviceParam (device, Reverb::mix, 0.6f);
        return device;
    }
};

/** Temporary files are removed after all test readers and writers have closed. */
struct MasterTailDirectory
{
    juce::File directory = juce::File::createTempFile ("-master-tail");
    ~MasterTailDirectory() { directory.deleteRecursively(); }
    juce::File file (const juce::String& name) const { return directory.getChildFile (name); }
};

float audioDifference (const Audio& a, const Audio& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return std::numeric_limits<float>::infinity();
    float difference = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const auto x = a.getSample (ch, i), y = b.getSample (ch, i);
            if (! std::isfinite (x) || ! std::isfinite (y))
                return std::numeric_limits<float>::infinity();
            difference = std::max (difference, std::abs (x - y));
        }
    return difference;
}

void setCompressor (Project& project, const juce::ValueTree& device)
{
    project.setDeviceParam (device, Compressor::threshold, -12.0f);
    project.setDeviceParam (device, Compressor::ratio, 10.0f);
    project.setDeviceParam (device, Compressor::knee, 0.0f);
    project.setDeviceParam (device, Compressor::attack, 0.1f);
    project.setDeviceParam (device, Compressor::release, 10.0f);
}

/** Shared assertions, called only by sequential unit tests. */
class MasterTailTestBase : public juce::UnitTest
{
public:
    explicit MasterTailTestBase (const juce::String& testName) : juce::UnitTest (testName, "diyDaw") {}

protected:
    bool success (const juce::Result& result)
    {
        expect (result.wasOk(), result.getErrorMessage());
        return result.wasOk();
    }

    bool writeXml (const juce::File& file, const juce::String& xml)
    {
        const auto ok = file.getParentDirectory().createDirectory().wasOk() && file.replaceWithText (xml);
        expect (ok, "Write XML fixture");
        return ok;
    }

    Audio readWav (const juce::File& file, int expectedSamples)
    {
        juce::WavAudioFormat wav;
        auto stream = file.createInputStream();
        std::unique_ptr<juce::AudioFormatReader> reader (stream == nullptr ? nullptr : wav.createReaderFor (stream.release(), true));
        expect (reader != nullptr, "Readable exported WAV");
        if (reader == nullptr)
            return {};
        expectEquals (reader->lengthInSamples, static_cast<juce::int64> (expectedSamples), "WAV frame count");
        expectEquals (reader->sampleRate, testRate);
        expectEquals (static_cast<int> (reader->numChannels), 2);
        Audio result (2, static_cast<int> (reader->lengthInSamples));
        expect (reader->read (&result, 0, result.getNumSamples(), 0, true, true));
        return result;
    }

    void equalAudio (const Audio& a, const Audio& b, float tolerance = 0.0f)
    {
        const auto difference = audioDifference (a, b);
        expect (difference <= tolerance, "Stereo audio difference = " + juce::String (difference, 10));
    }

    void structure (Project& project, int expectedTracks)
    {
        const auto state = project.getState();
        int masters = 0, tracks = 0;
        for (const auto& child : state)
        {
            if (Project::isMaster (child))
                ++masters;
            if (child.hasType (daw::ids::TRACK))
            {
                expect (project.getTrack (tracks) == child, "getTrack indexes TRACK children only");
                expectEquals (project.indexOfTrack (child), tracks);
                expectEquals (static_cast<int> (child[daw::ids::engineIndex]), tracks);
                ++tracks;
            }
        }
        expectEquals (masters, 1);
        expectEquals (tracks, expectedTracks);
        expectEquals (project.getNumTracks(), expectedTracks);
        expectEquals (project.getEngine().getNumTracks(), expectedTracks);
        expect (state.getChild (state.getNumChildren() - 1) == project.getMaster(), "MASTER is last");
        expectEquals (project.indexOfTrack (project.getMaster()), -1);
        expect (! project.getTrack (-1).isValid());
        expect (! project.getTrack (expectedTracks).isValid());
    }

    void uniqueUids (const Project& project)
    {
        juce::Array<int> used;
        for (const auto& parent : project.getState())
            for (const auto& device : parent)
                if (device.hasType (daw::ids::DEVICE))
                {
                    const auto uid = static_cast<int> (device[daw::ids::uid]);
                    expect (uid > 0 && ! used.contains (uid), "Positive project-wide unique DEVICE uid");
                    used.add (uid);
                }
    }
};

/** Exercises MASTER structure, persistence and undo on the test thread. */
class MasterEffectsModelTests final : public MasterTailTestBase
{
public:
    MasterEffectsModelTests() : MasterTailTestBase ("Master effects model and persistence") {}

    void runTest() override
    {
        defaultsAndMalformedXml();
        undoAndRedo();
        persistenceAndReset();
        automationIsolation();
    }

private:
    void defaultsAndMalformedXml()
    {
        beginTest ("New documents have one last MASTER; added TRACKs keep indices and unique device uids");
        MasterTailFixture f;
        structure (f.project, 0);
        const auto master = f.project.getMaster();
        f.project.addEffect (master, "eq");
        const auto midi = f.project.addTrack ("MIDI", Project::TrackKind::midi);
        f.project.addEffect (midi, "delay");
        f.project.addEffect (master, "reverb");
        const auto audio = f.project.addTrack ("Audio");
        f.project.addEffect (audio, "compressor");
        structure (f.project, 2);
        expect (f.project.getMaster() == master);
        uniqueUids (f.project);
        f.project.newDocument (2, 2);
        structure (f.project, 4);
        expect (f.project.getEffects (f.project.getMaster()).isEmpty());

        MasterTailDirectory temp;
        for (const auto& xml : {
            juce::String (R"(<PROJECT version="1"><TRACK name="Legacy"/></PROJECT>)"),
            juce::String (R"(<PROJECT version="1"><MASTER><DEVICE type="eq" uid="7" output="-6"/></MASTER><TRACK name="First"/><TRACK kind="midi" name="Second"><DEVICE type="synth" uid="7"/></TRACK></PROJECT>)"),
            juce::String (R"(<PROJECT version="1"><MASTER><JUNK/><CLIP/><TRACK/><AUTOMATION target="gain"/><DEVICE type="synth"/><DEVICE type="unknown"/><DEVICE type="eq" uid="4" output="-99" enabled="0"/><DEVICE type="delay" uid="4" sync="0.8"/></MASTER><TRACK name="First"><DEVICE type="eq" uid="4"/></TRACK><MASTER/><TRACK kind="midi" name="Second"><DEVICE type="synth"/></TRACK></PROJECT>)") })
        {
            beginTest ("Hand-written XML normalises MASTER placement, duplicates, children, parameters and uids");
            const auto file = temp.file ("malformed.diydaw");
            if (! writeXml (file, xml) || ! success (f.project.loadFrom (file)))
                continue;
            structure (f.project, xml.contains ("Legacy") ? 1 : 2);
            uniqueUids (f.project);
            for (const auto& device : f.project.getMaster())
            {
                expect (Project::isEffect (device));
                expect (daw::getEffectTypes().contains (Project::getDeviceType (device)));
                expect (f.project.getEffectInstance (device) != nullptr);
            }
            if (xml.contains ("-99"))
            {
                const auto effects = f.project.getEffects (f.project.getMaster());
                expectEquals (effects.size(), 2);
                if (effects.size() == 2)
                {
                    expectEquals (Project::getDeviceParam (effects[0], EQ::output), -18.0f);
                    expect (! Project::isEffectEnabled (effects[0]));
                    expectEquals (Project::getDeviceParam (effects[1], Delay::sync), 1.0f);
                }
            }
            f.project.addTrack ("Inserted before MASTER", Project::TrackKind::midi);
            f.project.addEffect (f.project.getMaster(), "compressor");
            structure (f.project, xml.contains ("Legacy") ? 2 : 3);
            uniqueUids (f.project);
        }
    }

    void undoAndRedo()
    {
        beginTest ("Master add, parameter gesture, bypass, move and remove undo and redo into the live engine");
        MasterTailFixture f;
        f.addAudio();
        const auto master = f.project.getMaster();
        const auto dry = f.live();
        auto previous = master.createCopy();
        auto previousAudio = dry;
        const auto verifyGesture = [&] (const juce::ValueTree& before, const Audio& beforeAudio)
        {
            const auto after = master.createCopy();
            const auto afterAudio = f.live();
            expect (f.project.undo());
            expect (master.isEquivalentTo (before), "Undo restores the MASTER tree");
            equalAudio (f.live(), beforeAudio);
            expect (f.project.redo());
            expect (master.isEquivalentTo (after), "Redo restores the MASTER tree");
            equalAudio (f.live(), afterAudio);
            previous = after;
            previousAudio = afterAudio;
        };
        f.project.beginTransaction ("Add master EQ");
        const auto eq = f.project.addEffect (master, "eq");
        verifyGesture (previous, previousAudio);
        f.project.beginTransaction ("Output gesture");
        f.project.setDeviceParam (eq, EQ::output, -3.0f);
        f.project.setDeviceParam (eq, EQ::output, -6.0f);
        verifyGesture (previous, previousAudio);
        expect (audioDifference (f.live(), dry) > 0.05f, "Parameter edit is audible");
        f.project.beginTransaction ("Bypass");
        f.project.setEffectEnabled (eq, false);
        verifyGesture (previous, previousAudio);
        equalAudio (f.live(), dry);
        f.project.beginTransaction ("Enable and add compressor");
        f.project.setEffectEnabled (eq, true);
        const auto compressor = f.project.addEffect (master, "compressor");
        setCompressor (f.project, compressor);
        verifyGesture (previous, previousAudio);
        f.project.beginTransaction ("Move compressor");
        f.project.moveEffect (compressor, 0);
        verifyGesture (previous, previousAudio);
        expect (f.project.getEffects (master)[0] == compressor);
        f.project.beginTransaction ("Remove EQ");
        f.project.removeEffect (eq);
        verifyGesture (previous, previousAudio);
        expectEquals (f.project.getEffects (master).size(), 1);
    }

    void persistenceAndReset()
    {
        beginTest ("Save/load preserves master order, uids, bypass and exactly identical rendered audio");
        MasterTailFixture f;
        MasterTailDirectory temp;
        f.addAudio (true);
        const auto eq = f.project.addEffect (f.project.getMaster(), "eq");
        f.project.setDeviceParam (eq, EQ::output, -6.0f);
        f.delay (f.project.getMaster(), 73.0f, 0.4f);
        f.reverb (f.project.getMaster());
        const auto bypassed = f.project.addEffect (f.project.getMaster(), "compressor");
        f.project.setEffectEnabled (bypassed, false);
        f.project.setMasterGain (-3.0f);
        const auto masterBefore = f.project.getMaster().createCopy();
        const auto s = f.settings (16384, 8192);
        const auto reference = f.bounce (s);
        const auto liveReference = f.live();
        const auto projectFile = temp.file ("round-trip.diydaw");
        if (success (f.project.saveTo (projectFile)) && success (f.project.loadFrom (projectFile)))
        {
            structure (f.project, 1);
            uniqueUids (f.project);
            expect (f.project.getMaster().isEquivalentTo (masterBefore));
            equalAudio (f.bounce (s), reference);
            equalAudio (f.live(), liveReference);
            daw::ProjectRenderer renderer (f.project, s);
            const auto wav = temp.file ("master.wav");
            if (success (renderer.renderToFile (wav)))
                equalAudio (readWav (wav, reference.getNumSamples()), reference);
        }

        beginTest ("newDocument clears master inserts in both the tree and the engine");
        f.project.newDocument (0);
        structure (f.project, 0);
        expect (f.project.getEffects (f.project.getMaster()).isEmpty());
        f.addAudio();
        MasterTailFixture dry;
        dry.addAudio();
        equalAudio (f.live(), dry.live());

        beginTest ("Loading legacy XML without MASTER clears an already audible master chain");
        const auto attenuator = f.project.addEffect (f.project.getMaster(), "eq");
        f.project.setDeviceParam (attenuator, EQ::output, -12.0f);
        expect (audioDifference (f.live(), dry.live()) > 0.05f);
        const auto legacy = temp.file ("legacy.diydaw");
        const auto xml = "<PROJECT version=\"1\"><TRACK><CLIP file=\"" + f.constantFile.getFullPathName()
                       + "\" start=\"0\" length=\"6\"/></TRACK></PROJECT>";
        if (writeXml (legacy, xml) && success (f.project.loadFrom (legacy)))
        {
            structure (f.project, 1);
            expect (f.project.getEffects (f.project.getMaster()).isEmpty());
            equalAudio (f.live(), dry.live());
        }
    }

    void automationIsolation()
    {
        beginTest ("Master effects expose no automation targets on themselves or on tracks");
        MasterTailFixture f;
        const auto track = f.addAudio();
        const auto eq = f.project.addEffect (f.project.getMaster(), "eq");
        f.project.setDeviceParam (eq, EQ::output, -6.0f);
        expect (Project::getAutomationTargets (f.project.getMaster()).empty());
        const auto target = Project::getDeviceParamTargetId (eq, EQ::output);
        const auto targets = Project::getAutomationTargets (track);
        expectEquals (static_cast<int> (targets.size()), 2);
        for (const auto& t : targets)
            expect (t.id != target);
        Project::AutomationTarget found;
        expect (! Project::findAutomationTarget (track, target, found));

        beginTest ("Automation XML naming a master uid is ignored safely and leaves its parameter unchanged");
        const auto reference = f.live();
        auto state = f.project.getState().createCopy();
        juce::ValueTree lane (daw::ids::AUTOMATION), point (daw::ids::POINT);
        lane.setProperty (daw::ids::target, target, nullptr);
        point.setProperty (daw::ids::beat, 0.0, nullptr);
        point.setProperty (daw::ids::value, 18.0, nullptr);
        lane.appendChild (point, nullptr);
        state.getChild (0).appendChild (lane, nullptr);
        MasterTailDirectory temp;
        const auto file = temp.file ("foreign-target.diydaw");
        if (writeXml (file, state.createXml()->toString()) && success (f.project.loadFrom (file)))
        {
            equalAudio (f.live(), reference);
            const auto device = f.project.getEffects (f.project.getMaster())[0];
            const auto instance = f.project.getEffectInstance (device);
            expect (instance != nullptr);
            if (instance != nullptr)
                expectEquals (instance->getParam (EQ::output), -6.0f);
        }
    }
};

/** Verifies master routing using live, device-independent Engine renders. */
class MasterEffectsAudioTests final : public MasterTailTestBase
{
public:
    MasterEffectsAudioTests() : MasterTailTestBase ("Master effects audio routing") {}

    void runTest() override
    {
        beginTest ("Master EQ attenuates the stereo sum and export includes the same master processing");
        MasterTailFixture f;
        f.addAudio();
        f.addAudio();
        const auto dry = f.live();
        expectWithinAbsoluteError (dry.getSample (0, 8192), 0.4f / std::sqrt (2.0f), 1.0e-6f);
        const auto eq = f.project.addEffect (f.project.getMaster(), "eq");
        f.project.setDeviceParam (eq, EQ::output, -6.0f);
        auto expected = dry;
        expected.applyGain (juce::Decibels::decibelsToGain (-6.0f));
        equalAudio (f.live(), expected, 1.0e-6f);
        equalAudio (f.bounce (f.settings (16384)), expected, 1.0e-6f);

        beginTest ("Bypass crossfades to dry over 10 ms with no instantaneous level jump");
        f.live();
        f.project.setEffectEnabled (eq, false);
        const auto transition = daw::renderOffline (f.engine, 0.03, testBlock);
        const auto wet = expected.getSample (0, 8192), dryLevel = dry.getSample (0, 8192);
        expectWithinAbsoluteError (transition.getSample (0, 0), wet, 0.001f);
        expectWithinAbsoluteError (transition.getSample (0, 239), 0.5f * (wet + dryLevel), 0.002f);
        expectWithinAbsoluteError (transition.getSample (0, 600), dryLevel, 1.0e-6f);
        float largestStep = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 1; i < transition.getNumSamples(); ++i)
                largestStep = std::max (largestStep, std::abs (transition.getSample (ch, i) - transition.getSample (ch, i - 1)));
        expect (largestStep < 0.001f, "Crossfade has small per-sample changes");

        beginTest ("Master compressor reacts to the sum of two individually sub-threshold tracks");
        MasterTailFixture compressed;
        const auto first = compressed.addAudio();
        const auto second = compressed.addAudio();
        const auto compressor = compressed.project.addEffect (compressed.project.getMaster(), "compressor");
        setCompressor (compressed.project, compressor);
        const auto masterAudio = compressed.live();
        const auto sum = 0.4f / std::sqrt (2.0f);
        const auto inputDb = juce::Decibels::gainToDecibels (sum);
        const auto compressedLevel = sum * juce::Decibels::decibelsToGain (-(inputDb + 12.0f) * 0.9f);
        expectWithinAbsoluteError (masterAudio.getSample (0, 8192), compressedLevel, 1.0e-5f);
        compressed.project.removeEffect (compressor);
        setCompressor (compressed.project, compressed.project.addEffect (first, "compressor"));
        setCompressor (compressed.project, compressed.project.addEffect (second, "compressor"));
        expectWithinAbsoluteError (compressed.live().getSample (0, 8192), sum, 1.0e-5f);
        expect (sum > compressedLevel * 1.05f, "Separate track compression differs from master compression");

        beginTest ("Master insert order matters and the master fader scales after compression");
        MasterTailFixture ordered;
        ordered.addAudio();
        const auto boost = ordered.project.addEffect (ordered.project.getMaster(), "eq");
        ordered.project.setDeviceParam (boost, EQ::output, 12.0f);
        const auto comp = ordered.project.addEffect (ordered.project.getMaster(), "compressor");
        setCompressor (ordered.project, comp);
        const auto boostThenCompress = ordered.live();
        ordered.project.moveEffect (comp, 0);
        const auto compressThenBoost = ordered.live();
        expect (compressThenBoost.getSample (0, 8192) > boostThenCompress.getSample (0, 8192) * 1.5f);
        ordered.project.moveEffect (comp, 1);
        ordered.project.setMasterGain (-6.0f);
        auto scaled = boostThenCompress;
        scaled.applyGain (juce::Decibels::decibelsToGain (-6.0f));
        equalAudio (ordered.live(), scaled, 1.0e-6f);
    }
};

/** Checks offline tail duration, stopped sources and callbacks on the test thread. */
class ExportTailTests final : public MasterTailTestBase
{
public:
    ExportTailTests() : MasterTailTestBase ("Export tail rendering and files") {}

    void runTest() override
    {
        lengthsAndStoppedSources();
        effectTails();
        autoTail();
        normalisation();
        progressAndCancellation();
    }

private:
    void lengthsAndStoppedSources()
    {
        MasterTailFixture f;
        f.addAudio();
        for (const auto range : { 12288, 12000 })
            for (const auto tail : { 0, 37, 9600 })
            {
                beginTest ("Exact fixed length and stopped audio: range=" + juce::String (range) + ", tail=" + juce::String (tail));
                const auto s = f.settings (range, tail);
                daw::ProjectRenderer renderer (f.project, s);
                expectEquals (renderer.getLengthInSamples(), static_cast<int64_t> (range));
                const auto audio = renderer.render();
                expectEquals (audio.getNumSamples(), range + tail);
                expect (audio.getMagnitude (0, range) > 0.1f, "Range contains the source");
                if (tail > 0 && audio.getNumSamples() >= range + tail)
                    expectEquals (audio.getMagnitude (range, tail), 0.0f, "A continuing audio clip must stop exactly at endBeat");
                MasterTailDirectory temp;
                const auto wav = temp.file ("fixed.wav");
                if (success (renderer.renderToFile (wav)))
                    equalAudio (readWav (wav, range + tail), audio);
            }

        for (const auto range : { 12288, 12000 })
        {
            beginTest ("Held MIDI note releases at range end: range=" + juce::String (range));
            MasterTailFixture midi;
            const auto track = midi.project.addTrack ("Held note", Project::TrackKind::midi);
            const auto clip = midi.project.addMidiClip (track, 0.0, 8.0);
            midi.project.addNote (clip, 69, 0.0, 8.0, 1.0f);
            midi.project.setDeviceParam (track, daw::Synth::ampAttack, 0.001f);
            midi.project.setDeviceParam (track, daw::Synth::ampSustain, 1.0f);
            midi.project.setDeviceParam (track, daw::Synth::ampRelease, 0.08f);
            const auto audio = midi.bounce (midi.settings (range, 24000));
            expect (audio.getMagnitude (0, range) > 0.01f);
            expect (audio.getMagnitude (range, 1024) > 0.001f, "Note releases with an audible decay");
            const auto latePeak = audio.getMagnitude (range + 12000, 12000);
            expect (latePeak < 1.0e-7f, "Released note finishes before the tail ends; late peak=" + juce::String (latePeak, 9));
        }
    }

    void effectTails()
    {
        for (const auto onMaster : { false, true })
            for (const auto reverb : { false, true })
            {
                beginTest (juce::String (onMaster ? "Master " : "Track ") + (reverb ? "reverb" : "delay") + " continues after the transport stops");
                MasterTailFixture f;
                const auto track = f.addAudio (true);
                const auto parent = onMaster ? f.project.getMaster() : track;
                if (reverb)
                    f.reverb (parent);
                else
                    f.delay (parent, 100.0f, 0.6f);
                const auto audio = f.bounce (f.settings (12288, 48000));
                expectEquals (audio.getNumSamples(), 60288);
                expect (audio.getMagnitude (12288, 4800) > 0.001f, "Effect remains audible after sources stop");
                expect (audio.getMagnitude (12288 + 4800, 4800) > 0.0001f, "Later echoes / reverberation remain audible");
                expect (audio.getRMSLevel (0, 12288 + 36000, 12000) < audio.getRMSLevel (0, 12288, 12000), "Tail decays");
            }
    }

    void expectAutoSilence (const Audio& audio, int range)
    {
        constexpr int quietSamples = static_cast<int> (testRate * 0.25);
        const auto threshold = juce::Decibels::decibelsToGain (daw::ExportSettings::silenceThresholdDb);
        const auto count = audio.getNumSamples();
        expect (count >= range + quietSamples, "Auto tail includes the silence hold time");
        if (count < range + quietSamples)
            return;
        expect (audio.getMagnitude (count - quietSamples, quietSamples) < threshold, "Last 250 ms are below -90 dBFS on both channels");
        int consecutive = 0, earliest = -1;
        for (int i = range; i < count; ++i)
        {
            const auto peak = std::max (std::abs (audio.getSample (0, i)), std::abs (audio.getSample (1, i)));
            consecutive = peak < threshold ? consecutive + 1 : 0;
            if (consecutive >= quietSamples)
            {
                earliest = i + 1;
                break;
            }
        }
        expect (earliest >= 0);
        if (earliest >= 0)
            expect (count - earliest < testBlock, "Ends within one block of 250 ms continuous silence; excess samples=" + juce::String (count - earliest));
    }

    void autoTail()
    {
        for (const auto range : { 12288, 12000 })
        {
            beginTest ("Silent project auto tail ends about 250 ms after range=" + juce::String (range));
            MasterTailFixture silent;
            auto s = silent.settings (range);
            s.autoTail = true;
            const auto audio = silent.bounce (s);
            expectAutoSilence (audio, range);
            expectEquals (audio.getMagnitude (0, audio.getNumSamples()), 0.0f);
        }

        for (const auto range : { 12288, 12000 })
        {
            beginTest ("Continuing audio stops before the dry auto tail: range=" + juce::String (range));
            MasterTailFixture drySource;
            drySource.addAudio();
            auto drySettings = drySource.settings (range);
            drySettings.autoTail = true;
            const auto audio = drySource.bounce (drySettings);
            expectAutoSilence (audio, range);
            expect (audio.getNumSamples() < range + static_cast<int> (testRate * 0.25) + testBlock,
                    "A dry source must stop at endBeat, not at its later clip end; actual length="
                        + juce::String (audio.getNumSamples()));
            if (audio.getNumSamples() > range)
                expectEquals (audio.getMagnitude (range, audio.getNumSamples() - range), 0.0f,
                              "Dry auto tail is silent from the exact range end");
        }

        beginTest ("A long reverb extends auto tail beyond a dry source and eventually reaches silence");
        MasterTailFixture f;
        f.addAudio (true);
        auto s = f.settings (24576);
        s.autoTail = true;
        const auto dry = f.bounce (s);
        expectAutoSilence (dry, 24576);
        f.reverb (f.project.getMaster());
        const auto wet = f.bounce (s);
        expect (wet.getNumSamples() > dry.getNumSamples() + 48000, "Reverb lasts at least one second longer");
        expect (wet.getNumSamples() < 24576 + 480000, "This reverb decays before the cap");
        expectAutoSilence (wet, 24576);

        beginTest ("High feedback delay that is still audible is capped at exactly ten seconds");
        MasterTailFixture capped;
        capped.addAudio (true);
        capped.delay (capped.project.getMaster(), 200.0f, 0.95f);
        const auto audio = capped.bounce (s);
        const auto maximum = 24576 + static_cast<int> (testRate * daw::ExportSettings::maxAutoTailSeconds);
        expectEquals (audio.getNumSamples(), maximum);
        if (audio.getNumSamples() >= 12000)
            expect (audio.getMagnitude (audio.getNumSamples() - 12000, 12000)
                        > juce::Decibels::decibelsToGain (daw::ExportSettings::silenceThresholdDb), "Cap is exercised by an audible tail");
    }

    void normalisation()
    {
        beginTest ("Normalised auto-tail passes agree on length and audio; peak is -0.3 dBFS");
        MasterTailFixture f;
        f.addAudio (true);
        f.reverb (f.project.getMaster());
        auto s = f.settings (24576);
        s.autoTail = true;
        daw::ProjectRenderer plain (f.project, s);
        const auto raw = plain.render();
        s.normalise = true;
        daw::ProjectRenderer normalised (f.project, s);
        equalAudio (normalised.render(), raw);
        MasterTailDirectory temp;
        const auto plainFile = temp.file ("plain.wav"), normalisedFile = temp.file ("normalised.wav");
        if (! success (plain.renderToFile (plainFile)) || ! success (normalised.renderToFile (normalisedFile)))
            return;
        equalAudio (readWav (plainFile, raw.getNumSamples()), raw);
        const auto audio = readWav (normalisedFile, raw.getNumSamples());
        const auto target = juce::Decibels::decibelsToGain (daw::ProjectRenderer::normaliseTargetDb);
        expectWithinAbsoluteError (audio.getMagnitude (0, audio.getNumSamples()), target, 2.0e-6f);
        auto expected = raw;
        expected.applyGain (target / raw.getMagnitude (0, raw.getNumSamples()));
        equalAudio (audio, expected, 2.0e-6f);
    }

    void checkProgress (const std::vector<double>& values)
    {
        expect (! values.empty());
        double previous = 0.0;
        int completions = 0;
        for (const auto value : values)
        {
            expect (std::isfinite (value) && value >= previous && value >= 0.0 && value <= 1.0, "Monotonic progress in [0, 1]");
            completions += juce::exactlyEqual (value, 1.0) ? 1 : 0;
            previous = value;
        }
        expectEquals (completions, 1, "Completion is reported exactly once");
        if (! values.empty())
            expectEquals (values.back(), 1.0, "Completion is the final callback");
    }

    void progressAndCancellation()
    {
        for (const auto automatic : { false, true })
            for (const auto normalise : { false, true })
            {
                beginTest ("Progress completes exactly once: auto=" + juce::String (automatic ? 1 : 0)
                           + ", normalise=" + juce::String (normalise ? 1 : 0));
                MasterTailFixture f;
                f.addAudio();
                auto s = f.settings (12288, 12288);
                s.autoTail = automatic;
                s.normalise = normalise;
                daw::ProjectRenderer renderer (f.project, s);
                std::vector<double> values;
                const auto progress = [&values] (double value) { values.push_back (value); return true; };
                renderer.render (progress);
                checkProgress (values);
                values.clear();
                MasterTailDirectory temp;
                if (success (renderer.renderToFile (temp.file ("complete.wav"), progress)))
                    checkProgress (values);
            }

        for (const auto automatic : { false, true })
            for (const auto normalise : { false, true })
                for (const auto secondPass : { false, true })
                {
                    if (! normalise && secondPass)
                        continue;
                    beginTest ("Cancel during tail: auto=" + juce::String (automatic ? 1 : 0)
                               + ", normalise=" + juce::String (normalise ? 1 : 0)
                               + ", second pass=" + juce::String (secondPass ? 1 : 0));
                    MasterTailFixture f;
                    f.addAudio();
                    auto s = f.settings (12288, 12288);
                    s.autoTail = automatic;
                    s.normalise = normalise;
                    const auto total = 12288.0 + (automatic ? testRate * daw::ExportSettings::maxAutoTailSeconds : 12288.0);
                    const auto scale = normalise ? 0.5 : 1.0;
                    const auto offset = secondPass ? 0.5 : 0.0;
                    const auto rangeEndProgress = offset + scale * 12288.0 / total;
                    std::vector<double> values;
                    bool cancelled = false;
                    daw::ProjectRenderer renderer (f.project, s);
                    MasterTailDirectory temp;
                    const auto file = temp.file ("cancelled.wav");
                    const auto result = renderer.renderToFile (file, [&] (double value)
                    {
                        values.push_back (value);
                        if (value > rangeEndProgress && (! secondPass || value > 0.5))
                        {
                            cancelled = true;
                            return false;
                        }
                        return true;
                    });
                    expect (cancelled, "Callback reached the tail of the requested pass");
                    expect (result.failed(), "Cancellation must fail the export");
                    expect (! file.existsAsFile(), "Cancelled export leaves no output file");
                    expect (temp.directory.findChildFiles (juce::File::findFiles, false).isEmpty(), "No temporary WAV is leaked");
                    expect (std::find (values.begin(), values.end(), 1.0) == values.end(), "Cancellation must not report completion");
                }
    }
};

static MasterEffectsModelTests masterEffectsModelTests;
static MasterEffectsAudioTests masterEffectsAudioTests;
static ExportTailTests exportTailTests;
} // namespace
