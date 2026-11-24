#include "Engine.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <initializer_list>
#include <memory>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;
constexpr int settledStart = 1500; // Beyond the 960-sample (20 ms) gain ramp.
constexpr float centreGain = 0.70710678f;

std::shared_ptr<daw::SampleData> makeConstantSample (float amplitude, int frames = 1024,
                                                     int channels = 1, const juce::String& name = "constant")
{
    auto sample = std::make_shared<daw::SampleData>();
    sample->sampleRate = sampleRate;
    sample->name = name;
    sample->buffer.setSize (channels, frames);
    for (int channel = 0; channel < channels; ++channel)
        for (int frame = 0; frame < frames; ++frame)
            sample->buffer.setSample (channel, frame, amplitude);
    return sample;
}

void prepareEngine (daw::Engine& engine, int maxBlockSize = blockSize)
{
    engine.prepare (sampleRate, maxBlockSize);
    engine.getTransport().setBpm (120.0);
    engine.getTransport().play();
    engine.getMetronome().setEnabled (false);
}

void setTrackClips (daw::AudioTrack& track, std::initializer_list<daw::Clip> clips)
{
    auto list = std::make_unique<daw::ClipList>();
    list->clips.assign (clips);
    track.setClips (std::move (list));
}

double framesToBeats (int frames) { return static_cast<double> (frames) / 24000.0; }

daw::AudioTrack& addConstantTrack (daw::Engine& engine, float amplitude = 0.5f)
{
    auto& track = engine.getTrack (engine.addTrack());
    setTrackClips (track, { { makeConstantSample (amplitude), 0.0, 100.0, 0.0, true } });
    return track;
}

juce::AudioBuffer<float> renderFrames (daw::Engine& engine, int frames, int renderBlockSize = blockSize)
{
    return daw::renderOffline (engine, static_cast<double> (frames) / sampleRate, renderBlockSize);
}
}

/** Simulates message/audio thread handovers sequentially using in-memory clips and offline renders. */
struct TrackTests : juce::UnitTest
{
    TrackTests() : juce::UnitTest ("Track", "diyDaw") {}

    // Check every sample in the requested region, so gaps and wrong polarity cannot hide in an RMS average.
    void expectLevel (const juce::AudioBuffer<float>& audio, int channel, float expected,
                      int start = settledStart, float tolerance = 1.0e-6f)
    {
        expect (start >= 0 && start < audio.getNumSamples(), "Level check must cover audio");
        float largestError = 0.0f;
        for (int i = start; i < audio.getNumSamples(); ++i)
        {
            const auto value = audio.getSample (channel, i);
            if (! std::isfinite (value))
            {
                expect (false, "Non-finite audio sample");
                return;
            }
            largestError = juce::jmax (largestError, std::abs (value - expected));
        }
        expectWithinAbsoluteError (largestError, 0.0f, tolerance,
                                   "Channel " + juce::String (channel) + ", expected level " + juce::String (expected));
    }

    void runTest() override
    {
        runClipTests();

        beginTest ("A clip at beat one is silent before its start and audible after it");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = addConstantTrack (engine);
            setTrackClips (track, { { makeConstantSample (0.5f), 1.0, 100.0, 0.0, true } });
            const auto audio = renderFrames (engine, 28000, 997);
            for (int channel = 0; channel < 2; ++channel)
            {
                expectEquals (audio.getMagnitude (channel, 0, 24000), 0.0f);
                expectLevel (audio, channel, 0.5f * centreGain, 24000 + settledStart);
            }
        }

