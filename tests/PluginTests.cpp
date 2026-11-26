#include "Project.h"
#include "PluginHost.h"
#include "ProjectRenderer.h"
#include "Effects.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace
{
constexpr double rate = 48000.0;
constexpr int blockSize = 256;
const std::array<juce::String, 2> formats { "VST3", "LV2" };

std::unique_ptr<daw::PluginHost> scanTestPlugins()
{
    auto host = std::make_unique<daw::PluginHost> ([] (juce::AudioPluginFormatManager& manager)
    {
        juce::addHeadlessDefaultFormatsToManager (manager);
    });
    host->setExtraSearchPaths (juce::FileSearchPath (DAW_TEST_PLUGIN_DIR));
    host->scanNow();
    return host;
}

juce::AudioBuffer<float> renderSamples (daw::Engine& engine, int samples)
{
    juce::AudioBuffer<float> result (2, samples);
    engine.render (result, samples);
    return result;
}

float difference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return std::numeric_limits<float>::infinity();
    float error = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const auto delta = std::abs (a.getSample (ch, i) - b.getSample (ch, i));
            if (! std::isfinite (delta))
                return std::numeric_limits<float>::infinity();
            error = juce::jmax (error, delta);
        }
    return error;
}

/** Test/message thread only: edits and render callbacks run sequentially. */
struct PluginFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    daw::Project project { engine, pool };
    const juce::File dcFile { "/virtual/plugin-tests-dc.wav" };

    explicit PluginFixture (daw::PluginHost* host)
    {
        if (host != nullptr)
            project.setPluginHost (host);
        engine.prepare (rate, blockSize);
        engine.getMetronome().setEnabled (false);
        auto sample = std::make_shared<daw::SampleData>();
        sample->sampleRate = rate;
        sample->name = "Plugin test DC";
        sample->buffer.setSize (1, 96000);
        for (int i = 0; i < sample->buffer.getNumSamples(); ++i)
            sample->buffer.setSample (0, i, 0.5f);
        pool.insert (daw::SamplePool::keyFor (dcFile), sample);
    }

    juce::ValueTree addAudio()
    {
        const auto track = project.addTrack ("DC");
        project.setTrackPan (track, -1.0f);
        project.addClip (track, dcFile, 0.0);
        return track;
    }

    juce::ValueTree addMidi()
    {
        const auto track = project.addTrack ("Sine", daw::Project::TrackKind::midi);
        project.setTrackPan (track, -1.0f);
        return track;
    }

    void warmUp()
    {
        engine.getTransport().stop();
        renderSamples (engine, 4096); // settle the 20 ms fader ramp while stopped
    }

    juce::AudioBuffer<float> playFromStart (int samples)
    {
        engine.getTransport().stop();
        engine.prepare (engine.getSampleRate(), blockSize);
        engine.getTransport().setPositionBeats (0.0);
        warmUp();
        engine.getTransport().play();
        return renderSamples (engine, samples);
    }
};

/** Shared assertions; plugin APIs are called on the test/message thread. */
class PluginTests : public juce::UnitTest
{
public:
    explicit PluginTests (const juce::String& testName) : juce::UnitTest (testName, "diyDaw") {}

protected:
    juce::PluginDescription description (daw::PluginHost& host, const juce::String& format, bool instrument)
    {
        const juce::String pluginName = instrument ? "diyDaw Test Synth" : "diyDaw Test Gain";
        for (const auto& d : host.getKnownPlugins().getTypes())
            if (d.name == pluginName && d.pluginFormatName == format)
                return d;
        expect (false, "Missing " + format + " " + pluginName + " in " + juce::String (DAW_TEST_PLUGIN_DIR));
        return {};
    }

    juce::AudioProcessorParameter* parameter (daw::Project& project, const juce::ValueTree& device,
                                            const juce::String& parameterName)
    {
        auto* plugin = project.getPluginInstance (device);
        expect (plugin != nullptr, project.getPluginError (device));
        if (plugin != nullptr)
            for (auto* p : plugin->getParameters())
                if (p->getName (64).equalsIgnoreCase (parameterName))
                    return p;
        expect (false, "Missing parameter " + parameterName);
        return nullptr;
    }

    bool writeTree (const juce::ValueTree& tree, const juce::File& file)
    {
        const auto xml = tree.createXml();
        const auto ok = xml != nullptr && xml->writeTo (file);
        expect (ok, "Couldn't write fixture " + file.getFullPathName());
        return ok;
    }

