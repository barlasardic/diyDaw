#include "Automation.h"
#include "Effects.h"
#include "Project.h"
#include "ProjectRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <vector>

namespace
{
using Project = daw::Project;
using Audio = juce::AudioBuffer<float>;
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 1024;
constexpr float signalLevel = 0.2f;
constexpr int samplesPerBeat = 24000; // 120 BPM at 48 kHz

juce::String paramTarget (const juce::ValueTree& device, int index)
{
    return device[daw::ids::uid].toString() + ":"
         + Project::getDeviceParamInfos (device)[static_cast<size_t> (index)].id;
}

float difference (const Audio& a, const Audio& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
        return std::numeric_limits<float>::infinity();

    auto result = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
        {
            const auto x = a.getSample (ch, i), y = b.getSample (ch, i);
            if (! std::isfinite (x) || ! std::isfinite (y))
                return std::numeric_limits<float>::infinity();
            result = std::max (result, std::abs (x - y));
        }
    return result;
}

/** Model edits and simulated audio callbacks run sequentially on the test thread. */
struct AutomationFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    Project project { engine, pool };
    const juce::File sampleFile { "/virtual/automation-constant.wav" };

    AutomationFixture()
    {
        engine.prepare (sampleRate, blockSize);
        engine.getMetronome().setEnabled (false);
        auto sample = std::make_shared<daw::SampleData>();
        sample->sampleRate = sampleRate;
        sample->buffer.setSize (2, 6 * static_cast<int> (sampleRate));
        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (sample->buffer.getWritePointer (ch), signalLevel,
                                               sample->getNumFrames());
        pool.insert (daw::SamplePool::keyFor (sampleFile), sample);
    }

    juce::ValueTree audioTrack()
    {
        const auto track = project.addTrack ("Constant");
        project.addClip (track, sampleFile, 0.0);
        return track;
    }

    juce::ValueTree midiTrack()
    {
        const auto track = project.addTrack ("Synth", Project::TrackKind::midi);
        const auto clip = project.addMidiClip (track, 0.0, 8.0);
        project.addNote (clip, 60, 0.0, 8.0, 1.0f);
        project.setDeviceParam (track, daw::Synth::ampAttack, 0.001f);
        project.setDeviceParam (track, daw::Synth::ampSustain, 1.0f);
        project.setDeviceParam (track, daw::Synth::filterEnvAmount, 0.0f);
        return track;
    }

    juce::ValueTree envelope (const juce::ValueTree& track, const juce::String& target,
                              float first, float last, double end = 4.0)
    {
        const auto lane = project.getOrCreateAutomationLane (track, target);
        project.addAutomationPoint (lane, 0.0, first);
        project.addAutomationPoint (lane, end, last);
        return lane;
    }

    Audio render (int count)
    {
        Audio audio (2, count);
        engine.render (audio, count);
        return audio;
    }

    void restart (double beat = 0.0, int preparedBlock = blockSize)
    {
        auto& transport = engine.getTransport();
        transport.stop();
        transport.setPositionBeats (0.0);
        render (1); // Apply initial automation before resetting DSP state.
        engine.prepare (sampleRate, preparedBlock);
        for (int done = 0; done < static_cast<int> (sampleRate * 0.05); done += blockSize)
            render (blockSize);
        transport.setPositionBeats (beat);
        transport.play();
    }

    Audio freshAudio (double beat = 0.5, int count = 16384)
    {
        restart (beat);
        return render (count);
    }

    void stoppedAt (double beat)
    {
        engine.getTransport().stop();
        engine.getTransport().setPositionBeats (beat);
        render (blockSize);
    }
};

/** Pure envelope evaluation, called only on the test thread. */
class AutomationEnvelopeTests : public juce::UnitTest
{
public:
    AutomationEnvelopeTests() : juce::UnitTest ("Automation envelope evaluation", "diyDaw") {}

    void runTest() override
    {
        beginTest ("Linear interpolation holds the endpoints before and after the envelope");
        daw::AutomationLane lane;
        lane.points = { { 1.0, -12.0f }, { 3.0, 4.0f }, { 7.0, -4.0f } };
        for (const auto beat : { -10.0, 0.0, 1.0 })
            expectEquals (lane.valueAt (beat), -12.0f);
        expectEquals (lane.valueAt (2.0), -4.0f);
        expectEquals (lane.valueAt (3.0), 4.0f);
        expectEquals (lane.valueAt (5.0), 0.0f);
        for (const auto beat : { 7.0, 8.0, 100.0 })
            expectEquals (lane.valueAt (beat), -4.0f);

        beginTest ("A single point is constant at every position");
        lane.points = { { 2.5, 0.375f } };
        for (const auto beat : { -100.0, 0.0, 2.5, 100.0 })
            expectEquals (lane.valueAt (beat), 0.375f);

        beginTest ("Coincident interior points form a finite jump, with the last point winning");
        lane.points = { { 0.0, 0.0f }, { 2.0, 2.0f }, { 2.0, 6.0f }, { 4.0, 10.0f } };
        expectEquals (lane.valueAt (1.0), 1.0f);
        expectEquals (lane.valueAt (2.0), 6.0f);
        expectEquals (lane.valueAt (3.0), 8.0f);
        expectWithinAbsoluteError (lane.valueAt (2.0 - 1.0e-6), 2.0f, 2.0e-6f);
        expectWithinAbsoluteError (lane.valueAt (2.0 + 1.0e-6), 6.0f, 3.0e-6f);

        beginTest ("Coincident endpoints hold the first and last values outside their range");
        lane.points = { { 1.0, 2.0f }, { 1.0, 3.0f }, { 4.0, 5.0f }, { 4.0, 7.0f } };
        expectEquals (lane.valueAt (0.0), 2.0f);
        expectEquals (lane.valueAt (2.5), 4.0f);
        expectEquals (lane.valueAt (5.0), 7.0f);
        lane.points = { { 1.0, 2.0f }, { 1.0, 7.0f } };
        expect (std::isfinite (lane.valueAt (1.0)));
        expectEquals (lane.valueAt (0.0), 2.0f);
        expectEquals (lane.valueAt (2.0), 7.0f);
    }
};

