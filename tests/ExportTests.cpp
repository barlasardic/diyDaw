#include "ProjectRenderer.h"

#include <cmath>
#include <memory>
#include <vector>

namespace
{
constexpr int exportBlockSize = 1024;
constexpr double fixtureSampleRate = 48000.0;

/** Test-thread fixture: virtual samples, model edits and offline callbacks run sequentially. */
struct ExportFixture
{
    daw::Engine engine;
    daw::SamplePool pool;
    daw::Project project { engine, pool };
    const juce::File firstSample { "/virtual/export-first.wav" };
    const juce::File secondSample { "/virtual/export-second.wav" };
    const juce::File constantSample { "/virtual/export-constant.wav" };

    ExportFixture()
    {
        engine.prepare (fixtureSampleRate, exportBlockSize);
        engine.getMetronome().setEnabled (false);
        insert (firstSample, 1.0, 0.35f, false);
        insert (secondSample, 0.5, -0.2f, false);
        insert (constantSample, 2.0, 0.5f, true);
    }

    void insert (const juce::File& file, double seconds, float amplitude, bool constant)
    {
        auto sample = std::make_shared<daw::SampleData>();
        sample->sampleRate = fixtureSampleRate;
        sample->name = file.getFileNameWithoutExtension();
        sample->buffer.setSize (2, static_cast<int> (std::llround (seconds * fixtureSampleRate)));
        for (int i = 0; i < sample->getNumFrames(); ++i)
        {
            // A bipolar stereo signal exposes offsets, resampling and channel routing.
            const auto value = constant ? amplitude
                                       : amplitude * static_cast<float> (i % 257 - 128) / 128.0f;
            sample->buffer.setSample (0, i, value);
            sample->buffer.setSample (1, i, constant ? value : -0.5f * value);
        }
        pool.insert (daw::SamplePool::keyFor (file), sample);
    }

    void populate()
    {
        project.setBpm (97.5);
        project.setMasterGain (-4.25f);
        const auto first = project.addTrack ("Solo left");
        const auto second = project.addTrack ("Solo right");
        const auto muted = project.addTrack ("Muted solo");
        const auto excluded = project.addTrack ("Not soloed");
        project.setTrackGain (first, -8.5f);
        project.setTrackPan (first, -0.375f);
        project.setTrackSolo (first, true);
        project.setTrackGain (second, -3.25f);
        project.setTrackPan (second, 0.625f);
        project.setTrackSolo (second, true);
        project.setTrackSolo (muted, true);
        project.setTrackMute (muted, true);

        const auto loop = project.addClip (first, firstSample, 0.125);
        project.trimClipStart (loop, 0.53125);
        project.setClipLooping (loop, true);
        project.trimClipEnd (loop, 3.5);
        project.addClip (first, secondSample, 3.75);
        const auto offset = project.addClip (second, firstSample, 0.75);
        project.trimClipStart (offset, 1.15625);
        project.addClip (second, secondSample, 2.75);
        project.addClip (muted, constantSample, 0.0);
        project.addClip (excluded, constantSample, 0.0);
    }

    juce::AudioBuffer<float> renderLive (const daw::ExportSettings& settings)
    {
        auto& transport = engine.getTransport();
        transport.stop();
        engine.prepare (settings.sampleRate, exportBlockSize);
        engine.getMetronome().setEnabled (settings.includeMetronome);
        // Match the renderer's block boundaries and settle the 20 ms faders while stopped.
        // This permits exact comparison from sample 0, including the clip-edge fades.
        juce::AudioBuffer<float> warmUp (2, exportBlockSize);
        for (int done = 0; done < static_cast<int> (settings.sampleRate * 0.05); done += exportBlockSize)
            engine.render (warmUp, exportBlockSize);
        transport.setPositionBeats (settings.startBeat);
        transport.play();
        return daw::renderOffline (engine, (settings.endBeat - settings.startBeat) * 60.0 / project.getBpm(),
                                   exportBlockSize);
    }
};

/** Owns temporary export files; cleanup runs after readers and writers are destroyed. */
struct ExportDirectory
{
    juce::File directory = juce::File::createTempFile ("-export");