    bool save (daw::Project& project, const juce::File& file)
    {
        const auto result = project.saveTo (file);
        expect (result.wasOk(), result.getErrorMessage());
        return result.wasOk();
    }

    bool load (daw::Project& project, const juce::File& file)
    {
        const auto result = project.loadFrom (file);
        expect (result.wasOk(), result.getErrorMessage());
        return result.wasOk();
    }
};

class PluginCatalogueTests final : public PluginTests
{
public:
    PluginCatalogueTests() : PluginTests ("Plugins catalogue") {}

    void runTest() override
    {
        beginTest ("Scanning discovers both effects and instruments in VST3 and LV2");
        auto host = scanTestPlugins();
        const auto testPluginCount = [&]
        {
            int count = 0;
            for (const auto& d : host->getKnownPlugins().getTypes())
                if (d.name == "diyDaw Test Gain" || d.name == "diyDaw Test Synth")
                    ++count;
            return count;
        };
        expectEquals (testPluginCount(), 4); // unrelated installed plugins do not affect this assertion
        for (const auto& format : formats)
            for (const bool instrument : { false, true })
            {
                const auto d = description (*host, format, instrument);
                if (d.name.isEmpty())
                    continue;
                expect (d.isInstrument == instrument);
                const auto matches = [&d] (const auto& list)
                {
                    int count = 0;
                    for (const auto& candidate : list)
                        if (candidate.createIdentifierString() == d.createIdentifierString())
                            ++count;
                    return count;
                };
                expectEquals (matches (host->getInstruments()), instrument ? 1 : 0);
                expectEquals (matches (host->getEffects()), instrument ? 0 : 1);
                juce::PluginDescription found;
                expect (host->findDescription (d.createIdentifierString(), found));
                expectEquals (found.createIdentifierString(), d.createIdentifierString());
                expect (found.isInstrument == instrument);
            }

        beginTest ("A second scan does not duplicate descriptions");
        const auto count = host->getKnownPlugins().getNumTypes();
        host->scanNow();
        expectEquals (host->getKnownPlugins().getNumTypes(), count);
        expectEquals (testPluginCount(), 4);
        juce::PluginDescription unknown;
        expect (! host->findDescription ("diyDaw deliberately unknown plugin", unknown));
        for (const auto& format : formats)
        {
            beginTest (format + " creation reports an error for a missing plugin");
            auto bogus = description (*host, format, false);
            if (bogus.name.isEmpty())
                continue;
            bogus.fileOrIdentifier = "/nonexistent/diyDaw-test-plugin-missing";
            juce::String error;
            auto instance = host->createInstance (bogus, rate, blockSize, error);
            expect (instance == nullptr);
            expect (error.isNotEmpty());
        }
    }
};

class PluginEffectTests final : public PluginTests
{
public:
    PluginEffectTests() : PluginTests ("Plugins effects") {}

