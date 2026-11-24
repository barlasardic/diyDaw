#include "Metronome.h"

#include <cmath>
#include <limits>

namespace daw
{

void Metronome::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate;
    // ~60 dB of decay over the click length.
    envelopeDecay = std::exp (std::log (0.001) / (clickLengthSeconds * sampleRate));
    reset();
}

void Metronome::trigger (bool accent) noexcept
{
    phase = 0.0;
    phaseIncrement = juce::MathConstants<double>::twoPi * (accent ? accentFrequency : beatFrequency) / sampleRate;
    envelope = accent ? 1.0 : 0.7;
    samplesLeft = (int) std::ceil (clickLengthSeconds * sampleRate);
}

void Metronome::render (juce::AudioBuffer<float>& out, int start, int end) noexcept
{
    const auto g = (double) gain.load (std::memory_order_relaxed);

    for (int i = start; i < end && samplesLeft > 0; ++i, --samplesLeft)
    {
        const auto s = (float) (std::sin (phase) * envelope * g);
        phase += phaseIncrement;
        envelope *= envelopeDecay;

        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            out.addSample (ch, i, s);
    }
}

void Metronome::process (juce::AudioBuffer<float>& out, const Transport::Block& block) noexcept
{
    const auto n = block.numSamples;
    int cursor = 0;

    if (block.playing && enabled.load (std::memory_order_relaxed) && block.beatsPerSample > 0.0)
    {
        // The playhead is accumulated in floating point, so a beat that falls exactly on a
        // sample can come out as 447.9999999 or 448.0000001. Work in samples with a small
        // tolerance, and remember the next beat to click so a beat sitting right on a block
        // boundary is clicked exactly once even if both blocks' arithmetic disagree slightly.
        constexpr double toleranceSamples = 1.0e-3;

        // Bit-exact on purpose: Transport makes each block start exactly where the last ended.
        if (! juce::exactlyEqual (block.startBeat, expectedStartBeat)) // first block, seek, or restart
            nextBeat = std::ceil (block.startBeat - toleranceSamples * block.beatsPerSample);

        expectedStartBeat = block.endBeat();

        for (;;)
        {
            const auto position = (nextBeat - block.startBeat) / block.beatsPerSample;
            auto offset = (int) std::ceil (position - toleranceSamples);

            if (offset >= n)
                break;

            offset = juce::jlimit (cursor, n - 1, offset);
            render (out, cursor, offset);
            cursor = offset;

            trigger ((long long) nextBeat % block.beatsPerBar == 0);
            nextBeat += 1.0;
        }
    }
    else
    {
        expectedStartBeat = std::numeric_limits<double>::quiet_NaN(); // force a resync on the next play
    }

    render (out, cursor, n);
}

} // namespace daw
