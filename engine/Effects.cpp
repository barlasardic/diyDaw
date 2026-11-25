#include "Effects.h"

#include <algorithm>
#include <cmath>

namespace daw
{
namespace
{
constexpr double twoPi = juce::MathConstants<double>::twoPi;

// Owned by each effect; prepare may allocate, all other operations are audio-thread safe.
template <size_t N>
struct Controls
{
    std::array<juce::SmoothedValue<double>, N> values;

    void prepare (double rate)
    {
        for (auto& value : values)
            value.reset (rate, 0.02);
    }

    void reset (const AudioEffect& effect) noexcept
    {
        for (size_t i = 0; i < N; ++i)
            values[i].setCurrentAndTargetValue (effect.getParam (static_cast<int> (i)));
    }

    void target (const AudioEffect& effect) noexcept
    {
        for (size_t i = 0; i < N; ++i)
            values[i].setTargetValue (effect.getParam (static_cast<int> (i)));
    }

    std::array<double, N> next() noexcept
    {
        std::array<double, N> result;
        for (size_t i = 0; i < N; ++i)
            result[i] = values[i].getNextValue();
        return result;
    }
};

double dbToGain (double db) noexcept { return std::pow (10.0, db * 0.05); }

// Circular storage is allocated only by prepare(). Read before write, with fractional taps.
struct DelayLine
{
    std::vector<double> samples;
    size_t position = 0;

    void prepare (int length) { samples.resize (static_cast<size_t> (length)); }
    void reset() noexcept
    {
        std::fill (samples.begin(), samples.end(), 0.0);
        position = 0;
    }
    double read (double delay) const noexcept
    {
        const auto size = static_cast<double> (samples.size());
        auto index = static_cast<double> (position) - delay;
        if (index < 0.0)
            index += size;

        // A tiny negative index plus the size can round to exactly 'size': wrap it back.
        if (index >= size)
            index -= size;
        index = juce::jlimit (0.0, size - 1.0e-9, index);

        auto first = static_cast<size_t> (index);
        if (first >= samples.size())
            first = samples.size() - 1;
        const auto second = (first + 1) % samples.size();
        const auto fraction = index - static_cast<double> (first);
        return samples[first] + fraction * (samples[second] - samples[first]);
    }
    double oldest() const noexcept { return samples[position]; }
    void write (double input) noexcept
    {
        samples[position] = input;
        if (++position == samples.size())
            position = 0;
    }
};

// Limit the read-head speed to avoid large sample jumps even for a four-second change.
double glide (double current, double target) noexcept
{
    return current + juce::jlimit (-0.25, 0.25, target - current);
}

struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    std::array<double, 2> z1 {}, z2 {};

    void reset() noexcept { z1.fill (0.0); z2.fill (0.0); }
    double process (double input, size_t channel) noexcept
    {
        const auto result = b0 * input + z1[channel];
        z1[channel] = b1 * input - a1 * result + z2[channel];
        z2[channel] = b2 * input - a2 * result;
        return result;
    }

    // RBJ shelf slope S=1; frequency is bounded below Nyquist at every supported rate.
    void set (int band, double frequency, double gain, double q, double rate) noexcept
    {
        if (juce::exactlyEqual (gain, 0.0))
        {
            b0 = 1.0;
            b1 = b2 = a1 = a2 = 0.0;
            return;
        }
        const auto a = std::pow (10.0, gain / 40.0);
        const auto omega = twoPi * std::min (frequency, rate * 0.45) / rate;
        const auto c = std::cos (omega), s = std::sin (omega);
        double n0, n1, n2, d0, d1, d2;
        if (band == 1)
        {
            const auto alpha = s / (2.0 * q);
            n0 = 1.0 + alpha * a; n1 = -2.0 * c; n2 = 1.0 - alpha * a;
            d0 = 1.0 + alpha / a; d1 = -2.0 * c; d2 = 1.0 - alpha / a;
        }
        else
        {
            const auto beta = std::sqrt (2.0 * a) * s;
            const auto plus = a + 1.0, minus = a - 1.0;
            if (band == 0)
            {
                n0 = a * (plus - minus * c + beta);
                n1 = 2.0 * a * (minus - plus * c);
                n2 = a * (plus - minus * c - beta);
                d0 = plus + minus * c + beta;
                d1 = -2.0 * (minus + plus * c);
                d2 = plus + minus * c - beta;
            }
            else
            {
                n0 = a * (plus + minus * c + beta);
                n1 = -2.0 * a * (minus + plus * c);
                n2 = a * (plus + minus * c - beta);
                d0 = plus - minus * c + beta;
                d1 = 2.0 * (minus - plus * c);
                d2 = plus - minus * c - beta;
            }
        }
        b0 = n0 / d0; b1 = n1 / d0; b2 = n2 / d0;
        a1 = d1 / d0; a2 = d2 / d0;
    }
};
} // namespace

