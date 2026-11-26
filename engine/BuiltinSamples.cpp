#include "BuiltinSamples.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace daw
{

const juce::StringArray& getBuiltinSampleNames()
{
    static const juce::StringArray names { "kick", "rim", "snare", "clap", "shaker", "lowTom", "closedHat", "midTom",
                                           "cowbell", "highTom", "openHat", "conga", "zap", "crash", "sub", "ride" };
    return names;
}

namespace
{
constexpr double pi = juce::MathConstants<double>::pi;

// Local, deterministic synthesis state. None of these helpers run on the audio thread.
struct Noise
{
    std::uint32_t state = 0x6d2b79f5u;

    double next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double> (state) / 2147483648.0 - 1.0;
    }
};

struct BandPass
{
    double low = 0, high = 0, lowCoefficient, highCoefficient;

    BandPass (double lowHz, double highHz, double rate)
        : lowCoefficient (1.0 - std::exp (-2.0 * pi * std::min (lowHz, rate * 0.4) / rate)),
          highCoefficient (1.0 - std::exp (-2.0 * pi * std::min (highHz, rate * 0.45) / rate)) {}

    double tick (double input) noexcept
    {
        low += lowCoefficient * (input - low);
        high += highCoefficient * (input - low - high);
        return high;
    }
};

double blep (double phase, double step) noexcept
{
    if (phase < step)
    {
        const auto x = phase / step;
        return 2.0 * x - x * x - 1.0;
    }
    if (phase > 1.0 - step)
    {
        const auto x = (phase - 1.0) / step;
        return x * x + 2.0 * x + 1.0;
    }
    return 0;
}

double square (double& phase, double hz, double rate) noexcept
{
    const auto step = std::min (0.45, hz / rate);
    const auto opposite = phase < 0.5 ? phase + 0.5 : phase - 0.5;
    const auto result = (phase < 0.5 ? 1.0 : -1.0) + blep (phase, step) - blep (opposite, step);
    phase += step;
    phase -= std::floor (phase);
    return result;
}
}