/** Target enumeration and undoable model gestures, executed on the test thread. */
class AutomationModelTests : public juce::UnitTest
{
public:
    AutomationModelTests() : juce::UnitTest ("Automation targets and point gestures", "diyDaw") {}

    void runTest() override
    {
        targets();
        gestures();
        deviceIdentity();
    }

private:
    void checkTargets (const juce::ValueTree& track)
    {
        const auto targets = Project::getAutomationTargets (track);
        expect (targets.size() >= 2);
        if (targets.size() < 2)
            return;
        expectEquals (targets[0].id, juce::String ("gain"));
        expectEquals (targets[1].id, juce::String ("pan"));
        expectEquals (targets[0].info.minValue, daw::AudioTrack::minGainDb);
        expectEquals (targets[0].info.maxValue, daw::AudioTrack::maxGainDb);
        expectEquals (targets[1].info.minValue, -1.0f);
        expectEquals (targets[1].info.maxValue, 1.0f);
        size_t index = 2;
        std::set<juce::String> seen { "gain", "pan" };
        for (const auto& device : track)
        {
            if (! device.hasType (daw::ids::DEVICE))
                continue;
            for (const auto& info : Project::getDeviceParamInfos (device))
            {
                if (index >= targets.size())
                {
                    expect (false, "A device parameter is missing from the target list");
                    return;
                }
                const auto& target = targets[index++];
                expectEquals (target.id, device[daw::ids::uid].toString() + ":" + info.id);
                expect (seen.insert (target.id).second, "Target ids must be unique on a track");
                expect (target.name.isNotEmpty());
                expectEquals (target.info.minValue, info.minValue);
                expectEquals (target.info.maxValue, info.maxValue);
                expectEquals (target.info.defaultValue, info.defaultValue);
                expect (target.info.stepped == info.stepped);
                Project::AutomationTarget found;
                expect (Project::findAutomationTarget (track, target.id, found));
                expectEquals (found.id, target.id);
                expectEquals (Project::getTargetModelValue (track, target.id),
                              static_cast<float> (device.getProperty (info.id, info.defaultValue)));
            }
        }
        expectEquals (static_cast<int> (targets.size()), static_cast<int> (index));
    }

    void targets()
    {
        beginTest ("Targets are gain, pan, then all device parameters in chain order");
        AutomationFixture f;
        for (const bool midi : { false, true })
        {
            const auto track = midi ? f.midiTrack() : f.audioTrack();
            for (const auto& type : daw::getEffectTypes())
                f.project.addEffect (track, type);
            f.project.getOrCreateAutomationLane (track, "gain");
            checkTargets (track);
            const auto effects = f.project.getEffects (track);
            f.project.moveEffect (effects.getLast(), 0);
            checkTargets (track);
            f.project.addEffect (track, "eq", 1);
            checkTargets (track);
        }
        expect (Project::getAutomationTargets ({}).empty());

        beginTest ("Creating an existing lane is idempotent; unknown targets are refused");
        const auto track = f.project.getTrack (0);
        const auto before = f.project.getState().createCopy();
        const auto existing = Project::getAutomationLane (track, "gain");
        f.project.getUndoManager().clearUndoHistory();
        expect (f.project.getOrCreateAutomationLane (track, "gain") == existing);
        for (const auto* id : { "missing", "99999:output", "1:missing" })
            expect (! f.project.getOrCreateAutomationLane (track, id).isValid());
        expect (! f.project.getOrCreateAutomationLane ({}, "gain").isValid());
        expect (! f.project.addAutomationPoint ({}, 0.0, 1.0f).isValid());
        expect (f.project.getState().isEquivalentTo (before));
        expect (! f.project.getUndoManager().canUndo());
    }

    template <typename Edit>
    void gesture (AutomationFixture& f, const juce::String& actionName, Edit&& edit)
    {
        const auto before = f.project.getState().createCopy();
        f.project.getUndoManager().clearUndoHistory();
        f.project.beginTransaction (actionName);
        edit();
        f.project.beginTransaction ("Next gesture");
        const auto after = f.project.getState().createCopy();
        expect (! after.isEquivalentTo (before), actionName + " must change the document");
        expect (f.project.undo(), actionName + " must be undoable");
        expect (f.project.getState().isEquivalentTo (before), actionName + " undo must restore the whole gesture");
        expect (! f.project.getUndoManager().canUndo(), actionName + " must take exactly one undo step");
        expect (f.project.redo(), actionName + " must be redoable");
        expect (f.project.getState().isEquivalentTo (after), actionName + " redo must restore the whole gesture");
        expect (! f.project.getUndoManager().canRedo());
    }