struct EqEffect::State
{
    Controls<numParams> controls;
    std::array<Biquad, 3> filters;
    std::array<double, numParams> previous {};
    double rate = 48000.0;
    int phase = 0;
    bool dirty = true;
};

EqEffect::EqEffect() : state (std::make_unique<State>()) { initialiseParams(); }
EqEffect::~EqEffect() = default;

const std::vector<ParamInfo>& EqEffect::infos()
{
    static const std::vector<ParamInfo> result {
        { "lowFreq",  "Low Freq",  20, 1000, 100, 141.421f, false, "Hz" },
        { "lowGain",  "Low Gain", -18, 18, 0, 0, false, "dB" },
        { "midFreq",  "Mid Freq", 100, 10000, 1000, 1000, false, "Hz" },
        { "midGain",  "Mid Gain", -18, 18, 0, 0, false, "dB" },
        { "midQ",     "Mid Q", 0.3f, 8, 1, 1.5f, false, "" },
        { "highFreq", "High Freq", 1000, 20000, 8000, 4472.136f, false, "Hz" },
        { "highGain", "High Gain", -18, 18, 0, 0, false, "dB" },
        { "output",   "Output", -18, 18, 0, 0, false, "dB" }
    };
    return result;
}

void EqEffect::prepareEffect (double rate, int)
{
    state->rate = rate;
    state->controls.prepare (rate);
}

void EqEffect::reset() noexcept
{
    state->controls.reset (*this);
    for (auto& filter : state->filters)
        filter.reset();
    state->phase = 0;
    state->dirty = true;
}

void EqEffect::processEffect (juce::AudioBuffer<float>& buffer, int count, const Transport::Block&) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    auto& s = *state;
    s.controls.target (*this);
    auto* l = buffer.getWritePointer (0);
    auto* r = buffer.getWritePointer (1);
    for (int i = 0; i < count; ++i)
    {
        const auto p = s.controls.next();
        if (s.phase == 0 && (s.dirty || p != s.previous))
        {
            s.filters[0].set (0, p[lowFreq], p[lowGain], 1.0, s.rate);
            s.filters[1].set (1, p[midFreq], p[midGain], p[midQ], s.rate);
            s.filters[2].set (2, p[highFreq], p[highGain], 1.0, s.rate);
            s.previous = p;
            s.dirty = false;
        }
        s.phase = (s.phase + 1) % 32;
        const auto gain = dbToGain (p[output]);
        double left = l[i], right = r[i];
        for (auto& filter : s.filters)
        {
            left = filter.process (left, 0);
            right = filter.process (right, 1);
        }
        l[i] = static_cast<float> (left * gain);
        r[i] = static_cast<float> (right * gain);
    }
}

struct CompressorEffect::State
{
    Controls<numParams> controls;
    double rate = 48000.0, reduction = 0.0;
};

CompressorEffect::CompressorEffect() : state (std::make_unique<State>()) { initialiseParams(); }
CompressorEffect::~CompressorEffect() = default;