std::shared_ptr<const SampleData> createBuiltinSample (const juce::String& name, double sampleRate)
{
    const auto index = getBuiltinSampleNames().indexOf (name);
    if (index < 0 || ! std::isfinite (sampleRate) || sampleRate < 1000.0 || sampleRate > 768000.0)
        return nullptr;

    constexpr std::array<double, 16> durations {
        0.65, 0.12, 0.38, 0.42, 0.16, 0.65, 0.12, 0.52,
        0.4, 0.42, 0.75, 0.38, 0.3, 2.3, 1.8, 2.1
    };
    const auto duration = durations[static_cast<size_t> (index)];
    const auto frames = static_cast<int> (std::ceil (duration * sampleRate));
    auto sample = std::make_shared<SampleData>();
    sample->name = name;
    sample->sampleRate = sampleRate;
    sample->buffer.setSize (1, frames);
    auto* data = sample->buffer.getWritePointer (0);

    Noise noise;
    BandPass snareNoise (1100, 10000, sampleRate), metalFilter (6500, 14000, sampleRate),
             clapNoise (750, 7500, sampleRate), bellFilter (350, 6500, sampleRate);
    std::array<double, 6> phases {};
    constexpr std::array<double, 6> metalHz { 205.3, 369.6, 522.7, 800.0, 1133.1, 1567.7 };
    double phase = 0, secondaryPhase = 0;
    double peak = 0;
    for (int i = 0; i < frames; ++i)
    {
        const auto t = i / sampleRate;
        const auto n = noise.next();
        const auto sine = [&] (double hz)
        {
            phase += 2.0 * pi * std::min (hz, sampleRate * 0.45) / sampleRate;
            phase = std::fmod (phase, 2.0 * pi);
            return std::sin (phase);
        };
        double value = 0;
        switch (index)
        {
            case 0: // Kick: falling sine oscillator and a separate, very short beater click.
                value = sine (49 + 145 * std::exp (-t / 0.018)) * std::exp (-t / 0.16)
                        + 0.22 * n * std::exp (-t / 0.002);
                break;
            case 1: // Rim: two inharmonic resonances with a woody, noisy attack.
                value = (std::sin (2 * pi * 790 * t) + 0.55 * std::sin (2 * pi * 1843 * t))
                        * std::exp (-t / 0.018) + 0.2 * n * std::exp (-t / 0.0025);
                break;
            case 2: // Snare: a swept shell mode, a second mode, and a bright wire tail.
                value = 0.48 * sine (175 + 65 * std::exp (-t / 0.012)) * std::exp (-t / 0.055)
                        + 0.22 * std::sin (2 * pi * 335 * t) * std::exp (-t / 0.04)
                        + 0.9 * snareNoise.tick (n) * std::exp (-t / 0.085);
                break;
            case 3: // Clap: three spaced bursts, then the diffuse room/noise tail.
            {
                double envelope = 0;
                for (const auto onset : { 0.0, 0.011, 0.023 })
                    if (t >= onset)
                        envelope += std::exp (-(t - onset) / 0.003);
                if (t >= 0.034)
                    envelope += 0.55 * std::exp (-(t - 0.034) / 0.075);
                value = clapNoise.tick (n) * envelope;
                break;
            }
            case 4:
                value = metalFilter.tick (n) * (1.0 - std::exp (-t / 0.003)) * std::exp (-t / 0.028);
                break;
            case 5:
            case 7:
            case 9:
            {
                const auto hz = index == 5 ? 92.0 : index == 7 ? 137.0 : 196.0;
                const auto envelope = std::exp (-t / (index == 5 ? 0.15 : index == 7 ? 0.115 : 0.085));
                value = (sine (hz * (1.0 + 0.65 * std::exp (-t / 0.023)))
                         + 0.12 * std::sin (2 * pi * hz * 1.59 * t)) * envelope
                        + 0.08 * n * std::exp (-t / 0.004);
                break;
            }
            case 6:
            case 10:
            case 13:
            case 15:
            {
                double metal = 0;
                const auto scale = index == 13 ? 1.37 : index == 15 ? 1.83 : 1.0;
                for (size_t oscillator = 0; oscillator < phases.size(); ++oscillator)
                    metal += square (phases[oscillator], metalHz[oscillator] * scale, sampleRate) / 6.0;
                const auto filtered = metalFilter.tick (0.7 * metal + 0.3 * n);
                const auto seconds = index == 6 ? 0.023 : index == 10 ? 0.19 : index == 13 ? 0.54 : 0.65;
                value = filtered * std::exp (-t / seconds);
                if (index == 13)
                    value += 0.35 * snareNoise.tick (n) * (1 - std::exp (-t / 0.008)) * std::exp (-t / 0.4);
                if (index == 15)
                    value += 0.15 * (std::sin (2 * pi * 2453 * t) + 0.6 * std::sin (2 * pi * 3989 * t))
                             * std::exp (-t / 0.8); // Ride's pitched bell.
                break;
            }
            case 8:
                value = bellFilter.tick (square (phase, 540, sampleRate) + square (secondaryPhase, 811, sampleRate))
                        * std::exp (-t / 0.085);
                break;
            case 11:
                value = (sine (178 + 105 * std::exp (-t / 0.009))
                         + 0.18 * std::sin (2 * pi * 397 * t)) * std::exp (-t / 0.075);
                break;
            case 12:
                value = sine (95 + 3200 * std::exp (-t / 0.035)) * std::exp (-t / 0.065);
                break;
            case 14:
                value = sine (48 + 35 * std::exp (-t / 0.03)) * std::exp (-t / 0.45);
                break;
            default: break;
        }
        // Half-cosine tapers give exactly zero endpoints, even for long cymbal tails.
        const auto attack = std::min (1.0, t / 0.0005);
        const auto release = std::min (1.0, (frames - 1 - i) / (sampleRate * 0.01));
        value *= 0.5 * (1.0 - std::cos (pi * attack)) * 0.5 * (1.0 - std::cos (pi * release));
        data[i] = static_cast<float> (value);
        peak = std::max (peak, std::abs (static_cast<double> (data[i])));
    }
    if (peak > 0)
        sample->buffer.applyGain (static_cast<float> (std::pow (10.0, -3.0 / 20.0) / peak));
    sample->computePeaks();
    return sample;
}

} // namespace daw
