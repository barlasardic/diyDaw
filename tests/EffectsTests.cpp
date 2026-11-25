#include "Effects.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace
{
constexpr double rate = 48000.0;
constexpr double twoPi = juce::MathConstants<double>::twoPi;
using Audio = juce::AudioBuffer<float>;
using EQ = daw::EqEffect;
using Compressor = daw::CompressorEffect;
using Reverb = daw::ReverbEffect;
using Delay = daw::DelayEffect;

Audio signal (int length, double frequency = 0.0, float amplitude = 0.0f, double sampleRate = rate)
{
    Audio result (2, length);
    result.clear();
    for (int i = 0; i < length; ++i)
        for (int ch = 0; ch < 2; ++ch)
            result.setSample (ch, i, amplitude * static_cast<float> (std::sin (twoPi * frequency * i / sampleRate)));
    return result;
}

Audio impulse (int length)
{
    auto result = signal (length);
    result.setSample (0, 0, 1.0f);
    return result;
}

Audio render (daw::AudioEffect& effect, const Audio& input, int blockSize = 512,
              double sampleRate = rate, double bpm = 120.0, int beatsPerBar = 4)
{
    Audio result (2, input.getNumSamples()), scratch (2, blockSize);
    daw::Transport::Block block;
    block.playing = true;
    block.sampleRate = sampleRate;
    block.bpm = bpm;
    block.beatsPerBar = beatsPerBar;
    block.beatsPerSample = bpm / (60.0 * sampleRate);
    for (int start = 0; start < input.getNumSamples(); start += blockSize)
    {
        const auto count = std::min (blockSize, input.getNumSamples() - start);
        block.startBeat = start * block.beatsPerSample;
        block.numSamples = count;
        for (int ch = 0; ch < 2; ++ch)
            scratch.copyFrom (ch, 0, input, ch, start, count);
        effect.process (scratch, count, block);
        for (int ch = 0; ch < 2; ++ch)
            result.copyFrom (ch, start, scratch, ch, 0, count);
    }
    return result;
}

double difference (const Audio& a, const Audio& b)
{
    double result = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            result = std::max (result, std::abs (static_cast<double> (a.getSample (ch, i)) - b.getSample (ch, i)));
    return result;
}

double rms (const Audio& audio, int start, int length, int channel = 0)
{
    double energy = 0.0;
    for (int i = start; i < start + length; ++i)
    {
        const auto value = static_cast<double> (audio.getSample (channel, i));
        energy += value * value;
    }
    return std::sqrt (energy / length);
}

bool finite (const Audio& audio, double bound = 1000.0)
{
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            if (! std::isfinite (audio.getSample (ch, i)) || std::abs (audio.getSample (ch, i)) > bound)
                return false;
    return true;
}

void configureDelay (Delay& effect, bool ping = false)
{
    effect.setParam (Delay::mix, 1);
    effect.setParam (Delay::feedback, 0.5f);
    effect.setParam (Delay::lowCut, 20);
    effect.setParam (Delay::highCut, 20000);
    effect.setParam (Delay::pingPong, ping ? 1.0f : 0.0f);
}
} // namespace

struct EffectsTests : juce::UnitTest
{
    EffectsTests() : juce::UnitTest ("Effects", "diyDaw") {}

    void runTest() override
    {
        testInfos();
        testEq();
        testCompressor();
        testReverb();
        testDelay();
        testAutomationAndBypass();
        testBlockSizes();
        testExtremesAndReset();
    }

