#include "Project.h"
#include "ProjectRenderer.h"
#include "Effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

#if JUCE_LINUX
 #include <cerrno>
 #include <sys/resource.h>
 #include <sys/wait.h>
 #include <unistd.h>
#endif

namespace
{
using Project = daw::Project;
using EQ = daw::EqEffect;
using Compressor = daw::CompressorEffect;
using Delay = daw::DelayEffect;
using Synth = daw::Synth;
using Audio = juce::AudioBuffer<float>;
constexpr double rate = 48000.0;
constexpr int preparedBlockSize = 1024;
constexpr std::array<int, 5> blockSizes { 1, 7, 64, 512, 4096 };

/** Test-thread fixture: model edits and simulated audio callbacks run sequentially. */
struct ChainFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    Project project { engine, pool };
    const juce::File constantFile { "/virtual/device-chain-constant.wav" };
    const juce::File impulseFile { "/virtual/device-chain-impulse.wav" };
    const juce::File sineFile { "/virtual/device-chain-sine.wav" };
    double sampleRate;

    explicit ChainFixture (double newRate = rate) : sampleRate (newRate)
    {
        engine.prepare (sampleRate, preparedBlockSize);
        insert (constantFile, 3.0, 0);
        insert (impulseFile, 0.05, 1);
        insert (sineFile, 3.0, 2);
    }

    void insert (const juce::File& file, double seconds, int shape)
    {
        auto sample = std::make_shared<daw::SampleData>();
        sample->sampleRate = sampleRate;
        sample->buffer.setSize (2, static_cast<int> (std::llround (sampleRate * seconds)));
        for (int i = 0; i < sample->getNumFrames(); ++i)
        {
            const auto value = shape == 0 ? 0.2f
                             : shape == 1 ? (i == 256 ? 0.5f : 0.0f)
                             : static_cast<float> (0.2 * std::sin (juce::MathConstants<double>::twoPi * 431.0 * i / sampleRate));
            sample->buffer.setSample (0, i, value);
            sample->buffer.setSample (1, i, shape == 2 ? -0.5f * value : value);
        }
        pool.insert (daw::SamplePool::keyFor (file), sample);
    }

    juce::ValueTree audioTrack (const juce::File& file)
    {
        const auto track = project.addTrack ("Signal");
        project.addClip (track, file, 0.0);
        return track;
    }

    Audio render (int count, int blockSize = preparedBlockSize)
    {
        return daw::renderOffline (engine, count / sampleRate, blockSize);
    }

    void restart (double startBeat = 0.0)
    {
        auto& transport = engine.getTransport();
        transport.stop();
        engine.prepare (sampleRate, preparedBlockSize);
        // Match ProjectRenderer: settle faders and parameters while stopped.
        for (int done = 0; done < static_cast<int> (sampleRate * 0.05); done += preparedBlockSize)
            render (preparedBlockSize);
        transport.setPositionBeats (startBeat);
        transport.play();
    }

    Audio freshRender (int count = 16384, double startBeat = 0.25)
    {
        restart (startBeat);
        return render (count);
    }
};

float maxDifference (const Audio& a, const Audio& b)
{
    if (a.getNumSamples() != b.getNumSamples() || a.getNumChannels() != b.getNumChannels())
        return std::numeric_limits<float>::infinity();

    float error = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const auto x = a.getSample (ch, i), y = b.getSample (ch, i);
            if (! std::isfinite (x) || ! std::isfinite (y))
                return std::numeric_limits<float>::infinity();
            error = std::max (error, std::abs (x - y));
        }
    return error;
}

void configureDelay (Project& project, const juce::ValueTree& device)
{
    project.setDeviceParam (device, Delay::sync, 0);
    project.setDeviceParam (device, Delay::timeMs, 100);
    project.setDeviceParam (device, Delay::feedback, 0);
    project.setDeviceParam (device, Delay::mix, 1);
}

void configureCompressor (Project& project, const juce::ValueTree& device)
{
    project.setDeviceParam (device, Compressor::threshold, -24);
    project.setDeviceParam (device, Compressor::ratio, 8);
    project.setDeviceParam (device, Compressor::attack, 0.1f);
    project.setDeviceParam (device, Compressor::release, 10);
    project.setDeviceParam (device, Compressor::knee, 0);
}

/** Model and audio assertions run only on the test thread. */
class DeviceChainModelTests : public juce::UnitTest
{
public:
    DeviceChainModelTests() : juce::UnitTest ("Device chain model and undo", "diyDaw") {}

