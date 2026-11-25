#include "Project.h"
#include "Instrument.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
using Project = daw::Project;
using Synth = daw::Synth;
using Event = daw::NoteEvent;
constexpr double sampleRate = 48000.0;
constexpr double bpm = 120.0;
constexpr double samplesPerBeat = sampleRate * 60.0 / bpm;
constexpr float audioTolerance = 2.0e-6f;

// Test-thread helpers: prepare, edits, and simulated audio callbacks run sequentially.
const std::vector<std::pair<int, float>>& sineParams()
{
    static const std::vector<std::pair<int, float>> values {
        { Synth::osc1Wave, 3.0f }, { Synth::osc2Wave, 3.0f }, { Synth::oscMix, 0.0f },
        { Synth::filterCutoff, 20000.0f }, { Synth::filterResonance, 0.0f },
        { Synth::filterEnvAmount, 0.0f }, { Synth::filterKeyTrack, 0.0f },
        { Synth::ampAttack, 0.001f }, { Synth::ampDecay, 0.001f },
        { Synth::ampSustain, 1.0f }, { Synth::ampRelease, 0.02f }
    };
    return values;
}

void configure (Synth& synth)
{
    for (const auto& [param, value] : sineParams())
        synth.setParam (param, value);
}

int sampleAt (double beat)
{
    return static_cast<int> (std::ceil (beat * samplesPerBeat - 1.0e-3));
}

void appendNote (std::vector<Event>& events, double start, double end, int pitch = 60, float velocity = 0.8f)
{
    events.push_back ({ sampleAt (start), pitch, velocity, true });
    events.push_back ({ sampleAt (end), pitch, 0.0f, false });
}

daw::Clip midiClip (double start, double length, std::vector<daw::MidiNote> notes,
                    bool looping = false, double loopLength = 0.0, double offset = 0.0)
{
    auto sequence = std::make_shared<daw::MidiSequence>();
    std::sort (notes.begin(), notes.end(), [] (const auto& a, const auto& b) { return a.startBeat < b.startBeat; });
    sequence->notes = std::move (notes);
    daw::Clip clip;
    clip.startBeat = start;
    clip.lengthBeats = length;
    clip.looping = looping;
    clip.loopLengthBeats = loopLength;
    clip.offsetBeats = offset;
    clip.notes = std::move (sequence);
    return clip;
}

void publish (daw::AudioTrack& track, const std::vector<daw::Clip>& clips)
{
    auto list = std::make_unique<daw::ClipList>();
    list->clips = clips;
    std::sort (list->clips.begin(), list->clips.end(), [] (const auto& a, const auto& b) { return a.startBeat < b.startBeat; });
    track.setClips (std::move (list));
}

void prepare (daw::Engine& engine, int blockSize)
{
    engine.getTransport().stop();
    engine.getTransport().setPositionBeats (0.0);
    engine.getMetronome().setEnabled (false);
    engine.prepare (sampleRate, blockSize);
    // Settle the track's 20 ms fader ramp without advancing the timeline.
    daw::renderOffline (engine, 4096.0 / sampleRate, blockSize);
}

juce::AudioBuffer<float> renderSamples (daw::Engine& engine, int samples, int blockSize)
{
    return daw::renderOffline (engine, samples / sampleRate, blockSize);
}

juce::AudioBuffer<float> reference (int samples, std::vector<Event> events)
{
    Synth synth;
    configure (synth);
    synth.prepare (sampleRate, samples);
    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b)
    {
        if (a.sampleOffset != b.sampleOffset)
            return a.sampleOffset < b.sampleOffset;
        return ! a.isNoteOn && b.isNoteOn;
    });
    juce::AudioBuffer<float> result (2, samples);
    result.clear();
    synth.process (result, samples, events.data(), static_cast<int> (events.size()));
    result.applyGain (std::sqrt (0.5f)); // AudioTrack's centred constant-power pan.
    return result;
}

float difference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumSamples() != b.getNumSamples() || a.getNumChannels() != b.getNumChannels())
        return 1.0f;
    float error = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            error = std::max (error, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
    return error;
}

int firstSound (const juce::AudioBuffer<float>& audio)
{
    for (int i = 0; i < audio.getNumSamples(); ++i)
        if (std::abs (audio.getSample (0, i)) > 1.0e-8f)
            return i;
    return -1;
}

/** Owns one prepared MIDI track; driven only by the unit-test thread. */
struct EngineFixture
{
    daw::Engine engine;
    daw::AudioTrack& track;
    int blockSize;

    explicit EngineFixture (int size = 512) : track (engine.getTrack (engine.addTrack())), blockSize (size)
    {
        track.setInstrumentEnabled (true);
        configure (track.getSynth());
        engine.getTransport().setBpm (bpm);
        prepare (engine, blockSize);
    }

    juce::AudioBuffer<float> render (int samples)
    {
        return renderSamples (engine, samples, blockSize);
    }

    juce::AudioBuffer<float> play (const std::vector<daw::Clip>& clips, int samples)
    {
        publish (track, clips);
        engine.getTransport().play();
        return render (samples);
    }
};

/** Model edits and engine renders stay on the test thread; samples never need disk decoding. */
struct ModelFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    Project project { engine, pool };

    juce::ValueTree addMidi()
    {
        const auto track = project.addTrack ("Keys", Project::TrackKind::midi);
        for (const auto& [param, value] : sineParams())
            project.setDeviceParam (track, param, value);
        project.getUndoManager().clearUndoHistory();
        return track;
    }

    juce::AudioBuffer<float> render (double beats = 4.0, int blockSize = 100)
    {
        prepare (engine, blockSize);
        engine.getTransport().play();
        return renderSamples (engine, sampleAt (beats), blockSize);
    }
};

