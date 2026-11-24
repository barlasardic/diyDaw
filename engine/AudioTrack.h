#pragma once

#include "PeakMeter.h"
#include "RealtimeSwap.h"
#include "SampleData.h"
#include "Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace daw
{

/** One mixer channel that plays a single audio clip on the timeline.

    For this milestone a track holds one sample, placed at clipStartBeat, played
    at its native speed (no warping), optionally looping.

    Threading: parameter setters/getters and setSample() are message-thread;
    prepare()/process() are audio-thread. All shared state is atomic or handed
    over through RealtimeSwap.
*/
class AudioTrack
{
public:
    static constexpr float minGainDb = -60.0f; // at or below this the fader is treated as silent
    static constexpr float maxGainDb = 6.0f;

    // ---- message thread ---------------------------------------------------
    void setName (const juce::String& newName) { name = newName; }
    const juce::String& getName() const noexcept { return name; }

    void setGainDb (float db) noexcept      { gainDb.store (juce::jlimit (minGainDb, maxGainDb, db)); }
    float getGainDb() const noexcept        { return gainDb.load(); }

    /** -1 = hard left, 0 = centre, +1 = hard right (constant-power pan law). */
    void setPan (float p) noexcept          { pan.store (juce::jlimit (-1.0f, 1.0f, p)); }
    float getPan() const noexcept           { return pan.load(); }

    void setMute (bool m) noexcept          { mute.store (m); }
    bool isMuted() const noexcept           { return mute.load(); }

    void setSolo (bool s) noexcept          { solo.store (s); }
    bool isSoloed() const noexcept          { return solo.load(); }

    void setClipStartBeat (double b) noexcept { clipStartBeat.store (juce::jmax (0.0, b)); }
    double getClipStartBeat() const noexcept  { return clipStartBeat.load(); }

    void setLooping (bool l) noexcept       { looping.store (l); }
    bool isLooping() const noexcept         { return looping.load(); }

    /** Replace the clip's audio. Pass an empty SampleData (0 frames) to clear it. */
    void setSample (std::unique_ptr<SampleData> newSample);

    /** Name of the last sample handed to setSample(), for display. */
    const juce::String& getSampleName() const noexcept { return sampleName; }

    /** Free samples the audio thread has finished with. Call regularly (e.g. from a UI timer). */
    void collectGarbage() { sample.collectGarbage(); }

    PeakMeter& getMeter() noexcept { return meter; }

    // ---- audio thread -----------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);

    /** Renders this track and *adds* it into 'mixBus' (stereo). */
    void process (juce::AudioBuffer<float>& mixBus, const Transport::Block& block, bool anyTrackSoloed) noexcept;

private:
    void renderClip (const SampleData& s, const Transport::Block& block) noexcept;

    juce::String name { "Audio" }, sampleName;

    std::atomic<float> gainDb { 0.0f }, pan { 0.0f };
    std::atomic<bool> mute { false }, solo { false }, looping { false };
    std::atomic<double> clipStartBeat { 0.0 };

    RealtimeSwap<SampleData> sample;
    PeakMeter meter;

    // audio thread only
    juce::AudioBuffer<float> scratch;
    juce::SmoothedValue<float> leftGain, rightGain;
};

} // namespace daw