    void runTest() override
    {
        defaultsAndInsertion();
        parameters();
        undoGestures();
        instanceLifetime();
    }

private:
    void defaultsAndInsertion()
    {
        ChainFixture f;
        for (const auto kind : { Project::TrackKind::audio, Project::TrackKind::midi })
        {
            beginTest ("Every effect has default parameters and is enabled on "
                       + juce::String (kind == Project::TrackKind::midi ? "MIDI" : "audio") + " tracks");
            const auto track = f.project.addTrack ("Chain", kind);
            // Interleave clips and devices so indices must be among effects, not children.
            if (kind == Project::TrackKind::midi)
                f.project.addMidiClip (track, 0, 1);
            else
                f.project.addClip (track, f.constantFile, 0);

            for (const auto& type : daw::getEffectTypes())
            {
                const auto device = f.project.addEffect (track, type);
                expect (device.hasType (daw::ids::DEVICE));
                expectEquals (Project::getDeviceType (device), type);
                expect (device.hasProperty (daw::ids::enabled) && Project::isEffectEnabled (device));
                const auto instance = f.project.getEffectInstance (device);
                expect (instance != nullptr);
                const auto& infos = Project::getDeviceParamInfos (device);
                expectEquals (static_cast<int> (infos.size()), static_cast<int> (daw::getEffectParamInfos (type).size()));
                for (size_t i = 0; i < infos.size(); ++i)
                {
                    const auto index = static_cast<int> (i);
                    expect (device.hasProperty (infos[i].id));
                    expectEquals (Project::getDeviceParam (device, index), infos[i].defaultValue, type + ": " + infos[i].id);
                    if (instance != nullptr)
                        expectEquals (instance->getParam (index), infos[i].defaultValue);
                }
                if (instance != nullptr)
                    expect (instance->isEnabled());
            }
            const auto original = f.project.getEffects (track);
            expectEquals (original.size(), 4);
            const auto front = f.project.addEffect (track, "eq", 0);
            const auto middle = f.project.addEffect (track, "delay", 2);
            const auto end = f.project.addEffect (track, "reverb", 100);
            auto effects = f.project.getEffects (track);
            expect (effects[0] == front && effects[1] == original[0] && effects[2] == middle);
            expect (effects.getLast() == end);
            expectEquals (effects.size(), 7);
            if (kind == Project::TrackKind::midi)
            {
                const auto synth = Project::getDevice (track);
                expect (synth == track.getChild (0));
                for (const auto& effect : effects)
                    expect (track.indexOf (effect) > track.indexOf (synth));
                expectEquals (static_cast<int> (Project::getDeviceParamInfos (synth).size()), static_cast<int> (Synth::numParams));
            }
            else
                expect (! Project::getDevice (track).isValid());

            beginTest ("Unknown effect types are refused without model or undo changes");
            const auto before = track.createCopy();
            f.project.getUndoManager().clearUndoHistory();
            expect (! f.project.addEffect (track, "unknown").isValid());
            expect (! f.project.addEffect (track, "synth").isValid());
            expect (! f.project.addEffect ({}, "eq").isValid());
            expect (track.isEquivalentTo (before));
            expect (! f.project.undo());

            beginTest ("Moves use effect indices in both directions and clamp to the ends");
            f.project.moveEffect (front, 6);
            effects = f.project.getEffects (track);
            expect (effects.getLast() == front);
            expect (effects[0] == original[0] && effects[1] == middle);
            f.project.moveEffect (front, -20);
            expect (f.project.getEffects (track)[0] == front);
            f.project.moveEffect (middle, 999);
            expect (f.project.getEffects (track).getLast() == middle);
            if (kind == Project::TrackKind::midi)
                expect (track.getChild (0) == Project::getDevice (track));
        }
    }

    void parameters()
    {
        beginTest ("Effect DEVICE setters clamp, round stepped values and update their live instance");
        ChainFixture f;
        const auto track = f.project.addTrack();
        for (const auto& type : daw::getEffectTypes())
        {
            const auto device = f.project.addEffect (track, type);
            const auto instance = f.project.getEffectInstance (device);
            if (instance == nullptr)
            {
                expect (false, "Missing instance for " + type);
                continue;
            }
            const auto& infos = Project::getDeviceParamInfos (device);
            for (size_t i = 0; i < infos.size(); ++i)
            {
                const auto index = static_cast<int> (i);
                const auto& info = infos[i];
                for (const auto value : { info.minValue - 10000, info.maxValue + 10000, info.minValue + 0.6f })
                {
                    auto expected = juce::jlimit (info.minValue, info.maxValue, value);
                    if (info.stepped)
                        expected = std::round (expected);
                    f.project.setDeviceParam (device, index, value);
                    expectEquals (Project::getDeviceParam (device, index), expected, type + ": " + info.id);
                    expectEquals (instance->getParam (index), expected);
                    // The base effect's direct setter must have the same range contract.
                    instance->setParam (index, value);
                    expectEquals (instance->getParam (index), expected);
                }
            }
            const auto before = device.createCopy();
            f.project.setDeviceParam (device, -1, 123);
            f.project.setDeviceParam (device, static_cast<int> (infos.size()), 123);
            expect (device.isEquivalentTo (before));
            expectEquals (Project::getDeviceParam (device, -1), 0.0f);
            expectEquals (Project::getDeviceParam (device, static_cast<int> (infos.size())), 0.0f);
            f.project.setEffectEnabled (device, false);
            expect (! Project::isEffectEnabled (device) && ! instance->isEnabled());
            f.project.setEffectEnabled (device, true);
            expect (Project::isEffectEnabled (device) && instance->isEnabled());
        }
    }