juce::ValueTree clipAt (const juce::ValueTree& track, double start)
{
    for (const auto& child : track)
        if (Project::isClip (child) && std::abs (Project::getClipStart (child) - start) < 1.0e-9)
            return child;
    return {};
}

int countClips (const juce::ValueTree& track)
{
    int result = 0;
    for (const auto& child : track)
        if (Project::isClip (child))
            ++result;
    return result;
}

/** Test-thread assertions shared by the engine and model suites. */
class MidiTestBase : public juce::UnitTest
{
public:
    explicit MidiTestBase (const juce::String& testName) : juce::UnitTest (testName, "diyDaw") {}

protected:
    void expectAudio (const juce::AudioBuffer<float>& actual, const juce::AudioBuffer<float>& expected,
                      const juce::String& context)
    {
        const auto error = difference (actual, expected);
        int firstMismatch = -1;
        for (int i = 0; i < std::min (actual.getNumSamples(), expected.getNumSamples()); ++i)
            if (std::abs (actual.getSample (0, i) - expected.getSample (0, i)) > audioTolerance)
            {
                firstMismatch = i;
                break;
            }
        expect (error <= audioTolerance, context + ": maximum sample error " + juce::String (error, 9)
                                        + ", first mismatch at sample " + juce::String (firstMismatch));
    }

    void expectClip (const juce::ValueTree& clip, double start, double length, double offset)
    {
        expect (Project::isMidiClip (clip), "Expected a MIDI clip");
        expectEquals (Project::getClipStart (clip), start, "Timeline start");
        expectEquals (Project::getClipLength (clip), length, "Timeline length");
        expectEquals (Project::getClipOffsetBeats (clip), offset, "Content offset");
    }
};

/** Exercises scheduling through Engine::render; no private scheduler state is inspected. */
class MidiEngineTests final : public MidiTestBase
{
public:
    MidiEngineTests() : MidiTestBase ("MIDI engine scheduling") {}

    void runTest() override
    {
        timing();
        endings();
        loops();
        transportAndEdits();
        retrigger();
        live();
        trackKinds();
    }

private:
    void timing()
    {
        beginTest ("Timeline plus content beat is sample accurate and independent of block size");
        juce::AudioBuffer<float> baseline;
        for (const auto size : { 64, 100, 512, 4096 })
        {
            EngineFixture f (size);
            const auto clip = midiClip (0.25, 2.0, { { 0.125, 0.5, 69, 0.8f } });
            const auto audio = f.play ({ clip }, 48000);
            std::vector<Event> events;
            appendNote (events, 0.375, 0.875, 69);
            expectAudio (audio, reference (48000, events), "Block size " + juce::String (size));
            // The synth uses deterministic nonzero initial phases, audible at the note-on.
            expectEquals (firstSound (audio), static_cast<int> (std::llround (0.375 * samplesPerBeat)));
            if (size == 64)
                baseline = audio;
            else
                expectAudio (audio, baseline, "Same output across block sizes");
        }

        beginTest ("Integer onset at a block boundary survives accumulated beat rounding");
        // Regression: collectMidiClip clamps an event belonging to the next block to n - 1.
        EngineFixture boundary (100);
        std::vector<Event> boundaryEvents;
        appendNote (boundaryEvents, 0.25, 0.375);
        const auto boundaryAudio = boundary.play ({ midiClip (0.125, 1.0, { { 0.125, 0.125, 60, 0.8f } }) }, 12000);
        expectAudio (boundaryAudio, reference (12000, boundaryEvents), "Integer onset must dispatch at sample 6000");
        expectEquals (firstSound (boundaryAudio), 6000);

        beginTest ("Fractional onset just before a block end rounds into the next block");
        EngineFixture f (64);
        const auto start = 63.5 / samplesPerBeat;
        const auto audio = f.play ({ midiClip (0.0, 1.0, { { start, 0.25, 60, 0.8f } }) }, 12000);
        std::vector<Event> events;
        appendNote (events, start, start + 0.25);
        expectAudio (audio, reference (12000, events), "Onset at sample 63.5 must dispatch at sample 64");
        expectEquals (firstSound (audio), 64, "Rounded note-on at sample 64");
    }

    void endings()
    {
        for (const auto mode : { 0, 1, 2 })
        {
            beginTest (mode == 0 ? "Natural note end releases to silence"
                                : mode == 1 ? "Clip end cuts a longer note with release"
                                            : "Loop end cuts a longer note with release");
            EngineFixture f;
            auto clip = midiClip (0.0, mode == 1 ? 0.5 : 2.0,
                                  { { 0.125, mode == 0 ? 0.25 : 1.0, 60, 0.8f } }, mode == 2, 0.5);
            std::vector<Event> events;
            if (mode == 2)
                for (int i = 0; i < 4; ++i)
                    appendNote (events, 0.125 + i * 0.5, 0.5 + i * 0.5);
            else
                appendNote (events, 0.125, mode == 0 ? 0.375 : 0.5);
            const auto audio = f.play ({ clip }, 60000);
            expectAudio (audio, reference (60000, events), "Release and cut timing");
            const auto end = sampleAt (mode == 2 ? 2.0 : mode == 0 ? 0.375 : 0.5);
            expect (audio.getMagnitude (0, end, 400) > 1.0e-4f, "Release remains audible immediately after note-off");
            expectEquals (audio.getMagnitude (0, end + 1100, 1000), 0.0f, "Silent after the 20 ms release");
            expectEquals (f.track.getSynth().getNumActiveVoices(), 0);
        }
    }