        beginTest ("Mute silences a running track after smoothing, and unmute restores it");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = addConstantTrack (engine);
            auto audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.5f * centreGain);
            track.setMute (true);
            audio = renderFrames (engine, 4096);
            expect (audio.getMagnitude (0, 64) > 0.0f, "Mute ramps from the previous gain");
            expectLevel (audio, 0, 0.0f);
            expectLevel (audio, 1, 0.0f);
            track.setMute (false);
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.5f * centreGain);
            expectLevel (audio, 1, 0.5f * centreGain);
        }

        beginTest ("Solo suppresses the other track after smoothing and follows solo changes");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& left = addConstantTrack (engine, 0.25f);
            auto& right = addConstantTrack (engine, 0.5f);
            left.setPan (-1.0f);
            right.setPan (1.0f);
            auto audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.25f);
            expectLevel (audio, 1, 0.5f);

            left.setSolo (true);
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.25f);
            expectLevel (audio, 1, 0.0f);

            left.setSolo (false);
            right.setSolo (true);
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.0f);
            expectLevel (audio, 1, 0.5f);

            right.setSolo (false);
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.25f);
            expectLevel (audio, 1, 0.5f);
        }

        beginTest ("Pan extremes isolate channels and centre uses constant power");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = addConstantTrack (engine);
            struct PanCase
            {
                float pan;
                float left;
                float right;
            };
            for (const auto test : { PanCase { -1.0f, 0.5f, 0.0f },
                                     PanCase { 1.0f, 0.0f, 0.5f },
                                     PanCase { 0.0f, 0.5f * centreGain, 0.5f * centreGain } })
            {
                track.setPan (test.pan);
                const auto audio = renderFrames (engine, 4096);
                expectLevel (audio, 0, test.left);
                expectLevel (audio, 1, test.right);
                const auto count = audio.getNumSamples() - settledStart;
                const auto left = audio.getRMSLevel (0, settledStart, count);
                const auto right = audio.getRMSLevel (1, settledStart, count);
                expectWithinAbsoluteError (left * left + right * right, 0.25f, 1.0e-5f);
            }
        }

        beginTest ("Looping stays audible over many sample wraps; a one-shot ends");
        for (const auto looping : { false, true })
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = addConstantTrack (engine);
            setTrackClips (track, { { makeConstantSample (0.5f, 257, 2), 0.0, 100.0, 0.0, looping } });
            const auto audio = renderFrames (engine, 8192, 997);
            expect (audio.getMagnitude (0, 200) > 0.0f, "Both modes initially play");
            expectLevel (audio, 0, looping ? 0.5f * centreGain : 0.0f);
            expectLevel (audio, 1, looping ? 0.5f * centreGain : 0.0f);
        }

        beginTest ("Minus six dB gives approximately half amplitude after the gain ramp");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = addConstantTrack (engine);
            track.setPan (-1.0f);
            const auto unity = renderFrames (engine, 4096);
            expectLevel (unity, 0, 0.5f);
            track.setGainDb (-6.0f);
            const auto attenuated = renderFrames (engine, 4096);
            const auto gain = std::pow (10.0f, -6.0f / 20.0f);
            expectLevel (attenuated, 0, 0.5f * gain);
            const auto count = unity.getNumSamples() - settledStart;
            const auto ratio = attenuated.getRMSLevel (0, settledStart, count)
                             / unity.getRMSLevel (0, settledStart, count);
            expectWithinAbsoluteError (ratio, 0.5f, 0.002f);
            expectLevel (attenuated, 1, 0.0f);
        }

        beginTest ("Master meter captures rendered peaks and consumption resets it");
        {
            daw::Engine engine;
            prepareEngine (engine);
            addConstantTrack (engine);
            const auto audio = renderFrames (engine, 4096);
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto peak = engine.getMasterMeter().consume (channel);
                expect (peak > 0.0f);
                expectWithinAbsoluteError (peak, audio.getMagnitude (channel, 0, audio.getNumSamples()), 1.0e-6f);
                expectEquals (engine.getMasterMeter().consume (channel), 0.0f);
            }
        }

        beginTest ("A running track adopts a replacement clip snapshot and survives garbage collection");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = addConstantTrack (engine, 0.25f);
            track.setPan (-1.0f);
            auto audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.25f);
            const auto position = engine.getTransport().getPositionBeats();

            setTrackClips (track, { { makeConstantSample (0.75f, 257, 2, "replacement"), 0.0, 100.0, 0.0, true } });
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.75f, 0);
            expect (engine.getTransport().getPositionBeats() > position);
            engine.collectGarbage();
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.75f, 0);
            expectLevel (audio, 1, 0.0f, 0);

            setTrackClips (track, {});
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.0f, 0);
            expectLevel (audio, 1, 0.0f, 0);
            engine.collectGarbage();
        }
    }

    void expectRegion (const juce::AudioBuffer<float>& audio, int channel, float level,
                       int start, int count)
    {
        float error = 0.0f;
        for (int i = start; i < start + count; ++i)
        {
            const auto value = audio.getSample (channel, i);
            if (! std::isfinite (value))
            {
                expect (false, "Non-finite sample at frame " + juce::String (i));
                return;
            }
            error = juce::jmax (error, std::abs (value - level));
        }
        expectWithinAbsoluteError (error, 0.0f, 1.0e-6f,
                                   "Region starting at frame " + juce::String (start));
    }

    void runClipTests()
    {
        const auto fadeFrames = static_cast<int> (std::ceil (sampleRate * daw::AudioTrack::clipFadeSeconds));

        beginTest ("Two clips play at independent positions with silence before, between and after");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = engine.getTrack (engine.addTrack());
            setTrackClips (track, {
                { makeConstantSample (0.25f, 8192), framesToBeats (4096), framesToBeats (2048) },
                { makeConstantSample (-0.5f, 8192, 2), framesToBeats (8192), framesToBeats (2048) }
            });
            const auto audio = renderFrames (engine, 12288);
            for (int channel = 0; channel < 2; ++channel)
            {
                expectRegion (audio, channel, 0.0f, 0, 4096);
                expectRegion (audio, channel, 0.25f * centreGain, 4096 + fadeFrames, 2048 - 2 * fadeFrames);
                expectRegion (audio, channel, 0.0f, 6144, 2048);
                expectRegion (audio, channel, -0.5f * centreGain, 8192 + fadeFrames, 2048 - 2 * fadeFrames);
                expectRegion (audio, channel, 0.0f, 10240, 2048);
            }
        }

        beginTest ("Offset selects the correct source frames, including fractional-frame interpolation");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = engine.getTrack (engine.addTrack());
            auto sample = makeConstantSample (0.0f, 12000, 2);
            for (int i = 0; i < sample->getNumFrames(); ++i)
            {
                sample->buffer.setSample (0, i, static_cast<float> (i) / 12000.0f);
                sample->buffer.setSample (1, i, -static_cast<float> (i) / 24000.0f);
            }
            constexpr double offsetFrames = 1234.5;
            setTrackClips (track, { { sample, framesToBeats (4096), framesToBeats (4096), offsetFrames / sampleRate } });
            const auto audio = renderFrames (engine, 9000, 997);
            for (int channel = 0; channel < 2; ++channel)
            {
                float error = 0.0f;
                for (int elapsed = fadeFrames; elapsed < 4096 - fadeFrames; ++elapsed)
                {
                    const auto source = static_cast<float> (offsetFrames + elapsed);
                    const auto expected = centreGain * (channel == 0 ? source / 12000.0f : -source / 24000.0f);
                    error = juce::jmax (error, std::abs (audio.getSample (channel, 4096 + elapsed) - expected));
                }
                expectWithinAbsoluteError (error, 0.0f, 1.0e-6f, "Source-frame encoding agrees at every interior frame");
            }
        }

        beginTest ("Clip length cuts a longer sample off with a 2 ms fade at both edges");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = engine.getTrack (engine.addTrack());
            // Empty blocks settle mixer gains before the clip starts on a block boundary.
            setTrackClips (track, { { makeConstantSample (0.5f, 48000), framesToBeats (4096), framesToBeats (2048) } });
            const auto audio = renderFrames (engine, 8192);
            const auto fullLevel = 0.5f * centreGain;
            for (int channel = 0; channel < 2; ++channel)
            {
                expectWithinAbsoluteError (audio.getSample (channel, 4096), 0.0f, 1.0e-6f, "First clip sample is zero");
                for (int i = 0; i <= fadeFrames; ++i)
                {
                    const auto expected = fullLevel * static_cast<float> (i) / static_cast<float> (fadeFrames);
                    expectWithinAbsoluteError (audio.getSample (channel, 4096 + i), expected, 1.0e-6f,
                                               "Fade-in frame " + juce::String (i));
                    if (i > 0)
                        expectWithinAbsoluteError (audio.getSample (channel, 6144 - i), expected, 1.0e-6f,
                                                   "Fade-out frame " + juce::String (i));
                }
                expectRegion (audio, channel, fullLevel, 4096 + fadeFrames, 2048 - 2 * fadeFrames);
                expectRegion (audio, channel, 0.0f, 6144, 2048);
            }
        }

        beginTest ("A short encoded sample repeats its source frames only when looping is enabled");
        for (const auto looping : { false, true })
        {
            daw::Engine engine;
            prepareEngine (engine, 997);
            auto& track = engine.getTrack (engine.addTrack());
            auto sample = makeConstantSample (0.0f, 257);
            for (int i = 0; i < sample->getNumFrames(); ++i)
                sample->buffer.setSample (0, i, 0.1f + 0.7f * static_cast<float> (i) / 257.0f);
            setTrackClips (track, { { sample, framesToBeats (4096), framesToBeats (4096), 0.0, looping } });
            const auto audio = renderFrames (engine, 10000, 997);
            for (int channel = 0; channel < 2; ++channel)
            {
                expectWithinAbsoluteError (audio.getSample (channel, 4096 + 128),
                                           sample->buffer.getSample (0, 128) * centreGain, 1.0e-6f);
                for (int cycle = 1; cycle < 14; ++cycle)
                    for (const auto sourceFrame : { 32, 128, 240 })
                    {
                        const auto expected = looping ? sample->buffer.getSample (0, sourceFrame) * centreGain : 0.0f;
                        expectWithinAbsoluteError (audio.getSample (channel, 4096 + cycle * 257 + sourceFrame),
                                                   expected, 1.0e-6f,
                                                   "Loop " + juce::String (cycle) + ", source frame " + juce::String (sourceFrame));
                    }
                if (! looping)
                    expectRegion (audio, channel, 0.0f, 4096 + 257, 4096 - 257);
            }
        }

        for (const auto startFrame : { 1237, 0 })
        {
            beginTest ("Whole render is independent of 64/512/997-sample blocks, clip start frame " + juce::String (startFrame));
            auto sample = makeConstantSample (0.0f, 120000, 2);
            for (int i = 0; i < sample->getNumFrames(); ++i)
            {
                sample->buffer.setSample (0, i, static_cast<float> (0.4 * std::sin (0.013 * i)));
                sample->buffer.setSample (1, i, static_cast<float> (0.3 * std::cos (0.017 * i)));
            }
            const auto lengthFrames = startFrame == 0 ? 100003 : 7013;
            const auto totalFrames = startFrame + lengthFrames + 2048;
            juce::AudioBuffer<float> reference;
            for (const auto renderBlockSize : { 64, 512, 997 })
            {
                daw::Engine engine;
                prepareEngine (engine, renderBlockSize);
                auto& track = engine.getTrack (engine.addTrack());
                setTrackClips (track, { { sample, framesToBeats (startFrame), framesToBeats (lengthFrames), 37.25 / sampleRate } });
                const auto audio = renderFrames (engine, totalFrames, renderBlockSize);
                expect (audio.getMagnitude (0, audio.getNumSamples()) > 0.1f, "Comparison includes audible audio");
                for (int channel = 0; channel < 2; ++channel)
                {
                    expectRegion (audio, channel, 0.0f, 0, startFrame);
                    expectRegion (audio, channel, 0.0f, startFrame + lengthFrames, 2048);
                }
                if (renderBlockSize == 64)
                {
                    reference.makeCopyOf (audio);
                    continue;
                }

                float error = 0.0f;
                int worstFrame = 0;
                for (int channel = 0; channel < 2; ++channel)
                    for (int i = 0; i < totalFrames; ++i)
                    {
                        const auto value = audio.getSample (channel, i);
                        if (! std::isfinite (value))
                        {
                            expect (false, "Non-finite output at frame " + juce::String (i));
                            return;
                        }
                        const auto difference = std::abs (value - reference.getSample (channel, i));
                        if (difference > error)
                        {
                            error = difference;
                            worstFrame = i;
                        }
                    }
                expectWithinAbsoluteError (error, 0.0f, 1.0e-6f,
                                           "Block size " + juce::String (renderBlockSize) + ", worst frame " + juce::String (worstFrame));
            }
        }
    }
};

static TrackTests trackTests;
