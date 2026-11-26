#include "DrumRack.h"
#include "BuiltinSamples.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <set>
#include <thread>
#include <vector>

namespace
{
using Rack = daw::DrumRack;
using Event = daw::NoteEvent;
constexpr double rate = 48000;

// Test-thread helpers; allocation here is deliberately outside the render callback.
std::shared_ptr<const daw::SampleData> testSample (int frames = 48000, double sampleRate = rate,
                                                  double hz = 0, bool stereo = false)
{
    auto sample = std::make_shared<daw::SampleData>();
    sample->sampleRate = sampleRate;
    sample->buffer.setSize (stereo ? 2 : 1, frames);
    for (int i = 0; i < frames; ++i)
    {
        const auto value = hz <= 0 ? 0.5f : static_cast<float> (0.5 * std::sin (2 * juce::MathConstants<double>::pi * hz * i / sampleRate));
        sample->buffer.setSample (0, i, value);
        if (stereo)
            sample->buffer.setSample (1, i, -value * 0.5f);
    }
    sample->computePeaks();
    return sample;
}

void loadKit (Rack& rack, std::shared_ptr<const daw::SampleData> first,
              std::shared_ptr<const daw::SampleData> second = nullptr)
{
    auto kit = std::make_unique<Rack::Kit>();
    kit->samples[0] = std::move (first);
    kit->samples[1] = std::move (second);
    rack.setKit (std::move (kit));
}

juce::AudioBuffer<float> render (Rack& rack, int samples, const std::vector<Event>& events = {}, int blockSize = 512)
{
    juce::AudioBuffer<float> result (2, samples), block (2, blockSize);
    result.clear();
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
        rack.process (block, count, local.data(), static_cast<int> (local.size()));
        for (int channel = 0; channel < 2; ++channel)
            result.copyFrom (channel, start, block, channel, 0, count);
    }
    return result;
}

double difference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double result = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            result = std::max (result, std::abs (static_cast<double> (a.getSample (ch, i)) - b.getSample (ch, i)));
    return result;
}

double frequency (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    double first = 0, last = 0;
    int crossings = 0;
    for (int i = 101; i < audio.getNumSamples(); ++i)
    {
        const auto a = audio.getSample (0, i - 1), b = audio.getSample (0, i);
        if (a <= 0 && b > 0)
        {
            const auto crossing = i - 1.0 + static_cast<double> (-a) / (b - a);
            if (crossings++ == 0)
                first = crossing;
            last = crossing;
        }
    }
    return crossings > 1 ? (crossings - 1) * sampleRate / (last - first) : 0;
}

float largestStep (const juce::AudioBuffer<float>& audio)
{
    float step = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 1; i < audio.getNumSamples(); ++i)
            step = std::max (step, std::abs (audio.getSample (ch, i) - audio.getSample (ch, i - 1)));
    return step;
}
}

class DrumRackTests final : public juce::UnitTest
{
public:
    DrumRackTests() : UnitTest ("Drum rack instrument", "diyDaw") {}