    void loops()
    {
        beginTest ("Loop repetition and nonzero offsets map content to the correct timeline beats");
        for (const auto offset : { 0.0, 0.25, 1.25 })
        {
            EngineFixture f (512);
            const auto clip = midiClip (0.25, 2.0, { { 0.375, 0.0625, 65, 0.8f } }, true, 0.5, offset);
            std::vector<Event> events;
            const auto first = juce::exactlyEqual (offset, 0.0) ? 0.625 : 0.375;
            for (int i = 0; i < 4; ++i)
                appendNote (events, first + i * 0.5, first + i * 0.5 + 0.0625, 65);
            expectAudio (f.play ({ clip }, 60000), reference (60000, events), "Offset " + juce::String (offset));
        }

        beginTest ("A non-looping content offset skips earlier notes and shifts later ones");
        EngineFixture f;
        const auto clip = midiClip (0.5, 1.0, { { 0.125, 0.125, 60, 0.8f }, { 0.75, 0.125, 67, 0.8f } }, false, 0.0, 0.5);
        std::vector<Event> events;
        appendNote (events, 0.75, 0.875, 67);
        expectAudio (f.play ({ clip }, 48000), reference (48000, events), "Offset without wrapping");

        beginTest ("Multiple same-pitch notes inside one block retain each original note-off");
        // Regression: the later onset overwrites noteEndBeat before the first off is emitted.
        EngineFixture repeated (4096);
        std::vector<Event> repeatedEvents;
        appendNote (repeatedEvents, 0.02, 0.04);
        appendNote (repeatedEvents, 0.10, 0.12);
        expectAudio (repeated.play ({ midiClip (0.0, 1.0, { { 0.02, 0.02, 60, 0.8f }, { 0.10, 0.02, 60, 0.8f } }) }, 12000),
                     reference (12000, repeatedEvents), "First note must release at beat 0.04, before the next onset");

        beginTest ("Several short loop iterations in one block retain note-offs and loop cuts");
        EngineFixture shortLoop (4096);
        std::vector<Event> loopEvents;
        for (int i = 0; i < 4; ++i)
            appendNote (loopEvents, 0.01 + i * 0.04, 0.04 + i * 0.04);
        expectAudio (shortLoop.play ({ midiClip (0.0, 0.16, { { 0.01, 0.1, 60, 0.8f } }, true, 0.04) }, 12000),
                     reference (12000, loopEvents), "Each iteration must cut at its loop end");
    }

    void transportAndEdits()
    {
        for (const auto seek : { false, true })
        {
            beginTest (seek ? "Seeking while playing releases all sequenced notes" : "Stopping releases all sequenced notes");
            EngineFixture f;
            f.play ({ midiClip (0.0, 20.0, { { 0.0, 16.0, 60, 0.8f } }) }, 2000);
            expectEquals (f.track.getSynth().getNumActiveVoices(), 1);
            if (seek)
                f.engine.getTransport().setPositionBeats (18.0);
            else
                f.engine.getTransport().stop();
            f.render (2048);
            expectEquals (f.track.getSynth().getNumActiveVoices(), 0, "No hanging voice after release");
        }

        beginTest ("Deleting a sounding note's snapshot preserves its original scheduled end");
        EngineFixture f (100);
        const auto first = f.play ({ midiClip (0.0, 2.0, { { 0.0, 0.5, 60, 0.8f } }) }, 2400);
        publish (f.track, {});
        const auto rest = f.render (21600);
        auto expected = reference (24000, { { 0, 60, 0.8f, true }, { 12000, 60, 0.0f, false } });
        juce::AudioBuffer<float> combined (2, 24000);
        for (int ch = 0; ch < 2; ++ch)
        {
            combined.copyFrom (ch, 0, first, ch, 0, 2400);
            combined.copyFrom (ch, 2400, rest, ch, 0, 21600);
        }
        expectAudio (combined, expected, "Snapshot replacement neither cuts early nor hangs");
        expectEquals (f.track.getSynth().getNumActiveVoices(), 0);
    }

    void retrigger()
    {
        beginTest ("Back-to-back same-pitch notes retrigger at their shared boundary");
        for (const auto size : { 64, 100, 512, 4096 })
        {
            EngineFixture f (size);
            std::vector<Event> events;
            appendNote (events, 0.125, 0.25);
            appendNote (events, 0.25, 0.375, 60, 0.4f);
            const auto audio = f.play ({ midiClip (0.0, 1.0, { { 0.125, 0.125, 60, 0.8f }, { 0.25, 0.125, 60, 0.4f } }) }, 24000);
            expectAudio (audio, reference (24000, events), "Two retriggered onsets, block " + juce::String (size));
            std::vector<Event> sustained;
            appendNote (sustained, 0.125, 0.375);
            expect (difference (audio, reference (24000, sustained)) > 0.01f, "Second onset is distinct from a sustained voice");
        }
    }