    void testInfos()
    {
        beginTest ("Parameter metadata, defaults, bounds and stepped selectors");
        const std::array<int, 4> counts { EQ::numParams, Compressor::numParams, Reverb::numParams, Delay::numParams };
        const std::array<std::vector<juce::String>, 4> ids {{
            { "lowFreq", "lowGain", "midFreq", "midGain", "midQ", "highFreq", "highGain", "output" },
            { "threshold", "ratio", "attack", "release", "knee", "makeup", "mix" },
            { "size", "damping", "predelay", "width", "mix" },
            { "sync", "syncDivision", "timeMs", "feedback", "pingPong", "lowCut", "highCut", "mix" }
        }};
        int typeIndex = 0;
        for (const auto& type : daw::getEffectTypes())
        {
            auto effect = daw::createEffect (type);
            expect (effect != nullptr);
            const auto& infos = effect->getParamInfos();
            expectEquals (static_cast<int> (infos.size()), counts[static_cast<size_t> (typeIndex)]);
            std::set<juce::String> unique;
            for (size_t i = 0; i < infos.size(); ++i)
            {
                const auto& p = infos[i];
                expectEquals (juce::String (p.id), ids[static_cast<size_t> (typeIndex)][i]);
                expect (unique.insert (p.id).second);
                expect (juce::String (p.name).isNotEmpty() && juce::String (p.name).length() <= 10);
                expect (p.minValue < p.maxValue && p.defaultValue >= p.minValue && p.defaultValue <= p.maxValue);
                expect (p.centreValue > p.minValue && p.centreValue < p.maxValue);
                expect (p.unit != nullptr);
                if (juce::String (p.unit) == "%")
                {
                    expectEquals (p.minValue, 0.0f);
                    expect (p.maxValue <= 1.0f);
                }
                const auto index = static_cast<int> (i);
                expectEquals (effect->getParam (index), p.defaultValue);
                effect->setParam (index, p.minValue - 1000);
                expectEquals (effect->getParam (index), p.minValue);
                effect->setParam (index, p.maxValue + 1000);
                expectEquals (effect->getParam (index), p.maxValue);
                if (p.stepped)
                {
                    effect->setParam (index, 0.6f);
                    expectEquals (effect->getParam (index), 1.0f);
                }
            }
            ++typeIndex;
        }
        const std::array<double, 7> divisions { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 7.0 };
        for (size_t i = 0; i < divisions.size(); ++i)
            expectEquals (Delay::divisionInBeats (static_cast<int> (i), 7), divisions[i]);
    }

    void testEq()
    {
        beginTest ("EQ is transparent with every band at zero dB");
        EQ effect;
        effect.prepare (rate, 512);
        auto input = signal (48000, 997, 0.7f);
        input.setSample (1, 0, 1.0f);
        expectWithinAbsoluteError (difference (render (effect, input), input), 0.0, 1.0e-6);

        beginTest ("A twelve dB mid peak boosts 1 kHz and barely affects 100 Hz");
        const auto response = [] (double frequency, int parameter, float value, float lowFrequency = 100)
        {
            EQ eq;
            eq.setParam (parameter, value);
            eq.setParam (EQ::lowFreq, lowFrequency);
            eq.prepare (rate, 512);
            const auto dry = signal (48000, frequency, 0.1f);
            const auto wet = render (eq, dry);
            return 20.0 * std::log10 (rms (wet, 24000, 24000) / rms (dry, 24000, 24000));
        };
        expectWithinAbsoluteError (response (1000, EQ::midGain, 12), 12.0, 0.05);
        expect (std::abs (response (100, EQ::midGain, 12)) < 0.5);

        beginTest ("Low and high shelves provide the requested cut and boost");
        expect (response (50, EQ::lowGain, -18, 1000) < -17.5);
        expect (response (18000, EQ::highGain, 12) > 11.5);
    }