    void runTest() override
    {
        testParameters();
        testEvents();
        testResampling();
        testLevels();
        testDecayAndReset();
        testChoke();
        testStealing();
        testBlocks();
        testKitLifetime();
    }

private:
    void testParameters()
    {
        beginTest ("Drum rack parameter identities, ranges, defaults, clamping and steps");
        Rack rack;
        const auto& infos = Rack::getParamInfos();
        expectEquals (static_cast<int> (infos.size()), 80);
        const char* names[] { "Gain", "Pan", "Tune", "Decay", "Choke" };
        const float minima[] { -60, -1, -24, 0.01f, 0 };
        const float maxima[] { 6, 1, 24, 10, 8 };
        const float defaults[] { 0, 0, 0, 10, 0 };
        const float centres[] { -12, 0, 0, 0.5f, 0 };
        std::set<juce::String> ids;
        for (int i = 0; i < Rack::numParams; ++i)
        {
            const auto& info = infos[static_cast<size_t> (i)];
            const auto p = i % Rack::numPadParams;
            expectEquals (juce::String (info.id), "pad" + juce::String (i / Rack::numPadParams + 1) + names[p]);
            expectEquals (juce::String (info.name), juce::String (names[p]));
            expect (ids.insert (info.id).second);
            expectEquals (info.minValue, minima[p]);
            expectEquals (info.maxValue, maxima[p]);
            expectEquals (info.defaultValue, defaults[p]);
            expectEquals (info.centreValue, centres[p]);
            expectEquals (rack.getParam (i), defaults[p]);
            expect (info.stepped == (p == Rack::tune || p == Rack::choke));
            rack.setParam (i, -1000);
            expectEquals (rack.getParam (i), minima[p]);
            rack.setParam (i, 1000);
            expectEquals (rack.getParam (i), maxima[p]);
            rack.setParam (i, info.defaultValue + 0.4f);
            const auto clamped = std::clamp (info.defaultValue + 0.4f, info.minValue, info.maxValue);
            expectEquals (rack.getParam (i), info.stepped ? std::round (clamped) : clamped);
            const auto before = rack.getParam (i);
            rack.setParam (i, std::numeric_limits<float>::quiet_NaN());
            expectEquals (rack.getParam (i), before);
        }
        rack.setParam (-1, 1);
        rack.setParam (80, 1);
        expectEquals (rack.getParam (-1), 0.0f);
        expectEquals (rack.getParam (80), 0.0f);
    }

    void testEvents()
    {
        beginTest ("Drum rack exact event offset, note range, ignored note-offs and additive stereo");
        Rack rack;
        loadKit (rack, testSample());
        rack.prepare (rate, 128);
        juce::AudioBuffer<float> audio (3, 128);
        for (int ch = 0; ch < 3; ++ch)
            for (int i = 0; i < 128; ++i)
                audio.setSample (ch, i, 0.125f);
        const Event events[] { { 0, 35, 1, true }, { 0, 52, 1, true }, { 3, 36, 0, true },
                               { 37, 36, 1, true }, { 38, 36, 0, false } };
        rack.process (audio, 128, events, 5);
        const auto level = 0.5f * std::sqrt (0.5f);
        for (int i = 0; i < 128; ++i)
        {
            expectWithinAbsoluteError (audio.getSample (0, i), 0.125f + (i >= 37 ? level : 0), 1.0e-6f);
            expectEquals (audio.getSample (0, i), audio.getSample (1, i));
            expectEquals (audio.getSample (2, i), 0.125f);
        }
        expectEquals (rack.getNumActiveVoices(), 1);

        beginTest ("Drum rack all 16 notes map to their pads; empty pads and absent kits are silent");
        for (int pad = 0; pad < Rack::numPads; ++pad)
        {
            Rack mapped;
            auto kit = std::make_unique<Rack::Kit>();
            kit->samples[static_cast<size_t> (pad)] = testSample (100);
            mapped.setKit (std::move (kit));
            mapped.prepare (rate, 128);
            const auto hit = render (mapped, 128, { { 7, 36 + pad, 1, true } });
            expectEquals (hit.getSample (0, 6), 0.0f);
            expectWithinAbsoluteError (hit.getSample (0, 7), level, 1.0e-6f);
            expectEquals (hit.getSample (0, 107), 0.0f);
            mapped.reset();
            expectEquals (render (mapped, 128, { { 0, 36 + (pad + 1) % 16, 1, true } }).getMagnitude (0, 128), 0.0f);
        }
        Rack empty;
        expectEquals (render (empty, 100, { { 0, 36, 1, true } }).getMagnitude (0, 100), 0.0f);
        loadKit (empty, nullptr);
        expectEquals (render (empty, 100, { { 0, 36, 1, true } }).getMagnitude (0, 100), 0.0f);
    }