    void live()
    {
        for (const auto playing : { false, true })
        {
            beginTest (playing ? "Live notes start at the next block while playing" : "Live notes start at the next block while stopped");
            EngineFixture f (100);
            if (playing)
                f.engine.getTransport().play();
            f.render (100);
            f.track.pushLiveNote (69, 0.8f, true);
            const auto on = f.render (1000);
            expectAudio (on, reference (1000, { { 0, 69, 0.8f, true } }), "Live note-on at offset zero");
            f.track.pushLiveNote (69, 0.0f, false);
            const auto off = f.render (2000);
            const auto expected = reference (3000, { { 0, 69, 0.8f, true }, { 1000, 69, 0.0f, false } });
            juce::AudioBuffer<float> expectedOff (2, 2000);
            for (int ch = 0; ch < 2; ++ch)
                expectedOff.copyFrom (ch, 0, expected, ch, 1000, 2000);
            expectAudio (off, expectedOff, "Live note-off at next block offset zero");
            expectEquals (f.track.getSynth().getNumActiveVoices(), 0);
        }

        beginTest ("Live queue overflow, wraparound and invalid pitches stay bounded and recover");
        EngineFixture f (64);
        f.track.pushLiveNote (-1, 1.0f, true);
        f.track.pushLiveNote (128, 1.0f, true);
        expectEquals (f.render (64).getMagnitude (0, 64), 0.0f);
        for (int round = 0; round < 3; ++round)
        {
            for (int i = 0; i < 1024; ++i)
                f.track.pushLiveNote (48 + i % 24, 2.0f, true);
            const auto audio = f.render (128);
            bool finite = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < audio.getNumSamples(); ++i)
                    finite = finite && std::isfinite (audio.getSample (ch, i));
            expect (finite, "Overflow must not corrupt audio");
            expect (f.track.getSynth().getNumActiveVoices() <= Synth::numVoices);
            for (int pitch = 0; pitch < 128; ++pitch)
                f.track.pushLiveNote (pitch, 0.0f, false);
            f.render (2048);
            expectEquals (f.track.getSynth().getNumActiveVoices(), 0, "Queue can still deliver all note-offs after overflow");
        }
    }

    void trackKinds()
    {
        beginTest ("Instrument-disabled tracks ignore MIDI clips");
        EngineFixture f;
        f.track.setInstrumentEnabled (false);
        const auto audio = f.play ({ midiClip (0.0, 2.0, { { 0.0, 1.0, 60, 0.8f } }) }, 24000);
        expectEquals (audio.getMagnitude (0, audio.getNumSamples()), 0.0f);
        expectEquals (f.track.getSynth().getNumActiveVoices(), 0);

        beginTest ("Engine MIDI tracks also render audio clips (the model prevents this layout)");
        auto sample = std::make_shared<daw::SampleData>();
        sample->sampleRate = sampleRate;
        sample->buffer.setSize (1, 24000);
        for (int i = 0; i < 24000; ++i)
            sample->buffer.setSample (0, i, 0.25f);
        daw::Clip clip;
        clip.sample = sample;
        clip.lengthBeats = 1.0;
        EngineFixture midi;
        EngineFixture plain;
        plain.track.setInstrumentEnabled (false);
        const auto midiAudio = midi.play ({ clip }, 24000);
        expectAudio (midiAudio, plain.play ({ clip }, 24000), "Audio clips render identically with the instrument enabled");
        expect (midiAudio.getMagnitude (0, 24000) > 0.1f);
    }
};

/** Public model APIs, persistence, and their audible engine snapshots; test-thread only. */
class MidiModelTests final : public MidiTestBase
{
public:
    MidiModelTests() : MidiTestBase ("MIDI project model") {}

    void runTest() override
    {
        devices();
        clipKindsAndOverwrite();
        notes();
        clipEdits();
        loopLength();
        persistence();
        normalisation();
        newDocument();
    }

private:
    void devices()
    {
        beginTest ("MIDI track has a complete default DEVICE and matching enabled engine synth");
        ModelFixture f;
        const auto track = f.project.addTrack ("Default synth", Project::TrackKind::midi);
        expectEquals (track[daw::ids::kind].toString(), juce::String ("midi"));
        const auto device = Project::getDevice (track);
        expect (device.hasType (daw::ids::DEVICE));
        expectEquals (device[daw::ids::type].toString(), juce::String ("synth"));
        expect (f.project.getEngineTrack (track).isInstrumentEnabled());
        for (int param = 0; param < Synth::numParams; ++param)
        {
            const auto& info = Synth::getParamInfos()[static_cast<size_t> (param)];
            expect (device.hasProperty (info.id), info.id);
            expectEquals (static_cast<double> (device[info.id]), static_cast<double> (info.defaultValue), info.id);
            expectEquals (f.project.getEngineTrack (track).getSynth().getParam (param), info.defaultValue, info.id);
        }

        beginTest ("Every DEVICE parameter clamps, steps, pushes to the synth and undoes/redoes");
        for (int param = 0; param < Synth::numParams; ++param)
        {
            const auto& info = Synth::getParamInfos()[static_cast<size_t> (param)];
            for (const auto input : { info.minValue - 10.0f, info.maxValue + 10.0f, info.minValue + 0.6f })
            {
                const auto before = f.project.getState().createCopy();
                auto expected = juce::jlimit (info.minValue, info.maxValue, input);
                if (info.stepped)
                    expected = std::round (expected);
                f.project.beginTransaction (info.id);
                f.project.setDeviceParam (track, param, input);
                expectEquals (Project::getDeviceParam (track, param), expected, info.id);
                expectEquals (f.project.getEngineTrack (track).getSynth().getParam (param), expected, info.id);
                const auto after = f.project.getState().createCopy();
                if (after.isEquivalentTo (before))
                    continue; // Setting the existing value does not create an undo action.
                expect (f.project.undo());
                expect (f.project.getState().isEquivalentTo (before), "DEVICE undo restores tree");
                expectEquals (f.project.getEngineTrack (track).getSynth().getParam (param), Project::getDeviceParam (track, param));
                expect (f.project.redo());
                expect (f.project.getState().isEquivalentTo (after), "DEVICE redo restores tree");
                expectEquals (f.project.getEngineTrack (track).getSynth().getParam (param), expected);
            }
        }
        const auto before = f.project.getState().createCopy();
        f.project.setDeviceParam (track, -1, 123.0f);
        f.project.setDeviceParam (track, Synth::numParams, 123.0f);
        const auto audio = f.project.addTrack();
        f.project.setDeviceParam (audio, Synth::volume, -12.0f);
        expect (! Project::getDevice (audio).isValid());
        expect (track.isEquivalentTo (before.getChild (0)));
    }

