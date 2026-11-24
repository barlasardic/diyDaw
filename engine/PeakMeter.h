#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

namespace daw
{

/** Stereo peak level handed from the audio thread to the UI.
    The audio thread accumulates the max; the UI reads-and-resets with consume(). */
class PeakMeter
{
public:
    // audio thread
    void push (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto src = juce::jmin (ch, buffer.getNumChannels() - 1);
            if (src < 0)
                return;

            const auto level = buffer.getMagnitude (src, 0, numSamples);
            auto& p = peaks[(size_t) ch];
            if (level > p.load (std::memory_order_relaxed))
                p.store (level, std::memory_order_relaxed);
        }
    }

    // message thread: returns the peak (linear gain) since the last call
    float consume (int channel) noexcept
    {
        return peaks[(size_t) juce::jlimit (0, 1, channel)].exchange (0.0f, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<float>, 2> peaks {};
};

} // namespace daw