    template <typename Edit>
    void gesture (ChainFixture& f, const juce::String& actionName, Edit&& edit)
    {
        beginTest (actionName + " is one transaction and undo/redo restores model and rendered audio");
        const auto before = f.project.getState().createCopy();
        const auto beforeAudio = f.freshRender();
        f.project.getUndoManager().clearUndoHistory();
        f.project.beginTransaction (actionName);
        edit();
        f.project.beginTransaction ("Next gesture");
        const auto after = f.project.getState().createCopy();
        const auto afterAudio = f.freshRender();
        expect (! after.isEquivalentTo (before));
        expect (maxDifference (beforeAudio, afterAudio) > 0.001f, "Gesture must change the audible result");
        expect (f.project.undo());
        expect (f.project.getState().isEquivalentTo (before));
        expectWithinAbsoluteError (maxDifference (f.freshRender(), beforeAudio), 0.0f, 1.0e-6f);
        expect (! f.project.getUndoManager().canUndo(), "One undo consumes the entire gesture");
        expect (f.project.redo());
        expect (f.project.getState().isEquivalentTo (after));
        expectWithinAbsoluteError (maxDifference (f.freshRender(), afterAudio), 0.0f, 1.0e-6f);
        expect (! f.project.getUndoManager().canRedo(), "One redo consumes the entire gesture");
    }

    void undoGestures()
    {
        ChainFixture f;
        const auto track = f.audioTrack (f.constantFile);
        juce::ValueTree eq;
        gesture (f, "Add effect", [&]
        {
            eq = f.project.addEffect (track, "eq");
            f.project.setDeviceParam (eq, EQ::output, -6);
        });
        gesture (f, "Parameter drag", [&]
        {
            for (const auto value : { -3.0f, 1.0f, 4.0f, 18.0f })
                f.project.setDeviceParam (eq, EQ::output, value);
        });
        gesture (f, "Enable toggle", [&] { f.project.setEffectEnabled (eq, false); });
        f.project.setEffectEnabled (eq, true);
        const auto compressor = f.project.addEffect (track, "compressor", 0);
        configureCompressor (f.project, compressor);
        gesture (f, "Move effect", [&] { f.project.moveEffect (eq, 0); });
        gesture (f, "Remove effect", [&] { f.project.removeEffect (eq); });
    }

    void instanceLifetime()
    {
        beginTest ("Unrelated chain edits retain the same delay instance and its pending echo");
        ChainFixture f;
        const auto track = f.audioTrack (f.impulseFile);
        const auto delay = f.project.addEffect (track, "delay");
        configureDelay (f.project, delay);
        const auto instance = f.project.getEffectInstance (delay);
        expect (instance != nullptr);
        f.restart();
        const auto before = f.render (2048);
        expectEquals (before.getMagnitude (0, before.getNumSamples()), 0.0f);
        const auto eq = f.project.addEffect (track, "eq", 0);
        const auto compressor = f.project.addEffect (track, "compressor");
        f.project.moveEffect (compressor, 0);
        f.project.removeEffect (compressor);
        f.project.setDeviceParam (eq, EQ::output, -6);
        expect (f.project.getEffectInstance (delay) == instance);
        const auto echo = f.render (6000);
        // The impulse has already entered the delay. A newly inserted EQ must not alter it.
        expectWithinAbsoluteError (echo.getSample (0, 256 + 4800 - 2048),
                                   0.5f * std::sqrt (0.5f), 1.0e-6f);

        beginTest ("Remove plus undo creates a fresh delay instance, deliberately discarding DSP history");
        f.restart();
        f.render (2048);
        f.project.beginTransaction ("Remove delay");
        f.project.removeEffect (delay);
        expect (f.project.getEffectInstance (delay) == nullptr);
        expect (f.project.undo());
        const auto replacement = f.project.getEffectInstance (delay);
        expect (replacement != nullptr && replacement != instance,
                "Undo restores parameters and order, but not the removed instance's delay memory");
        expectEquals (Project::getDeviceParam (delay, Delay::timeMs), 100.0f);
        expectEquals (f.render (6000).getMagnitude (0, 6000), 0.0f, "The removed delay's pending echo is not resurrected");
    }
};

/** Device-chain signal, persistence and export tests, executed on the test thread. */
class DeviceChainAudioTests : public juce::UnitTest
{
public:
    DeviceChainAudioTests() : juce::UnitTest ("Device chain audio, persistence and export", "diyDaw") {}