const std::vector<ParamInfo>& CompressorEffect::infos()
{
    static const std::vector<ParamInfo> result {
        { "threshold", "Threshold", -60, 0, -18, -30, false, "dB" },
        { "ratio", "Ratio", 1, 20, 4, 4, false, ":1" },
        { "attack", "Attack", 0.1f, 100, 10, 3.1623f, false, "ms" },
        { "release", "Release", 10, 1000, 150, 100, false, "ms" },
        { "knee", "Knee", 0, 24, 6, 12, false, "dB" },
        { "makeup", "Makeup", 0, 24, 0, 12, false, "dB" },
        { "mix", "Mix", 0, 1, 1, 0.5f, false, "%" }
    };
    return result;
}

void CompressorEffect::prepareEffect (double rate, int)
{
    state->rate = rate;
    state->controls.prepare (rate);
}

void CompressorEffect::reset() noexcept
{
    state->controls.reset (*this);
    state->reduction = 0.0;
    gainReductionDb.store (0.0f, std::memory_order_relaxed);
}

void CompressorEffect::processEffect (juce::AudioBuffer<float>& buffer, int count, const Transport::Block&) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    auto& s = *state;
    s.controls.target (*this);
    auto* l = buffer.getWritePointer (0);
    auto* r = buffer.getWritePointer (1);
    for (int i = 0; i < count; ++i)
    {
        const auto p = s.controls.next();
        const auto peak = std::max (std::abs (static_cast<double> (l[i])), std::abs (static_cast<double> (r[i])));
        const auto over = 20.0 * std::log10 (std::max (peak, 1.0e-15)) - p[threshold];
        const auto slope = 1.0 - 1.0 / p[ratio];
        double target = 0.0;
        if (over >= p[knee] * 0.5)
            target = slope * over;
        else if (p[knee] > 0.0 && over > -p[knee] * 0.5)
        {
            const auto x = over + p[knee] * 0.5;
            target = slope * x * x / (2.0 * p[knee]);
        }
        const auto time = target > s.reduction ? p[attack] : p[release];
        const auto coefficient = std::exp (-1.0 / (0.001 * time * s.rate));
        s.reduction = target + coefficient * (s.reduction - target);
        const auto gain = 1.0 + p[mix] * (dbToGain (p[makeup] - s.reduction) - 1.0);
        l[i] = static_cast<float> (l[i] * gain);
        r[i] = static_cast<float> (r[i] * gain);
    }
    gainReductionDb.store (static_cast<float> (s.reduction), std::memory_order_relaxed);
}

struct ReverbEffect::State
{
    struct Comb
    {
        DelayLine line;
        double lowpass = 0.0;
        double process (double input, double feedbackGain, double damp) noexcept
        {
            const auto delayed = line.oldest();
            lowpass = delayed * (1.0 - damp) + lowpass * damp;
            line.write (input + feedbackGain * lowpass);
            return delayed;
        }
    };
    struct Allpass
    {
        DelayLine line;
        double process (double input) noexcept
        {
            const auto output = line.oldest() - 0.5 * input;
            line.write (input + 0.5 * output);
            return output;
        }
    };
    Controls<numParams> controls;
    std::array<std::array<Comb, 8>, 2> combs;
    std::array<std::array<Allpass, 4>, 2> allpasses;
    DelayLine pre;
    double rate = 48000.0, preSamples = 1.0;
};

ReverbEffect::ReverbEffect() : state (std::make_unique<State>()) { initialiseParams(); }
ReverbEffect::~ReverbEffect() = default;

const std::vector<ParamInfo>& ReverbEffect::infos()
{
    static const std::vector<ParamInfo> result {
        { "size", "Size", 0, 1, 0.5f, 0.5f, false, "%" },
        { "damping", "Damping", 0, 1, 0.5f, 0.5f, false, "%" },
        { "predelay", "Pre-delay", 0, 200, 10, 50, false, "ms" },
        { "width", "Width", 0, 1, 1, 0.5f, false, "%" },
        { "mix", "Mix", 0, 1, 0.25f, 0.5f, false, "%" }
    };
    return result;
}