    void runTest() override
    {
        auto host = scanTestPlugins();
        for (const auto& format : formats)
        {
            beginTest (format + " effect description");
            const auto gain = description (*host, format, false);
            if (gain.name.isEmpty())
                continue;
            for (const bool master : { false, true })
                testChain (*host, gain, master);
        }
    }

private:
    void testChain (daw::PluginHost& host, const juce::PluginDescription& gain, bool master)
    {
        const auto context = gain.pluginFormatName + (master ? " master" : " track");
        PluginFixture f (&host);
        const auto track = f.addAudio();
        const auto owner = master ? f.project.getMaster() : track;
        const auto device = f.project.addPluginEffect (owner, gain);
        auto* p = parameter (f.project, device, "gain");
        if (p == nullptr)
            return;

        beginTest (context + " default gain halves the audio (smoke test)");
        expectWithinAbsoluteError (p->getValue(), 0.25f, 1.0e-6f);
        auto out = f.playFromStart (8192);
        expectWithinAbsoluteError (out.getSample (0, 8000), 0.25f, 1.0e-6f);
        expectWithinAbsoluteError (out.getMagnitude (1, 0, 8192), 0.0f, 1.0e-6f);

        beginTest (context + " bypass crossfades to dry over ten ms");
        f.project.setEffectEnabled (device, false);
        out = renderSamples (f.engine, 1024);
        float rampError = 0.0f;
        for (int i = 0; i < 1024; ++i)
        {
            const auto expected = 0.25f + 0.25f * juce::jmin (1.0f, (float) (i + 1) / 480.0f);
            rampError = juce::jmax (rampError, std::abs (out.getSample (0, i) - expected));
        }
        expectWithinAbsoluteError (rampError, 0.0f, 2.0e-6f);
        expectWithinAbsoluteError (out.getSample (0, 1023), 0.5f, 1.0e-6f);
        f.project.setEffectEnabled (device, true);
        out = renderSamples (f.engine, 1024);
        expect (out.getSample (0, 0) > 0.49f);
        expectWithinAbsoluteError (out.getSample (0, 1023), 0.25f, 1.0e-6f);

        beginTest (context + " order with a nonlinear builtin compressor changes audio");
        const auto compressor = f.project.addEffect (owner, "compressor");
        f.project.setDeviceParam (compressor, daw::CompressorEffect::threshold, -12.0f);
        f.project.setDeviceParam (compressor, daw::CompressorEffect::ratio, 4.0f);
        f.project.setDeviceParam (compressor, daw::CompressorEffect::attack, 0.1f);
        f.project.setDeviceParam (compressor, daw::CompressorEffect::release, 1.0f);
        f.project.setDeviceParam (compressor, daw::CompressorEffect::knee, 0.0f);
        out = f.playFromStart (8192);
        expectEquals (f.project.getEffects (owner).indexOf (device), 0);
        expectWithinAbsoluteError (out.getSample (0, 8000), 0.25f, 2.0e-5f);
        f.project.moveEffect (device, 1);
        out = f.playFromStart (8192);
        expectEquals (f.project.getEffects (owner).indexOf (device), 1);
        const auto compressed = 0.5f * std::pow (10.0f, (-12.0f + (20.0f * std::log10 (0.5f) + 12.0f) / 4.0f) / 20.0f);
        expectWithinAbsoluteError (out.getSample (0, 8000), compressed, 2.0e-5f);

        beginTest (context + " remove/undo creates a fresh instance and retains edited state");
        f.project.removeEffect (compressor);
        p->setValueNotifyingHost (0.7f); // actual gain = 1.4
        f.warmUp(); // LV2 applies parameter edits in the audio callback (the app's audio always runs)
        const auto snapshot = f.project.createSnapshot();
        const auto snapshotOwner = master ? snapshot.getChildWithName (daw::ids::MASTER) : snapshot.getChild (0);
        expect (snapshotOwner.getChildWithName (daw::ids::DEVICE)[daw::ids::state].toString().isNotEmpty());
        juce::TemporaryFile saved (".diydaw");
        if (! save (f.project, saved.getFile()))
            return;
        // Keep the wrapper alive so allocator address reuse cannot hide replacement.
        auto oldEffect = f.project.getEffectInstance (device);
        auto* oldPlugin = f.project.getPluginInstance (device);
        f.project.beginTransaction ("Remove plugin");
        f.project.removeEffect (device);
        expect (f.project.getPluginInstance (device) == nullptr);
        out = f.playFromStart (8192);
        expectWithinAbsoluteError (out.getSample (0, 8000), 0.5f, 1.0e-6f);
        expect (f.project.undo());
        expect (device.getParent() == owner);
        expect (f.project.getPluginInstance (device) != oldPlugin);
        f.warmUp(); // a restored LV2 state also takes effect in the next audio callback
        if (auto* restored = parameter (f.project, device, "gain"))
        {
            // removeEffect captures the live state into the DEVICE kept by the undo action.
            expectWithinAbsoluteError (restored->getValue(), 0.7f, 1.0e-6f, "Undo lost the edited plugin gain, even after saving");
            out = f.playFromStart (8192);
            expectWithinAbsoluteError (out.getSample (0, 8000), 0.7f, 1.0e-6f, "Expected DC 0.5 * restored gain 1.4");
        }
        expect (f.project.redo());
        expect (f.project.getPluginInstance (device) == nullptr);
    }
};

class PluginInstrumentTests final : public PluginTests
{
public:
    PluginInstrumentTests() : PluginTests ("Plugins instruments") {}