    void runTest() override
    {
        signalPath();
        delayReadBoundary();
        roundTrip();
        importedDevices();
        exportTails();
    }

private:
    void signalPath()
    {
        beginTest ("EQ output gain reaches the engine on audio and MIDI tracks; removal restores dry");
        for (const bool midi : { false, true })
        {
            ChainFixture f;
            const auto track = midi ? f.project.addTrack ("Synth", Project::TrackKind::midi)
                                    : f.audioTrack (f.constantFile);
            if (midi)
            {
                const auto clip = f.project.addMidiClip (track, 0, 4);
                f.project.addNote (clip, 60, 0, 3, 0.8f);
            }
            const auto dry = f.freshRender (24000, 0);
            expect (dry.getMagnitude (0, dry.getNumSamples()) > 0.01f);
            const auto eq = f.project.addEffect (track, "eq");
            f.project.setDeviceParam (eq, EQ::output, -6);
            const auto wet = f.freshRender (24000, 0);
            auto expected = Audio (dry);
            expected.applyGain (juce::Decibels::decibelsToGain (-6.0f));
            expectWithinAbsoluteError (maxDifference (wet, expected), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (20.0f * std::log10 (wet.getRMSLevel (0, 12000, 12000)
                                                          / dry.getRMSLevel (0, 12000, 12000)), -6.0f, 0.001f);
            f.project.removeEffect (eq);
            expectWithinAbsoluteError (maxDifference (f.freshRender (24000, 0), dry), 0.0f, 1.0e-6f);
        }

        beginTest ("Compressor then EQ boost differs audibly from EQ boost then compressor");
        ChainFixture f;
        const auto track = f.audioTrack (f.constantFile);
        const auto compressor = f.project.addEffect (track, "compressor");
        configureCompressor (f.project, compressor);
        const auto eq = f.project.addEffect (track, "eq");
        f.project.setDeviceParam (eq, EQ::output, 18);
        const auto boostAfter = f.freshRender();
        f.project.moveEffect (eq, 0);
        const auto boostBefore = f.freshRender();
        expect (boostAfter.getRMSLevel (0, 8192, 8192) > 5 * boostBefore.getRMSLevel (0, 8192, 8192));
        f.project.moveEffect (eq, 1);
        expectWithinAbsoluteError (maxDifference (f.freshRender(), boostAfter), 0.0f, 1.0e-6f);

        beginTest ("Disabling an effect crossfades continuously to exact dry within ten ms");
        f.project.removeEffect (compressor);
        f.project.setDeviceParam (eq, EQ::output, -12);
        const auto settled = f.freshRender (4096);
        f.project.setEffectEnabled (eq, false);
        const auto bypass = f.render (2048);
        const auto dryLevel = 0.2f * std::sqrt (0.5f);
        const auto oldLevel = settled.getSample (0, 4095);
        expect (bypass.getSample (0, 0) > oldLevel && bypass.getSample (0, 0) < dryLevel);
        float step = std::abs (bypass.getSample (0, 0) - oldLevel);
        for (int i = 1; i < bypass.getNumSamples(); ++i)
            step = std::max (step, std::abs (bypass.getSample (0, i) - bypass.getSample (0, i - 1)));
        expect (step < 0.001f);
        for (int ch = 0; ch < 2; ++ch)
            expectWithinAbsoluteError (bypass.getSample (ch, 480), dryLevel, 1.0e-7f);
        expectWithinAbsoluteError (bypass.getSample (0, 2047), dryLevel, 1.0e-7f);
        f.project.setEffectEnabled (eq, true);
        const auto enabled = f.render (2048);
        expect (std::abs (enabled.getSample (0, 0) - dryLevel) < 0.001f);
        expectWithinAbsoluteError (enabled.getSample (0, 480), oldLevel, 1.0e-7f);
    }

    void delayReadBoundary()
    {
#if JUCE_LINUX
        beginTest ("A valid free delay time must render through its circular-buffer boundary without aborting");
        // Isolate this regression: the debug STL aborts rather than returning bad audio.
        // All allocation/preparation happens before fork; the child only runs DSP and exits.
        Delay delay;
        delay.setParam (Delay::sync, 0);
        delay.setParam (Delay::timeMs, 625.6875f);
        delay.setParam (Delay::mix, 1);
        delay.prepare (rate, 512);
        Audio block (2, 512);
        block.clear();
        daw::Transport::Block timing;
        timing.sampleRate = rate;
        timing.bpm = 120;
        const auto child = fork();
        if (child == 0)
        {
            const rlimit noCore { 0, 0 };
            setrlimit (RLIMIT_CORE, &noCore);
            for (int done = 0; done < 31000; done += 512)
            {
                delay.process (block, 512, timing);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 512; ++i)
                        if (! std::isfinite (block.getSample (ch, i))
                            || ! juce::exactlyEqual (block.getSample (ch, i), 0.0f))
                            std::_Exit (1);
            }
            std::_Exit (0);
        }
        expect (child > 0, "Could not create the isolated delay regression process");
        if (child < 0)
            return;
        int status = 0;
        pid_t waited;
        do
        {
            waited = waitpid (child, &status, 0);
        }
        while (waited < 0 && errno == EINTR);
        expect (waited == child && WIFEXITED (status) && WEXITSTATUS (status) == 0,
                "DelayLine::read rounds a tiny negative read position up to the buffer size at sample 30033");
#endif
    }

