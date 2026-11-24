#pragma once

#include "Clip.h"
#include "PeakMeter.h"
#include "RealtimeSwap.h"
#include "Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace daw
{

/** One mixer channel that plays audio clips from the timeline.

    The clip layout arrives as an immutable ClipList snapshot (built by the model on the
    message thread). Overlapping clips are summed; the model normally prevents overlaps.

    Threading: parameter setters/getters and setClips() are message-thread;
    prepare()/process() are audio-thread. Shared state is atomic or handed over
    through RealtimeSwap.
*/
class AudioTrack
{
public:
    static constexpr float minGainDb = -60.0f; // at or below this the fader is treated as silent
    static constexpr float maxGainDb = 6.0f;

    /** Short linear fade applied at every clip edge so cuts never click. */
    static constexpr double clipFadeSeconds = 0.002;

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

    /** Replace the track's clip layout. Clips must be sorted by startBeat. */
    void setClips (std::unique_ptr<ClipList> newClips) { clips.publish (std::move (newClips)); }

    /** Free clip lists (and samples) the audio thread has finished with. Call regularly. */
    void collectGarbage() { clips.collectGarbage(); }

    PeakMeter& getMeter() noexcept { return meter; }

    // ---- audio thread -----------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);

    /** Renders this track and *adds* it into 'mixBus' (stereo). */
    void process (juce::AudioBuffer<float>& mixBus, const Transport::Block& block, bool anyTrackSoloed) noexcept;

private:
    void renderClip (const Clip& clip, const Transport::Block& block, int numSamples) noexcept;

    juce::String name { "Audio" };

    std::atomic<float> gainDb { 0.0f }, pan { 0.0f };
    std::atomic<bool> mute { false }, solo { false };

    RealtimeSwap<ClipList> clips;
    PeakMeter meter;

    // audio thread only
    juce::AudioBuffer<float> scratch;
    juce::SmoothedValue<float> leftGain, rightGain;
};

} // namespace daw