    void runTest() override
    {
        auto host = scanTestPlugins();
        for (const auto& format : formats)
        {
            beginTest (format + " instrument description");
            const auto synth = description (*host, format, true);
            if (synth.name.isEmpty())
                continue;
            testMidi (*host, synth);
            testSwitch (*host, synth);
            testRestart (*host, synth);
        }
    }

private:
    void testMidi (daw::PluginHost& host, const juce::PluginDescription& synth)
    {
        const auto format = synth.pluginFormatName;
        PluginFixture f (&host);
        const auto track = f.addMidi();
        f.project.setPluginInstrument (track, synth);
        auto* level = parameter (f.project, daw::Project::getDevice (track), "level");
        if (level == nullptr)
            return;
        level->setValueNotifyingHost (0.6f);
        beginTest (format + " MIDI onset, pitch, level and note-off are sample accurate");
        const auto clip = f.project.addMidiClip (track, 0.0, 4.0);
        constexpr int onset = 3013; // neither a host block nor an automation boundary
        constexpr int duration = 6000;
        constexpr float velocity = 64.0f / 127.0f; // exactly representable by MIDI 1.0 velocity
        f.project.addNote (clip, 69, (double) onset / 24000.0, (double) duration / 24000.0, velocity);
        auto out = f.playFromStart (12000);
        float error = 0.0f;
        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            const auto expected = i >= onset && i < onset + duration
                                    ? 0.15f * velocity * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * (i - onset) / rate)
                                    : 0.0f;
            error = juce::jmax (error, std::abs (out.getSample (0, i) - expected));
        }
        expectWithinAbsoluteError (error, 0.0f, 2.0e-6f, "NoteEvent offsets must become plugin MIDI sample positions");
        expectEquals (out.getMagnitude (0, 0, onset), 0.0f);
        expect (out.getSample (0, onset + 1) > 0.004f);
        expectEquals (out.getMagnitude (0, onset + duration, 12000 - onset - duration), 0.0f);

        beginTest (format + " stopping a sounding note sends note-off immediately");
        out = f.playFromStart (4096);
        expect (out.getMagnitude (0, onset + 1, 1000) > 0.07f);
        f.engine.getTransport().stop();
        out = renderSamples (f.engine, 2048);
        expectEquals (out.getMagnitude (0, 0, 2048), 0.0f);

        beginTest (format + " seeking away from a sounding note sends note-off immediately");
        f.playFromStart (4096);
        f.engine.getTransport().setPositionBeats (8.0);
        out = renderSamples (f.engine, 2048);
        expectEquals (out.getMagnitude (0, 0, 2048), 0.0f);
    }

    void testSwitch (daw::PluginHost& host, const juce::PluginDescription& synth)
    {
        const auto format = synth.pluginFormatName;
        PluginFixture f (&host);
        const auto track = f.addMidi();
        const auto builtin = daw::Project::getDevice (track);
        const auto target = daw::Project::getDeviceParamTargetId (builtin, 0);
        const auto lane = f.project.getOrCreateAutomationLane (track, target);
        expect (lane.isValid());
        f.project.addAutomationPoint (lane, 0.0, daw::Project::getDeviceParam (builtin, 0));
        const auto clip = f.project.addMidiClip (track, 0.0, 4.0);
        f.project.addNote (clip, 69, 0.125, 0.5, 1.0f);

        beginTest (format + " replacing builtin instrument removes its automation; undo restores it");
        f.project.beginTransaction ("Plugin instrument");
        f.project.setPluginInstrument (track, synth);
        const auto pluginDevice = daw::Project::getDevice (track);
        expectEquals (daw::Project::getDeviceType (pluginDevice), juce::String ("pluginInstrument"));
        expect (! daw::Project::getAutomationLane (track, target).isValid());
        const auto pluginAudio = f.playFromStart (12000);
        expect (pluginAudio.getMagnitude (0, 0, 12000) > 0.24f);
        expect (f.project.undo());
        expect (daw::Project::getDevice (track) == builtin);
        expect (daw::Project::getAutomationLane (track, target) == lane);
        expect (f.project.getPluginInstance (pluginDevice) == nullptr);
        expect (f.project.redo());
        expect (daw::Project::getDevice (track) == pluginDevice);
        expect (! daw::Project::getAutomationLane (track, target).isValid());
        expectWithinAbsoluteError (difference (pluginAudio, f.playFromStart (12000)), 0.0f, 2.0e-6f);

        beginTest (format + " setInstrument synth and undo/redo select the correct instrument");
        f.project.beginTransaction ("Builtin instrument");
        f.project.setInstrument (track, "synth");
        expectEquals (daw::Project::getDeviceType (daw::Project::getDevice (track)), juce::String ("synth"));
        expect (f.project.getPluginInstance (pluginDevice) == nullptr);
        expect (f.project.getEngineTrack (track).getInstrumentType() == daw::AudioTrack::InstrumentType::synth);
        const auto builtinAudio = f.playFromStart (12000);
        expect (builtinAudio.getMagnitude (0, 0, 12000) > 0.001f);
        expect (difference (pluginAudio, builtinAudio) > 0.01f);
        expect (f.project.undo());
        expect (daw::Project::getDevice (track) == pluginDevice);
        expect (f.project.getEngineTrack (track).getInstrumentType() == daw::AudioTrack::InstrumentType::plugin);
        expectWithinAbsoluteError (difference (pluginAudio, f.playFromStart (12000)), 0.0f, 2.0e-6f);
        expect (f.project.redo());
        expect (f.project.getEngineTrack (track).getInstrumentType() == daw::AudioTrack::InstrumentType::synth);
        expectWithinAbsoluteError (difference (builtinAudio, f.playFromStart (12000)), 0.0f, 2.0e-6f);

        beginTest (format + " instrument replacement undo preserves edited plugin state");
        expect (f.project.undo());
        if (auto* p = parameter (f.project, pluginDevice, "level"))
        {
            p->setValueNotifyingHost (0.37f);
            f.warmUp();
            const auto edited = f.playFromStart (12000);
            f.project.beginTransaction ("Replace edited plugin instrument");
            f.project.setInstrument (track, "synth");
            expect (f.project.undo());
            f.warmUp(); // a restored LV2 state takes effect in the next audio callback
            // replaceInstrument captures the live state into the DEVICE kept by the undo action.
            if (auto* restored = parameter (f.project, pluginDevice, "level"))
                expectWithinAbsoluteError (restored->getValue(), 0.37f, 1.0e-6f, "Undo lost instrument state");
            expectWithinAbsoluteError (difference (edited, f.playFromStart (12000)), 0.0f, 2.0e-6f);
        }
    }

    void testRestart (daw::PluginHost& host, const juce::PluginDescription& synth)
    {
        beginTest (synth.pluginFormatName + " restart re-prepares plugin instrument and track/master effects");
        // Regression: setting rate/block details before VST3 prepareToPlay makes its
        // already-active fast path skip setupProcessing, retaining the previous DSP rate.
        const auto gain = description (host, synth.pluginFormatName, false);
        if (gain.name.isEmpty())
            return;
        PluginFixture f (&host);
        const auto track = f.addMidi();
        f.project.setPluginInstrument (track, synth);
        const auto instrument = daw::Project::getDevice (track);
        const auto effect = f.project.addPluginEffect (track, gain);
        const auto masterEffect = f.project.addPluginEffect (f.project.getMaster(), gain);
        const auto clip = f.project.addMidiClip (track, 0.0, 4.0);
        f.project.addNote (clip, 69, 0.125, 0.5, 1.0f);
        for (const auto sampleRate : { 48000.0, 96000.0, 44100.0 })
        {
            f.engine.getTransport().stop();
            f.engine.prepare (sampleRate, 1024);
            for (const auto& device : { instrument, effect, masterEffect })
                if (auto* instance = f.project.getPluginInstance (device))
                    expectWithinAbsoluteError (instance->getSampleRate(), sampleRate, 0.01);
                else
                    expect (false, f.project.getPluginError (device));
            f.engine.getTransport().setPositionBeats (0.0);
            f.warmUp();
            f.engine.getTransport().play();
            const auto out = renderSamples (f.engine, (int) (sampleRate * 0.2));
            const auto onset = (int) std::ceil (sampleRate * 0.0625 - 1.0e-3);
            float error = 0.0f;
            for (int i = onset; i < out.getNumSamples(); ++i)
            {
                const auto expected = 0.0625f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * (i - onset) / sampleRate);
                error = juce::jmax (error, std::abs (out.getSample (0, i) - expected));
            }
            expectWithinAbsoluteError (error, 0.0f, 2.0e-6f, "Wrong pitch/gain after prepare at " + juce::String (sampleRate));
        }
    }
};

