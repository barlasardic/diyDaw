#include "AudioTrack.h"

#include <cmath>

namespace daw
{

void AudioTrack::setSample (std::unique_ptr<SampleData> newSample)
{
    sampleName = newSample != nullptr ? newSample->name : juce::String();
    sample.publish (newSample != nullptr ? std::move (newSample) : std::make_unique<SampleData>());
}

void AudioTrack::prepare (double sampleRate, int maxBlockSize)
{
    scratch.setSize (2, maxBlockSize, false, false, true);
    leftGain.reset (sampleRate, 0.02);
    rightGain.reset (sampleRate, 0.02);
    leftGain.setCurrentAndTargetValue (0.0f);
    rightGain.setCurrentAndTargetValue (0.0f);
}

void AudioTrack::renderClip (const SampleData& s, const Transport::Block& block) noexcept
{
    const auto frames = s.getNumFrames();
    if (frames < 2 || ! block.playing)
        return;

    // Native-speed playback: position in source frames = elapsed seconds * source rate.
    const auto secondsPerBeat = 60.0 / block.bpm;
    const auto clipStart = clipStartBeat.load (std::memory_order_relaxed);
    const auto increment = s.sampleRate / block.sampleRate;
    const auto loop = looping.load (std::memory_order_relaxed);

    auto pos = (block.startBeat - clipStart) * secondsPerBeat * s.sampleRate;

    const auto* srcL = s.buffer.getReadPointer (0);
    const auto* srcR = s.buffer.getReadPointer (juce::jmin (1, s.buffer.getNumChannels() - 1));
    auto* dstL = scratch.getWritePointer (0);
    auto* dstR = scratch.getWritePointer (1);

    for (int i = 0; i < block.numSamples; ++i, pos += increment)
    {
        if (pos < 0.0)
            continue;

        auto p = pos;
        if (loop)
            p = std::fmod (p, (double) frames);
        else if (p >= frames - 1)
            break;

        const auto i0 = (int) p;
        const auto i1 = i0 + 1 < frames ? i0 + 1 : 0; // wraps only when looping
        const auto frac = (float) (p - i0);

        dstL[i] = srcL[i0] + frac * (srcL[i1] - srcL[i0]);
        dstR[i] = srcR[i0] + frac * (srcR[i1] - srcR[i0]);
    }
}

void AudioTrack::process (juce::AudioBuffer<float>& mixBus, const Transport::Block& block, bool anyTrackSoloed) noexcept
{
    const auto n = juce::jmin (block.numSamples, scratch.getNumSamples());
    scratch.clear (0, n);

    if (auto* s = sample.acquire())
        renderClip (*s, block);

    // Fader + pan + mute/solo, smoothed to avoid zipper noise and clicks.
    const auto db = gainDb.load (std::memory_order_relaxed);
    const auto silent = mute.load (std::memory_order_relaxed)
                     || (anyTrackSoloed && ! solo.load (std::memory_order_relaxed))
                     || db <= minGainDb;

    const auto g = silent ? 0.0f : juce::Decibels::decibelsToGain (db);
    const auto angle = (pan.load (std::memory_order_relaxed) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    leftGain.setTargetValue (g * std::cos (angle));
    rightGain.setTargetValue (g * std::sin (angle));

    leftGain.applyGain (scratch.getWritePointer (0), n);
    rightGain.applyGain (scratch.getWritePointer (1), n);

    meter.push (scratch, n);

    for (int ch = 0; ch < juce::jmin (2, mixBus.getNumChannels()); ++ch)
        mixBus.addFrom (ch, 0, scratch, ch, 0, n);
}

} // namespace daw
