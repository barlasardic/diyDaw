#include "Engine.h"

#include <cmath>

namespace daw
{

Engine::Engine()
{
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        tracks[i] = std::make_unique<AudioTrack>();
        tracks[i]->setName ("Audio " + juce::String ((int) i + 1));
    }
}

int Engine::addTrack()
{
    const auto index = numTracks.load();
    if (index >= maxTracks)
        return -1;

    numTracks.store (index + 1);
    return index;
}

void Engine::collectGarbage()
{
    for (auto& t : tracks)
        t->collectGarbage();
}

void Engine::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    bus.setSize (2, maxBlockSize, false, false, true);
    transport.prepare (sampleRate);
    metronome.prepare (sampleRate);

    for (auto& t : tracks)
        t->prepare (sampleRate, maxBlockSize);

    masterGain.reset (sampleRate, 0.02);
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (masterGainDb.load()));
}

void Engine::render (juce::AudioBuffer<float>& out, int numSamples) noexcept
{
    out.clear (0, numSamples);

    const auto chunk = bus.getNumSamples();
    if (chunk == 0) // not prepared yet
        return;

    for (int start = 0; start < numSamples; start += chunk)
        renderChunk (out, start, juce::jmin (chunk, numSamples - start));
}

void Engine::renderChunk (juce::AudioBuffer<float>& out, int startSample, int numSamples) noexcept
{
    bus.clear (0, numSamples);

    const auto block = transport.advance (numSamples);
    const auto activeTracks = numTracks.load (std::memory_order_acquire);

    bool anySolo = false;
    for (int i = 0; i < activeTracks; ++i)
        anySolo = anySolo || tracks[(size_t) i]->isSoloed();

    for (int i = 0; i < activeTracks; ++i)
        tracks[(size_t) i]->process (bus, block, anySolo);

    const auto db = masterGainDb.load (std::memory_order_relaxed);
    masterGain.setTargetValue (db <= AudioTrack::minGainDb ? 0.0f : juce::Decibels::decibelsToGain (db));

    if (masterGain.isSmoothing())
    {
        // Same gain curve on both channels.
        for (int i = 0; i < numSamples; ++i)
        {
            const auto g = masterGain.getNextValue();
            bus.setSample (0, i, bus.getSample (0, i) * g);
            bus.setSample (1, i, bus.getSample (1, i) * g);
        }
    }
    else
    {
        bus.applyGain (0, numSamples, masterGain.getTargetValue());
    }

    // The click goes after the master fader, like a cue send.
    metronome.process (bus, block);

    masterMeter.push (bus, numSamples);

    for (int ch = 0; ch < juce::jmin (2, out.getNumChannels()); ++ch)
        out.copyFrom (ch, startSample, bus, ch, 0, numSamples);
}

juce::AudioBuffer<float> renderOffline (Engine& engine, double seconds, int blockSize)
{
    const auto total = (int) std::llround (seconds * engine.getSampleRate());
    juce::AudioBuffer<float> result (2, total);
    juce::AudioBuffer<float> block (2, blockSize);

    for (int pos = 0; pos < total; pos += blockSize)
    {
        const auto n = juce::jmin (blockSize, total - pos);
        engine.render (block, n);

        for (int ch = 0; ch < 2; ++ch)
            result.copyFrom (ch, pos, block, ch, 0, n);
    }

    return result;
}

} // namespace daw