class PluginPersistenceTests final : public PluginTests
{
public:
    PluginPersistenceTests() : PluginTests ("Plugins persistence and export") {}

    void runTest() override
    {
        auto host = scanTestPlugins();
        for (const auto& format : formats)
        {
            beginTest (format + " persistence descriptions");
            const auto gain = description (*host, format, false);
            const auto synth = description (*host, format, true);
            if (gain.name.isEmpty() || synth.name.isEmpty())
                continue;
            testImmediateState (*host, gain);
            testState (*host, gain, synth);
            testMissing (*host, gain, synth, false);
            testMissing (*host, gain, synth, true);
        }
    }

private:
    void testImmediateState (daw::PluginHost& host, const juce::PluginDescription& gain)
    {
        beginTest (gain.pluginFormatName + " saving before the next audio callback preserves a parameter edit");
        PluginFixture f (&host);
        const auto track = f.addAudio();
        const auto device = f.project.addPluginEffect (track, gain);
        auto* p = parameter (f.project, device, "gain");
        if (p == nullptr)
            return;
        p->setValueNotifyingHost (0.65f);
        // LV2 applies parameter edits in the audio callback; in the app the audio device is
        // always running, so an edit reaches the plugin within one block before any save.
        f.warmUp();
        juce::TemporaryFile file (".diydaw");
        if (! save (f.project, file.getFile()))
            return;
        PluginFixture restored (&host);
        if (! load (restored.project, file.getFile()))
            return;
        restored.warmUp();
        const auto effects = restored.project.getEffects (restored.project.getTrack (0));
        expectEquals (effects.size(), 1);
        if (! effects.isEmpty())
            if (auto* loaded = parameter (restored.project, effects[0], "gain"))
            {
                expectWithinAbsoluteError (loaded->getValue(), 0.65f, 1.0e-6f, "Save dropped a pending plugin edit");
                const auto out = restored.playFromStart (8192);
                expectWithinAbsoluteError (out.getSample (0, 8000), 0.65f, 1.0e-6f);
            }
    }