    void testCompressor()
    {
        beginTest ("Signals below threshold pass unchanged with zero makeup");
        Compressor effect;
        effect.prepare (rate, 512);
        const auto quiet = signal (48000, 1000, 0.03f);
        expectWithinAbsoluteError (difference (quiet, render (effect, quiet)), 0.0, 1.0e-7);
        expectEquals (effect.getGainReductionDb(), 0.0f);

        beginTest ("Settled loud sine reduction and meter follow the ratio");
        effect.setParam (Compressor::attack, 0.1f);
        effect.setParam (Compressor::release, 1000);
        effect.setParam (Compressor::knee, 0);
        effect.reset();
        const auto loud = signal (96000, 1000, 0.8f);
        const auto compressed = render (effect, loud);
        const auto expected = (20.0 * std::log10 (0.8) + 18.0) * 0.75;
        const auto actual = -20.0 * std::log10 (rms (compressed, 48000, 48000) / rms (loud, 48000, 48000));
        expectWithinAbsoluteError (actual, expected, 0.25);
        expectWithinAbsoluteError (static_cast<double> (effect.getGainReductionDb()), actual, 0.05);

        beginTest ("Quadratic knee, makeup and stereo linking agree with the gain computer");
        for (const auto over : { -4.0, 0.0, 4.0, 12.0 })
        {
            Compressor knee;
            knee.setParam (Compressor::attack, 0.1f);
            knee.setParam (Compressor::threshold, -18);
            knee.setParam (Compressor::knee, 8);
            knee.setParam (Compressor::makeup, 3);
            knee.prepare (rate, 512);
            auto dc = signal (6000);
            const auto level = static_cast<float> (std::pow (10.0, (-18.0 + over) / 20.0));
            for (int i = 0; i < dc.getNumSamples(); ++i)
            {
                dc.setSample (0, i, level);
                dc.setSample (1, i, level * 0.1f);
            }
            const auto out = render (knee, dc);
            const auto reduction = over < 4.0 ? 0.75 * std::pow (over + 4.0, 2.0) / 16.0 : 0.75 * over;
            expectWithinAbsoluteError (static_cast<double> (knee.getGainReductionDb()), reduction, 1.0e-4);
            expectWithinAbsoluteError (20.0 * std::log10 (out.getSample (0, 5999) / level), 3.0 - reduction, 1.0e-4);
            expectWithinAbsoluteError (out.getSample (1, 5999), out.getSample (0, 5999) * 0.1f, 1.0e-7f);
        }

        beginTest ("Attack and release settle over their specified time constants");
        Compressor envelope;
        envelope.setParam (Compressor::threshold, -20);
        envelope.setParam (Compressor::ratio, 2);
        envelope.setParam (Compressor::knee, 0);
        envelope.setParam (Compressor::attack, 10);
        envelope.setParam (Compressor::release, 100);
        envelope.prepare (rate, 512);
        auto dc = signal (480);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < dc.getNumSamples(); ++i)
                dc.setSample (ch, i, 1.0f);
        render (envelope, dc);
        expectWithinAbsoluteError (static_cast<double> (envelope.getGainReductionDb()), 10.0 * (1.0 - std::exp (-1.0)), 1.0e-4);
        render (envelope, signal (4800));
        expectWithinAbsoluteError (static_cast<double> (envelope.getGainReductionDb()), 10.0 * (1.0 - std::exp (-1.0)) * std::exp (-1.0), 1.0e-4);
    }

    void testReverb()
    {
        beginTest ("Reverb is exactly dry at zero mix");
        Reverb dry;
        dry.setParam (Reverb::mix, 0);
        dry.prepare (rate, 512);
        const auto input = signal (24000, 731, 0.5f);
        expectEquals (difference (render (dry, input), input), 0.0);

        beginTest ("Stereo reverb has a finite tail past 200 ms and decays to silence");
        Reverb effect;
        effect.setParam (Reverb::mix, 1);
        effect.setParam (Reverb::size, 0.4f);
        effect.prepare (rate, 512);
        const auto tail = render (effect, impulse (12 * 48000));
        expect (finite (tail, 1.0));
        expect (rms (tail, 9600, 38400) > 1.0e-5);
        expect (rms (tail, 11 * 48000, 48000) < 1.0e-7);
        expect (difference (tail, signal (tail.getNumSamples())) > 0.001);
        expect (std::abs (rms (tail, 0, 48000, 0) - rms (tail, 0, 48000, 1)) < 0.01);

        beginTest ("Zero width folds the wet tail to mono; pre-delay moves its onset exactly");
        const auto make = [] (float pre)
        {
            Reverb reverb;
            reverb.setParam (Reverb::mix, 1);
            reverb.setParam (Reverb::width, 0);
            reverb.setParam (Reverb::predelay, pre);
            reverb.prepare (rate, 512);
            return render (reverb, impulse (12000));
        };
        const auto immediate = make (0), delayed = make (100);
        double shiftError = 0.0, monoError = 0.0;
        for (int i = 0; i < 12000; ++i)
        {
            monoError = std::max (monoError, std::abs (static_cast<double> (immediate.getSample (0, i)) - immediate.getSample (1, i)));
            if (i >= 4800)
                shiftError = std::max (shiftError, std::abs (static_cast<double> (delayed.getSample (0, i)) - immediate.getSample (0, i - 4800)));
        }
        expectWithinAbsoluteError (monoError, 0.0, 1.0e-7);
        expectWithinAbsoluteError (shiftError, 0.0, 1.0e-7);
        expectEquals (delayed.getMagnitude (0, 0, 4800), 0.0f);
    }

    void testDelay()
    {
        beginTest ("Quarter-note delay at 120 bpm starts exactly at sample 24000");
        Delay effect;
        configureDelay (effect);
        effect.prepare (rate, 512);
        const auto echoes = render (effect, impulse (96001));
        expectEquals (echoes.getMagnitude (0, 0, 24000), 0.0f);
        expectEquals (echoes.getSample (0, 24000), 1.0f);
        expectEquals (echoes.getSample (0, 24001), 0.0f);
        // Feedback filtering reshapes each impulse: the leading tap is feedback times
        // the HP/LP impulse coefficient per repeat, rather than an unfiltered delta.
        const auto filterTap = 0.5 * (1.0 + std::exp (-twoPi * 20.0 / rate)) * (1.0 - std::exp (-twoPi * 20000.0 / rate));
        expectWithinAbsoluteError (static_cast<double> (echoes.getSample (0, 48000)), 0.5 * filterTap, 1.0e-6);
        expectWithinAbsoluteError (static_cast<double> (echoes.getSample (0, 72000)), 0.25 * filterTap * filterTap, 1.0e-6);
        expectEquals (echoes.getMagnitude (1, 0, 96001), 0.0f);

        beginTest ("Feedback scales each repeat and ping-pong alternates left and right");
        Delay quieter, ping;
        configureDelay (quieter);
        quieter.setParam (Delay::feedback, 0.25f);
        quieter.prepare (rate, 512);
        const auto quietEchoes = render (quieter, impulse (72001));
        expectWithinAbsoluteError (quietEchoes.getSample (0, 48000), echoes.getSample (0, 48000) * 0.5f, 1.0e-6f);
        expectWithinAbsoluteError (quietEchoes.getSample (0, 72000), echoes.getSample (0, 72000) * 0.25f, 1.0e-6f);
        configureDelay (ping, true);
        ping.prepare (rate, 512);
        const auto alternating = render (ping, impulse (72001));
        expectEquals (alternating.getSample (0, 24000), 1.0f);
        expectEquals (alternating.getSample (1, 24000), 0.0f);
        expectEquals (alternating.getSample (0, 48000), 0.0f);
        expectWithinAbsoluteError (alternating.getSample (1, 48000), echoes.getSample (0, 48000), 1.0e-6f);
        expectWithinAbsoluteError (alternating.getSample (0, 72000), echoes.getSample (0, 72000), 1.0e-6f);
        expect (std::abs (alternating.getSample (1, 72000)) < 1.0e-6f);

        beginTest ("Free fractional time interpolates and slow bar divisions clamp to four seconds");
        Delay fractional;
        fractional.setParam (Delay::sync, 0);
        fractional.setParam (Delay::timeMs, 10.01f);
        fractional.setParam (Delay::feedback, 0);
        fractional.setParam (Delay::mix, 1);
        fractional.prepare (rate, 512);
        const auto fraction = render (fractional, impulse (1000));
        expectWithinAbsoluteError (fraction.getSample (0, 480), 0.52f, 2.0e-5f);
        expectWithinAbsoluteError (fraction.getSample (0, 481), 0.48f, 2.0e-5f);
        Delay clamped;
        clamped.setParam (Delay::syncDivision, 6);
        clamped.setParam (Delay::mix, 1);
        clamped.prepare (rate, 512);
        const auto longEcho = render (clamped, impulse (192001), 512, rate, 20, 7);
        expectEquals (longEcho.getMagnitude (0, 0, 192000), 0.0f);
        expectEquals (longEcho.getSample (0, 192000), 1.0f);
    }

    void testAutomationAndBypass()
    {
        beginTest ("Free and tempo-synced delay time changes glide without a large discontinuity");
        for (const bool synced : { false, true })
        {
            Delay effect;
            effect.setParam (Delay::sync, synced ? 1.0f : 0.0f);
            effect.setParam (Delay::timeMs, 100);
            effect.setParam (Delay::mix, 1);
            effect.setParam (Delay::feedback, 0);
            effect.prepare (rate, 512);
            const auto tone = signal (48000, 200, 0.5f);
            const auto before = render (effect, tone);
            effect.setParam (Delay::timeMs, 2000);
            const auto after = render (effect, tone, 512, rate, synced ? 20 : 120);
            double step = std::abs (static_cast<double> (after.getSample (0, 0)) - before.getSample (0, 47999));
            for (int i = 1; i < after.getNumSamples(); ++i)
                step = std::max (step, std::abs (static_cast<double> (after.getSample (0, i)) - after.getSample (0, i - 1)));
            expect (step < 0.025, juce::String (step));
        }

        beginTest ("Every effect crossfades to dry in ten ms without a bypass step");
        for (const auto& type : daw::getEffectTypes())
        {
            auto effect = daw::createEffect (type);
            if (type == "eq") effect->setParam (EQ::output, -12);
            if (type == "compressor") effect->setParam (Compressor::makeup, 12);
            if (type == "reverb") effect->setParam (Reverb::mix, 1);
            if (type == "delay")
            {
                effect->setParam (Delay::mix, 1);
                // Keep the first echo outside the bypass window, so a natural repeat
                // onset cannot be mistaken for a discontinuity in the crossfade.
                effect->setParam (Delay::syncDivision, 6);
            }
            effect->prepare (rate, 512);
            auto dc = signal (48000);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < dc.getNumSamples(); ++i)
                    dc.setSample (ch, i, 0.05f);
            const auto before = render (*effect, dc);
            effect->setEnabled (false);
            const auto after = render (*effect, dc);
            expect (std::abs (after.getSample (0, 0) - before.getSample (0, 47999)) < 0.002f, type);
            float step = 0;
            for (int i = 1; i < 512; ++i)
                step = std::max (step, std::abs (after.getSample (0, i) - after.getSample (0, i - 1)));
            expect (step < 0.002f, type);
            expectWithinAbsoluteError (after.getSample (0, 480), 0.05f, 1.0e-7f, type);
            expectWithinAbsoluteError (after.getSample (0, 47999), 0.05f, 1.0e-7f, type);
            effect->setEnabled (true);
            const auto resumed = render (*effect, dc);
            expect (finite (resumed), type);
            expect (std::abs (resumed.getSample (0, 0) - 0.05f) < 0.002f, type);
        }

        beginTest ("EQ automation ramps rather than stepping the output gain");
        EQ eq;
        eq.prepare (rate, 512);
        auto dc = signal (2048);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < dc.getNumSamples(); ++i)
                dc.setSample (ch, i, 0.1f);
        render (eq, dc);
        eq.setParam (EQ::output, 18);
        const auto automated = render (eq, dc);
        expect (std::abs (automated.getSample (0, 0) - 0.1f) < 0.001f);
        expectWithinAbsoluteError (automated.getSample (0, 2047), 0.7943282f, 1.0e-6f);
    }

    void testBlockSizes()
    {
        beginTest ("DSP and parameter smoothing are independent of blocks of 1, 37 and 512 samples");
        auto input = signal (54000, 337, 0.2f);
        input.setSample (0, 0, 1);
        input.setSample (1, 137, -0.7f);
        for (const auto& type : daw::getEffectTypes())
        {
            const auto make = [&] (int blockSize)
            {
                auto effect = daw::createEffect (type);
                effect->prepare (rate, blockSize);
                if (type == "eq")
                {
                    effect->setParam (EQ::midGain, 12);
                    effect->setParam (EQ::lowFreq, 700);
                    effect->setParam (EQ::highGain, -9);
                }
                if (type == "compressor")
                {
                    effect->setParam (Compressor::threshold, -30);
                    effect->setParam (Compressor::attack, 0.3f);
                }
                if (type == "reverb")
                {
                    effect->setParam (Reverb::predelay, 125);
                    effect->setParam (Reverb::damping, 0.9f);
                    effect->setParam (Reverb::mix, 0.8f);
                }
                if (type == "delay")
                {
                    effect->setParam (Delay::feedback, 0.7f);
                    effect->setParam (Delay::lowCut, 1000);
                    effect->setParam (Delay::pingPong, 1);
                    effect->setParam (Delay::mix, 0.8f);
                }
                const auto first = render (*effect, input, blockSize);
                if (type == "delay") effect->setParam (Delay::syncDivision, 0);
                if (type == "eq") effect->setParam (EQ::midGain, -18);
                const auto second = render (*effect, input, blockSize);
                Audio combined (2, 2 * input.getNumSamples());
                for (int ch = 0; ch < 2; ++ch)
                {
                    combined.copyFrom (ch, 0, first, ch, 0, first.getNumSamples());
                    combined.copyFrom (ch, first.getNumSamples(), second, ch, 0, second.getNumSamples());
                }
                return combined;
            };
            const auto reference = make (512);
            for (const int blockSize : { 1, 37 })
                expectWithinAbsoluteError (difference (reference, make (blockSize)), 0.0, 1.0e-5,
                                           type + " block " + juce::String (blockSize));
        }
    }

    void testExtremesAndReset()
    {
        beginTest ("Extreme parameters remain finite from 44.1 to 192 kHz; reset clears all history");
        for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (const auto& type : daw::getEffectTypes())
                for (int pattern = 0; pattern < 4; ++pattern)
                {
                    auto effect = daw::createEffect (type);
                    const auto& infos = effect->getParamInfos();
                    for (size_t i = 0; i < infos.size(); ++i)
                        effect->setParam (static_cast<int> (i), ((static_cast<int> (i) & pattern) == 0) ? infos[i].maxValue : infos[i].minValue);
                    effect->prepare (sampleRate, 512);
                    const auto input = signal (static_cast<int> (sampleRate / 2), 997, 0.5f, sampleRate);
                    expect (finite (render (*effect, input, 512, sampleRate)), type + " at " + juce::String (sampleRate));
                    // Exercise coefficient changes with existing history, not just a cold start.
                    for (size_t i = 0; i < infos.size(); ++i)
                        effect->setParam (static_cast<int> (i), infos[i].minValue);
                    expect (finite (render (*effect, input, 512, sampleRate)), type);
                    effect->reset();
                    const auto silence = render (*effect, signal (2048), 512, sampleRate);
                    expectEquals (silence.getMagnitude (0, 0, 2048), 0.0f, type);
                    expectEquals (silence.getMagnitude (1, 0, 2048), 0.0f, type);
                }
    }
};

static EffectsTests effectsTests;
