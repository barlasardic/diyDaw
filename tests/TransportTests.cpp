#include "Transport.h"

#include <juce_core/juce_core.h>

#include <array>
#include <bit>
#include <cstdint>

/** Exercises message-thread controls and audio-thread advances sequentially. */
struct TransportTests : juce::UnitTest
{
    TransportTests() : juce::UnitTest ("Transport", "diyDaw") {}

    void runTest() override
    {
        beginTest ("Only playing advances the playhead by the block duration");
        for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
        {
            daw::Transport transport;
            transport.prepare (sampleRate);
            transport.setBpm (137.0);
            expect (! transport.isPlaying());
            expect (! transport.advance (997).playing);
            expectEquals (transport.getPositionBeats(), 0.0);

            transport.play();
            double expectedPosition = 0.0;
            for (const auto size : { 1, 64, 512, 997, 48000 })
            {
                const auto block = transport.advance (size);
                expect (block.playing);
                expectEquals (block.numSamples, size);
                expectEquals (block.sampleRate, sampleRate);
                expectEquals (block.bpm, 137.0);
                expectWithinAbsoluteError (block.startBeat, expectedPosition, 1.0e-13);
                expectedPosition += static_cast<double> (size) * 137.0 / (60.0 * sampleRate);
                expectWithinAbsoluteError (transport.getPositionBeats(), expectedPosition, 1.0e-13);
                expectEquals (transport.getPositionBeats(), block.endBeat());
            }

            transport.stop();
            const auto stoppedPosition = transport.getPositionBeats();
            for (const auto size : { 64, 997, 48000 })
            {
                const auto block = transport.advance (size);
                expect (! block.playing);
                expectEquals (block.startBeat, stoppedPosition);
                expectEquals (transport.getPositionBeats(), stoppedPosition);
            }

            transport.play();
            expectEquals (transport.advance (64).startBeat, stoppedPosition);
            expect (transport.getPositionBeats() > stoppedPosition);
        }

        beginTest ("Pending seeks are visible immediately and start the next block");
        daw::Transport transport;
        transport.prepare (48000.0);
        transport.play();
        const auto originalBlock = transport.advance (512);
        transport.setPositionBeats (7.25);
        expectEquals (transport.getPositionBeats(), 7.25);
        expectEquals (originalBlock.startBeat, 0.0);
        const auto soughtBlock = transport.advance (997);
        expectEquals (soughtBlock.startBeat, 7.25);
        expectEquals (transport.getPositionBeats(), soughtBlock.endBeat());

        transport.setPositionBeats (12.0);
        transport.setPositionBeats (3.5);
        expectEquals (transport.getPositionBeats(), 3.5);
        expectEquals (transport.advance (64).startBeat, 3.5);

        transport.stop();
        transport.setPositionBeats (9.0);
        expectEquals (transport.getPositionBeats(), 9.0);
        expectEquals (transport.advance (512).startBeat, 9.0);
        expectEquals (transport.getPositionBeats(), 9.0);
        transport.setPositionBeats (-10.0);
        expectEquals (transport.getPositionBeats(), 0.0);
        expectEquals (transport.advance (64).startBeat, 0.0);

        beginTest ("BPM is clamped and the next block uses the clamped tempo");
        for (const auto requested : { -100.0, 0.0, 20.0, 137.5, 999.0, 2000.0 })
        {
            const auto expected = juce::jlimit (daw::Transport::minBpm, daw::Transport::maxBpm, requested);
            transport.setBpm (requested);
            expectEquals (transport.getBpm(), expected);
            expectEquals (transport.advance (512).bpm, expected);
        }

        beginTest ("Consecutive playing blocks are contiguous bit for bit");
        constexpr std::array sizes { 1, 64, 512, 997, 31, 4096 };
        for (const auto sampleRate : { 44100.0, 48000.0 })
        {
            for (const auto bpm : { 20.0, 120.0, 137.5, 999.0 })
            {
                daw::Transport clock;
                clock.prepare (sampleRate);
                clock.setBpm (bpm);
                clock.setPositionBeats (0.125);
                clock.play();
                auto previous = clock.advance (997);
                bool contiguous = true;
                for (int i = 0; i < 2000; ++i)
                {
                    const auto next = clock.advance (sizes[static_cast<size_t> (i) % sizes.size()]);
                    contiguous = contiguous
                              && std::bit_cast<std::uint64_t> (next.startBeat)
                                 == std::bit_cast<std::uint64_t> (previous.endBeat());
                    previous = next;
                }
                expect (contiguous, "sample rate=" + juce::String (sampleRate)
                                    + ", BPM=" + juce::String (bpm));
            }
        }
    }
};

static TransportTests transportTests;
