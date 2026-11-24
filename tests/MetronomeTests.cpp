#include "Metronome.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int samplesPerBeat = 24000;
constexpr int clickSamples = 2400;

// Test-thread helper: preserve DSP state while changing callback block sizes.
juce::AudioBuffer<float> renderClicks (daw::Metronome& metronome, daw::Transport& transport,
                                     int totalSamples, int blockSize)
{
    juce::AudioBuffer<float> result (2, totalSamples);
    juce::AudioBuffer<float> block (2, blockSize);
    for (int position = 0; position < totalSamples; position += blockSize)
    {
        const auto count = juce::jmin (blockSize, totalSamples - position);
        block.clear();
        metronome.process (block, transport.advance (count));
        for (int channel = 0; channel < 2; ++channel)
            result.copyFrom (channel, position, block, channel, 0, count);
    }
    return result;
}

std::vector<int> findClickStarts (const juce::AudioBuffer<float>& audio)
{
    std::vector<int> starts;
    int lastNonzero = -clickSamples;
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        if (std::abs (audio.getSample (0, i)) <= 1.0e-6f)
            continue;

        // A click's sine starts at zero, so its first nonzero sample is onset + 1.
        // A 10 ms gap distinguishes clicks from zero crossings inside a click.
        if (i - lastNonzero > 480)
            starts.push_back (i - 1);
        lastNonzero = i;
    }
    return starts;
}

int countZeroCrossings (const juce::AudioBuffer<float>& audio, int start)
{
    int previousSign = 0;
    int crossings = 0;
    for (int i = start; i < start + 240; ++i)
    {
        const auto value = audio.getSample (0, i);
        if (std::abs (value) < 1.0e-7f)
            continue;
        const auto sign = value > 0.0f ? 1 : -1;
        if (previousSign != 0 && sign != previousSign)
            ++crossings;
        previousSign = sign;
    }
    return crossings;
}
}

/** Runs message-thread controls and audio-thread processing sequentially, without a device. */
struct MetronomeTests : juce::UnitTest
{
    MetronomeTests() : juce::UnitTest ("Metronome", "diyDaw") {}

    void runTest() override
    {
        for (const auto blockSize : { 64, 512, 997 })
        {
            beginTest ("Exact click offsets at 120 BPM / 48 kHz, block size " + juce::String (blockSize));
            daw::Transport transport;
            transport.prepare (sampleRate);
            transport.setBpm (120.0);
            transport.setBeatsPerBar (4);
            transport.play();
            daw::Metronome metronome;
            metronome.prepare (sampleRate);
            metronome.setEnabled (true);
            const auto audio = renderClicks (metronome, transport, 8 * samplesPerBeat + clickSamples + 100, blockSize);
            const auto starts = findClickStarts (audio);
            expectEquals (static_cast<int> (starts.size()), 9, "Exactly one click per beat, including beat zero");
            for (int beat = 0; beat < juce::jmin (9, static_cast<int> (starts.size())); ++beat)
                expectEquals (starts[static_cast<size_t> (beat)], beat * samplesPerBeat,
                              "Beat " + juce::String (beat) + " onset (samples)");

            beginTest ("Bar accents have higher pitch, block size " + juce::String (blockSize));
            for (int beat = 0; beat <= 8; ++beat)
            {
                const auto crossings = countZeroCrossings (audio, beat * samplesPerBeat);
                const auto accented = beat % 4 == 0;
                expect (accented ? crossings >= 13 && crossings <= 15 : crossings >= 8 && crossings <= 10,
                        "Beat " + juce::String (beat) + ": " + juce::String (crossings) + " crossings in 5 ms");
                if (accented)
                    expect (crossings > countZeroCrossings (audio, samplesPerBeat));
            }
        }

        beginTest ("Disabled metronome is silent throughout playing beats");
        daw::Transport transport;
        transport.prepare (sampleRate);
        transport.play();
        daw::Metronome metronome;
        metronome.prepare (sampleRate);
        metronome.setEnabled (false);
        auto audio = renderClicks (metronome, transport, 3 * samplesPerBeat, 997);
        expectEquals (audio.getMagnitude (0, audio.getNumSamples()), 0.0f);

        transport.setPositionBeats (0.0);
        metronome.setEnabled (true);
        audio = renderClicks (metronome, transport, 4096, 512);
        expect (audio.getMagnitude (0, audio.getNumSamples()) > 0.0f);
        metronome.setEnabled (false);
        audio = renderClicks (metronome, transport, 3 * samplesPerBeat, 997);
        expectEquals (audio.getMagnitude (0, audio.getNumSamples()), 0.0f);

        beginTest ("Enabled metronome does not trigger while stopped");
        transport.stop();
        transport.setPositionBeats (0.0);
        metronome.setEnabled (true);
        audio = renderClicks (metronome, transport, 2 * samplesPerBeat, 512);
        expectEquals (audio.getMagnitude (0, audio.getNumSamples()), 0.0f);

        beginTest ("Stopping lets the current click finish but triggers no more clicks");
        transport.play();
        audio = renderClicks (metronome, transport, 64, 64);
        expect (audio.getMagnitude (0, audio.getNumSamples()) > 0.0f);
        transport.stop();
        audio = renderClicks (metronome, transport, 2 * samplesPerBeat, 997);
        expect (audio.getMagnitude (0, 64) > 0.0f, "Existing click rings out");
        expectEquals (audio.getMagnitude (clickSamples, audio.getNumSamples() - clickSamples), 0.0f);
    }
};

static MetronomeTests metronomeTests;