    void clipKindsAndOverwrite()
    {
        beginTest ("Clip creation and moves enforce track kinds");
        ModelFixture f;
        const auto midi = f.addMidi();
        const auto audio = f.project.addTrack();
        const juce::File file ("/virtual/midi-tests-audio.wav");
        auto sample = std::make_shared<daw::SampleData>();
        sample->sampleRate = sampleRate;
        sample->buffer.setSize (1, 24000);
        sample->buffer.clear();
        f.pool.insert (daw::SamplePool::keyFor (file), sample);
        expect (! f.project.addMidiClip (audio, 0.0, 4.0).isValid());
        expect (! f.project.addClip (midi, file, 0.0).isValid());
        const auto midiClipTree = f.project.addMidiClip (midi, 0.0, 4.0);
        const auto audioClip = f.project.addClip (audio, file, 0.0);
        expectClip (midiClipTree, 0.0, 4.0, 0.0);
        expect (Project::isClipLooping (midiClipTree));
        expectEquals (Project::getClipLoopLength (midiClipTree), 4.0);
        const auto before = f.project.getState().createCopy();
        f.project.moveClip (midiClipTree, audio, 2.0);
        f.project.moveClip (audioClip, midi, 2.0);
        expect (f.project.getState().isEquivalentTo (before));
        const auto otherMidi = f.addMidi();
        f.project.moveClip (midiClipTree, otherMidi, 1.0);
        expect (midiClipTree.getParent() == otherMidi);
        expectClip (midiClipTree, 1.0, 4.0, 0.0);
        const auto minimum = f.project.addMidiClip (midi, -2.0, -1.0);
        expectClip (minimum, 0.0, Project::minClipLengthBeats, 0.0);

        beginTest ("MIDI overwrite splits/trims content offsets and keeps DEVICE and notes");
        ModelFixture overwrite;
        const auto track = overwrite.addMidi();
        const auto old = overwrite.project.addMidiClip (track, 2.0, 8.0);
        overwrite.project.addNote (old, 64, 1.0, 0.25);
        const auto beforeOverwrite = overwrite.project.getState().createCopy();
        overwrite.project.beginTransaction ("Overwrite middle");
        const auto landing = overwrite.project.addMidiClip (track, 4.0, 2.0);
        expectEquals (countClips (track), 3);
        expectClip (old, 2.0, 2.0, 0.0);
        expectClip (landing, 4.0, 2.0, 0.0);
        const auto right = clipAt (track, 6.0);
        expectClip (right, 6.0, 4.0, 4.0);
        expectEquals (right.getNumChildren(), 1);
        expect (right.getChild (0).isEquivalentTo (old.getChild (0)));
        expect (Project::getDevice (track).isValid());
        expect (overwrite.project.undo());
        expect (overwrite.project.getState().isEquivalentTo (beforeOverwrite));
        expect (overwrite.project.redo());
        overwrite.project.addMidiClip (track, 5.0, 2.0); // trim right remnant's head
        expectClip (right, 7.0, 3.0, 5.0);
        overwrite.project.addMidiClip (track, 0.0, 12.0); // cover all remnants
        expectEquals (countClips (track), 1);
        expect (Project::getDevice (track).isValid());
    }

