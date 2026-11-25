#pragma once

#include "AudioEffect.h"
#include "Automation.h"
#include "Clip.h"
#include "PeakMeter.h"
#include "RealtimeSwap.h"
#include "Synth.h"
#include "Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <mutex>

namespace daw
{

/** One mixer channel that plays clips from the timeline.

    The clip layout arrives as an immutable ClipList snapshot (built by the model on the
    message thread). Overlapping clips are summed; the model normally prevents overlaps.

    Audio clips are rendered directly. When the instrument is enabled (a MIDI track), MIDI
    clips are turned into sample-accurate note events for the track's Synth, together with
    live notes pushed from a keyboard. Sequenced notes are always ended by the track itself
    (it remembers each sounding note's end), so editing, stopping or jumping never leaves a
    note hanging.

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

    /** Replace the insert effects (processed in order, before the fader). Every effect must
        already be prepared at the engine's current sample rate. */
    void setEffects (std::unique_ptr<EffectChain> chain) { effects.publish (std::move (chain)); }

    /** Replace the track's automation. Every block, each lane's value at the playhead (also
        while stopped) is written into its parameter before rendering, overriding the value the
        model set; when a lane goes away the model must push the parameter's own value again. */
    void setAutomation (std::unique_ptr<AutomationList> list) { automation.publish (std::move (list)); }

    /** Free clip lists, samples, effect chains and automation the audio thread is done with. */
    void collectGarbage() { clips.collectGarbage(); effects.collectGarbage(); automation.collectGarbage(); }

    PeakMeter& getMeter() noexcept { return meter; }

    /** MIDI track: render MIDI clips and live notes through the synth. */
    void setInstrumentEnabled (bool shouldBeEnabled) noexcept { instrumentEnabled.store (shouldBeEnabled); }
    bool isInstrumentEnabled() const noexcept { return instrumentEnabled.load(); }
    Synth& getSynth() noexcept { return synth; }

    /** Live note from a MIDI keyboard or the computer keyboard; plays at the start of the next
        block. Any non-audio thread (several producers are fine). Drops notes if the queue is full. */
    void pushLiveNote (int pitch, float velocity, bool isNoteOn);

    // ---- audio thread -----------------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);

    /** Renders this track and *adds* it into 'mixBus' (stereo). */
    void process (juce::AudioBuffer<float>& mixBus, const Transport::Block& block, bool anyTrackSoloed) noexcept;

    static constexpr int maxEventsPerBlock = 512;

private:
    void renderClip (const Clip& clip, const Transport::Block& block, int numSamples) noexcept;
    void renderInstrument (const ClipList* list, const Transport::Block& block, int numSamples) noexcept;
    void applyAutomation (const Transport::Block& block) noexcept;
    void collectMidiClip (const Clip& clip, const Transport::Block& block, int numSamples) noexcept;
    void addEvent (int offset, int pitch, float velocity, bool isNoteOn) noexcept;

    juce::String name { "Audio" };

    std::atomic<float> gainDb { 0.0f }, pan { 0.0f };
    std::atomic<bool> mute { false }, solo { false };

    RealtimeSwap<ClipList> clips;
    RealtimeSwap<EffectChain> effects;
    RealtimeSwap<AutomationList> automation;
    PeakMeter meter;

    std::atomic<bool> instrumentEnabled { false };
    Synth synth;

    // Live notes: producers serialise on liveMutex; the audio thread reads lock-free.
    static constexpr int liveQueueSize = 256;
    std::mutex liveMutex;
    juce::AbstractFifo liveFifo { liveQueueSize };
    std::array<NoteEvent, liveQueueSize> liveQueue;

    // audio thread only
    std::array<NoteEvent, maxEventsPerBlock> events;
    int numEvents = 0;
    std::array<double, 128> noteEndBeat;   // end of each sounding sequenced note; < 0 = not sounding
    std::array<int, 128> noteOnIndex;      // sample index of this block's note-on per pitch, or -1
    double expectedStartBeat = 0.0;        // NaN when the next block can't be a continuation
    juce::AudioBuffer<float> scratch;
    juce::SmoothedValue<float> leftGain, rightGain;
};

} // namespace daw
