#include "Synth.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
using Synth = daw::Synth;
using Event = daw::NoteEvent;

// Test helpers run on the test thread; all buffers/event vectors are built outside process().
void configureSine (Synth& synth, float mix = 0.0f)
{
    synth.setParam (Synth::osc1Wave, 3);
    synth.setParam (Synth::osc2Wave, 3);
    synth.setParam (Synth::oscMix, mix);
    synth.setParam (Synth::filterCutoff, 20000);
    synth.setParam (Synth::filterResonance, 0);
    synth.setParam (Synth::filterEnvAmount, 0);
    synth.setParam (Synth::filterKeyTrack, 0);
    synth.setParam (Synth::ampAttack, 0.001f);
    synth.setParam (Synth::ampDecay, 0.001f);
    synth.setParam (Synth::ampSustain, 1);
    synth.setParam (Synth::ampRelease, 0.02f);
}

juce::AudioBuffer<float> render (Synth& synth, int samples, const std::vector<Event>& events = {},
                               int blockSize = 512)
{
    juce::AudioBuffer<float> result (2, samples);
    result.clear();
    juce::AudioBuffer<float> block (2, blockSize);
    std::vector<Event> local;
    local.reserve (events.size());
    size_t next = 0;
    for (int start = 0; start < samples; start += blockSize)
    {
        const auto count = std::min (blockSize, samples - start);
        local.clear();
        while (next < events.size() && events[next].sampleOffset < start + count)
        {
            auto e = events[next++];
            e.sampleOffset -= start;
            local.push_back (e);
        }
        block.clear();
        synth.process (block, count, local.data(), static_cast<int> (local.size()));
        for (int channel = 0; channel < 2; ++channel)
            result.copyFrom (channel, start, block, channel, 0, count);
    }
    return result;
}

double estimateFrequency (const juce::AudioBuffer<float>& audio, double rate)
{
    double first = 0.0, last = 0.0;
    int crossings = 0;
    for (int i = 4001; i < audio.getNumSamples(); ++i)
    {
        const auto a = audio.getSample (0, i - 1);
        const auto b = audio.getSample (0, i);
        if (a <= 0.0f && b > 0.0f)
        {
            const auto position = i - 1.0 + static_cast<double> (-a) / (b - a);
            if (crossings++ == 0)
                first = position;
            last = position;
        }
    }
    return crossings > 1 ? (crossings - 1) * rate / (last - first) : 0.0;
}

double maxDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double error = 0.0;
    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < a.getNumSamples(); ++i)
            error = std::max (error, std::abs (static_cast<double> (a.getSample (channel, i)) - b.getSample (channel, i)));
    return error;
}

// Hann-windowed Fourier measurements at the saw's harmonics above 5 kHz.
double highHarmonicEnergy (const juce::AudioBuffer<float>& audio)
{
    constexpr int length = 8192;
    constexpr double fundamental = 440.0 * 0.5946035575013605; // C4
    double energy = 0.0;
    for (int harmonic = 20; harmonic * fundamental < 22000.0; ++harmonic)
    {
        double real = 0.0, imaginary = 0.0;
        for (int i = 0; i < length; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * harmonic * fundamental * i / sampleRate;
            const auto window = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (length - 1));
            const auto value = audio.getSample (0, audio.getNumSamples() - length + i) * window;
            real += value * std::cos (phase);
            imaginary += value * std::sin (phase);
        }
        energy += real * real + imaginary * imaginary;
    }
    return energy;
}
}

struct SynthTests : juce::UnitTest
{
    SynthTests() : juce::UnitTest ("Synth", "diyDaw") {}

    void runTest() override
    {
        testParameters();
        testTimingAndPitch();
        testReleaseAndReset();
        testBlockSizes();
        testStealingAndRetrigger();
        testFilterAndExtremes();
        testAutomation();
    }