    void gestures()
    {
        AutomationFixture f;
        const auto track = f.midiTrack();
        for (const auto& type : daw::getEffectTypes())
            f.project.addEffect (track, type);

        // Exercise every range, including all stepped synth and delay parameters.
        for (const auto& target : Project::getAutomationTargets (track))
        {
            beginTest ("Lane and point gestures clamp and undo atomically: " + target.id);
            juce::ValueTree lane;
            gesture (f, "Create lane", [&] { lane = f.project.getOrCreateAutomationLane (track, target.id); });
            expect (Project::isAutomationLane (lane));
            expectEquals (Project::getAutomationValueAt (lane, 1.0, 123.0f), 123.0f);
            juce::ValueTree point;
            gesture (f, "Add point", [&]
            {
                point = f.project.addAutomationPoint (lane, -2.0, target.info.minValue - 100.0f);
            });
            expectEquals (Project::getPointBeat (point), 0.0);
            expectEquals (Project::getPointValue (point), target.info.minValue);
            gesture (f, "Drag point", [&]
            {
                f.project.moveAutomationPoint (point, 2.0, target.info.minValue);
                f.project.moveAutomationPoint (point, 5.0, target.info.maxValue + 100.0f);
            });
            expectEquals (Project::getPointBeat (point), 5.0);
            expectEquals (Project::getPointValue (point), target.info.maxValue);
            const auto input = target.info.minValue + 0.6f * (target.info.maxValue - target.info.minValue);
            gesture (f, "Drag to a fractional value", [&]
            {
                f.project.moveAutomationPoint (point, 6.0, input);
                f.project.moveAutomationPoint (point, -1.0, input);
            });
            expectEquals (Project::getPointBeat (point), 0.0);
            expectEquals (Project::getPointValue (point), target.info.stepped ? std::round (input) : input);
            gesture (f, "Delete point", [&] { f.project.deleteAutomationPoint (point); });
            expectEquals (lane.getNumChildren(), 0);
            gesture (f, "Remove lane", [&] { f.project.removeAutomationLane (lane); });
            expect (! Project::getAutomationLane (track, target.id).isValid());
        }

        beginTest ("Adding fractional and excessive values clamps and rounds immediately");
        for (const auto& target : Project::getAutomationTargets (track))
        {
            const auto lane = f.project.getOrCreateAutomationLane (track, target.id);
            const auto high = f.project.addAutomationPoint (lane, 3.0, target.info.maxValue + 1000.0f);
            expectEquals (Project::getPointValue (high), target.info.maxValue);
            const auto input = target.info.minValue + 0.6f;
            const auto point = f.project.addAutomationPoint (lane, 1.0, input);
            expectEquals (Project::getPointValue (point), target.info.stepped ? std::round (input) : input);
        }

        beginTest ("Model evaluation sorts out-of-order points and agrees with the engine envelope");
        const auto lane = f.project.getOrCreateAutomationLane (track, "pan");
        while (lane.getNumChildren() > 0)
            f.project.deleteAutomationPoint (lane.getChild (0));
        f.project.addAutomationPoint (lane, 4.0, 1.0f);
        f.project.addAutomationPoint (lane, 0.0, -1.0f);
        f.project.addAutomationPoint (lane, 2.0, -0.5f);
        f.project.addAutomationPoint (lane, 2.0, 0.5f);
        daw::AutomationLane expected;
        expected.points = { { 0.0, -1.0f }, { 2.0, -0.5f }, { 2.0, 0.5f }, { 4.0, 1.0f } };
        for (const auto beat : { -1.0, 0.0, 1.0, 2.0, 3.0, 4.0, 9.0 })
            expectEquals (Project::getAutomationValueAt (lane, beat, 99.0f), expected.valueAt (beat));
    }

    void deviceIdentity()
    {
        beginTest ("Device uids are positive and unique across MIDI instruments and all effect chains");
        AutomationFixture f;
        f.project.newDocument (2, 3);
        std::set<int> used;
        for (const auto& track : f.project.getState())
        {
            for (const auto& type : daw::getEffectTypes())
                f.project.addEffect (track, type);
            for (const auto& device : track)
                if (device.hasType (daw::ids::DEVICE))
                {
                    const auto uid = static_cast<int> (device[daw::ids::uid]);
                    expect (uid > 0);
                    expect (used.insert (uid).second, "A uid must not be shared by different tracks");
                }
        }
        const auto track = f.project.addTrack ("Another synth", Project::TrackKind::midi);
        const auto synthUid = static_cast<int> (Project::getDevice (track)[daw::ids::uid]);
        expect (used.insert (synthUid).second, "New instruments need a fresh project-wide uid");
        const auto effect = f.project.addEffect (track, "eq");
        expect (used.insert (static_cast<int> (effect[daw::ids::uid])).second,
                "New effects need a fresh project-wide uid");
    }
};

/** Rendered signal and automation lifecycle tests, run sequentially on the test thread. */
class AutomationEngineTests : public juce::UnitTest
{
public:
    AutomationEngineTests() : juce::UnitTest ("Automation engine rendering and lifecycle", "diyDaw") {}