void ReverbEffect::prepareEffect (double rate, int)
{
    auto& s = *state;
    s.rate = rate;
    s.controls.prepare (rate);
    constexpr std::array<int, 8> combLengths { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
    constexpr std::array<int, 4> allpassLengths { 556, 441, 341, 225 };
    for (size_t ch = 0; ch < 2; ++ch)
    {
        const auto spread = ch == 0 ? 0 : 23;
        for (size_t j = 0; j < combLengths.size(); ++j)
            s.combs[ch][j].line.prepare (std::max (1, static_cast<int> (std::round ((combLengths[j] + spread) * rate / 44100.0))));
        for (size_t j = 0; j < allpassLengths.size(); ++j)
            s.allpasses[ch][j].line.prepare (std::max (1, static_cast<int> (std::round ((allpassLengths[j] + spread) * rate / 44100.0))));
    }
    s.pre.prepare (static_cast<int> (std::ceil (0.2 * rate)) + 2);
}

void ReverbEffect::reset() noexcept
{
    auto& s = *state;
    s.controls.reset (*this);
    s.pre.reset();
    s.preSamples = getParam (predelay) * 0.001 * s.rate;
    for (auto& channel : s.combs)
        for (auto& comb : channel)
        {
            comb.line.reset();
            comb.lowpass = 0.0;
        }
    for (auto& channel : s.allpasses)
        for (auto& allpass : channel)
            allpass.line.reset();
}

void ReverbEffect::processEffect (juce::AudioBuffer<float>& buffer, int count, const Transport::Block&) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    auto& s = *state;
    s.controls.target (*this);
    auto* l = buffer.getWritePointer (0);
    auto* r = buffer.getWritePointer (1);
    for (int i = 0; i < count; ++i)
    {
        const auto p = s.controls.next();
        const auto input = (static_cast<double> (l[i]) + r[i]) * 0.015;
        s.preSamples = glide (s.preSamples, p[predelay] * 0.001 * s.rate);
        // The direct tap permits an exact zero pre-delay; sub-sample times blend with it.
        const auto delayed = s.preSamples < 1.0 ? input + s.preSamples * (s.pre.read (1.0) - input)
                                               : s.pre.read (s.preSamples);
        s.pre.write (input);
        std::array<double, 2> wet {};
        for (size_t ch = 0; ch < 2; ++ch)
        {
            for (auto& comb : s.combs[ch])
                wet[ch] += comb.process (delayed, 0.7 + 0.27 * p[size], 0.4 * p[damping]);
            for (auto& allpass : s.allpasses[ch])
                wet[ch] = allpass.process (wet[ch]);
        }
        const auto same = 0.5 * (1.0 + p[width]), cross = 0.5 * (1.0 - p[width]);
        l[i] = static_cast<float> (l[i] + p[mix] * (wet[0] * same + wet[1] * cross - l[i]));
        r[i] = static_cast<float> (r[i] + p[mix] * (wet[1] * same + wet[0] * cross - r[i]));
    }
}

struct DelayEffect::State
{
    Controls<numParams> controls;
    std::array<DelayLine, 2> lines;
    std::array<double, 2> previousInput {}, highpass {}, lowpass {};
    double rate = 48000.0, delaySamples = 1.0;
    double hpCoefficient = 0.0, lpCoefficient = 0.0;
    juce::SmoothedValue<double> pingAmount;
    int phase = 0;
    bool firstBlock = true;
};

DelayEffect::DelayEffect() : state (std::make_unique<State>()) { initialiseParams(); }
DelayEffect::~DelayEffect() = default;

const std::vector<ParamInfo>& DelayEffect::infos()
{
    static const std::vector<ParamInfo> result {
        { "sync", "Sync", 0, 1, 1, 0.5f, true, "" },
        { "syncDivision", "Division", 0, 6, 3, 3, true, "" },
        { "timeMs", "Time", 1, 2000, 375, 44.7214f, false, "ms" },
        { "feedback", "Feedback", 0, 0.95f, 0.35f, 0.475f, false, "%" },
        { "pingPong", "Ping Pong", 0, 1, 0, 0.5f, true, "" },
        { "lowCut", "Low Cut", 20, 2000, 80, 200, false, "Hz" },
        { "highCut", "High Cut", 1000, 20000, 8000, 4472.136f, false, "Hz" },
        { "mix", "Mix", 0, 1, 0.3f, 0.5f, false, "%" }
    };
    return result;
}

