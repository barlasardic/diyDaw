#include "Engine.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <memory>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;
constexpr int settledStart = 1500; // Beyond the 960-sample (20 ms) gain ramp.
constexpr float centreGain = 0.70710678f;

std::unique_ptr<daw::SampleData> makeConstantSample (float amplitude, int frames = 1024,
                                                   int channels = 1, const juce::String& name = "constant")
{
    auto sample = std::make_unique<daw::SampleData>();
    sample->sampleRate = sampleRate;
    sample->name = name;
    sample->buffer.setSize (channels, frames);
    for (int channel = 0; channel < channels; ++channel)
        for (int frame = 0; frame < frames; ++frame)
            sample->buffer.setSample (channel, frame, amplitude);
    return sample;
}

void prepareEngine (daw::Engine& engine)
{
    engine.prepare (sampleRate, blockSize);
    engine.getTransport().setBpm (120.0);
    engine.getTransport().play();
    engine.getMetronome().setEnabled (false);
}

daw::AudioTrack& addConstantTrack (daw::Engine& engine, float amplitude = 0.5f)
{
    auto& track = engine.getTrack (engine.addTrack());
    track.setSample (makeConstantSample (amplitude));
    track.setLooping (true);
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
        beginTest ("A clip at beat one is silent before its start and audible after it");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = addConstantTrack (engine);
            track.setClipStartBeat (1.0);
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
            track.setSample (makeConstantSample (0.5f, 257, 2));
            track.setLooping (looping);
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

        beginTest ("A running track adopts a replacement sample and survives garbage collection");
        {
            daw::Engine engine;
            prepareEngine (engine);
            auto& track = addConstantTrack (engine, 0.25f);
            track.setPan (-1.0f);
            auto audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.25f);
            const auto position = engine.getTransport().getPositionBeats();

            track.setSample (makeConstantSample (0.75f, 257, 2, "replacement"));
            expectEquals (track.getSampleName(), juce::String ("replacement"));
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.75f, 0);
            expect (engine.getTransport().getPositionBeats() > position);
            engine.collectGarbage();
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.75f, 0);
            expectLevel (audio, 1, 0.0f, 0);

            track.setSample (nullptr);
            audio = renderFrames (engine, 4096);
            expectLevel (audio, 0, 0.0f, 0);
            expectLevel (audio, 1, 0.0f, 0);
            engine.collectGarbage();
        }
    }
};

static TrackTests trackTests;
