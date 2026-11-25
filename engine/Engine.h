#pragma once

#include "AudioTrack.h"
#include "Metronome.h"
#include "PeakMeter.h"
#include "Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>

namespace daw
{

/** The whole audio engine, independent of any audio device.

    The app drives render() from an audio device callback; tests and offline
    export drive it directly. Output is always stereo.

    Tracks live in a fixed pool so adding one never allocates on the audio thread.
*/
class Engine
{
public:
    static constexpr int maxTracks = 16;

    /** Longest block rendered in one go; automation values update at this granularity. */
    static constexpr int automationBlockSize = 256;

    Engine();

    Transport& getTransport() noexcept { return transport; }
    Metronome& getMetronome() noexcept { return metronome; }

    // ---- message thread ---------------------------------------------------
    /** Activates the next free track slot. Returns its index, or -1 if all are in use. */
    int addTrack();
    int getNumTracks() const noexcept { return numTracks.load(); }

    /** Deactivates every track, empties its clips and resets its mixer settings, so the next
        addTrack() returns slot 0 again (used when a project is replaced). */
    void resetTracks();
    AudioTrack& getTrack (int index) noexcept { return *tracks[(size_t) index]; }

    void setMasterGainDb (float db) noexcept { masterGainDb.store (juce::jlimit (AudioTrack::minGainDb, AudioTrack::maxGainDb, db)); }
    float getMasterGainDb() const noexcept   { return masterGainDb.load(); }
    PeakMeter& getMasterMeter() noexcept     { return masterMeter; }

    /** Pattern mode: only this track is rendered (-1 = all tracks). */
    void setIsolatedTrack (int index) noexcept { isolatedTrack.store (index); }
    int getIsolatedTrack() const noexcept { return isolatedTrack.load(); }

    /** The sample rate / block size of the last prepare(), for preparing new effects on the
        message thread. */
    double getPreparedSampleRate() const noexcept { return preparedSampleRate.load(); }
    int getPreparedBlockSize() const noexcept { return preparedBlockSize.load(); }

    /** Frees resources the audio thread has finished with. Call from a UI timer. */
    void collectGarbage();

    // ---- audio thread -----------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);
    double getSampleRate() const noexcept { return sampleRate; }

    /** Clears 'out' and renders the next numSamples into its first two channels
        (any further channels are silenced). Handles blocks larger than maxBlockSize. */
    void render (juce::AudioBuffer<float>& out, int numSamples) noexcept;

private:
    void renderBlock (juce::AudioBuffer<float>& out, int startSample, const Transport::Block& block) noexcept;

    Transport transport;
    Metronome metronome;
    std::array<std::unique_ptr<AudioTrack>, maxTracks> tracks;
    std::atomic<int> numTracks { 0 };

    std::atomic<float> masterGainDb { 0.0f };
    std::atomic<int> isolatedTrack { -1 };
    std::atomic<double> preparedSampleRate { 44100.0 };
    std::atomic<int> preparedBlockSize { 512 };
    PeakMeter masterMeter;

    // audio thread only
    double sampleRate = 44100.0;
    juce::AudioBuffer<float> bus;
    juce::SmoothedValue<float> masterGain;
};

/** Renders 'seconds' of audio offline (for tests and, later, export). */
juce::AudioBuffer<float> renderOffline (Engine& engine, double seconds, int blockSize = 512);

} // namespace daw
