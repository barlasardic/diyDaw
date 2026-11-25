#include "Synth.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace daw
{

const std::array<Synth::ParamInfo, Synth::numParams>& Synth::getParamInfos()
{
    static const std::array<ParamInfo, numParams> infos {{
        { "osc1Wave", "Oscillator 1 wave", 0, 3, 0, 1.5f, true, "" },
        { "osc2Wave", "Oscillator 2 wave", 0, 3, 0, 1.5f, true, "" },
        { "osc2Semitones", "Oscillator 2 transpose", -24, 24, 0, 0, true, "st" },
        { "osc2Detune", "Oscillator 2 detune", -50, 50, 7, 0, false, "ct" },
        { "oscMix", "Oscillator mix", 0, 1, 0.5f, 0.5f, false, "%" },
        { "filterCutoff", "Filter cutoff", 20, 20000, 2000, 1000, false, "Hz" },
        { "filterResonance", "Filter resonance", 0, 1, 0.2f, 0.5f, false, "%" },
        { "filterEnvAmount", "Filter envelope amount", -1, 1, 0.25f, 0, false, "" },
        { "filterKeyTrack", "Filter key tracking", 0, 1, 0.5f, 0.5f, false, "%" },
        { "filterAttack", "Filter attack", 0.001f, 10, 0.005f, 0.5f, false, "s" },
        { "filterDecay", "Filter decay", 0.001f, 10, 0.3f, 0.5f, false, "s" },
        { "filterSustain", "Filter sustain", 0, 1, 0.5f, 0.5f, false, "%" },
        { "filterRelease", "Filter release", 0.001f, 10, 0.3f, 0.5f, false, "s" },
        { "ampAttack", "Amplitude attack", 0.001f, 10, 0.005f, 0.5f, false, "s" },
        { "ampDecay", "Amplitude decay", 0.001f, 10, 0.3f, 0.5f, false, "s" },
        { "ampSustain", "Amplitude sustain", 0, 1, 0.7f, 0.5f, false, "%" },
        { "ampRelease", "Amplitude release", 0.001f, 10, 0.3f, 0.5f, false, "s" },
        { "velocitySens", "Velocity sensitivity", 0, 1, 0.8f, 0.5f, false, "%" },
        { "volume", "Volume", -60, 6, 0, 0, false, "dB" }
    }};
    return infos;
}

namespace
{
using Values = std::array<float, Synth::numParams>;
constexpr double pi = juce::MathConstants<double>::pi;

// Audio thread only. Linear timed segments; attack/release always start at the current level.
struct Envelope
{
    enum Stage { idle, attack, decay, sustain, release } stage = idle;
    double level = 0.0, step = 0.0, target = 0.0;
    int remaining = 0;

    void ramp (Stage next, double end, float seconds, double rate) noexcept
    {
        stage = next;
        target = end;
        remaining = std::max (1, static_cast<int> (std::ceil (rate * seconds)));
        step = (target - level) / remaining;
    }

    double tick (float decayTime, float sustainLevel, double rate) noexcept
    {
        if (stage == idle)
            return 0.0;
        if (stage == sustain)
            return level;

        level += step;
        if (--remaining == 0)
        {
            level = target;
            if (stage == attack)
                ramp (decay, sustainLevel, decayTime, rate);
            else if (stage == decay)
                stage = sustain;
            else
                stage = idle;
        }
        return level;
    }
};

double polyBlep (double phase, double delta) noexcept
{
    if (phase < delta)
    {
        const auto t = phase / delta;
        return 2.0 * t - t * t - 1.0;
    }
    if (phase > 1.0 - delta)
    {
        const auto t = (phase - 1.0) / delta;
        return t * t + 2.0 * t + 1.0;
    }
    return 0.0;
}

// Integral of PolyBLEP: rounds the triangle's slope discontinuities without an integrator's DC drift.
double polyBlamp (double phase, double delta) noexcept
{
    if (phase < delta)
        return delta / 3.0 * std::pow (1.0 - phase / delta, 3.0);
    if (phase > 1.0 - delta)
        return delta / 3.0 * std::pow (1.0 + (phase - 1.0) / delta, 3.0);
    return 0.0;
}

double oscillator (double& phase, double delta, int wave) noexcept
{
    const auto opposite = phase < 0.5 ? phase + 0.5 : phase - 0.5;
    double value = 0.0;
    switch (wave)
    {
        case 0: value = 2.0 * phase - 1.0 - polyBlep (phase, delta); break;
        case 1: value = (phase < 0.5 ? 1.0 : -1.0)
                       + polyBlep (phase, delta) - polyBlep (opposite, delta); break;
        case 2: value = 1.0 - 4.0 * std::abs (phase - 0.5)
                       + 4.0 * (polyBlamp (phase, delta) - polyBlamp (opposite, delta)); break;
        case 3: value = std::sin (2.0 * pi * phase); break;
        default: break;
    }
    phase += delta;
    phase -= std::floor (phase);
    return value;
}
}

// All voice state is owned by the audio thread. No dynamically sized render storage.
struct Synth::Voice
{
    struct Sound
    {
        Envelope amp, filter;
        int pitch = 60;
        double phase1 = 0.0, phase2 = 0.0;
        juce::SmoothedValue<double> velocity { 1.0 };
        double ic1 = 0.0, ic2 = 0.0;
        std::uint64_t age = 0, releaseAge = 0;

        bool active() const noexcept { return amp.stage != Envelope::idle; }

        void start (int note, float strength, std::uint64_t order, const Values& p,
                    double rate, bool retrigger) noexcept
        {
            if (! retrigger)
            {
                *this = {};
                // Independent, reproducible phases keep stacked notes from adding coherently.
                phase1 = std::fmod (0.173 + static_cast<double> (order % 1024) * 0.61803398875, 1.0);
                phase2 = std::fmod (phase1 + 0.237, 1.0);
                velocity.reset (rate, 0.004);
                velocity.setCurrentAndTargetValue (strength);
            }
            pitch = note;
            age = order;
            releaseAge = 0;
            // Retain phase/filter state and ramp velocity when retriggering a sounding pitch.
            velocity.setTargetValue (strength);
            amp.ramp (Envelope::attack, 1.0, p[ampAttack], rate);
            filter.ramp (Envelope::attack, 1.0, p[filterAttack], rate);
        }

        void release (std::uint64_t order, const Values& p, double rate) noexcept
        {
            if (! active() || amp.stage == Envelope::release)
                return;
            releaseAge = order;
            amp.ramp (Envelope::release, 0.0, p[ampRelease], rate);
            filter.ramp (Envelope::release, 0.0, p[filterRelease], rate);
        }

        double tick (const Values& p, double rate, double cutoff, double mix) noexcept
        {
            if (! active())
                return 0.0;
            const auto frequency = 440.0 * std::exp2 ((pitch - 69) / 12.0);
            const auto frequency2 = frequency * std::exp2 ((p[osc2Semitones] + p[osc2Detune] / 100.0) / 12.0);
            const auto a = oscillator (phase1, std::min (0.45, frequency / rate), static_cast<int> (p[osc1Wave]));
            const auto b = oscillator (phase2, std::min (0.45, frequency2 / rate), static_cast<int> (p[osc2Wave]));
            const auto env = filter.tick (p[filterDecay], p[filterSustain], rate);
            const auto hz = std::clamp (cutoff * std::exp2 (p[filterEnvAmount] * 6.0 * env
                                                + p[filterKeyTrack] * (pitch - 60) / 12.0), 20.0, 0.45 * rate);
            // TPT/ZDF two-pole SVF. Positive damping (Q <= 20) at every resonance setting.
            const auto g = std::tan (pi * hz / rate);
            const auto k = 2.0 - 1.95 * p[filterResonance];
            const auto v3 = a + mix * (b - a) - ic2;
            const auto v1 = (ic1 + g * v3) / (1.0 + g * (g + k));
            const auto v2 = ic2 + g * v1;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            const auto amplitude = amp.tick (p[ampDecay], p[ampSustain], rate);
            // Gentle analogue-style saturation bounds resonant peaks; 0.4 leaves polyphonic headroom.
            return 0.4 * std::tanh (v2) * amplitude
                   * (1.0 - p[velocitySens] + p[velocitySens] * velocity.getNextValue());
        }
    } sound, tail;

    int fadeRemaining = 0;
    double lastTail = 0.0, residual = 0.0;
};

struct Synth::Impl
{
    std::array<Voice, numVoices> voices;
    double sampleRate = 44100.0;
    int fadeSamples = 177;
    std::uint64_t order = 0;
    std::atomic<int> activeVoices { 0 };
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> cutoff;
    juce::SmoothedValue<double> gain, mix;

    void event (const NoteEvent& e, const Values& p) noexcept
    {
        const auto pitch = std::clamp (e.pitch, 0, 127);
        if (! e.isNoteOn || e.velocity <= 0.0f)
        {
            for (auto& v : voices)
                if (v.sound.pitch == pitch)
                    v.sound.release (++order, p, sampleRate);
            return;
        }

        Voice* chosen = nullptr;
        for (auto& v : voices)
            if (v.sound.active() && v.sound.pitch == pitch)
            {
                chosen = &v;
                break;
            }
        if (chosen != nullptr)
        {
            chosen->sound.start (pitch, std::clamp (e.velocity, 0.0f, 1.0f), ++order, p, sampleRate, true);
            return;
        }
        for (auto& v : voices)
            if (! v.sound.active())
            {
                chosen = &v;
                break;
            }
        if (chosen == nullptr)
        {
            chosen = &voices.front();
            for (auto& v : voices)
            {
                const bool released = v.sound.amp.stage == Envelope::release;
                const bool chosenReleased = chosen->sound.amp.stage == Envelope::release;
                if ((released && ! chosenReleased)
                    || (released == chosenReleased
                        && (released ? v.sound.releaseAge < chosen->sound.releaseAge
                                     : v.sound.age < chosen->sound.age)))
                    chosen = &v;
            }
            // Keep an actual fading DSP copy. Repeated steals within 4 ms retain the previous
            // tail's last contribution as a decaying residual, so fixed storage cannot overflow.
            chosen->residual = chosen->lastTail;
            chosen->tail = chosen->sound;
            chosen->fadeRemaining = fadeSamples;
        }
        chosen->sound.start (pitch, std::clamp (e.velocity, 0.0f, 1.0f), ++order, p, sampleRate, false);
    }
};

Synth::Synth() : impl (std::make_unique<Impl>())
{
    for (int i = 0; i < numParams; ++i)
        params[static_cast<size_t> (i)].store (getParamInfos()[static_cast<size_t> (i)].defaultValue);
    prepare (44100.0, 512);
}

Synth::~Synth() = default;

void Synth::setParam (int index, float value) noexcept
{
    if (index < 0 || index >= numParams || std::isnan (value))
        return;
    const auto& info = getParamInfos()[static_cast<size_t> (index)];
    value = std::clamp (value, info.minValue, info.maxValue);
    params[static_cast<size_t> (index)].store (info.stepped ? std::round (value) : value, std::memory_order_relaxed);
}

float Synth::getParam (int index) const noexcept
{
    return index >= 0 && index < numParams ? params[static_cast<size_t> (index)].load (std::memory_order_relaxed) : 0.0f;
}

void Synth::prepare (double sampleRate, int maxBlockSize)
{
    juce::ignoreUnused (maxBlockSize); // Rendering is scalar and needs no block-sized scratch buffer.
    impl->sampleRate = std::isfinite (sampleRate) && sampleRate >= 1000.0 ? sampleRate : 44100.0;
    impl->fadeSamples = static_cast<int> (std::ceil (0.004 * impl->sampleRate));
    impl->cutoff.reset (impl->sampleRate, 0.02);
    impl->gain.reset (impl->sampleRate, 0.02);
    impl->mix.reset (impl->sampleRate, 0.02);
    reset();
}

void Synth::process (juce::AudioBuffer<float>& out, int numSamples,
                     const NoteEvent* events, int numEvents) noexcept
{
    const juce::ScopedNoDenormals noDenormals;
    Values p;
    for (int i = 0; i < numParams; ++i)
        p[static_cast<size_t> (i)] = getParam (i);
    impl->cutoff.setTargetValue (p[filterCutoff]);
    impl->gain.setTargetValue (std::pow (10.0, p[volume] / 20.0));
    impl->mix.setTargetValue (p[oscMix]);
    const auto count = std::clamp (numSamples, 0, out.getNumSamples());
    auto* left = out.getNumChannels() > 0 ? out.getWritePointer (0) : nullptr;
    auto* right = out.getNumChannels() > 1 ? out.getWritePointer (1) : nullptr;
    int position = 0;
    const auto renderTo = [&] (int end)
    {
        for (; position < end; ++position)
        {
            const auto cutoff = impl->cutoff.getNextValue();
            const auto gain = impl->gain.getNextValue();
            const auto mix = impl->mix.getNextValue();
            double sum = 0.0;
            for (auto& v : impl->voices)
            {
                sum += v.sound.tick (p, impl->sampleRate, cutoff, mix);
                v.lastTail = 0.0;
                if (v.fadeRemaining > 0)
                {
                    const auto fade = static_cast<double> (v.fadeRemaining) / impl->fadeSamples;
                    v.lastTail = fade * (v.tail.tick (p, impl->sampleRate, cutoff, mix) + v.residual);
                    sum += v.lastTail;
                    if (--v.fadeRemaining == 0)
                    {
                        v.tail = {};
                        v.residual = 0.0;
                    }
                }
            }
            const auto sample = static_cast<float> (sum * gain);
            if (left != nullptr) left[position] += sample;
            if (right != nullptr) right[position] += sample;
        }
    };
    if (events != nullptr)
        for (int i = 0; i < numEvents; ++i)
        {
            renderTo (std::clamp (events[i].sampleOffset, position, count));
            impl->event (events[i], p);
        }
    renderTo (count);
    int active = 0;
    for (const auto& v : impl->voices)
        if (v.sound.active() || v.fadeRemaining > 0)
            ++active;
    impl->activeVoices.store (active, std::memory_order_relaxed);
}

void Synth::allNotesOff() noexcept
{
    Values p;
    for (int i = 0; i < numParams; ++i)
        p[static_cast<size_t> (i)] = getParam (i);
    for (auto& v : impl->voices)
    {
        v.sound.release (++impl->order, p, impl->sampleRate);
        v.tail.release (++impl->order, p, impl->sampleRate);
    }
}

void Synth::reset() noexcept
{
    impl->voices = {};
    impl->order = 0;
    impl->activeVoices.store (0, std::memory_order_relaxed);
    impl->cutoff.setCurrentAndTargetValue (getParam (filterCutoff));
    impl->gain.setCurrentAndTargetValue (std::pow (10.0, getParam (volume) / 20.0));
    impl->mix.setCurrentAndTargetValue (getParam (oscMix));
}

int Synth::getNumActiveVoices() const noexcept
{
    return impl->activeVoices.load (std::memory_order_relaxed);
}

} // namespace daw