    void testResampling()
    {
        beginTest ("Drum rack tune doubles pitch and halves length; sample-rate conversion and interpolation");
        for (const auto semitones : { -12, 0, 12 })
        {
            Rack rack;
            loadKit (rack, testSample (2400, 24000));
            rack.setParam (Rack::paramIndex (0, Rack::tune), static_cast<float> (semitones));
            rack.prepare (rate, 512);
            const auto length = static_cast<int> (4800 / std::exp2 (semitones / 12.0));
            const auto audio = render (rack, length + 100, { { 0, 36, 1, true } });
            expect (audio.getSample (0, length - 1) > 0);
            expectEquals (audio.getMagnitude (0, length, 100), 0.0f);
            expectEquals (rack.getNumActiveVoices(), 0);

            Rack sine;
            loadKit (sine, testSample (24000, 24000, 440));
            sine.setParam (Rack::paramIndex (0, Rack::tune), static_cast<float> (semitones));
            sine.prepare (rate, 512);
            const auto pitched = render (sine, 10000, { { 0, 36, 1, true } });
            const auto expected = 440 * std::exp2 (semitones / 12.0);
            expectWithinAbsoluteError (frequency (pitched, rate), expected, expected * 0.001);
        }
        Rack interpolated;
        auto ramp = std::make_shared<daw::SampleData>();
        ramp->sampleRate = 24000;
        ramp->buffer.setSize (1, 3);
        ramp->buffer.setSample (0, 0, 0);
        ramp->buffer.setSample (0, 1, 1);
        ramp->buffer.setSample (0, 2, 0);
        loadKit (interpolated, ramp);
        interpolated.prepare (rate, 8);
        const auto audio = render (interpolated, 8, { { 0, 36, 1, true } });
        const float expected[] { 0, 0.5f, 1, 0.5f, 0, 0, 0, 0 };
        for (int i = 0; i < 8; ++i)
            expectWithinAbsoluteError (audio.getSample (0, i), expected[i] * std::sqrt (0.5f), 1.0e-6f);
    }