    void runTest() override
    {
        gainAndPan();
        deviceParameters();
        chunking();
        stopped();
        restoreModelValues();
        effectLifetime();
        effectOrder();
    }

private:
    void gainAndPan()
    {
        beginTest ("Volume ramp follows -60 to 0 dB, allowing 256 samples plus 20 ms smoothing");
        AutomationFixture f;
        const auto track = f.audioTrack();
        f.envelope (track, "gain", -60.0f, 0.0f);
        f.restart();
        const auto audio = f.render (static_cast<int> (4.25 * samplesPerBeat));
        // A monotonic ramp lies between its current target and the target one control
        // block plus one smoothing interval ago. Ignore the clip's edge fade.
        constexpr int lag = 256 + 960;
        const auto centre = std::sqrt (0.5f);
        auto previous = 0.0f;
        for (int i = samplesPerBeat / 2; i < audio.getNumSamples(); i += samplesPerBeat / 4)
        {
            const auto dbAt = [] (int sample)
            {
                return juce::jlimit (-60.0f, 0.0f, -60.0f + 15.0f * static_cast<float> (sample) / samplesPerBeat);
            };
            const auto low = signalLevel * centre * juce::Decibels::decibelsToGain (dbAt (i - lag));
            const auto high = signalLevel * centre * juce::Decibels::decibelsToGain (dbAt (i));
            const auto value = audio.getSample (0, i);
            expect (value >= low - 2.0e-6f && value <= high + 2.0e-6f,
                    "Volume ramp outside its control-rate/smoothing bounds at sample " + juce::String (i));
            expect (value > previous, "The rendered ramp must rise over time");
            previous = value;
            expectWithinAbsoluteError (audio.getSample (1, i), value, 1.0e-7f);
        }
        expectWithinAbsoluteError (audio.getSample (0, audio.getNumSamples() - 1), signalLevel * centre, 1.0e-6f);
        expectEquals (Project::getTrackGain (track), 0.0f, "Automation must not overwrite the document's fader");

        beginTest ("Pan automation traverses the stereo field with the constant-power pan law");
        f.project.removeAutomationLane (Project::getAutomationLane (track, "gain"));
        f.envelope (track, "pan", -1.0f, 1.0f);
        for (const auto beat : { 0.0, 2.0, 4.0 })
        {
            f.stoppedAt (beat);
            expectWithinAbsoluteError (f.project.getEngineTrack (track).getPan(),
                                       static_cast<float> (beat * 0.5 - 1.0), 1.0e-6f);
        }
        f.restart();
        const auto panned = f.render (static_cast<int> (4.25 * samplesPerBeat));
        for (const int i : { samplesPerBeat / 2, 2 * samplesPerBeat, 3 * samplesPerBeat })
        {
            const auto angle = juce::MathConstants<float>::halfPi * static_cast<float> (i) / (4 * samplesPerBeat);
            expectWithinAbsoluteError (panned.getSample (0, i), signalLevel * std::cos (angle), 0.006f);
            expectWithinAbsoluteError (panned.getSample (1, i), signalLevel * std::sin (angle), 0.006f);
        }
        const auto end = panned.getNumSamples() - 1;
        expectWithinAbsoluteError (panned.getSample (0, end), 0.0f, 1.0e-6f);
        expectWithinAbsoluteError (panned.getSample (1, end), signalLevel, 1.0e-6f);
        expectEquals (Project::getTrackPan (track), 0.0f);
    }

    void deviceParameters()
    {
        beginTest ("Synth volume automation changes the engine parameter and the rendered signal");
        AutomationFixture synthFixture;
        const auto track = synthFixture.midiTrack();
        const auto device = Project::getDevice (track);
        const auto dry = synthFixture.freshAudio (0.0);
        synthFixture.envelope (track, paramTarget (device, daw::Synth::volume), -24.0f, -24.0f);
        const auto quiet = synthFixture.freshAudio (0.0);
        expectEquals (synthFixture.project.getEngineTrack (track).getSynth().getParam (daw::Synth::volume), -24.0f);
        expectEquals (Project::getDeviceParam (device, daw::Synth::volume), 0.0f);
        const auto dryRms = dry.getRMSLevel (0, 4096, 8192);
        expect (dryRms > 0.001f, "The MIDI note must produce audible reference audio");
        if (dryRms > 0.0f)
            expectWithinAbsoluteError (quiet.getRMSLevel (0, 4096, 8192) / dryRms,
                                       juce::Decibels::decibelsToGain (-24.0f), 1.0e-5f);
        const auto volumeLane = Project::getAutomationLane (track, paramTarget (device, daw::Synth::volume));
        synthFixture.project.moveAutomationPoint (volumeLane.getChild (1), 4.0, 0.0f);
        synthFixture.stoppedAt (2.0);
        expectWithinAbsoluteError (synthFixture.project.getEngineTrack (track).getSynth().getParam (daw::Synth::volume),
                                   -12.0f, 1.0e-6f);

        beginTest ("EQ output automation drives its live instance and attenuates audio");
        AutomationFixture f;
        const auto audioTrack = f.audioTrack();
        const auto eq = f.project.addEffect (audioTrack, "eq");
        const auto lane = f.envelope (audioTrack, paramTarget (eq, daw::EqEffect::output), -12.0f, 0.0f);
        const auto instance = f.project.getEffectInstance (eq);
        expect (instance != nullptr);
        if (instance == nullptr)
            return;
        for (const auto beat : { 0.0, 2.0, 4.0 })
        {
            f.stoppedAt (beat);
            expectWithinAbsoluteError (instance->getParam (daw::EqEffect::output),
                                       static_cast<float> (-12.0 + 3.0 * beat), 1.0e-6f);
        }
        f.project.moveAutomationPoint (lane.getChild (1), 4.0, -12.0f);
        const auto attenuated = f.freshAudio();
        expectWithinAbsoluteError (attenuated.getRMSLevel (0, 4096, 8192),
                                   signalLevel * std::sqrt (0.5f) * juce::Decibels::decibelsToGain (-12.0f), 1.0e-6f);
        expectEquals (Project::getDeviceParam (eq, daw::EqEffect::output), 0.0f);
    }