    void testParameters()
    {
        beginTest ("Parameter metadata, defaults, clamping and integer steps agree");
        Synth synth;
        const auto& infos = Synth::getParamInfos();
        const std::array<const char*, Synth::numParams> ids {{
            "osc1Wave", "osc2Wave", "osc2Semitones", "osc2Detune", "oscMix", "filterCutoff",
            "filterResonance", "filterEnvAmount", "filterKeyTrack", "filterAttack", "filterDecay",
            "filterSustain", "filterRelease", "ampAttack", "ampDecay", "ampSustain", "ampRelease",
            "velocitySens", "volume"
        }};
        std::set<juce::String> uniqueIds;
        for (int i = 0; i < Synth::numParams; ++i)
        {
            const auto& info = infos[static_cast<size_t> (i)];
            expectEquals (juce::String (info.id), juce::String (ids[static_cast<size_t> (i)]));
            expect (uniqueIds.insert (info.id).second);
            expect (juce::String (info.name).isNotEmpty());
            expect (info.unit != nullptr);
            expect (info.minValue < info.maxValue);
            expect (info.defaultValue >= info.minValue && info.defaultValue <= info.maxValue);
            expect (info.centreValue > info.minValue && info.centreValue < info.maxValue);
            expectEquals (synth.getParam (i), info.defaultValue);
            synth.setParam (i, -std::numeric_limits<float>::infinity());
            expectEquals (synth.getParam (i), info.minValue);
            synth.setParam (i, std::numeric_limits<float>::infinity());
            expectEquals (synth.getParam (i), info.maxValue);
            const auto value = info.minValue + 0.371f * (info.maxValue - info.minValue);
            synth.setParam (i, value);
            expectEquals (synth.getParam (i), info.stepped ? std::round (value) : value);
            const auto previous = synth.getParam (i);
            synth.setParam (i, std::numeric_limits<float>::quiet_NaN());
            expectEquals (synth.getParam (i), previous);
        }
        synth.setParam (-1, 5);
        synth.setParam (Synth::numParams, 5);
        expectEquals (synth.getParam (-1), 0.0f);
        expectEquals (synth.getParam (Synth::numParams), 0.0f);
        expectEquals (infos[Synth::filterCutoff].centreValue, 1000.0f);
        for (const auto index : { Synth::ampAttack, Synth::ampDecay, Synth::ampRelease,
                                  Synth::filterAttack, Synth::filterDecay, Synth::filterRelease })
        {
            expectEquals (infos[static_cast<size_t> (index)].minValue, 0.001f);
            expectEquals (infos[static_cast<size_t> (index)].maxValue, 10.0f);
            expectEquals (infos[static_cast<size_t> (index)].centreValue, 0.5f);
        }
    }

    void testTimingAndPitch()
    {
        beginTest ("Note-on begins exactly at its offset, adds to stereo and preserves other channels");
        Synth synth;
        configureSine (synth);
        synth.prepare (sampleRate, 128);
        juce::AudioBuffer<float> audio (3, 128);
        audio.clear();
        for (int i = 0; i < 128; ++i)
            audio.setSample (2, i, 0.125f);
        const Event on { 37, 69, 1.0f, true };
        synth.process (audio, 128, &on, 1);
        expectEquals (audio.getMagnitude (0, 0, 37), 0.0f);
        expect (std::abs (audio.getSample (0, 37)) > 1.0e-8f);
        expect (audio.getMagnitude (0, 37, 91) > 0.01f);
        for (int i = 0; i < 128; ++i)
        {
            expectEquals (audio.getSample (0, i), audio.getSample (1, i));
            expectEquals (audio.getSample (2, i), 0.125f);
        }
        Synth additive;
        configureSine (additive);
        additive.prepare (sampleRate, 128);
        juce::AudioBuffer<float> added (2, 128);
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < 128; ++i)
                added.setSample (channel, i, 0.25f);
        additive.process (added, 128, &on, 1);
        for (int i = 0; i < 128; ++i)
            expectWithinAbsoluteError (added.getSample (0, i), audio.getSample (0, i) + 0.25f, 1.0e-7f);