    void testLevels()
    {
        beginTest ("Drum rack linear velocity, dB gain, constant-power pan and stereo channels");
        for (const float pan : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
            for (const float db : { -12.0f, 0.0f, 6.0f })
                for (const float velocity : { 0.25f, 1.0f })
                {
                    Rack rack;
                    loadKit (rack, testSample (1000, rate, 0, true));
                    rack.setParam (Rack::paramIndex (0, Rack::gain), db);
                    rack.setParam (Rack::paramIndex (0, Rack::pan), pan);
                    rack.prepare (rate, 128);
                    const auto audio = render (rack, 128, { { 0, 36, velocity, true } });
                    const auto scale = juce::Decibels::decibelsToGain (db) * velocity;
                    const auto angle = (pan + 1) * juce::MathConstants<float>::pi / 4;
                    expectWithinAbsoluteError (audio.getSample (0, 64), 0.5f * scale * std::cos (angle), 1.0e-6f);
                    expectWithinAbsoluteError (audio.getSample (1, 64), -0.25f * scale * std::sin (angle), 1.0e-6f);
                }
        beginTest ("Drum rack live gain and pan changes are smoothed over 5 ms");
        Rack rack;
        loadKit (rack, testSample());
        rack.prepare (rate, 512);
        const auto held = render (rack, 512, { { 0, 36, 1, true } });
        rack.setParam (Rack::paramIndex (0, Rack::gain), -12);
        rack.setParam (Rack::paramIndex (0, Rack::pan), 1);
        const auto changed = render (rack, 512);
        expect (std::abs (changed.getSample (0, 0) - held.getSample (0, 511)) < 0.005f);
        expect (largestStep (changed) < 0.005f);
        expectWithinAbsoluteError (changed.getSample (0, 511), 0.0f, 1.0e-6f);
        expectWithinAbsoluteError (changed.getSample (1, 511), 0.5f * juce::Decibels::decibelsToGain (-12.0f), 1.0e-6f);
    }

    void testDecayAndReset()
    {
        beginTest ("Drum rack decay reaches silence at the requested time; maximum disables fade");
        Rack rack;
        loadKit (rack, testSample());
        rack.setParam (Rack::paramIndex (0, Rack::decay), 0.01f);
        rack.prepare (rate, 512);
        const auto audio = render (rack, 1000, { { 0, 36, 1, true } });
        expectWithinAbsoluteError (audio.getSample (0, 240) / audio.getSample (0, 0), 0.125f, 1.0e-6f);
        expectEquals (audio.getMagnitude (0, 480, 520), 0.0f);
        expectEquals (rack.getNumActiveVoices(), 0);
        rack.setParam (Rack::paramIndex (0, Rack::decay), 10);
        const auto sustained = render (rack, 4000, { { 0, 36, 1, true }, { 50, 36, 0, false } });
        expectEquals (sustained.getSample (0, 0), sustained.getSample (0, 3999));

        beginTest ("Drum rack allNotesOff releases in 5 ms; reset immediately silences every tail");
        rack.allNotesOff();
        const auto release = render (rack, 512);
        expectWithinAbsoluteError (release.getSample (0, 0), sustained.getSample (0, 3999), 1.0e-6f);
        expect (largestStep (release) < 0.002f);
        expectEquals (release.getMagnitude (0, 240, 272), 0.0f);
        expectEquals (rack.getNumActiveVoices(), 0);
        render (rack, 128, { { 0, 36, 1, true } });
        rack.reset();
        expectEquals (rack.getNumActiveVoices(), 0);
        expectEquals (render (rack, 512).getMagnitude (0, 512), 0.0f);
    }

    void testChoke()
    {
        beginTest ("Drum rack other pads in a choke group fade within 5 ms; group zero never chokes");
        for (const int group : { 0, 1, 8 })
        {
            Rack rack;
            auto silent = std::make_shared<daw::SampleData>();
            silent->sampleRate = rate;
            silent->buffer.setSize (1, 2000);
            silent->buffer.clear();
            loadKit (rack, testSample(), silent);
            rack.setParam (Rack::paramIndex (0, Rack::choke), static_cast<float> (group));
            rack.setParam (Rack::paramIndex (1, Rack::choke), static_cast<float> (group));
            rack.prepare (rate, 512);
            const auto audio = render (rack, 2000, { { 0, 36, 1, true }, { 1000, 37, 1, true } });
            expectEquals (audio.getSample (0, 999), audio.getSample (0, 1000));
            if (group == 0)
                expectEquals (audio.getSample (0, 0), audio.getSample (0, 1999));
            else
            {
                expect (audio.getSample (0, 1100) > 0);
                expectEquals (audio.getMagnitude (0, 1240, 760), 0.0f);
                expect (largestStep (audio) < 0.002f);
            }
        }
        beginTest ("Drum rack same-pad retriggers ring, and different nonzero groups do not choke");
        Rack rack;
        const auto sample = testSample();
        loadKit (rack, sample, sample);
        rack.setParam (Rack::paramIndex (0, Rack::choke), 1);
        rack.setParam (Rack::paramIndex (1, Rack::choke), 2);
        rack.prepare (rate, 512);
        const auto audio = render (rack, 2000, { { 0, 36, 1, true }, { 100, 36, 1, true }, { 1000, 37, 1, true } });
        expectEquals (rack.getNumActiveVoices(), 3);
        expectWithinAbsoluteError (audio.getSample (0, 1999), 3 * 0.5f * std::sqrt (0.5f), 1.0e-6f);
    }

    void testStealing()
    {
        beginTest ("Drum rack oldest of 32 voices is stolen with a continuous crossfade");
        Rack rack, reference;
        const auto sample = testSample (48000, rate, 100);
        loadKit (rack, sample);
        loadKit (reference, sample);
        rack.prepare (rate, 512);
        reference.prepare (rate, 512);
        std::vector<Event> events;
        for (int i = 0; i < Rack::numVoices; ++i)
            events.push_back ({ i * 11, 36, 0.2f, true });
        render (rack, 1000, events);
        render (reference, 1000, events);
        expectEquals (rack.getNumActiveVoices(), 32);
        const auto stolen = render (rack, 500, { { 17, 36, 0.2f, true } });
        const auto held = render (reference, 500);
        for (int i = 0; i <= 17; ++i)
            expectWithinAbsoluteError (stolen.getSample (0, i), held.getSample (0, i), 1.0e-6f);
        expect (largestStep (stolen) < 0.04f);
        expectEquals (rack.getNumActiveVoices(), 32);
        // After the crossfade the result must equal the original mix minus the OLDEST
        // hit plus the new hit, including each oscillator's independent sample position.
        const auto expectedAt = [] (int frame)
        {
            return static_cast<float> (0.5 * std::sin (2 * juce::MathConstants<double>::pi * 100 * frame / rate))
                   * 0.2f * std::sqrt (0.5f);
        };
        expectWithinAbsoluteError (stolen.getSample (0, 400), held.getSample (0, 400)
                                   - expectedAt (1400) + expectedAt (383), 1.0e-6f);

        beginTest ("Drum rack repeated steals retain tails in fixed storage without spikes");
        Rack burstRack;
        loadKit (burstRack, testSample());
        burstRack.prepare (rate, 512);
        std::vector<Event> burst;
        for (int i = 0; i < 400; ++i)
            burst.push_back ({ i * 4, 36, 0.1f, true });
        const auto burstAudio = render (burstRack, 3000, burst);
        expect (std::isfinite (burstAudio.getMagnitude (0, 3000)));
        expect (burstAudio.getMagnitude (0, 3000) < 5.0f);
        // First 32 unstolen attacks may step by their legitimate 0.0354 level.
        expect (largestStep (burstAudio) < 0.04f);
        burstRack.reset();
        expectEquals (render (burstRack, 512).getMagnitude (0, 512), 0.0f);
    }

    void testBlocks()
    {
        beginTest ("Drum rack events, resampling, envelopes, choke and stealing ignore block size");
        const auto sample = testSample (48000, 44100, 337, true);
        std::vector<Event> events;
        for (int i = 0; i < 80; ++i)
            events.push_back ({ 3 + i * 37, 36 + (i >= 50 && i % 9 == 0 ? 1 : 0), 0.1f + (i % 7) * 0.1f, true });
        events.push_back ({ 4000, 36, 0, false });
        const auto make = [&] (int blockSize)
        {
            Rack rack;
            loadKit (rack, sample, sample);
            rack.setParam (Rack::paramIndex (0, Rack::tune), 7);
            rack.setParam (Rack::paramIndex (0, Rack::decay), 0.07f);
            rack.setParam (Rack::paramIndex (0, Rack::choke), 1);
            rack.setParam (Rack::paramIndex (1, Rack::choke), 1);
            rack.prepare (rate, blockSize);
            rack.setParam (Rack::paramIndex (0, Rack::gain), -9);
            rack.setParam (Rack::paramIndex (0, Rack::pan), 0.7f);
            return render (rack, 7000, events, blockSize);
        };
        const auto reference = make (7000);
        for (const auto size : { 1, 37, 512 })
            expectWithinAbsoluteError (difference (reference, make (size)), 0.0, 1.0e-6);
    }

    void testKitLifetime()
    {
        beginTest ("Drum rack retired samples are destroyed while their cached continuations fade");
        Rack rack;
        std::weak_ptr<const daw::SampleData> retired;
        {
            auto sample = testSample (48000, rate, 211, true);
            retired = sample;
            loadKit (rack, std::move (sample));
        }
        rack.prepare (rate, 37);
        const auto first = render (rack, 300, { { 0, 36, 1, true } }, 37);
        expect (! retired.expired());
        loadKit (rack, nullptr);
        const auto transition = render (rack, 37, {}, 37);
        rack.collectGarbage();
        expect (retired.expired());
        expect (std::abs (transition.getSample (0, 0) - first.getSample (0, 299)) < 0.02f);
        expect (transition.getMagnitude (0, 37) > 0.01f);
        const auto tail = render (rack, 500, {}, 37);
        expect (tail.getMagnitude (0, 100) > 0);
        expectEquals (tail.getMagnitude (0, 203, 297), 0.0f);

        beginTest ("Drum rack retained samples continue across pad moves and repeated publication");
        const auto shared = testSample();
        loadKit (rack, shared);
        rack.reset();
        render (rack, 100, { { 0, 36, 1, true } });
        loadKit (rack, nullptr, shared);
        const auto continued = render (rack, 512);
        rack.collectGarbage();
        expectEquals (continued.getSample (0, 0), continued.getSample (0, 511));

        beginTest ("Drum rack many kit swaps and garbage collections during sounding and stolen voices");
        for (int i = 0; i < 300; ++i)
        {
            loadKit (rack, testSample (3000, rate, 180 + i));
            const auto audio = render (rack, 37, { { 0, 36, 0.1f, true }, { 1, 36, 0.1f, true } }, 37);
            rack.collectGarbage();
            expect (std::isfinite (audio.getMagnitude (0, 37)));
            expect (audio.getMagnitude (0, 37) < 20);
        }
        rack.allNotesOff();
        render (rack, 1000);
        expectEquals (rack.getNumActiveVoices(), 0);

        beginTest ("Drum rack concurrent kit publication and retirement remain finite");
        std::atomic<bool> finished { false };
        std::thread publisher ([&]
        {
            for (int i = 0; i < 200; ++i)
            {
                loadKit (rack, testSample (1000, rate, 200 + i));
                rack.collectGarbage();
                std::this_thread::yield();
            }
            finished.store (true, std::memory_order_release);
        });
        juce::AudioBuffer<float> block (2, 37);
        const Event on { 0, 36, 0.1f, true };
        bool finite = true;
        do
        {
            block.clear();
            rack.process (block, 37, &on, 1);
            finite = finite && std::isfinite (block.getMagnitude (0, 37));
        }
        while (! finished.load (std::memory_order_acquire));
        publisher.join();
        rack.collectGarbage();
        expect (finite);
        rack.reset();
        expectEquals (render (rack, 512).getMagnitude (0, 512), 0.0f);
    }
};

class BuiltinSamplesTests final : public juce::UnitTest
{
public:
    BuiltinSamplesTests() : UnitTest ("Builtin samples synthesis", "diyDaw") {}