    void chunking()
    {
        beginTest ("Large callbacks evaluate automation in at most 256-sample chunks");
        AutomationFixture f;
        const auto track = f.audioTrack();
        f.envelope (track, "gain", -48.0f, 0.0f);
        for (const int prepared : { 64, 1024 })
            for (const int count : { 1, 7, 64, 256, 513, 4099 })
            {
                constexpr double start = 1.25;
                f.restart (start, prepared);
                f.render (count);
                const auto chunk = std::min (prepared, 256);
                const auto lastOffset = ((count - 1) / chunk) * chunk;
                const auto expected = -48.0f + 12.0f * static_cast<float> (start + static_cast<double> (lastOffset) / samplesPerBeat);
                expectWithinAbsoluteError (f.project.getEngineTrack (track).getGainDb(), expected, 1.0e-5f,
                                           "prepared=" + juce::String (prepared) + ", callback=" + juce::String (count));
                expectWithinAbsoluteError (f.engine.getTransport().getPositionBeats(),
                                           start + static_cast<double> (count) / samplesPerBeat, 1.0e-10);
            }

        beginTest ("Aligned large and small callbacks produce identical automated audio");
        f.restart();
        const auto large = f.render (32768);
        f.restart();
        const auto small = daw::renderOffline (f.engine, 32768.0 / sampleRate, 256);
        expectWithinAbsoluteError (difference (large, small), 0.0f, 1.0e-7f);

        beginTest ("Automation is reevaluated at a short loop wrap inside a large callback");
        const auto eq = f.project.addEffect (track, "eq");
        f.envelope (track, paramTarget (eq, daw::EqEffect::output), -12.0f, 12.0f);
        const auto instance = f.project.getEffectInstance (eq);
        f.restart (1.0);
        f.engine.getTransport().setLoop (1.0, 1.0 + 1.0 / 64.0); // 375 samples: a partial control block at each wrap
        f.engine.getTransport().setLoopEnabled (true);
        f.render (751); // two wraps, then one sample at beat 1
        expectWithinAbsoluteError (f.engine.getTransport().getPositionBeats(), 1.0 + 1.0 / samplesPerBeat, 1.0e-9);
        if (instance != nullptr)
            expectWithinAbsoluteError (instance->getParam (daw::EqEffect::output), -6.0f, 1.0e-5f);
        else
            expect (false, "Missing EQ instance");
    }

    void stopped()
    {
        beginTest ("Seeking while stopped applies mixer, synth and effect envelopes without advancing");
        AutomationFixture f;
        const auto track = f.midiTrack();
        const auto synth = Project::getDevice (track);
        const auto eq = f.project.addEffect (track, "eq");
        f.envelope (track, "gain", -48.0f, 0.0f);
        f.envelope (track, "pan", -1.0f, 1.0f);
        f.envelope (track, paramTarget (synth, daw::Synth::filterCutoff), 200.0f, 8200.0f);
        f.envelope (track, paramTarget (eq, daw::EqEffect::output), -12.0f, 12.0f);
        const auto instance = f.project.getEffectInstance (eq);
        for (const auto beat : { 3.0, 0.0, 1.0, 8.0 })
        {
            f.stoppedAt (beat);
            const auto t = static_cast<float> (std::min (beat / 4.0, 1.0));
            auto& engineTrack = f.project.getEngineTrack (track);
            expectWithinAbsoluteError (engineTrack.getGainDb(), -48.0f + 48.0f * t, 1.0e-5f);
            expectWithinAbsoluteError (engineTrack.getPan(), -1.0f + 2.0f * t, 1.0e-6f);
            expectWithinAbsoluteError (engineTrack.getSynth().getParam (daw::Synth::filterCutoff),
                                       200.0f + 8000.0f * t, 0.001f);
            if (instance != nullptr)
                expectWithinAbsoluteError (instance->getParam (daw::EqEffect::output), -12.0f + 24.0f * t, 1.0e-5f);
            else
                expect (false, "Missing EQ instance");
            expect (! f.engine.getTransport().isPlaying());
            expectEquals (f.engine.getTransport().getPositionBeats(), beat);
            expectEquals (f.render (1).getMagnitude (0, 1), 0.0f);
        }
    }

    void restoreModelValues()
    {
        for (int kind = 0; kind < 4; ++kind)
            for (const bool removeLane : { false, true })
            {
                beginTest (juce::String (removeLane ? "Removing lane" : "Deleting last point")
                           + " restores the model value; undo restores automation, target " + juce::String (kind));
                AutomationFixture f;
                const auto track = kind == 2 ? f.midiTrack() : f.audioTrack();
                const auto eq = kind == 3 ? f.project.addEffect (track, "eq") : juce::ValueTree{};
                const auto target = kind == 0 ? juce::String ("gain")
                                  : kind == 1 ? juce::String ("pan")
                                  : kind == 2 ? paramTarget (Project::getDevice (track), daw::Synth::volume)
                                              : paramTarget (eq, daw::EqEffect::output);
                const float base = kind == 1 ? 0.25f : -3.0f;
                const float automated = kind == 1 ? -0.75f : -18.0f;
                if (kind == 0) f.project.setTrackGain (track, base);
                if (kind == 1) f.project.setTrackPan (track, base);
                if (kind == 2) f.project.setDeviceParam (track, daw::Synth::volume, base);
                if (kind == 3) f.project.setDeviceParam (eq, daw::EqEffect::output, base);
                const auto startBeat = kind == 2 ? 0.0 : 0.5; // Start at the MIDI note-on.
                const auto baseAudio = f.freshAudio (startBeat);
                const auto lane = f.project.getOrCreateAutomationLane (track, target);
                const auto point = f.project.addAutomationPoint (lane, 0.0, automated);
                const auto automatedAudio = f.freshAudio (startBeat);
                const auto read = [&]
                {
                    auto& engineTrack = f.project.getEngineTrack (track);
                    if (kind == 0) return engineTrack.getGainDb();
                    if (kind == 1) return engineTrack.getPan();
                    if (kind == 2) return engineTrack.getSynth().getParam (daw::Synth::volume);
                    const auto effect = f.project.getEffectInstance (eq);
                    return effect != nullptr ? effect->getParam (daw::EqEffect::output)
                                             : std::numeric_limits<float>::quiet_NaN();
                };
                expectEquals (read(), automated);
                expect (difference (baseAudio, automatedAudio) > 0.001f);
                f.project.getUndoManager().clearUndoHistory();
                f.project.beginTransaction ("Remove automation");
                if (removeLane) f.project.removeAutomationLane (lane);
                else            f.project.deleteAutomationPoint (point);
                f.project.beginTransaction ("Next gesture");
                f.stoppedAt (1.0);
                expectEquals (read(), base);
                expectWithinAbsoluteError (difference (f.freshAudio (startBeat), baseAudio), 0.0f, 1.0e-6f);
                expect (f.project.undo());
                expect (! f.project.getUndoManager().canUndo());
                f.stoppedAt (1.0);
                expectEquals (read(), automated);
                expectWithinAbsoluteError (difference (f.freshAudio (startBeat), automatedAudio), 0.0f, 1.0e-6f);
                expect (f.project.redo());
                f.stoppedAt (1.0);
                expectEquals (read(), base);
                expectEquals (Project::getTargetModelValue (track, target), base);
            }
    }