    ~ExportDirectory() { directory.deleteRecursively(); }

    juce::File file (const juce::String& name) const { return directory.getChildFile (name); }
};
}

/** Exercises offline export on the test thread without touching an audio device. */
struct ExportTests : juce::UnitTest
{
    ExportTests() : juce::UnitTest ("Export", "diyDaw") {}

    void runTest() override
    {
        runLengthTests();
        runLiveComparisonTests();
        runInitialLevelTests();
        runIndependenceTests();
        runFileFormatTests();
        runNormaliseTests();
        runProgressTests();
        runInvalidSettingsTests();
        runMetronomeTests();
        runProjectEndTests();
    }

    bool expectSuccess (const juce::Result& result)
    {
        expect (result.wasOk(), result.getErrorMessage());
        return result.wasOk();
    }

    void expectAudioEqual (const juce::AudioBuffer<float>& actual,
                           const juce::AudioBuffer<float>& expected, float tolerance = 0.0f)
    {
        expectEquals (actual.getNumChannels(), expected.getNumChannels(), "Channel count");
        expectEquals (actual.getNumSamples(), expected.getNumSamples(), "Frame count");
        for (int ch = 0; ch < juce::jmin (actual.getNumChannels(), expected.getNumChannels()); ++ch)
            for (int i = 0; i < juce::jmin (actual.getNumSamples(), expected.getNumSamples()); ++i)
            {
                const auto a = actual.getSample (ch, i);
                const auto b = expected.getSample (ch, i);
                if (! std::isfinite (a) || ! std::isfinite (b) || std::abs (a - b) > tolerance)
                {
                    expect (false, "Audio differs at channel " + juce::String (ch) + ", frame " + juce::String (i)
                                   + ": actual " + juce::String (a, 10) + ", expected " + juce::String (b, 10)
                                   + ", tolerance " + juce::String (tolerance, 10));
                    return;
                }
            }
        expect (true, "Every sample is finite and matches within tolerance");
    }

    juce::AudioBuffer<float> readWav (const juce::File& file, const daw::ExportSettings& settings,
                                      int64_t expectedLength)
    {
        juce::AudioFormatManager formats;
        formats.registerFormat (new juce::WavAudioFormat(), true);
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        expect (reader != nullptr, "Export must be a readable WAV: " + file.getFileName());
        if (reader == nullptr)
            return {};
        expectEquals (reader->sampleRate, settings.sampleRate, "WAV sample rate");
        expectEquals (static_cast<int> (reader->numChannels), 2, "WAV channel count");
        expectEquals (static_cast<int> (reader->bitsPerSample), settings.bitDepth, "WAV bit depth");
        expectEquals (static_cast<int64_t> (reader->lengthInSamples), expectedLength, "WAV frame count");
        expect (reader->usesFloatingPointData == (settings.bitDepth == 32), "32-bit WAV must use float samples");
        juce::AudioBuffer<float> audio (2, static_cast<int> (reader->lengthInSamples));
        audio.clear();
        expect (reader->read (&audio, 0, audio.getNumSamples(), 0, true, true), "Read all WAV samples");
        return audio;
    }

    void expectProgress (const std::vector<double>& values, bool complete)
    {
        expect (! values.empty(), "Progress callback must run");
        auto previous = 0.0;
        for (const auto value : values)
        {
            if (! std::isfinite (value) || value < previous || value < 0.0 || value > 1.0)
            {
                expect (false, "Progress must be finite, monotonic and within [0, 1]");
                return;
            }
            previous = value;
        }
        if (complete && ! values.empty())
            expectEquals (values.back(), 1.0, "Successful render reports completion");
    }