    void runTest() override
    {
        beginTest ("Builtin samples names, key helpers and unknown-name handling");
        const auto& names = daw::getBuiltinSampleNames();
        const juce::StringArray expected { "kick", "rim", "snare", "clap", "shaker", "lowTom", "closedHat", "midTom",
                                           "cowbell", "highTom", "openHat", "conga", "zap", "crash", "sub", "ride" };
        expect (names == expected);
        expect (daw::createBuiltinSample ("unknown") == nullptr);
        expect (daw::createBuiltinSample ("") == nullptr);
        expect (daw::createBuiltinSample ("Kick") == nullptr);
        expect (daw::createBuiltinSample ("kick", 0) == nullptr);
        expect (daw::createBuiltinSample ("kick", std::numeric_limits<double>::quiet_NaN()) == nullptr);
        std::vector<std::shared_ptr<const daw::SampleData>> sounds;
        for (const auto& soundName : names)
        {
            beginTest ("Builtin samples " + soundName + " is deterministic, finite, mono, normalised and tapered");
            expect (daw::isBuiltinSampleKey (daw::builtinSampleKey (soundName)));
            expectEquals (daw::builtinSampleName (daw::builtinSampleKey (soundName)), soundName);
            auto first = daw::createBuiltinSample (soundName);
            const auto second = daw::createBuiltinSample (soundName);
            expect (first != nullptr && second != nullptr);
            if (first == nullptr || second == nullptr)
                continue;
            expectEquals (first->name, soundName);
            expectEquals (first->sampleRate, rate);
            expectEquals (first->buffer.getNumChannels(), 1);
            expectEquals (first->getNumFrames(), second->getNumFrames());
            expect (first->getLengthSeconds() >= 0.05);
            expect (first->getLengthSeconds() <= (soundName == "crash" || soundName == "ride" ? 2.5 : 2.0));
            const auto db = juce::Decibels::gainToDecibels (first->buffer.getMagnitude (0, first->getNumFrames()));
            expect (db >= -6 && db <= -1, soundName + " peak: " + juce::String (db));
            bool finite = true, identical = true;
            for (int i = 0; i < first->getNumFrames(); ++i)
            {
                finite = finite && std::isfinite (first->buffer.getSample (0, i));
                identical = identical && std::abs (first->buffer.getSample (0, i) - second->buffer.getSample (0, i)) <= 0.0f;
            }
            expect (finite);
            expect (identical);
            expectEquals (first->buffer.getSample (0, 0), 0.0f);
            expectEquals (first->buffer.getSample (0, first->getNumFrames() - 1), 0.0f);
            expect (first->buffer.getMagnitude (0, 0, 3) < 0.03f);
            expect (first->buffer.getMagnitude (0, first->getNumFrames() - 32, 32) < 0.001f);
            expectEquals (static_cast<int> (first->peaks.size()), (first->getNumFrames() + 255) / 256);
            bool overviewCorrect = true;
            for (int p = 0; p < static_cast<int> (first->peaks.size()); ++p)
            {
                const auto start = p * 256;
                const auto end = std::min (start + 256, first->getNumFrames());
                for (int i = start; i < end; ++i)
                    overviewCorrect = overviewCorrect && first->buffer.getSample (0, i) >= first->peaks[static_cast<size_t> (p)].getStart()
                                                      && first->buffer.getSample (0, i) <= first->peaks[static_cast<size_t> (p)].getEnd();
            }
            expect (overviewCorrect);
            sounds.push_back (std::move (first));
        }
        beginTest ("Builtin samples all pairs have distinct waveforms, including equal-length sounds");
        for (size_t a = 0; a < sounds.size(); ++a)
            for (size_t b = a + 1; b < sounds.size(); ++b)
            {
                double error = 0;
                for (int i = 0; i < 2400; ++i)
                    error += std::abs (sounds[a]->buffer.getSample (0, i) - sounds[b]->buffer.getSample (0, i));
                expect (error > 1, sounds[a]->name + " versus " + sounds[b]->name);
            }
        beginTest ("Builtin samples render at 44.1 and 96 kHz with the requested duration and level");
        for (const double sampleRate : { 44100.0, 96000.0 })
            for (const auto& soundName : names)
            {
                const auto sound = daw::createBuiltinSample (soundName, sampleRate);
                expect (sound != nullptr);
                if (sound == nullptr)
                    continue;
                expectEquals (sound->sampleRate, sampleRate);
                const auto canonical = daw::createBuiltinSample (soundName);
                expectWithinAbsoluteError (sound->getLengthSeconds(), canonical->getLengthSeconds(), 1.0 / sampleRate);
                expectWithinAbsoluteError (juce::Decibels::gainToDecibels (sound->buffer.getMagnitude (0, sound->getNumFrames())), -3.0f, 1.0e-4f);
                expectEquals (sound->buffer.getSample (0, sound->getNumFrames() - 1), 0.0f);
            }
    }
};

static DrumRackTests drumRackTests;
static BuiltinSamplesTests builtinSamplesTests;