void DelayEffect::prepareEffect (double rate, int)
{
    auto& s = *state;
    s.rate = rate;
    s.controls.prepare (rate);
    s.pingAmount.reset (rate, 0.02);
    for (auto& line : s.lines)
        line.prepare (static_cast<int> (std::ceil (4.0 * rate)) + 2);
}

void DelayEffect::reset() noexcept
{
    auto& s = *state;
    s.controls.reset (*this);
    s.pingAmount.setCurrentAndTargetValue (getParam (pingPong));
    for (auto& line : s.lines)
        line.reset();
    s.previousInput.fill (0.0);
    s.highpass.fill (0.0);
    s.lowpass.fill (0.0);
    s.firstBlock = true;
    s.phase = 0;
}

void DelayEffect::processEffect (juce::AudioBuffer<float>& buffer, int count, const Transport::Block& block) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    auto& s = *state;
    s.controls.target (*this);
    // Read stepped routing/time selectors once, separately from continuous smoothing.
    const auto synced = s.controls.values[sync].getTargetValue() >= 0.5;
    const auto division = static_cast<int> (s.controls.values[syncDivision].getTargetValue());
    s.pingAmount.setTargetValue (s.controls.values[pingPong].getTargetValue());
    const auto bpm = std::isfinite (block.bpm) && block.bpm > 0.0 ? block.bpm : 120.0;
    const auto syncSeconds = divisionInBeats (division, block.beatsPerBar) * 60.0 / bpm;
    auto* l = buffer.getWritePointer (0);
    auto* r = buffer.getWritePointer (1);
    for (int i = 0; i < count; ++i)
    {
        const auto p = s.controls.next();
        const auto time = juce::jlimit (1.0, 4.0 * s.rate, (synced ? syncSeconds : p[timeMs] * 0.001) * s.rate);
        s.delaySamples = s.firstBlock ? time : glide (s.delaySamples, time);
        s.firstBlock = false;
        if (s.phase == 0)
        {
            s.hpCoefficient = std::exp (-twoPi * p[lowCut] / s.rate);
            s.lpCoefficient = std::exp (-twoPi * std::min (p[highCut], s.rate * 0.45) / s.rate);
        }
        s.phase = (s.phase + 1) % 32;
        const std::array<double, 2> drySamples { l[i], r[i] };
        const std::array<double, 2> wet { s.lines[0].read (s.delaySamples), s.lines[1].read (s.delaySamples) };
        const auto ping = s.pingAmount.getNextValue();
        for (size_t ch = 0; ch < 2; ++ch)
        {
            const auto input = wet[ch] + ping * (wet[1 - ch] - wet[ch]);
            s.highpass[ch] = 0.5 * (1.0 + s.hpCoefficient) * (input - s.previousInput[ch])
                             + s.hpCoefficient * s.highpass[ch];
            s.previousInput[ch] = input;
            s.lowpass[ch] = (1.0 - s.lpCoefficient) * s.highpass[ch] + s.lpCoefficient * s.lowpass[ch];
            s.lines[ch].write (drySamples[ch] + p[feedback] * s.lowpass[ch]);
        }
        l[i] = static_cast<float> (drySamples[0] + p[mix] * (wet[0] - drySamples[0]));
        r[i] = static_cast<float> (drySamples[1] + p[mix] * (wet[1] - drySamples[1]));
    }
}

double DelayEffect::divisionInBeats (int division, int beatsPerBar) noexcept
{
    switch (division)
    {
        case 0: return 0.25;
        case 1: return 0.5;
        case 2: return 0.75;
        case 3: return 1.0;
        case 4: return 1.5;
        case 5: return 2.0;
        default: return (double) beatsPerBar;
    }
}

} // namespace daw