    void roundTrip()
    {
        beginTest ("Save/load preserves exact effect order, enabled flags, parameters and rendered samples");
        ChainFixture f;
        const auto audio = f.audioTrack (f.sineFile);
        const auto midi = f.project.addTrack ("MIDI", Project::TrackKind::midi);
        const auto clip = f.project.addMidiClip (midi, 0, 4);
        f.project.addNote (clip, 67, 0, 2);
        for (const auto& track : { audio, midi })
        {
            for (const auto& type : daw::getEffectTypes())
            {
                const auto device = f.project.addEffect (track, type, 0);
                const auto& infos = Project::getDeviceParamInfos (device);
                for (size_t i = 0; i < infos.size(); ++i)
                {
                    const auto& info = infos[i];
                    f.project.setDeviceParam (device, static_cast<int> (i),
                                              info.minValue + 0.3125f * (info.maxValue - info.minValue));
                }
                // A fractional sample tap avoids the separately tested delay read-boundary bug.
                if (type == "delay")
                    f.project.setDeviceParam (device, Delay::timeMs, 123.456f);
                f.project.setEffectEnabled (device, type != "compressor");
            }
        }
        const auto before = f.freshRender (48000, 0);
        juce::TemporaryFile file (".diydaw");
        expect (f.project.saveTo (file.getFile()).wasOk());
        ChainFixture loaded;
        expect (loaded.project.loadFrom (file.getFile()).wasOk());
        expectEquals (loaded.project.getNumTracks(), 2);
        for (int t = 0; t < 2; ++t)
        {
            const auto original = f.project.getEffects (f.project.getTrack (t));
            const auto restored = loaded.project.getEffects (loaded.project.getTrack (t));
            expectEquals (restored.size(), original.size());
            for (int d = 0; d < juce::jmin (original.size(), restored.size()); ++d)
            {
                expect (original[d].isEquivalentTo (restored[d]));
                expectEquals (Project::getDeviceType (restored[d]), Project::getDeviceType (original[d]));
                expect (Project::isEffectEnabled (restored[d]) == Project::isEffectEnabled (original[d]));
                const auto instance = loaded.project.getEffectInstance (restored[d]);
                expect (instance != nullptr);
                if (instance != nullptr)
                    expect (instance->isEnabled() == Project::isEffectEnabled (restored[d]));
                const auto count = static_cast<int> (Project::getDeviceParamInfos (original[d]).size());
                for (int p = 0; p < count; ++p)
                {
                    const auto value = Project::getDeviceParam (original[d], p);
                    expectEquals (Project::getDeviceParam (restored[d], p), value);
                    if (instance != nullptr)
                        expectEquals (instance->getParam (p), value);
                }
            }
        }
        expectEquals (maxDifference (loaded.freshRender (48000, 0), before), 0.0f);
        expect (! loaded.project.undo(), "Loading clears effect edit history");
    }

    void importedDevices()
    {
        beginTest ("Hand-written XML drops unknown devices, moves the synth first and defaults enabled to true");
        juce::TemporaryFile file (".diydaw");
        expect (file.getFile().replaceWithText (R"xml(<PROJECT version="1">
            <TRACK kind="midi"><DEVICE type="delay"/><DEVICE type="unknown"/>
                <DEVICE type="eq" output="900" lowFreq="-4"/><DEVICE type="synth"/>
                <DEVICE type="synth"/><DEVICE type="compressor" enabled="0"/></TRACK>
            <TRACK kind="audio"><DEVICE type="unknown"/><DEVICE type="synth"/>
                <DEVICE type="reverb"/><DEVICE type="delay" sync="0.6" timeMs="-500"/></TRACK>
            </PROJECT>)xml"));
        ChainFixture f;
        expect (f.project.loadFrom (file.getFile()).wasOk());
        const auto midi = f.project.getTrack (0), audio = f.project.getTrack (1);
        expect (midi.getChild (0) == Project::getDevice (midi));
        expectEquals (midi.getNumChildren(), 4, "Exactly one synth plus three known effects");
        expect (! Project::getDevice (audio).isValid());
        const auto effects = f.project.getEffects (midi);
        const auto audioEffects = f.project.getEffects (audio);
        expectEquals (effects.size(), 3);
        expectEquals (audioEffects.size(), 2);
        if (effects.size() != 3 || audioEffects.size() != 2)
            return;
        expectEquals (Project::getDeviceType (effects[0]), juce::String ("delay"));
        expectEquals (Project::getDeviceType (effects[1]), juce::String ("eq"));
        expect (Project::isEffectEnabled (effects[0]) && Project::isEffectEnabled (effects[1]));
        expect (! Project::isEffectEnabled (effects[2]));
        expect (Project::isEffectEnabled (audioEffects[0]));
        expectEquals (Project::getDeviceParam (effects[0], Delay::mix), Delay::infos()[Delay::mix].defaultValue);

        beginTest ("Imported effect parameters are clamped and stepped in the document as well as DSP");
        // Regression: normaliseTypes currently parses these numbers without clamping them.
        // AudioEffect::setParam clamps the live copy, leaving model/UI/save values inconsistent.
        expectEquals (Project::getDeviceParam (effects[1], EQ::output), 18.0f);
        expectEquals (Project::getDeviceParam (effects[1], EQ::lowFreq), 20.0f);
        expectEquals (Project::getDeviceParam (audioEffects[1], Delay::sync), 1.0f);
        expectEquals (Project::getDeviceParam (audioEffects[1], Delay::timeMs), Delay::infos()[Delay::timeMs].minValue);
        const auto eqInstance = f.project.getEffectInstance (effects[1]);
        expect (eqInstance != nullptr);
        if (eqInstance != nullptr)
        {
            expectEquals (eqInstance->getParam (EQ::output), 18.0f);
            expectEquals (eqInstance->getParam (EQ::lowFreq), 20.0f);
        }

        beginTest ("setDeviceParam explicitly normalises imported out-of-range properties");
        f.project.setDeviceParam (effects[1], EQ::output, 900);
        f.project.setDeviceParam (effects[1], EQ::lowFreq, -4);
        f.project.setDeviceParam (audioEffects[1], Delay::sync, 0.6f);
        f.project.setDeviceParam (audioEffects[1], Delay::timeMs, -500);
        expectEquals (Project::getDeviceParam (effects[1], EQ::output), 18.0f);
        expectEquals (Project::getDeviceParam (effects[1], EQ::lowFreq), 20.0f);
        expectEquals (Project::getDeviceParam (audioEffects[1], Delay::sync), 1.0f);
        expectEquals (Project::getDeviceParam (audioEffects[1], Delay::timeMs), Delay::infos()[Delay::timeMs].minValue);
    }

    void exportTails()
    {
        beginTest ("ProjectRenderer includes EQ and delay/reverb tails identically to the live engine");
        for (const auto& type : { juce::String ("delay"), juce::String ("reverb") })
        {
            ChainFixture f;
            const auto track = f.audioTrack (f.impulseFile);
            const auto eq = f.project.addEffect (track, "eq");
            f.project.setDeviceParam (eq, EQ::output, -6);
            const auto effect = f.project.addEffect (track, type);
            if (type == "delay")
            {
                configureDelay (f.project, effect);
                f.project.setDeviceParam (effect, Delay::feedback, 0.6f);
            }
            else
            {
                f.project.setDeviceParam (effect, daw::ReverbEffect::mix, 1);
                f.project.setDeviceParam (effect, daw::ReverbEffect::size, 0.6f);
            }
            daw::ExportSettings settings;
            settings.sampleRate = rate;
            settings.endBeat = 3.0; // Explicit export range includes silence after the clip.
            daw::ProjectRenderer renderer (f.project, settings);
            const auto live = f.freshRender (72000, 0);
            const auto bounced = renderer.render();
            expectEquals (renderer.getLengthInSamples(), static_cast<int64_t> (72000));
            expectEquals (Project::getClipEnd (track.getChild (0)), 0.1);
            expect (live.getMagnitude (0, 9600, 24000) > 1.0e-5f, type + " must ring after the clip ends");
            expectEquals (maxDifference (bounced, live), 0.0f, type);
            expectEquals (maxDifference (renderer.render(), bounced), 0.0f, "Repeated bounce resets effect history");
            f.project.setEffectEnabled (eq, false);
            const auto louder = f.freshRender (72000, 0);
            auto expected = Audio (live);
            expected.applyGain (juce::Decibels::decibelsToGain (6.0f));
            expectWithinAbsoluteError (maxDifference (louder, expected), 0.0f, 2.0e-6f);
        }
    }
};

/** Transport, looped rendering and isolated tracks are driven on the test thread. */
class TransportLoopTests : public juce::UnitTest
{
public:
    TransportLoopTests() : juce::UnitTest ("Transport loop and pattern isolation", "diyDaw") {}