    void notes()
    {
        beginTest ("All note edits clamp, undo/redo, and publish audible engine snapshots");
        ModelFixture f;
        const auto track = f.addMidi();
        const auto clip = f.project.addMidiClip (track, 0.0, 2.0);
        f.project.getUndoManager().clearUndoHistory();
        std::vector<juce::ValueTree> states { f.project.getState().createCopy() };
        std::vector<juce::AudioBuffer<float>> audio { f.render (3.0) };
        const auto save = [&] (double start, double length, int pitch, float velocity, bool present = true)
        {
            std::vector<Event> events;
            if (present)
                appendNote (events, start, std::min (start + length, 2.0), pitch, velocity);
            const auto rendered = f.render (3.0);
            expectAudio (rendered, reference (sampleAt (3.0), events), "Edited note snapshot");
            states.push_back (f.project.getState().createCopy());
            audio.push_back (rendered);
        };
        f.project.beginTransaction ("Add clamped note");
        const auto note = f.project.addNote (clip, 200, -1.0, -1.0, 2.0f);
        expectEquals (Project::getNotePitch (note), 127);
        expectEquals (Project::getNoteStart (note), 0.0);
        expectEquals (Project::getNoteLength (note), Project::minNoteLengthBeats);
        expectEquals (Project::getNoteVelocity (note), 1.0f);
        save (0.0, Project::minNoteLengthBeats, 127, 1.0f);
        f.project.beginTransaction ("Move clamped note");
        f.project.moveNote (note, -2.0, -20);
        expectEquals (Project::getNoteStart (note), 0.0);
        expectEquals (Project::getNotePitch (note), 0);
        save (0.0, Project::minNoteLengthBeats, 0, 1.0f);
        f.project.beginTransaction ("Move note");
        f.project.moveNote (note, 0.5, 69);
        save (0.5, Project::minNoteLengthBeats, 69, 1.0f);
        f.project.beginTransaction ("Lengthen note");
        f.project.setNoteLength (note, 0.25);
        expectEquals (Project::getNoteLength (note), 0.25);
        save (0.5, 0.25, 69, 1.0f);
        f.project.beginTransaction ("Clamp length");
        f.project.setNoteLength (note, -3.0);
        expectEquals (Project::getNoteLength (note), Project::minNoteLengthBeats);
        save (0.5, Project::minNoteLengthBeats, 69, 1.0f);
        f.project.beginTransaction ("Change velocity");
        f.project.setNoteVelocity (note, 0.3f);
        expectEquals (Project::getNoteVelocity (note), 0.3f);
        save (0.5, Project::minNoteLengthBeats, 69, 0.3f);
        f.project.beginTransaction ("Clamp velocity low");
        f.project.setNoteVelocity (note, -1.0f);
        expectEquals (Project::getNoteVelocity (note), 0.0f);
        save (0.5, Project::minNoteLengthBeats, 69, 0.0f);
        f.project.beginTransaction ("Clamp velocity high");
        f.project.setNoteVelocity (note, 3.0f);
        expectEquals (Project::getNoteVelocity (note), 1.0f);
        save (0.5, Project::minNoteLengthBeats, 69, 1.0f);
        f.project.beginTransaction ("Delete note");
        f.project.deleteNote (note);
        expect (! note.getParent().isValid());
        save (0.0, 0.0, 0, 0.0f, false);
        for (int i = static_cast<int> (states.size()) - 2; i >= 0; --i)
        {
            expect (f.project.undo());
            expect (f.project.getState().isEquivalentTo (states[static_cast<size_t> (i)]), "Note undo tree");
            expectAudio (f.render (3.0), audio[static_cast<size_t> (i)], "Note undo audio");
        }
        expect (! f.project.undo());
        for (size_t i = 1; i < states.size(); ++i)
        {
            expect (f.project.redo());
            expect (f.project.getState().isEquivalentTo (states[i]), "Note redo tree");
            expectAudio (f.render (3.0), audio[i], "Note redo audio");
        }
        expect (! f.project.redo());
        expect (! f.project.addNote (track, 60, 0.0, 1.0).isValid());
    }

    void clipEdits()
    {
        beginTest ("MIDI trim, split and duplicate preserve content with wrapped offsets");
        ModelFixture f;
        const auto track = f.addMidi();
        const auto clip = f.project.addMidiClip (track, 2.0, 4.0);
        f.project.addNote (clip, 60, 0.5, 0.125);
        f.project.trimClipStart (clip, 3.0);
        expectClip (clip, 3.0, 3.0, 1.0);
        expectAudio (f.render (8.0), reference (sampleAt (8.0), {}), "Left trim hides the note without moving it");
        f.project.trimClipStart (clip, 0.0);
        expectClip (clip, 0.0, 6.0, 2.0); // -2 wraps into [0, 4)
        std::vector<Event> trimEvents;
        appendNote (trimEvents, 2.5, 2.625);
        expectAudio (f.render (8.0, 512), reference (sampleAt (8.0), trimEvents), "Extending left reveals the original timeline content");
        f.project.trimClipEnd (clip, 8.0);
        expectClip (clip, 0.0, 8.0, 2.0);
        appendNote (trimEvents, 6.5, 6.625);
        expectAudio (f.render (10.0, 512), reference (sampleAt (10.0), trimEvents), "Extending right reveals another loop");
        const auto before = f.render (12.0);
        const auto state = f.project.getState().createCopy();
        f.project.beginTransaction ("Split in silence");
        const auto right = f.project.splitClip (clip, 3.0);
        expectClip (clip, 0.0, 3.0, 2.0);
        expectClip (right, 3.0, 5.0, 1.0);
        expectEquals (Project::getClipLoopLength (right), 4.0);
        expect (right.getChild (0).isEquivalentTo (clip.getChild (0)));
        expectAudio (f.render (12.0), before, "Split away from notes preserves rendered audio");
        expect (f.project.undo());
        expect (f.project.getState().isEquivalentTo (state));
        expect (f.project.redo());
        const auto copy = f.project.duplicateClip (right);
        expectClip (copy, 8.0, 5.0, 1.0);
        expect (copy.getChild (0).isEquivalentTo (right.getChild (0)));
        expect (copy.getChild (0) != right.getChild (0), "Duplicate notes are independent trees");
        expect (Project::isClipLooping (copy));
        expectEquals (Project::getClipLoopLength (copy), 4.0);
        appendNote (trimEvents, 11.5, 11.625);
        expectAudio (f.render (14.0, 512), reference (sampleAt (14.0), trimEvents), "Duplicate retains source content and loop phase");
        f.project.setClipLooping (copy, false);
        f.project.trimClipStart (copy, -10.0);
        expectClip (copy, 7.0, 6.0, 0.0);
        f.project.trimClipEnd (copy, -10.0);
        expectEquals (Project::getClipLength (copy), Project::minClipLengthBeats);

        beginTest ("Splitting through a sounding MIDI note cuts it at the split (Ableton behaviour)");
        // Like Ableton: a note crossing a clip boundary ends with the left clip; the right clip
        // doesn't retrigger a note whose onset lies before its start. Splitting therefore equals
        // shortening the note to the split point.
        ModelFixture crossing;
        const auto crossingTrack = crossing.addMidi();
        const auto crossingClip = crossing.project.addMidiClip (crossingTrack, 0.0, 2.0);
        const auto crossingNote = crossing.project.addNote (crossingClip, 69, 0.25, 1.0);
        const auto crossingRight = crossing.project.splitClip (crossingClip, 0.75);
        expectClip (crossingRight, 0.75, 1.25, 0.75);
        const auto split = crossing.render (3.0);

        ModelFixture shortened;
        const auto shortTrack = shortened.addMidi();
        const auto shortClip = shortened.project.addMidiClip (shortTrack, 0.0, 2.0);
        shortened.project.addNote (shortClip, 69, 0.25, 0.5);
        expectAudio (split, shortened.render (3.0), "A split at beat 0.75 ends the crossing note there");
        juce::ignoreUnused (crossingNote);
    }

