#pragma once

#include <atomic>

namespace daw
{

/** Musical timeline clock. The playhead is stored in beats (quarter notes) so
    tempo changes keep the musical position stable.

    Threading: setters/getters are called from the message thread; advance()
    is called once per audio block from the audio thread. Everything is lock-free.
*/
class Transport
{
public:
    /** Timing information for one audio block, produced by advance(). */
    struct Block
    {
        bool playing = false;
        double startBeat = 0.0;       // playhead at the first sample of the block
        double beatsPerSample = 0.0;  // bpm / (60 * sampleRate)
        double bpm = 120.0;
        double sampleRate = 44100.0;
        int numSamples = 0;
        int beatsPerBar = 4;

        double endBeat() const noexcept { return startBeat + beatsPerSample * numSamples; }
        double beatAt (int sampleOffset) const noexcept { return startBeat + beatsPerSample * sampleOffset; }
    };

    static constexpr double minBpm = 20.0;
    static constexpr double maxBpm = 999.0;

    // ---- message thread ---------------------------------------------------
    void play() noexcept                 { playing.store (true); }
    void stop() noexcept                 { playing.store (false); }
    void togglePlay() noexcept           { playing.store (! playing.load()); }
    bool isPlaying() const noexcept      { return playing.load(); }

    void setBpm (double newBpm) noexcept;
    double getBpm() const noexcept       { return bpm.load(); }

    void setBeatsPerBar (int n) noexcept;
    int getBeatsPerBar() const noexcept  { return beatsPerBar.load(); }

    /** Request a jump; applied at the start of the next audio block. */
    void setPositionBeats (double beats) noexcept;

    /** Playhead position as last written by the audio thread (or the pending seek target). */
    double getPositionBeats() const noexcept;

    // ---- audio thread -----------------------------------------------------
    void prepare (double newSampleRate) noexcept { sampleRate = newSampleRate; }

    /** Returns timing for the next block of numSamples and moves the playhead past it. */
    Block advance (int numSamples) noexcept;

private:
    std::atomic<bool> playing { false };
    std::atomic<double> bpm { 120.0 };
    std::atomic<int> beatsPerBar { 4 };

    std::atomic<double> positionBeats { 0.0 };
    std::atomic<double> seekTarget { 0.0 };
    std::atomic<bool> seekRequested { false };

    double sampleRate = 44100.0; // audio thread only
};

} // namespace daw