    void runTest() override
    {
        transportAccounting();
        linearPlayback();
        repeatedAudio();
        repeatedClicks();
        midiBoundaries();
        isolation();
    }

private:
    Audio checkedRender (ChainFixture& f, int total, int blockSize)
    {
        Audio result (2, total);
        bool valid = true;
        for (int done = 0; done < total;)
        {
            const auto count = juce::jmin (blockSize, total - done);
            Audio block (4, count + 2);
            for (int ch = 0; ch < 4; ++ch)
                for (int i = 0; i < count + 2; ++i)
                    block.setSample (ch, i, 123.0f);
            f.engine.render (block, count);
            for (int ch = 0; ch < 4; ++ch)
            {
                valid = valid && juce::exactlyEqual (block.getSample (ch, count), 123.0f)
                              && juce::exactlyEqual (block.getSample (ch, count + 1), 123.0f);
                for (int i = 0; i < count; ++i)
                    valid = valid && std::isfinite (block.getSample (ch, i))
                                  && (ch < 2 ? std::abs (block.getSample (ch, i)) < 2.0f
                                             : juce::exactlyEqual (block.getSample (ch, i), 0.0f));
            }
            for (int ch = 0; ch < 2; ++ch)
                result.copyFrom (ch, done, block, ch, 0, count);
            done += count;
        }
        expect (valid, "Every requested sample is filled, extra channels are silent and guard samples untouched");
        return result;
    }

    void transportAccounting()
    {
        beginTest ("Loop blocks account for every sample and wrap exactly to the start, including multiple wraps");
        for (const auto sampleRate : { rate, 8192.0 })
            for (const auto loopBeats : { 1.0 / 16.0, 1.0 / 64.0 })
                for (const auto blockSize : blockSizes)
                {
                    daw::Transport transport;
                    transport.prepare (sampleRate);
                    transport.setBpm (120);
                    const auto samplesPerBeat = sampleRate / 2;
                    const auto cycle = static_cast<int> (std::llround (loopBeats * samplesPerBeat));
                    const auto prefix = static_cast<int> (std::llround (samplesPerBeat / 32));
                    const auto start = 1.0 - prefix / samplesPerBeat;
                    transport.setLoop (1.0, 1.0 + loopBeats);
                    transport.setLoopEnabled (true);
                    expectEquals (transport.getLoopStart(), 1.0);
                    expectEquals (transport.getLoopEnd(), 1.0 + loopBeats);
                    expect (transport.isLoopEnabled());
                    transport.setPositionBeats (start);
                    transport.play();
                    const auto total = prefix + 5 * cycle + 17;
                    int done = 0, wraps = 0;
                    bool valid = true;
                    while (done < total && valid)
                    {
                        const auto request = juce::jmin (blockSize, total - done);
                        const auto block = transport.advance (request);
                        const auto expected = done < prefix ? start + done / samplesPerBeat
                                            : 1.0 + ((done - prefix) % cycle) / samplesPerBeat;
                        valid = block.numSamples > 0 && block.numSamples <= request
                             && std::abs (block.startBeat - expected) < 1.0e-9
                             && block.endBeat() <= transport.getLoopEnd() + 1.0e-9;
                        done += block.numSamples;
                        if (done >= prefix + cycle && (done - prefix) % cycle == 0)
                        {
                            ++wraps;
                            valid = valid && juce::exactlyEqual (transport.getPositionBeats(), 1.0);
                        }
                    }
                    expect (valid, "Block " + juce::String (blockSize) + " at " + juce::String (sampleRate));
                    expectEquals (done, total);
                    expect (wraps >= 5);
                    expectWithinAbsoluteError (transport.getPositionBeats(), 1.0 + 17.0 / samplesPerBeat, 1.0e-9);
                }
    }