    void loopLength()
    {
        beginTest ("Changing loop length changes repetition, rewraps offset, clamps and undoes");
        ModelFixture f;
        const auto track = f.addMidi();
        const auto clip = f.project.addMidiClip (track, 0.0, 4.0);
        f.project.addNote (clip, 64, 0.25, 0.125);
        f.project.trimClipStart (clip, 1.5);
        const auto before = f.project.getState().createCopy();
        const auto beforeAudio = f.render (5.0);
        f.project.beginTransaction ("Shorten loop");
        f.project.setClipLoopLength (clip, 1.0);
        expectEquals (Project::getClipLoopLength (clip), 1.0);
        expectEquals (Project::getClipOffsetBeats (clip), 0.5);
        std::vector<Event> events;
        appendNote (events, 2.25, 2.375, 64);
        appendNote (events, 3.25, 3.375, 64);
        expectAudio (f.render (5.0, 512), reference (sampleAt (5.0), events), "Shorter loop repeats at beats 2.25 and 3.25");
        expect (f.project.undo());
        expect (f.project.getState().isEquivalentTo (before));
        expectAudio (f.render (5.0), beforeAudio, "Loop-length undo restores audio");
        expect (f.project.redo());
        expectEquals (Project::getClipOffsetBeats (clip), 0.5);
        f.project.setClipLoopLength (clip, -1.0);
        expectEquals (Project::getClipLoopLength (clip), Project::minClipLengthBeats);
        expectEquals (Project::getClipOffsetBeats (clip), 0.0);
    }

    void expectExactTree (const juce::ValueTree& actual, const juce::ValueTree& expected)
    {
        expect (actual.getType() == expected.getType());
        expectEquals (actual.getNumProperties(), expected.getNumProperties());
        expectEquals (actual.getNumChildren(), expected.getNumChildren());
        for (int i = 0; i < expected.getNumProperties(); ++i)
        {
            const auto id = expected.getPropertyName (i);
            expect (actual.hasProperty (id), id.toString());
            const auto value = expected[id];
            if (value.isDouble() || value.isInt() || value.isInt64())
                expectEquals (static_cast<double> (actual[id]), static_cast<double> (value), id.toString());
            else
                expectEquals (actual[id].toString(), value.toString(), id.toString());
        }
        for (int i = 0; i < std::min (actual.getNumChildren(), expected.getNumChildren()); ++i)
            expectExactTree (actual.getChild (i), expected.getChild (i));
    }

    void persistence()
    {
        beginTest ("MIDI project save/load preserves all values exactly and renders identically");
        ModelFixture source;
        source.project.setBpm (137.1234567890123);
        for (int i = 0; i < 2; ++i)
        {
            const auto track = source.addMidi();
            source.project.setTrackGain (track, -3.125f * static_cast<float> (i + 1));
            source.project.setTrackPan (track, i == 0 ? -0.25f : 0.375f);
            for (int param = 0; param < Synth::numParams; ++param)
            {
                const auto& info = Synth::getParamInfos()[static_cast<size_t> (param)];
                source.project.setDeviceParam (track, param, info.minValue + (info.maxValue - info.minValue) * 0.37123f);
            }
            const auto clip = source.project.addMidiClip (track, 0.123456789012345, 2.34567890123456);
            source.project.addNote (clip, 60 + i, 0.234567890123456, 0.345678901234567, 0.6234567f);
            source.project.addNote (clip, 72 + i, 1.23456789012345, 0.456789012345678, 0.8123456f);
            source.project.trimClipStart (clip, 0.223456789012345);
            source.project.setClipLoopLength (clip, 1.98765432109876);
        }
        const auto expected = source.project.getState().createCopy();
        const auto before = source.render (5.0);
        const juce::TemporaryFile temporary (juce::File ("/tmp").getNonexistentChildFile ("diydaw-midi-roundtrip", ".diydaw", false));
        const auto saved = source.project.saveTo (temporary.getFile());
        expect (saved.wasOk(), saved.getErrorMessage());
        ModelFixture loaded;
        const auto result = loaded.project.loadFrom (temporary.getFile());
        expect (result.wasOk(), result.getErrorMessage());
        expectExactTree (loaded.project.getState(), expected);
        expectAudio (loaded.render (5.0), before, "Save/load audio");
        expect (! loaded.project.undo());
    }