    void runLengthTests()
    {
        beginTest ("Range length uses project tempo and export sample rate, rounded to a sample");
        ExportFixture f;
        for (const auto rate : { 44100.0, 48000.0 })
            for (const auto bpm : { 120.0, 97.5 })
                for (const auto start : { 0.0, 1.2345 })
                {
                    f.project.setBpm (bpm);
                    daw::ExportSettings settings;
                    settings.sampleRate = rate;
                    settings.startBeat = start;
                    settings.endBeat = start + 2.718;
                    daw::ProjectRenderer renderer (f.project, settings);
                    const auto length = static_cast<int64_t> (std::llround (2.718 * 60.0 / bpm * rate));
                    expectEquals (renderer.getLengthInSamples(), length);
                    expectEquals (renderer.render().getNumSamples(), static_cast<int> (length));
                }
    }

    void runLiveComparisonTests()
    {
        beginTest ("Bounce exactly matches a warmed live engine with clips, offsets, loops and mixer routing");
        ExportFixture f;
        f.populate();
        for (const auto rate : { 44100.0, 48000.0 })
            for (const auto start : { 0.0, 1.25 })
            {
                daw::ExportSettings settings;
                settings.sampleRate = rate;
                settings.startBeat = start;
                settings.endBeat = 4.625;
                daw::ProjectRenderer renderer (f.project, settings);
                const auto expected = f.renderLive (settings);
                expect (expected.getMagnitude (0, expected.getNumSamples()) > 0.01f, "Reference must be audible");
                expectAudioEqual (renderer.render(), expected);
                expectAudioEqual (renderer.render(), expected); // Rewind must also be deterministic.
            }
    }

