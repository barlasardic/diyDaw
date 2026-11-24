#include "AudioTrack.h"

#include <cmath>

namespace daw
{

void AudioTrack::prepare (double sampleRate, int maxBlockSize)
{
    scratch.setSize (2, maxBlockSize, false, false, true);
    leftGain.reset (sampleRate, 0.02);
    rightGain.reset (sampleRate, 0.02);
    leftGain.setCurrentAndTargetValue (0.0f);
    rightGain.setCurrentAndTargetValue (0.0f);
}

void AudioTrack::renderClip (const Clip& clip, const Transport::Block& block, int n) noexcept
{
    const auto* s = clip.sample.get();
    if (s == nullptr || s->getNumFrames() < 2 || clip.lengthBeats <= 0.0)
        return;

    // Sample index (within this block) at which a beat position is reached. The tolerance
    // keeps edges that land exactly on a sample stable against floating-point drift.
    const auto indexOf = [&block, n] (double beat)
    {
        const auto x = std::ceil ((beat - block.startBeat) / block.beatsPerSample - 1.0e-3);
        return (int) juce::jlimit (0.0, (double) n, x);
    };

    const auto first = indexOf (clip.startBeat);
    const auto last = indexOf (clip.endBeat());
    if (first >= last)
        return;

    const auto frames = s->getNumFrames();
    const auto secondsPerBeat = 60.0 / block.bpm;
    const auto secondsPerSample = 1.0 / block.sampleRate;
    const auto clipSeconds = clip.lengthBeats * secondsPerBeat;

    // Time since the clip started, at sample 'first'.
    auto elapsed = juce::jmax (0.0, (block.beatAt (first) - clip.startBeat) * secondsPerBeat);

    const auto* srcL = s->buffer.getReadPointer (0);
    const auto* srcR = s->buffer.getReadPointer (juce::jmin (1, s->buffer.getNumChannels() - 1));
    auto* dstL = scratch.getWritePointer (0);
    auto* dstR = scratch.getWritePointer (1);

    for (int i = first; i < last; ++i, elapsed += secondsPerSample)
    {
        auto pos = (elapsed + clip.offsetSeconds) * s->sampleRate;

        if (clip.looping)
        {
            pos = std::fmod (pos, (double) frames);
            if (pos < 0.0)
                pos += frames;
        }
        else if (pos >= frames - 1)
            break;
        else if (pos < 0.0)
            continue;

        const auto fade = juce::jmin (1.0, elapsed / clipFadeSeconds, (clipSeconds - elapsed) / clipFadeSeconds);
        const auto g = clip.gain * (float) juce::jmax (0.0, fade);

        const auto i0 = (int) pos;
        const auto i1 = i0 + 1 < frames ? i0 + 1 : 0; // wraps only when looping
        const auto frac = (float) (pos - i0);

        dstL[i] += g * (srcL[i0] + frac * (srcL[i1] - srcL[i0]));
        dstR[i] += g * (srcR[i0] + frac * (srcR[i1] - srcR[i0]));
    }
}

void AudioTrack::process (juce::AudioBuffer<float>& mixBus, const Transport::Block& block, bool anyTrackSoloed) noexcept
{
    const auto n = juce::jmin (block.numSamples, scratch.getNumSamples());
    scratch.clear (0, n);

    if (auto* list = clips.acquire(); list != nullptr && block.playing && block.beatsPerSample > 0.0)
    {
        const auto blockEnd = block.endBeat();

        for (const auto& clip : list->clips)
        {
            if (clip.startBeat >= blockEnd)
                break; // sorted: nothing later can overlap this block

            if (clip.endBeat() > block.startBeat)
                renderClip (clip, block, n);
        }
    }

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