    void linearPlayback()
    {
        beginTest ("Starting at or after the loop end continues linearly; disabling while playing stops wrapping");
        for (const auto blockSize : blockSizes)
        {
            daw::Transport transport;
            transport.prepare (rate);
            transport.setLoop (1, 2);
            transport.setLoopEnabled (true);
            transport.play();
            for (const auto start : { 2.0, 2.25 })
            {
                transport.setPositionBeats (start);
                for (int i = 0; i < 3; ++i)
                {
                    const auto block = transport.advance (blockSize);
                    expectEquals (block.numSamples, blockSize);
                    expectWithinAbsoluteError (block.startBeat, start + i * blockSize / 24000.0, 1.0e-10);
                }
            }
            transport.setPositionBeats (1.999);
            transport.advance (1);
            const auto before = transport.getPositionBeats();
            transport.setLoopEnabled (false);
            expect (! transport.isLoopEnabled());
            const auto next = transport.advance (blockSize);
            expectEquals (next.numSamples, blockSize);
            expectEquals (next.startBeat, before);
            expectWithinAbsoluteError (transport.getPositionBeats(), before + blockSize / 24000.0, 1.0e-10);
            transport.stop();
            const auto stopped = transport.getPositionBeats();
            transport.advance (blockSize);
            expectEquals (transport.getPositionBeats(), stopped);
        }
    }

    void expectCycles (const Audio& actual, const Audio& cycle, const juce::String& context)
    {
        float error = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < actual.getNumSamples(); ++i)
                error = std::max (error, std::abs (actual.getSample (ch, i)
                                                 - cycle.getSample (ch, i % cycle.getNumSamples())));
        expectEquals (error, 0.0f, context + ": every cycle must repeat sample-exactly");
    }

    void repeatedAudio()
    {
        beginTest ("Looped audio repeats sample-exactly with loops shorter than a render call or internal block");
        // At 48 kHz, a constant signal tests exact cycles independently of interpolation.
        // At 8192 Hz, the binary sample period also permits exact sine-cycle comparisons.
        for (const auto loopRate : { rate, 8192.0 })
            for (const auto loopBeats : { 1.0 / 64.0, 1.0 / 32.0 })
                for (const auto blockSize : blockSizes)
                {
                    const auto samplesPerBeat = loopRate / 2;
                    const auto loopSamples = static_cast<int> (std::llround (loopBeats * samplesPerBeat));
                    ChainFixture f (loopRate);
                    f.audioTrack (juce::exactlyEqual (loopRate, rate) ? f.constantFile : f.sineFile);
                    const auto cycle = f.freshRender (loopSamples, 1.0);
                    expect (cycle.getMagnitude (0, loopSamples) > 0.01f);
                    auto& transport = f.engine.getTransport();
                    transport.setLoop (1.0, 1.0 + loopBeats);
                    transport.setLoopEnabled (true);
                    f.restart (1.0);
                    const auto total = 35 * loopSamples + 11;
                    expectCycles (checkedRender (f, total, blockSize), cycle, "Audio block " + juce::String (blockSize));
                    expectWithinAbsoluteError (transport.getPositionBeats(), 1.0 + 11.0 / samplesPerBeat, 1.0e-9);
                }
    }

    void repeatedClicks()
    {
        beginTest ("Metronome clicks retrigger identically on each wrap, even inside a single render call");
        for (const auto loopRate : { rate, 8192.0 })
            for (const auto loopBeats : { 1.0 / 64.0, 1.0 })
                for (const auto blockSize : blockSizes)
                {
                    const auto loopSamples = static_cast<int> (std::llround (loopBeats * loopRate / 2));
                    ChainFixture f (loopRate);
                    f.engine.getMetronome().setEnabled (true);
                    const auto cycle = f.freshRender (loopSamples, 0);
                    expect (cycle.getMagnitude (0, loopSamples) > 0.1f);
                    f.engine.getTransport().setLoop (0, loopBeats);
                    f.engine.getTransport().setLoopEnabled (true);
                    f.restart();
                    expectCycles (checkedRender (f, 3 * loopSamples + 17, blockSize), cycle,
                                  "Metronome block " + juce::String (blockSize));
                }
    }