    void normalisation()
    {
        beginTest ("Loading adds a default DEVICE, normalises NOTE types and drops wrong-kind clips");
        const juce::String xml = R"xml(<PROJECT version="1" bpm="120">
  <TRACK kind="midi" name="Imported">
    <CLIP kind="midi" start="0" length="4" loop="1" loopLength="4">
      <NOTE pitch="900" start="0.25" length="-2" velocity="9"/>
      <NOTE pitch="junk" start="junk" length="junk" velocity="junk"/>
      <NOTE/>
      <UNEXPECTED/>
    </CLIP>
    <CLIP kind="audio" file="/tmp/nonexistent-midi-tests.wav" start="0" length="1"/>
  </TRACK>
  <TRACK kind="audio" name="Audio">
    <CLIP kind="midi" start="0" length="2"><NOTE pitch="60"/></CLIP>
  </TRACK>
</PROJECT>)xml";
        const juce::TemporaryFile temporary (juce::File ("/tmp").getNonexistentChildFile ("diydaw-midi-import", ".diydaw", false));
        expect (temporary.getFile().replaceWithText (xml));
        ModelFixture f;
        const auto loaded = f.project.loadFrom (temporary.getFile());
        expect (loaded.wasOk(), loaded.getErrorMessage());
        const auto track = f.project.getTrack (0);
        expect (Project::isMidiTrack (track));
        expectEquals (countClips (track), 1);
        expectEquals (countClips (f.project.getTrack (1)), 0);
        expect (! Project::getDevice (f.project.getTrack (1)).isValid());
        expect (f.project.getEngineTrack (track).isInstrumentEnabled());
        const auto device = Project::getDevice (track);
        for (int param = 0; param < Synth::numParams; ++param)
        {
            const auto& info = Synth::getParamInfos()[static_cast<size_t> (param)];
            expect (device.hasProperty (info.id), info.id);
            expectEquals (Project::getDeviceParam (track, param), info.defaultValue, info.id);
            expectEquals (f.project.getEngineTrack (track).getSynth().getParam (param), info.defaultValue, info.id);
        }
        const auto clip = clipAt (track, 0.0);
        expectEquals (clip.getNumChildren(), 3, "Only NOTE children retained");
        const auto high = clip.getChild (0);
        expect (high[daw::ids::pitch].isInt());
        expect (high[daw::ids::start].isDouble());
        expect (high[daw::ids::length].isDouble());
        expect (high[daw::ids::velocity].isDouble());
        expectEquals (Project::getNotePitch (high), 127);
        expectEquals (Project::getNoteStart (high), 0.25);
        expectEquals (Project::getNoteLength (high), Project::minNoteLengthBeats);
        expectEquals (Project::getNoteVelocity (high), 1.0f);
        const auto junk = clip.getChild (1);
        expectEquals (Project::getNotePitch (junk), 0);
        expectEquals (Project::getNoteStart (junk), 0.0);
        expectEquals (Project::getNoteLength (junk), Project::minNoteLengthBeats);
        expectEquals (Project::getNoteVelocity (junk), 0.0f);
        const auto defaults = clip.getChild (2);
        expectEquals (Project::getNotePitch (defaults), 60);
        expectEquals (Project::getNoteStart (defaults), 0.0);
        expectEquals (Project::getNoteLength (defaults), 0.25);
        expectEquals (Project::getNoteVelocity (defaults), 0.8f);

        beginTest ("Imported negative NOTE starts obey the same clamp as addNote/moveNote");
        // Regression: normaliseTypes converts NOTE start to double but never clamps it.
        expect (temporary.getFile().replaceWithText (R"xml(<PROJECT version="1"><TRACK kind="midi"><CLIP kind="midi" start="0" length="2"><NOTE pitch="60" start="-2" length="1" velocity="0.8"/></CLIP></TRACK></PROJECT>)xml"));
        expect (f.project.loadFrom (temporary.getFile()).wasOk());
        expectEquals (Project::getNoteStart (clipAt (f.project.getTrack (0), 0.0).getChild (0)), 0.0,
                      "Loaded notes must not retain negative content starts");
    }

    void newDocument()
    {
        beginTest ("newDocument (2, 2) puts two MIDI tracks before two audio tracks");
        ModelFixture f;
        f.project.newDocument (2, 2);
        expectEquals (f.project.getNumTracks(), 4);
        expectEquals (f.engine.getNumTracks(), 4);
        for (int i = 0; i < 4; ++i)
        {
            const auto track = f.project.getTrack (i);
            expectEquals (track[daw::ids::kind].toString(), juce::String (i < 2 ? "midi" : "audio"));
            expect (f.project.getEngineTrack (track).isInstrumentEnabled() == (i < 2));
            expect (Project::getDevice (track).isValid() == (i < 2));
            expectEquals (countClips (track), 0);
        }
        expect (! f.project.undo());
    }
};

static MidiEngineTests midiEngineTests;
static MidiModelTests midiModelTests;
} // namespace