    void effectLifetime()
    {
        beginTest ("Removing an effect removes only its lanes; one undo restores a fresh automated instance");
        AutomationFixture f;
        const auto track = f.audioTrack();
        const auto eq = f.project.addEffect (track, "eq");
        const auto other = f.project.addEffect (track, "eq");
        const auto outputId = paramTarget (eq, daw::EqEffect::output);
        const auto output = f.envelope (track, outputId, -12.0f, -12.0f);
        const auto frequency = f.envelope (track, paramTarget (eq, daw::EqEffect::lowFreq), 300.0f, 300.0f);
        const auto otherLane = f.envelope (track, paramTarget (other, daw::EqEffect::output), 0.0f, 0.0f);
        const auto gain = f.envelope (track, "gain", 0.0f, 0.0f);
        const auto instance = f.project.getEffectInstance (eq);
        const auto before = f.project.getState().createCopy();
        const auto wet = f.freshAudio();
        f.project.getUndoManager().clearUndoHistory();
        f.project.beginTransaction ("Remove EQ and its automation");
        f.project.removeEffect (eq);
        f.project.beginTransaction ("Next gesture");
        expect (! output.getParent().isValid() && ! frequency.getParent().isValid());
        expect (otherLane.getParent() == track && gain.getParent() == track);
        expect (f.project.getEffectInstance (eq) == nullptr);
        const auto dry = f.freshAudio();
        expectWithinAbsoluteError (dry.getSample (0, 8192), signalLevel * std::sqrt (0.5f), 1.0e-6f);
        expect (difference (dry, wet) > 0.01f);
        expect (f.project.undo());
        expect (! f.project.getUndoManager().canUndo());
        expect (f.project.getState().isEquivalentTo (before));
        const auto replacement = f.project.getEffectInstance (eq);
        expect (replacement != nullptr && replacement != instance);
        expect (Project::getAutomationLane (track, outputId) == output);
        expectWithinAbsoluteError (difference (f.freshAudio(), wet), 0.0f, 1.0e-6f);
        if (replacement != nullptr)
            expectEquals (replacement->getParam (daw::EqEffect::output), -12.0f);
        expect (f.project.redo());
        expect (f.project.getEffectInstance (eq) == nullptr);
        expect (! Project::getAutomationLane (track, outputId).isValid());
    }

    void effectOrder()
    {
        beginTest ("Reordering and inserting identical effect types preserves automation device identity");
        AutomationFixture f;
        const auto track = f.audioTrack();
        const auto first = f.project.addEffect (track, "eq");
        const auto second = f.project.addEffect (track, "eq");
        const auto firstId = paramTarget (first, daw::EqEffect::output);
        const auto secondId = paramTarget (second, daw::EqEffect::output);
        f.envelope (track, firstId, -12.0f, 0.0f);
        f.envelope (track, secondId, 6.0f, 2.0f);
        const auto firstInstance = f.project.getEffectInstance (first);
        const auto secondInstance = f.project.getEffectInstance (second);
        const auto before = f.freshAudio (2.0);
        f.project.moveEffect (second, 0);
        const auto inserted = f.project.addEffect (track, "eq", 1);
        const auto after = f.freshAudio (2.0);
        expect (f.project.getEffectInstance (first) == firstInstance);
        expect (f.project.getEffectInstance (second) == secondInstance);
        expectEquals (Project::getAutomationTargetId (Project::getAutomationLane (track, firstId)), firstId);
        expectEquals (Project::getAutomationTargetId (Project::getAutomationLane (track, secondId)), secondId);
        f.stoppedAt (2.0);
        if (firstInstance != nullptr && secondInstance != nullptr)
        {
            expectEquals (firstInstance->getParam (daw::EqEffect::output), -6.0f);
            expectEquals (secondInstance->getParam (daw::EqEffect::output), 4.0f);
        }
        else
            expect (false, "Missing EQ instances");
        const auto insertedInstance = f.project.getEffectInstance (inserted);
        if (insertedInstance != nullptr)
            expectEquals (insertedInstance->getParam (daw::EqEffect::output), 0.0f);
        else
            expect (false, "Missing inserted EQ instance");
        expectWithinAbsoluteError (difference (before, after), 0.0f, 1.0e-6f);
    }
};

/** File import, round-trip and independent offline export tests on the test thread. */
class AutomationPersistenceTests : public juce::UnitTest
{
public:
    AutomationPersistenceTests() : juce::UnitTest ("Automation persistence, uid repair and export", "diyDaw") {}