    void runInitialLevelTests()
    {
        beginTest ("Initial samples have settled mixer gain with only the 2 ms clip-edge fade");
        ExportFixture f;
        const auto track = f.project.addTrack();
        f.project.setTrackGain (track, -6.0f);
        f.project.setTrackPan (track, 0.3f);
        f.project.setMasterGain (-3.0f);
        f.project.addClip (track, f.constantSample, 1.25);
        for (const auto rate : { 44100.0, 48000.0 })
            for (const auto start : { 1.25, 1.5 })
            {
                daw::ExportSettings settings;
                settings.sampleRate = rate;
                settings.startBeat = start;
                settings.endBeat = start + 0.1;
                daw::ProjectRenderer renderer (f.project, settings);
                const auto audio = renderer.render();
                const auto angle = (0.3f + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
                const auto gain = 0.5f * juce::Decibels::decibelsToGain (-6.0f)
                                       * juce::Decibels::decibelsToGain (-3.0f);
                juce::AudioBuffer<float> expected (2, audio.getNumSamples());
                for (int i = 0; i < expected.getNumSamples(); ++i)
                {
                    const auto elapsed = (start - 1.25) * 0.5 + static_cast<double> (i) / rate;
                    const auto fade = static_cast<float> (juce::jmin (1.0, elapsed / 0.002));
                    expected.setSample (0, i, gain * std::cos (angle) * fade);
                    expected.setSample (1, i, gain * std::sin (angle) * fade);
                }
                // Check the whole first 50 ms: past 2 ms it must already be at full level,
                // well before a fresh 20 ms fader ramp would have settled. Mid-clip starts
                // must be at full level at sample 0 as well.
                expectAudioEqual (audio, expected, 1.0e-6f);
            }
    }

    void runIndependenceTests()
    {
        beginTest ("Renderer snapshots survive source edits, undo and playback without moving the source transport");
        ExportFixture f;
        f.populate();
        daw::ExportSettings settings;
        settings.startBeat = 1.25;
        settings.endBeat = 4.625;
        const auto expected = f.renderLive (settings);
        auto& transport = f.engine.getTransport();
        transport.stop();
        transport.setPositionBeats (17.25);
        const auto before = f.project.getState().createCopy();
        {
            daw::ProjectRenderer renderer (f.project, settings);
            expectEquals (transport.getPositionBeats(), 17.25, "Construction preserves pending source seek");
            expect (! transport.isPlaying());
            expect (f.project.getState().isEquivalentTo (before), "Construction preserves source document");

            // Mutate before the first render to catch a lazy copy of the source project.
            const auto track = f.project.getTrack (0);
            f.project.beginTransaction ("Change source after snapshot");
            f.project.setBpm (143.0);
            f.project.setMasterGain (-18.0f);
            f.project.setTrackGain (track, -30.0f);
            f.project.setTrackPan (track, 1.0f);
            f.project.setTrackMute (track, true);
            f.project.moveClip (track.getChild (0), track, 8.0);
            expectAudioEqual (renderer.render(), expected);
            expectEquals (transport.getPositionBeats(), 17.25, "Bounce preserves stopped source position");
            expect (! transport.isPlaying());

            expect (f.project.undo());
            expect (f.project.getState().isEquivalentTo (before), "Undo restores the source");
            expectAudioEqual (renderer.render(), expected);
            expectEquals (transport.getPositionBeats(), 17.25, "Bounce after undo preserves position");
            transport.setPositionBeats (1.75);
            transport.play();
            const auto sourceAudio = daw::renderOffline (f.engine, 0.1, exportBlockSize);
            expect (sourceAudio.getMagnitude (0, sourceAudio.getNumSamples()) > 0.01f, "Source actually plays");
            const auto position = transport.getPositionBeats();
            expect (position > 1.75, "Source transport actually advances");
            expectAudioEqual (renderer.render(), expected);
            expectEquals (transport.getPositionBeats(), position, "Bounce preserves playing source position");
            expect (transport.isPlaying(), "Bounce preserves source playback state");
            f.project.newDocument (0);
            expectAudioEqual (renderer.render(), expected);
            expectEquals (transport.getPositionBeats(), position, "Replacing source preserves snapshot isolation");
        }
        expect (transport.isPlaying(), "Renderer destruction preserves source playback state");
    }

    void runFileFormatTests()
    {
        ExportDirectory temp;
        ExportFixture f;
        f.populate();
        for (const auto rate : { 44100.0, 48000.0 })
            for (const auto depth : { 16, 24, 32 })
            {
                beginTest ("WAV metadata and content: " + juce::String (rate) + " Hz, " + juce::String (depth) + " bits");
                daw::ExportSettings settings;
                settings.startBeat = 1.25;
                settings.endBeat = 3.317;
                settings.sampleRate = rate;
                settings.bitDepth = depth;
                daw::ProjectRenderer renderer (f.project, settings);
                const auto expected = renderer.render();
                const auto file = temp.file (juce::String (rate) + "-" + juce::String (depth) + ".wav");
                if (! expectSuccess (renderer.renderToFile (file)))
                    continue;
                const auto audio = readWav (file, settings, renderer.getLengthInSamples());
                // 24-bit PCM rounds by at most one LSB; 16-bit TPDF adds at most one
                // LSB before quantisation, so allow three LSBs for the combined error.
                const auto tolerance = depth == 32 ? 0.0f : depth == 24 ? 1.0f / 8388608.0f : 3.0f / 32768.0f;
                expectAudioEqual (audio, expected, tolerance);
            }
    }

    void runNormaliseTests()
    {
        beginTest ("Normalised float WAV peaks at -0.3 dBFS; render() remains unnormalised");
        ExportDirectory temp;
        ExportFixture f;
        f.populate();
        daw::ExportSettings settings;
        settings.startBeat = 1.25;
        settings.endBeat = 3.317;
        settings.bitDepth = 32;
        settings.normalise = true;
        daw::ProjectRenderer renderer (f.project, settings);
        const auto raw = renderer.render();
        auto plainSettings = settings;
        plainSettings.normalise = false;
        daw::ProjectRenderer plain (f.project, plainSettings);
        expectAudioEqual (raw, plain.render());
        const auto file = temp.file ("normalised.wav");
        if (expectSuccess (renderer.renderToFile (file)))
        {
            const auto audio = readWav (file, settings, renderer.getLengthInSamples());
            const auto target = juce::Decibels::decibelsToGain (daw::ProjectRenderer::normaliseTargetDb);
            expectWithinAbsoluteError (audio.getMagnitude (0, audio.getNumSamples()), target, 2.0e-6f);
            auto expected = raw;
            expected.applyGain (target / raw.getMagnitude (0, raw.getNumSamples()));
            expectAudioEqual (audio, expected, 2.0e-6f);
        }

        beginTest ("Normalising silence stays finite and silent");
        ExportFixture silent;
        const auto silentTrack = silent.project.addTrack();
        silent.project.addClip (silentTrack, silent.constantSample, 0.0);
        silent.project.setTrackMute (silentTrack, true);
        daw::ProjectRenderer silentRenderer (silent.project, settings);
        const auto silentFile = temp.file ("silent.wav");
        if (expectSuccess (silentRenderer.renderToFile (silentFile)))
        {
            const auto audio = readWav (silentFile, settings, silentRenderer.getLengthInSamples());
            juce::AudioBuffer<float> zeros (2, static_cast<int> (silentRenderer.getLengthInSamples()));
            zeros.clear();
            expectAudioEqual (audio, zeros);
        }
    }

    void runProgressTests()
    {
        ExportDirectory temp;
        ExportFixture f;
        f.populate();
        for (const auto normalise : { false, true })
        {
            daw::ExportSettings settings;
            settings.endBeat = 1.25;
            settings.bitDepth = 32;
            settings.normalise = normalise;
            daw::ProjectRenderer renderer (f.project, settings);
            beginTest ("Complete render progress is monotonic and ends at 1.0 (normalise="
                       + juce::String (normalise ? "true" : "false") + ")");
            std::vector<double> values;
            const auto progress = [&values] (double value)
            {
                values.push_back (value);
                return true;
            };
            const auto raw = renderer.render (progress);
            expectProgress (values, true);
            values.clear();
            const auto completeFile = temp.file ("complete.wav");
            if (expectSuccess (renderer.renderToFile (completeFile, progress)))
                expectProgress (values, true);
            expect (completeFile.deleteFile());

            for (const auto threshold : { 0.25, 0.75, 1.0 })
                for (const auto existing : { false, true })
                {
                    beginTest ("Cancellation preserves target (normalise=" + juce::String (normalise ? "true" : "false")
                               + ", threshold=" + juce::String (threshold)
                               + ", existing=" + juce::String (existing ? "true" : "false") + ")");
                    const auto target = temp.file ("cancelled.wav");
                    const unsigned char bytes[] { 0x52, 0x49, 0x00, 0xff, 0x10, 0x0a, 0x46 };
                    const juce::MemoryBlock sentinel (bytes, sizeof (bytes));
                    if (existing)
                        expect (target.replaceWithData (sentinel.getData(), sentinel.getSize()));
                    values.clear();
                    const auto result = renderer.renderToFile (target, [&] (double value)
                    {
                        values.push_back (value);
                        return value < threshold;
                    });
                    expect (result.failed(), "Cancellation must return a failed Result");
                    expectProgress (values, false);
                    if (! values.empty())
                        expect (values.back() >= threshold, "Callback reached the requested cancellation point");
                    if (existing)
                    {
                        juce::MemoryBlock after;
                        expect (target.loadFileAsData (after));
                        expect (after == sentinel, "Existing target remains byte-for-byte unchanged");
                    }
                    else
                        expect (! target.exists(), "Cancelled export leaves no target");
                    expectEquals (temp.directory.getNumberOfChildFiles (juce::File::findFiles), existing ? 1 : 0,
                                  "Cancelled export removes its temporary file");
                    if (existing)
                        expect (target.deleteFile());
                    // A cancelled render must not poison a subsequent rewind.
                    expectAudioEqual (renderer.render(), raw);
                }
        }
    }

    void runInvalidSettingsTests()
    {
        beginTest ("Empty and reversed ranges fail without creating or replacing a file");
        ExportDirectory temp;
        ExportFixture f;
        const auto target = temp.file ("invalid.wav");
        for (const auto end : { 2.0, 1.0 })
        {
            daw::ExportSettings settings;
            settings.startBeat = 2.0;
            settings.endBeat = end;
            daw::ProjectRenderer renderer (f.project, settings);
            expectEquals (renderer.getLengthInSamples(), int64_t { 0 });
            expectEquals (renderer.render().getNumSamples(), 0);
            expect (renderer.renderToFile (target).failed());
            expect (! target.exists());
        }
        beginTest ("Unsupported bit depth fails and preserves an existing target");
        daw::ExportSettings settings;
        settings.endBeat = 1.0;
        settings.bitDepth = 20;
        daw::ProjectRenderer renderer (f.project, settings);
        expect (renderer.renderToFile (target).failed());
        expect (! target.exists());
        expectSuccess (temp.directory.createDirectory());
        expect (target.replaceWithText ("existing target"));
        expect (renderer.renderToFile (target).failed());
        expectEquals (target.loadFileAsString(), juce::String ("existing target"));
    }

    void runMetronomeTests()
    {
        beginTest ("includeMetronome controls clicks in an empty project independently of the live setting");
        ExportFixture f;
        daw::ExportSettings settings;
        settings.endBeat = 2.5;
        f.engine.getMetronome().setEnabled (true);
        daw::ProjectRenderer withoutClick (f.project, settings);
        const auto silent = withoutClick.render();
        juce::AudioBuffer<float> zeros (2, silent.getNumSamples());
        zeros.clear();
        expectAudioEqual (silent, zeros);
        settings.includeMetronome = true;
        f.engine.getMetronome().setEnabled (false);
        daw::ProjectRenderer withClick (f.project, settings);
        const auto clicks = withClick.render();
        expect (clicks.getMagnitude (0, 0, 1000) > 0.01f, "Click at beat 0");
        expect (clicks.getMagnitude (0, 22050, 1000) > 0.01f, "Click at beat 1");
        expectEquals (clicks.getMagnitude (0, 10000, 1000), 0.0f, "Silence between clicks");
        expectAudioEqual (clicks, f.renderLive (settings));
        expectAudioEqual (withClick.render(), clicks);
    }

    void runProjectEndTests()
    {
        beginTest ("Project end is zero without clips and otherwise the maximum end across tracks");
        ExportFixture f;
        expectEquals (daw::ProjectRenderer::getProjectEndBeat (f.project), 0.0);
        const auto first = f.project.addTrack();
        const auto second = f.project.addTrack();
        expectEquals (daw::ProjectRenderer::getProjectEndBeat (f.project), 0.0);
        f.project.addClip (second, f.secondSample, 2.0); // ends at 3
        const auto longest = f.project.addClip (first, f.firstSample, 7.0); // ends at 9
        f.project.addClip (second, f.constantSample, 0.0); // ends at 4, overwrites earlier clip
        expectEquals (daw::ProjectRenderer::getProjectEndBeat (f.project), 9.0);
        f.project.setClipLooping (longest, true);
        f.project.trimClipEnd (longest, 12.25);
        f.project.setTrackMute (first, true);
        expectEquals (daw::ProjectRenderer::getProjectEndBeat (f.project), 12.25, "Muted looping clip still defines end");
        f.project.deleteClip (longest);
        expectEquals (daw::ProjectRenderer::getProjectEndBeat (f.project), 4.0);
        f.project.deleteClip (second.getChild (0));
        expectEquals (daw::ProjectRenderer::getProjectEndBeat (f.project), 0.0);
    }
};

static ExportTests exportTests;