    void midiBoundaries()
    {
        beginTest ("MIDI loop boundaries send note-offs and retrigger start notes; stopping leaves no voices");
        constexpr int cycle = 2048, playingSamples = 3 * cycle + 200, tailSamples = 4096;
        for (const auto blockSize : blockSizes)
        {
            ChainFixture f (8192.0);
            const auto track = f.project.addTrack ("MIDI", Project::TrackKind::midi);
            const auto clip = f.project.addMidiClip (track, 0, 4);
            f.project.addNote (clip, 60, 0.5, 1.0);
            f.project.addNote (clip, 67, 0.875, 1.0);
            for (const auto& [param, value] : std::vector<std::pair<int, float>> {
                     { Synth::osc1Wave, 3 }, { Synth::osc2Wave, 3 }, { Synth::oscMix, 0 },
                     { Synth::filterCutoff, 20000 }, { Synth::filterResonance, 0 },
                     { Synth::filterEnvAmount, 0 }, { Synth::filterKeyTrack, 0 },
                     { Synth::ampAttack, 0.001f }, { Synth::ampDecay, 0.001f },
                     { Synth::ampSustain, 1 }, { Synth::ampRelease, 0.005f } })
                f.project.setDeviceParam (track, param, value);

            // Independent oracle: explicit note-offs at each boundary, then note-ons at
            // loop start. Unlike a cycle comparison this also detects released old voices.
            Synth reference;
            for (int p = 0; p < Synth::numParams; ++p)
                reference.setParam (p, Project::getDeviceParam (track, p));
            reference.prepare (8192.0, playingSamples + tailSamples);
            std::vector<daw::NoteEvent> events;
            for (int start = 0; start < playingSamples; start += cycle)
            {
                events.push_back ({ start, 60, 0.8f, true });
                const auto end = juce::jmin (start + cycle, playingSamples);
                events.push_back ({ end, 60, 0, false });
                if (start + 1536 < playingSamples)
                {
                    events.push_back ({ start + 1536, 67, 0.8f, true });
                    events.push_back ({ end, 67, 0, false });
                }
            }
            std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b)
            {
                if (a.sampleOffset != b.sampleOffset)
                    return a.sampleOffset < b.sampleOffset;
                return ! a.isNoteOn && b.isNoteOn;
            });
            Audio expected (2, playingSamples + tailSamples);
            expected.clear();
            reference.process (expected, expected.getNumSamples(), events.data(), static_cast<int> (events.size()));
            expected.applyGain (std::sqrt (0.5f));

            f.engine.getTransport().setLoop (0.5, 1.0);
            f.engine.getTransport().setLoopEnabled (true);
            f.restart (0.5);
            const auto playing = checkedRender (f, playingSamples, blockSize);
            f.engine.getTransport().stop();
            const auto tail = checkedRender (f, tailSamples, blockSize);
            Audio actual (2, expected.getNumSamples());
            for (int ch = 0; ch < 2; ++ch)
            {
                actual.copyFrom (ch, 0, playing, ch, 0, playingSamples);
                actual.copyFrom (ch, playingSamples, tail, ch, 0, tailSamples);
            }
            expectWithinAbsoluteError (maxDifference (actual, expected), 0.0f, 1.0e-6f,
                                       "Explicit boundary events, block " + juce::String (blockSize));
            expect (playing.getMagnitude (0, playingSamples) > 0.01f);
            expectEquals (f.project.getEngineTrack (track).getSynth().getNumActiveVoices(), 0);
            expectEquals (tail.getMagnitude (0, tailSamples - 1024, 1024), 0.0f);
        }
    }

    void isolation()
    {
        beginTest ("Pattern isolation renders only the selected track, ignores other solos, and -1 restores the mix");
        ChainFixture f;
        const auto left = f.audioTrack (f.constantFile);
        const auto right = f.audioTrack (f.constantFile);
        const auto centre = f.audioTrack (f.constantFile);
        f.project.setTrackPan (left, -1);
        f.project.setTrackPan (right, 1);
        f.project.setTrackGain (centre, -6);
        const auto eq = f.project.addEffect (left, "eq");
        f.project.setDeviceParam (eq, EQ::output, -6);
        f.project.setTrackSolo (right, true);
        const auto expectLevel = [this, &f] (float l, float r)
        {
            const auto audio = f.freshRender (4096);
            for (int i = 3072; i < 4096; ++i)
            {
                if (std::abs (audio.getSample (0, i) - l) > 1.0e-6f
                    || std::abs (audio.getSample (1, i) - r) > 1.0e-6f)
                {
                    expect (false, "Isolated output contains another track or is suppressed by solo");
                    return;
                }
            }
            expect (true);
        };
        expectEquals (f.engine.getIsolatedTrack(), -1);
        expectLevel (0, 0.2f);
        const auto cut = juce::Decibels::decibelsToGain (-6.0f);
        f.engine.setIsolatedTrack (0);
        expectEquals (f.engine.getIsolatedTrack(), 0);
        expectLevel (0.2f * cut, 0);
        f.engine.setIsolatedTrack (2);
        expectLevel (0.2f * cut * std::sqrt (0.5f), 0.2f * cut * std::sqrt (0.5f));
        f.engine.setIsolatedTrack (1);
        expectLevel (0, 0.2f);
        f.engine.setIsolatedTrack (-1);
        expectLevel (0, 0.2f);
        f.project.setTrackSolo (right, false);
        const auto middle = 0.2f * cut * std::sqrt (0.5f);
        expectLevel (0.2f * cut + middle, 0.2f + middle);
    }
};

static DeviceChainModelTests deviceChainModelTests;
static DeviceChainAudioTests deviceChainAudioTests;
static TransportLoopTests transportLoopTests;
} // namespace