    void runTest() override
    {
        roundTripAndExport();
        repeatedExport();
        importedUids();
        orphanLanes();
    }

private:
    void roundTripAndExport()
    {
        beginTest ("Saving and loading preserves every uid, target, point order and exact beat/value");
        AutomationFixture source;
        const auto track = source.audioTrack();
        const auto eq = source.project.addEffect (track, "eq");
        const auto midi = source.midiTrack();
        const auto synth = Project::getDevice (midi);
        source.envelope (track, "gain", -48.0f, 0.0f);
        source.envelope (track, "pan", -0.75f, 0.75f);
        source.envelope (track, paramTarget (eq, daw::EqEffect::output), -12.0f, 6.0f);
        source.envelope (midi, paramTarget (synth, daw::Synth::volume), -24.0f, -6.0f);
        source.envelope (midi, paramTarget (synth, daw::Synth::filterCutoff), 300.0f, 8000.0f);
        const auto gain = Project::getAutomationLane (track, "gain");
        source.project.addAutomationPoint (gain, 1.0 / 3.0, -23.456789f);
        source.project.addAutomationPoint (gain, 2.0, -18.125f);
        source.project.addAutomationPoint (gain, 2.0, -12.25f);
        source.project.getOrCreateAutomationLane (midi, "pan");
        const auto original = source.project.getState().createCopy();
        juce::TemporaryFile saved (".diydaw");
        expect (source.project.saveTo (saved.getFile()).wasOk());

        AutomationFixture loaded;
        expect (loaded.project.loadFrom (saved.getFile()).wasOk());
        expectEquals (loaded.project.getNumTracks(), source.project.getNumTracks());
        for (int i = 0; i < source.project.getNumTracks(); ++i)
        {
            const auto beforeTrack = source.project.getTrack (i);
            const auto afterTrack = loaded.project.getTrack (i);
            std::vector<juce::String> beforeUids, afterUids;
            for (const auto& child : beforeTrack)
            {
                if (child.hasType (daw::ids::DEVICE))
                    beforeUids.push_back (child[daw::ids::uid].toString());
                if (! Project::isAutomationLane (child))
                    continue;
                const auto after = Project::getAutomationLane (afterTrack, Project::getAutomationTargetId (child));
                expect (after.isValid());
                expect (child.isEquivalentTo (after), "Lane contents must survive exactly, including point order");
                expectEquals (after.getNumChildren(), child.getNumChildren());
                for (int p = 0; p < child.getNumChildren(); ++p)
                {
                    expectEquals (Project::getPointBeat (after.getChild (p)), Project::getPointBeat (child.getChild (p)));
                    expectEquals (Project::getPointValue (after.getChild (p)), Project::getPointValue (child.getChild (p)));
                }
            }
            for (const auto& child : afterTrack)
                if (child.hasType (daw::ids::DEVICE))
                    afterUids.push_back (child[daw::ids::uid].toString());
            expect (beforeUids == afterUids, "Device uids must survive saving and loading unchanged");
        }
        expect (source.project.getState().isEquivalentTo (original), "Saving must not alter the source tree");
        const auto sourceAudio = source.freshAudio (0.0, 4 * samplesPerBeat);
        const auto loadedAudio = loaded.freshAudio (0.0, 4 * samplesPerBeat);
        expect (sourceAudio.getMagnitude (0, sourceAudio.getNumSamples()) > 0.01f);
        expectWithinAbsoluteError (difference (sourceAudio, loadedAudio), 0.0f, 1.0e-6f);

        beginTest ("ProjectRenderer copies automation and its exported float WAV matches live rendering");
        daw::ExportSettings settings;
        settings.startBeat = 0.0;
        settings.endBeat = 4.0;
        settings.sampleRate = sampleRate;
        settings.bitDepth = 32;
        daw::ProjectRenderer renderer (source.project, settings);
        const auto exported = renderer.render();
        expectWithinAbsoluteError (difference (exported, sourceAudio), 0.0f, 1.0e-6f);
        juce::TemporaryFile wav (".wav");
        daw::ProjectRenderer fileRenderer (source.project, settings);
        expect (fileRenderer.renderToFile (wav.getFile()).wasOk());
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const auto decoded = daw::loadSample (wav.getFile(), formats);
        expect (decoded != nullptr);
        if (decoded != nullptr)
            expectWithinAbsoluteError (difference (decoded->buffer, exported), 0.0f, 1.0e-6f);

        beginTest ("Removing automation changes the bounce; the existing renderer retains its snapshot");
        daw::ProjectRenderer snapshotRenderer (source.project, settings);
        for (const auto& t : source.project.getState())
        {
            juce::Array<juce::ValueTree> lanes;
            for (const auto& child : t)
                if (Project::isAutomationLane (child))
                    lanes.add (child);
            for (const auto& lane : lanes)
                source.project.removeAutomationLane (lane);
        }
        daw::ProjectRenderer dryRenderer (source.project, settings);
        expect (difference (dryRenderer.render(), exported) > 0.01f);
        expectWithinAbsoluteError (difference (snapshotRenderer.render(), exported), 0.0f, 1.0e-6f);
    }

    void repeatedExport()
    {
        beginTest ("Repeated exports of the same automation start with identical audio");
        AutomationFixture f;
        const auto track = f.audioTrack();
        f.envelope (track, "gain", -48.0f, 0.0f);
        daw::ExportSettings settings;
        settings.endBeat = 4.0;
        settings.sampleRate = sampleRate;
        daw::ProjectRenderer renderer (f.project, settings);
        const auto first = renderer.render();
        const auto second = renderer.render();
        expectWithinAbsoluteError (difference (first, second), 0.0f, 1.0e-6f,
                                   "BUG: ProjectRenderer warms up at the previous end beat before seeking, retaining the wrong initial fader value");
    }