    std::array<juce::ValueTree, 3> populate (PluginFixture& f, const juce::PluginDescription& gain,
                                         const juce::PluginDescription& synth)
    {
        const auto audio = f.addAudio();
        const auto midi = f.addMidi();
        f.project.setPluginInstrument (midi, synth);
        const auto clip = f.project.addMidiClip (midi, 0.0, 4.0);
        f.project.addNote (clip, 69, 0.125, 0.5, 1.0f);
        return { f.project.addPluginEffect (audio, gain), daw::Project::getDevice (midi),
                 f.project.addPluginEffect (f.project.getMaster(), gain) };
    }

    std::array<juce::ValueTree, 3> devices (daw::Project& project)
    {
        const auto effects = project.getEffects (project.getTrack (0));
        const auto masterEffects = project.getEffects (project.getMaster());
        expectEquals (effects.size(), 1);
        expectEquals (masterEffects.size(), 1);
        return { effects.isEmpty() ? juce::ValueTree() : effects[0], daw::Project::getDevice (project.getTrack (1)),
                 masterEffects.isEmpty() ? juce::ValueTree() : masterEffects[0] };
    }

    void testState (daw::PluginHost& host, const juce::PluginDescription& gain, const juce::PluginDescription& synth)
    {
        const auto format = gain.pluginFormatName;
        PluginFixture f (&host);
        const auto original = populate (f, gain, synth);
        juce::TemporaryFile saved (".diydaw");
        if (! save (f.project, saved.getFile()))
            return;
        expect (! f.project.hasUnsavedChanges());
        constexpr std::array<float, 3> values { 0.65f, 0.35f, 0.4f };
        for (size_t i = 0; i < original.size(); ++i)
        {
            beginTest (format + " parameter edits mark dirty: " + (i == 1 ? "instrument" : i == 2 ? "master" : "track"));
            auto* p = parameter (f.project, original[i], i == 1 ? "level" : "gain");
            if (p == nullptr)
                return;
            p->setValueNotifyingHost (values[i]);
            expect (f.project.hasUnsavedChanges());
            f.warmUp(); // also test the ordinary case where the audio callback has applied the edit
            if (! save (f.project, saved.getFile()))
                return;
            expect (! f.project.hasUnsavedChanges());
        }

        beginTest (format + " snapshots and fresh file loads restore plugin parameters and audio");
        const auto snapshot = f.project.createSnapshot();
        expect (! f.project.hasUnsavedChanges());
        for (const auto& owner : snapshot)
            for (const auto& device : owner)
                if (daw::Project::isPluginDevice (device))
                {
                    juce::MemoryBlock state;
                    expect (state.fromBase64Encoding (device[daw::ids::state].toString()));
                    expect (state.getSize() > 0);
                }
        const auto reference = f.playFromStart (24000);
        expectWithinAbsoluteError (reference.getSample (0, 2000), 0.52f, 3.0e-6f,
                                   "Saved gains must still be audible after prepare (DC 0.5 * 1.3 * 0.8)");
        PluginFixture loaded (&host);
        if (! load (loaded.project, saved.getFile()))
            return;
        expectEquals (loaded.project.getNumTracks(), 2);
        expect (! loaded.project.hasUnsavedChanges(), "Loading plugin state must leave the document clean");
        // LV2 patch parameters announce restored values on the first callback. Check
        // the hosted parameter cache after that announcement, as well as checking audio.
        loaded.warmUp();
        const auto restored = devices (loaded.project);
        for (size_t i = 0; i < restored.size(); ++i)
            if (auto* p = parameter (loaded.project, restored[i], i == 1 ? "level" : "gain"))
            {
                expectWithinAbsoluteError (p->getValue(), values[i], 1.0e-6f);
                expect (loaded.project.getPluginInstance (restored[i]) != f.project.getPluginInstance (original[i]));
            }
        expectWithinAbsoluteError (difference (reference, loaded.playFromStart (24000)), 0.0f, 3.0e-6f);

        beginTest (format + " copyFrom creates independent instances with current unsaved state");
        auto* liveLevel = parameter (f.project, original[1], "level");
        if (liveLevel == nullptr)
            return;
        liveLevel->setValueNotifyingHost (0.8f); // deliberately newer than the file's DEVICE state
        f.warmUp();
        PluginFixture copied (&host);
        copied.project.copyFrom (f.project);
        copied.warmUp();
        const auto copyDevices = devices (copied.project);
        if (auto* p = parameter (copied.project, copyDevices[1], "level"))
            expectWithinAbsoluteError (p->getValue(), 0.8f, 1.0e-6f);
        for (size_t i = 0; i < original.size(); ++i)
            expect (f.project.getPluginInstance (original[i]) != copied.project.getPluginInstance (copyDevices[i]));
        expectWithinAbsoluteError (difference (f.playFromStart (24000), copied.playFromStart (24000)), 0.0f, 3.0e-6f);

        beginTest (format + " ProjectRenderer matches live audio and owns its state snapshot");
        daw::ExportSettings settings;
        settings.endBeat = 1.0;
        settings.sampleRate = rate;
        settings.tailSeconds = 0.05;
        daw::ProjectRenderer renderer (f.project, settings);
        expectEquals (renderer.getLengthInSamples(), (int64_t) 24000);
        auto live = f.playFromStart (24000);
        f.engine.getTransport().stop();
        const auto tail = renderSamples (f.engine, 2400);
        live.setSize (2, 26400, true, true, false);
        for (int ch = 0; ch < 2; ++ch)
            live.copyFrom (ch, 24000, tail, ch, 0, 2400);
        // Mutating the source after construction must not change the private instances.
        for (size_t i = 0; i < original.size(); ++i)
            if (auto* p = parameter (f.project, original[i], i == 1 ? "level" : "gain"))
                p->setValueNotifyingHost (0.0f);
        const auto exported = renderer.render();
        expectWithinAbsoluteError (difference (live, exported), 0.0f, 3.0e-6f);
        expectWithinAbsoluteError (difference (exported, renderer.render()), 0.0f, 3.0e-6f);
        juce::TemporaryFile wave (".wav");
        const auto result = renderer.renderToFile (wave.getFile());
        expect (result.wasOk(), result.getErrorMessage());
        juce::AudioFormatManager fileFormats;
        fileFormats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fileFormats.createReaderFor (wave.getFile()));
        expect (reader != nullptr);
        if (reader != nullptr)
        {
            expectEquals (reader->lengthInSamples, (juce::int64) 26400);
            juce::AudioBuffer<float> fileAudio (2, 26400);
            expect (reader->read (&fileAudio, 0, 26400, 0, true, true));
            expectWithinAbsoluteError (difference (exported, fileAudio), 0.0f, 3.0e-6f);
        }