        beginTest ("Sine oscillators track MIDI pitch, semitones and cents within 0.5 percent");
        for (const auto rate : { 44100.0, 96000.0 })
            for (const bool second : { false, true })
                for (const int semitones : { -12, 0, 12 })
                {
                    Synth pitched;
                    configureSine (pitched, second ? 1.0f : 0.0f);
                    pitched.setParam (Synth::osc2Semitones, static_cast<float> (semitones));
                    pitched.setParam (Synth::osc2Detune, -37);
                    pitched.prepare (rate, 512);
                    const auto output = render (pitched, static_cast<int> (rate / 2), { { 0, 69, 1, true } });
                    const auto expected = 440.0 * (second ? std::exp2 ((semitones - 0.37) / 12.0) : 1.0);
                    expectWithinAbsoluteError (estimateFrequency (output, rate), expected, expected * 0.005);
                }
    }

    void testReleaseAndReset()
    {
        beginTest ("Note-off releases from the current envelope level at its exact sample");
        Synth synth, held;
        configureSine (synth);
        configureSine (held);
        synth.setParam (Synth::ampAttack, 0.1f);
        held.setParam (Synth::ampAttack, 0.1f);
        synth.prepare (sampleRate, 512);
        held.prepare (sampleRate, 512);
        const auto released = render (synth, 8000, { { 19, 69, 1, true }, { 313, 69, 0, false } });
        const auto sustained = render (held, 8000, { { 19, 69, 1, true } });
        for (int i = 0; i < 313; ++i)
            expectEquals (released.getSample (0, i), sustained.getSample (0, i));
        expect (std::abs (released.getSample (0, 313) - released.getSample (0, 312)) < 0.01f);
        expect (std::abs (released.getSample (0, 313) - sustained.getSample (0, 313)) > 1.0e-8f);
        expectEquals (released.getMagnitude (0, 1400, 6600), 0.0f);
        expectEquals (synth.getNumActiveVoices(), 0);

        beginTest ("allNotesOff releases every voice; reset is immediately silent and reproducible");
        render (synth, 500, { { 0, 60, 1, true }, { 0, 67, 0.8f, true } });
        expectEquals (synth.getNumActiveVoices(), 2);
        synth.allNotesOff();
        expectEquals (synth.getNumActiveVoices(), 2);
        const auto release = render (synth, 4000);
        expect (release.getMagnitude (0, 0, 100) > 0.0f);
        expectEquals (release.getMagnitude (0, 1000, 3000), 0.0f);
        expectEquals (synth.getNumActiveVoices(), 0);
        render (synth, 500, { { 0, 60, 1, true } });
        synth.reset();
        expectEquals (synth.getNumActiveVoices(), 0);
        expectEquals (render (synth, 300).getMagnitude (0, 300), 0.0f);
        const auto restarted = render (synth, 500, { { 0, 60, 1, true } });
        // Reset also resets deterministic phase allocation; use a second reset as reference.
        synth.reset();
        const auto reference = render (synth, 500, { { 0, 60, 1, true } });
        expectEquals (maxDifference (restarted, reference), 0.0);
    }

    void testBlockSizes()
    {
        beginTest ("Events, envelope transitions, stealing and smoothing are independent of block size");
        std::vector<Event> events;
        for (int i = 0; i < 19; ++i)
            events.push_back ({ i * 137 + 3, 40 + i, 0.7f, true });
        events.push_back ({ 3000, 58, 1, true }); // Same pitch retrigger.
        for (int i = 0; i < 19; ++i)
            events.push_back ({ 4000 + i * 19, 40 + i, 0, false });
        const auto make = [&] (int blockSize)
        {
            Synth synth;
            synth.setParam (Synth::ampRelease, 0.03f);
            synth.setParam (Synth::filterRelease, 0.02f);
            synth.prepare (sampleRate, blockSize);
            // Exercise smoothers as well as note events across differently partitioned blocks.
            synth.setParam (Synth::filterCutoff, 317);
            synth.setParam (Synth::volume, -7);
            synth.setParam (Synth::oscMix, 0.8f);
            return render (synth, 10000, events, blockSize);
        };
        const auto reference = make (10000);
        for (const auto size : { 1, 7, 64, 512 })
            expectWithinAbsoluteError (maxDifference (reference, make (size)), 0.0, 1.0e-6,
                                       "Block size " + juce::String (size));
    }

    void testStealingAndRetrigger()
    {
        beginTest ("Same pitch reuses its voice and retriggers without resetting phase or level");
        Synth synth;
        configureSine (synth);
        synth.prepare (sampleRate, 512);
        const auto a = render (synth, 3000, { { 0, 60, 1, true }, { 2200, 60, 0.1f, true } });
        expectEquals (synth.getNumActiveVoices(), 1);
        expect (std::abs (a.getSample (0, 2200) - a.getSample (0, 2199)) < 0.03f);
        render (synth, 17, { { 0, 60, 0, false } });
        render (synth, 17, { { 0, 60, 0.8f, true } });
        expectEquals (synth.getNumActiveVoices(), 1);

        beginTest ("The seventeenth note fades the oldest held voice without a discontinuity");
        Synth stolen, control;
        configureSine (stolen);
        configureSine (control);
        stolen.prepare (sampleRate, 512);
        control.prepare (sampleRate, 512);
        std::vector<Event> notes;
        for (int i = 0; i < Synth::numVoices; ++i)
            notes.push_back ({ i * 2, 36 + i, 1, true });
        render (stolen, 3000, notes);
        render (control, 3000, notes);
        expectEquals (stolen.getNumActiveVoices(), 16);
        const auto replaced = render (stolen, 400, { { 7, 84, 1, true } });
        const auto unchanged = render (control, 400);
        for (int i = 0; i < 7; ++i)
            expectEquals (replaced.getSample (0, i), unchanged.getSample (0, i));
        expect (std::abs (replaced.getSample (0, 7) - unchanged.getSample (0, 7)) < 0.01f);
        float largestStep = 0.0f;
        for (int i = 1; i < 400; ++i)
            largestStep = std::max (largestStep, std::abs (replaced.getSample (0, i) - replaced.getSample (0, i - 1)));
        expect (largestStep < 0.15f, "No large step during the stolen voice fade");
        expectEquals (stolen.getNumActiveVoices(), 16);
        render (stolen, 2000, { { 0, 36, 0, false } });
        expectEquals (stolen.getNumActiveVoices(), 16); // Oldest held pitch was removed.
        render (stolen, 2000, { { 0, 37, 0, false } });
        expectEquals (stolen.getNumActiveVoices(), 15);

        beginTest ("Released voices are stolen before older held voices; oldest release wins");
        Synth releasing;
        configureSine (releasing);
        releasing.setParam (Synth::ampRelease, 1);
        releasing.prepare (sampleRate, 512);
        render (releasing, 3000, notes);
        render (releasing, 50, { { 0, 45, 0, false }, { 20, 44, 0, false } });
        render (releasing, 400, { { 0, 84, 1, true } });
        // Retrigger the stolen released pitch: it now needs a new voice and steals the other release.
        render (releasing, 400, { { 0, 45, 1, true } });
        render (releasing, 48000);
        expectEquals (releasing.getNumActiveVoices(), 16);
        releasing.allNotesOff();
        render (releasing, 49000);
        expectEquals (releasing.getNumActiveVoices(), 0);

        beginTest ("Repeated steals within the fade and reset remain bounded and silent after reset");
        std::vector<Event> burst;
        for (int i = 0; i < 300; ++i)
            burst.push_back ({ i, 24 + i % 100, 1, true });
        const auto burstAudio = render (releasing, 300, burst);
        expect (std::isfinite (burstAudio.getMagnitude (0, 300)));
        expect (burstAudio.getMagnitude (0, 300) < 12.0f);
        releasing.reset();
        expectEquals (render (releasing, 512).getMagnitude (0, 512), 0.0f);
    }

    void testFilterAndExtremes()
    {
        beginTest ("Low cutoff strongly attenuates saw harmonics above 5 kHz");
        const auto saw = [] (float cutoff)
        {
            Synth synth;
            configureSine (synth);
            synth.setParam (Synth::osc1Wave, 0);
            synth.setParam (Synth::filterCutoff, cutoff);
            synth.prepare (sampleRate, 512);
            return render (synth, 16000, { { 0, 60, 1, true } });
        };
        const auto low = highHarmonicEnergy (saw (200));
        const auto high = highHarmonicEnergy (saw (20000));
        expect (high > 1.0);
        expect (low < high * 0.001, "At least 30 dB less high harmonic energy");

        beginTest ("Extreme modulation and resonance stay finite and bounded at 44.1 and 96 kHz");
        for (const auto rate : { 44100.0, 96000.0 })
            for (const float amount : { -1.0f, 1.0f })
                for (int wave = 0; wave < 4; ++wave)
                {
                    Synth synth;
                    synth.setParam (Synth::osc1Wave, static_cast<float> (wave));
                    synth.setParam (Synth::osc2Wave, static_cast<float> ((wave + 1) % 4));
                    synth.setParam (Synth::osc2Semitones, 24);
                    synth.setParam (Synth::osc2Detune, 50);
                    synth.setParam (Synth::filterCutoff, 20000);
                    synth.setParam (Synth::filterResonance, 1);
                    synth.setParam (Synth::filterEnvAmount, amount);
                    synth.setParam (Synth::filterKeyTrack, 1);
                    synth.setParam (Synth::filterAttack, 0.001f);
                    synth.setParam (Synth::filterDecay, 0.001f);
                    synth.setParam (Synth::filterSustain, 1);
                    synth.setParam (Synth::ampAttack, 0.001f);
                    synth.setParam (Synth::ampSustain, 1);
                    synth.setParam (Synth::volume, 6);
                    synth.prepare (rate, 512);
                    std::vector<Event> notes;
                    for (int i = 0; i < 16; ++i)
                        notes.push_back ({ 0, i == 0 ? 0 : 7 + i * 8, 1, true });
                    const auto audio = render (synth, static_cast<int> (rate / 4), notes);
                    bool finite = true;
                    double peak = 0.0;
                    for (int i = 0; i < audio.getNumSamples(); ++i)
                    {
                        const auto value = audio.getSample (0, i);
                        finite = finite && std::isfinite (value);
                        peak = std::max (peak, std::abs (static_cast<double> (value)));
                    }
                    expect (finite);
                    expect (peak > 0.0 && peak < 13.0);
                }

        beginTest ("Default single saw voice has useful headroom near minus twelve dBFS");
        Synth defaults;
        defaults.setParam (Synth::oscMix, 0);
        defaults.prepare (sampleRate, 512);
        const auto audio = render (defaults, 24000, { { 0, 60, 1, true } });
        const auto peakDb = juce::Decibels::gainToDecibels (audio.getMagnitude (0, 24000));
        expect (peakDb > -16.0f && peakDb < -8.0f, "Peak dBFS: " + juce::String (peakDb));
    }

    void testAutomation()
    {
        beginTest ("Volume, cutoff and oscillator mix changes ramp rather than jumping");
        for (const auto parameter : { Synth::volume, Synth::filterCutoff, Synth::oscMix })
        {
            Synth changed, held;
            configureSine (changed);
            configureSine (held);
            changed.prepare (sampleRate, 512);
            held.prepare (sampleRate, 512);
            render (changed, 3000, { { 0, 69, 1, true } });
            render (held, 3000, { { 0, 69, 1, true } });
            changed.setParam (parameter, parameter == Synth::volume ? -60.0f
                                          : parameter == Synth::filterCutoff ? 20.0f : 1.0f);
            const auto a = render (changed, 2048);
            const auto b = render (held, 2048);
            expect (std::abs (a.getSample (0, 0) - b.getSample (0, 0)) < 0.002f);
            expect (maxDifference (a, b) > 0.01);
        }
    }
};

static SynthTests synthTests;