    void importedUids()
    {
        beginTest ("Hand-written XML repairs missing, zero, negative and project-wide duplicate device uids");
        juce::TemporaryFile xml (".diydaw");
        expect (xml.getFile().replaceWithText (R"xml(<PROJECT version="1" bpm="120">
            <TRACK kind="midi"><DEVICE type="synth" uid="7"/><DEVICE type="eq"/>
                <DEVICE type="delay" uid="0"/></TRACK>
            <TRACK kind="midi"><DEVICE type="synth" uid="7"/>
                <DEVICE type="eq" uid="-3"/><DEVICE type="reverb" uid="12"/>
                <DEVICE type="compressor" uid="12"/></TRACK>
            <TRACK kind="midi"/>
        </PROJECT>)xml"));
        AutomationFixture f;
        expect (f.project.loadFrom (xml.getFile()).wasOk());
        std::set<int> used;
        int count = 0;
        for (const auto& track : f.project.getState())
            for (const auto& device : track)
                if (device.hasType (daw::ids::DEVICE))
                {
                    const auto uid = static_cast<int> (device[daw::ids::uid]);
                    expect (uid > 0);
                    expect (used.insert (uid).second, "Loaded DEVICE uids must be unique across the project");
                    ++count;
                }
        expectEquals (count, 8);
        expectEquals (static_cast<int> (Project::getDevice (f.project.getTrack (0))[daw::ids::uid]), 7);
        const auto newDevice = f.project.addEffect (f.project.getTrack (0), "eq");
        expect (used.insert (static_cast<int> (newDevice[daw::ids::uid])).second);
        const auto newTrack = f.project.addTrack ("New", Project::TrackKind::midi);
        expect (used.insert (static_cast<int> (Project::getDevice (newTrack)[daw::ids::uid])).second);
        juce::TemporaryFile saved (".diydaw");
        expect (f.project.saveTo (saved.getFile()).wasOk());
        AutomationFixture reloaded;
        expect (reloaded.project.loadFrom (saved.getFile()).wasOk());
        for (int i = 0; i < f.project.getNumTracks(); ++i)
        {
            const auto before = Project::getAutomationTargets (f.project.getTrack (i));
            const auto after = Project::getAutomationTargets (reloaded.project.getTrack (i));
            expectEquals (static_cast<int> (before.size()), static_cast<int> (after.size()));
            for (size_t p = 0; p < std::min (before.size(), after.size()); ++p)
                expectEquals (before[p].id, after[p].id);
        }

        beginTest ("Uid repair must republish automation so a now-unresolvable target is ignored");
        // This lane resolves when the track is appended, before uid=0 is repaired.
        // Afterwards device 0 no longer exists, so the retained lane must be ignored.
        expect (xml.getFile().replaceWithText (R"xml(<PROJECT version="1" bpm="120">
            <TRACK kind="audio"><DEVICE type="eq" uid="0" output="3"/>
                <AUTOMATION target="0:output"><POINT beat="0" value="-12"/></AUTOMATION>
            </TRACK>
        </PROJECT>)xml"));
        expect (f.project.loadFrom (xml.getFile()).wasOk());
        const auto track = f.project.getTrack (0);
        const auto eq = f.project.getEffects (track).getFirst();
        expect (static_cast<int> (eq[daw::ids::uid]) > 0);
        Project::AutomationTarget target;
        expect (! Project::findAutomationTarget (track, "0:output", target));
        expect (Project::getAutomationLane (track, "0:output").isValid());
        const auto instance = f.project.getEffectInstance (eq);
        f.stoppedAt (2.0);
        if (instance != nullptr)
            expectEquals (instance->getParam (daw::EqEffect::output), 3.0f,
                          "BUG: replaceContents repairs the uid without rebuilding its resolved automation snapshot");
        else
            expect (false, "Missing repaired EQ instance");
    }

    void orphanLanes()
    {
        beginTest ("Missing device and parameter targets are silent engine no-ops and survive file round trips");
        juce::TemporaryFile xml (".diydaw");
        expect (xml.getFile().replaceWithText (R"xml(<PROJECT version="1" bpm="120">
            <TRACK kind="audio"><DEVICE type="eq" uid="42" output="0"/>
                <AUTOMATION target="9999:output"><POINT beat="0.3333333333333333" value="-18"/></AUTOMATION>
                <AUTOMATION target="42:removedParameter"><POINT beat="2" value="0.125"/></AUTOMATION>
                <AUTOMATION target="removedTarget"><POINT beat="4" value="17"/></AUTOMATION>
            </TRACK>
        </PROJECT>)xml"));
        AutomationFixture f;
        expect (f.project.loadFrom (xml.getFile()).wasOk());
        const auto track = f.project.getTrack (0);
        f.project.addClip (track, f.sampleFile, 0.0);
        const auto before = track.createCopy();
        const auto audio = f.freshAudio();
        expectWithinAbsoluteError (audio.getSample (0, 8192), signalLevel * std::sqrt (0.5f), 1.0e-6f);
        f.stoppedAt (3.0);
        const auto eq = f.project.getEffects (track).getFirst();
        const auto instance = f.project.getEffectInstance (eq);
        if (instance != nullptr)
            expectEquals (instance->getParam (daw::EqEffect::output), 0.0f);
        else
            expect (false, "Missing EQ instance");
        juce::TemporaryFile saved (".diydaw");
        expect (f.project.saveTo (saved.getFile()).wasOk());
        AutomationFixture loaded;
        expect (loaded.project.loadFrom (saved.getFile()).wasOk());
        for (const auto* id : { "9999:output", "42:removedParameter", "removedTarget" })
        {
            const auto lane = Project::getAutomationLane (loaded.project.getTrack (0), id);
            expect (lane.isValid());
            expect (lane.isEquivalentTo (Project::getAutomationLane (before, id)));
        }
        expectWithinAbsoluteError (difference (loaded.freshAudio(), audio), 0.0f, 1.0e-6f);
    }
};

static AutomationEnvelopeTests automationEnvelopeTests;
static AutomationModelTests automationModelTests;
static AutomationEngineTests automationEngineTests;
static AutomationPersistenceTests automationPersistenceTests;
} // namespace