        beginTest (format + " loadFrom clears the previous document's plugin dirty flag");
        expect (f.project.hasUnsavedChanges()); // source parameters were set to zero above
        if (load (f.project, saved.getFile()))
        {
            // replaceContents clears unsavedChanges but leaves pluginsChanged set. LV2
            // state restoration also emits parameter notifications during instance prepare.
            expect (! f.project.hasUnsavedChanges(), "Discarded plugin edits must not leave a loaded file dirty");
        }

        beginTest (format + " copyFrom clears the destination's previous plugin dirty flag");
        if (auto* p = parameter (copied.project, copyDevices[1], "level"))
            p->setValueNotifyingHost (0.2f);
        expect (copied.project.hasUnsavedChanges());
        copied.project.copyFrom (f.project);
        expect (! copied.project.hasUnsavedChanges(), "copyFrom must reset the old plugin dirty flag");
    }

    void testMissing (daw::PluginHost& host, const juce::PluginDescription& gain,
                      const juce::PluginDescription& synth, bool withoutHost)
    {
        const auto context = gain.pluginFormatName + (withoutHost ? " without a host" : " with missing plugin files");
        beginTest (context + " preserves DEVICE descriptions/state while other audio plays");
        PluginFixture source (&host);
        const auto original = populate (source, gain, synth);
        constexpr std::array<float, 3> values { 0.65f, 0.35f, 0.4f };
        for (size_t i = 0; i < original.size(); ++i)
        {
            auto* p = parameter (source.project, original[i], i == 1 ? "level" : "gain");
            if (p == nullptr)
                return;
            p->setValueNotifyingHost (values[i]);
        }
        source.warmUp();
        auto tree = source.project.createSnapshot();
        if (! withoutHost)
            for (auto owner : tree)
                for (auto device : owner)
                    if (daw::Project::isPluginDevice (device))
                    {
                        auto missing = daw::Project::getDeviceType (device) == "pluginInstrument" ? synth : gain;
                        missing.fileOrIdentifier = "/nonexistent/diyDaw-test-plugin-missing";
                        device.setProperty (daw::ids::description, missing.createXml()->toString(), nullptr);
                    }
        juce::TemporaryFile unavailable (".diydaw");
        if (! writeTree (tree, unavailable.getFile()))
            return;
        PluginFixture missing (withoutHost ? nullptr : &host);
        if (! load (missing.project, unavailable.getFile()))
            return;
        expectEquals (missing.project.getNumTracks(), 2);
        const auto missingDevices = devices (missing.project);
        for (const auto& device : missingDevices)
        {
            expect (daw::Project::isPluginDevice (device));
            expect (missing.project.getPluginInstance (device) == nullptr);
            expect (missing.project.getPluginError (device).isNotEmpty());
        }
        const auto out = missing.playFromStart (12000);
        // Missing effects pass dry; the missing instrument stays silent during its MIDI note.
        expectWithinAbsoluteError (out.getSample (0, 8000), 0.5f, 1.0e-6f);
        expectWithinAbsoluteError (out.getSample (0, 11000), 0.5f, 1.0e-6f);
        juce::TemporaryFile resaved (".diydaw");
        if (! save (missing.project, resaved.getFile()))
            return;
        const auto xml = juce::parseXML (resaved.getFile());
        expect (xml != nullptr);
        if (xml == nullptr)
            return;
        auto retained = juce::ValueTree::fromXml (*xml);
        int retainedCount = 0;
        for (auto owner : retained)
            for (auto device : owner)
                if (daw::Project::isPluginDevice (device))
                {
                    juce::ValueTree previous;
                    for (const auto& oldOwner : tree)
                        for (const auto& oldDevice : oldOwner)
                            if (daw::Project::isPluginDevice (oldDevice) && oldDevice[daw::ids::uid] == device[daw::ids::uid])
                                previous = oldDevice;
                    expect (previous.isValid());
                    expectEquals (device[daw::ids::description].toString(), previous[daw::ids::description].toString());
                    expectEquals (device[daw::ids::state].toString(), previous[daw::ids::state].toString());
                    ++retainedCount;
                    if (! withoutHost)
                    {
                        // Simulate relinking the missing bundle without altering its saved state.
                        const auto& available = daw::Project::getDeviceType (device) == "pluginInstrument" ? synth : gain;
                        device.setProperty (daw::ids::description, available.createXml()->toString(), nullptr);
                    }
                }
        expectEquals (retainedCount, 3);
        beginTest (context + " recovers last known parameters when plugins are available again");
        if (! withoutHost && ! writeTree (retained, resaved.getFile()))
            return;
        PluginFixture recovered (&host);
        if (! load (recovered.project, resaved.getFile()))
            return;
        recovered.warmUp();
        const auto recoveredDevices = devices (recovered.project);
        for (size_t i = 0; i < recoveredDevices.size(); ++i)
            if (auto* p = parameter (recovered.project, recoveredDevices[i], i == 1 ? "level" : "gain"))
                expectWithinAbsoluteError (p->getValue(), values[i], 1.0e-6f);
        expectWithinAbsoluteError (difference (source.playFromStart (12000), recovered.playFromStart (12000)), 0.0f, 3.0e-6f);
    }
};

PluginCatalogueTests pluginCatalogueTests;
PluginEffectTests pluginEffectTests;
PluginInstrumentTests pluginInstrumentTests;
PluginPersistenceTests pluginPersistenceTests;
}
