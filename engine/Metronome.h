#pragma once

#include "Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <limits>

namespace daw
{

/** Synthesised click on every beat; higher pitch on the first beat of each bar.
    Clicks are sample-accurate and ring out naturally after the transport stops.
*/
class Metronome
{
public:
    static constexpr double accentFrequency = 1500.0;
    static constexpr double beatFrequency = 1000.0;
    static constexpr double clickLengthSeconds = 0.05;

    // ---- message thread ---------------------------------------------------
    void setEnabled (bool shouldBeEnabled) noexcept { enabled.store (shouldBeEnabled); }
    bool isEnabled() const noexcept                 { return enabled.load(); }

    void setGain (float newGain) noexcept           { gain.store (newGain); }
    float getGain() const noexcept                  { return gain.load(); }

    // ---- audio thread -----------------------------------------------------
    void prepare (double newSampleRate) noexcept;
    void reset() noexcept
    {
        samplesLeft = 0;
        expectedStartBeat = std::numeric_limits<double>::quiet_NaN();
    }

    /** Adds clicks into every channel of 'out' (does not clear it). */
    void process (juce::AudioBuffer<float>& out, const Transport::Block& block) noexcept;

private:
    void trigger (bool accent) noexcept;
    void render (juce::AudioBuffer<float>& out, int start, int end) noexcept;

    std::atomic<bool> enabled { false };
    std::atomic<float> gain { 0.5f };

    double sampleRate = 44100.0;
    double phase = 0.0, phaseIncrement = 0.0;
    double envelope = 0.0, envelopeDecay = 0.0;
    int samplesLeft = 0;

    double nextBeat = 0.0;  // next integer beat to click
    double expectedStartBeat = std::numeric_limits<double>::quiet_NaN();
};

} // namespace daw
