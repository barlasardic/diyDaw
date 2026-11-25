#pragma once

#include "ParamInfo.h"
#include "Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace daw
{

/** An insert effect in a track's device chain (stereo in, stereo out, processed in place).

    Threading:
    - prepare() may allocate; it is called on the message thread *before* the effect is
      handed to the audio thread, and again from Engine::prepare (audio stopped).
    - process() and reset() are audio-thread and must be real-time safe.
    - setParam()/getParam()/setEnabled() are thread-safe (atomics). Implementations smooth
      parameters that would zipper or click.
*/
class AudioEffect
{
public:
    static constexpr int maxParams = 16;

    virtual ~AudioEffect() = default;

    /** Type id as stored in the project ("eq", "compressor", "reverb", "delay"). */
    virtual const char* getType() const noexcept = 0;
    virtual const std::vector<ParamInfo>& getParamInfos() const noexcept = 0;

    // ---- any thread -----------------------------------------------------------
    /** Clamped to the param's range (and rounded if stepped). Out-of-range indices are ignored. */
    void setParam (int index, float value) noexcept;
    float getParam (int index) const noexcept;
    void setEnabled (bool shouldBeEnabled) noexcept { enabled.store (shouldBeEnabled); }
    bool isEnabled() const noexcept { return enabled.load(); }

    // ---- message thread (before use) / audio stopped ------------------------------
    void prepare (double sampleRate, int maxBlockSize);

    // ---- audio thread -----------------------------------------------------------
    /** Processes buffer's first two channels in place. Skipped (passthrough) when disabled or
        when 'block.sampleRate' doesn't match the prepared rate (a device change in flight).
        Bypassing is click-free: the effect crossfades over ~10 ms. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const Transport::Block& block) noexcept;

    /** Clears delay lines / envelopes. */
    virtual void reset() noexcept = 0;

protected:
    /** Called by the constructor of each subclass: sets every param to its default. */
    void initialiseParams();

    virtual void prepareEffect (double sampleRate, int maxBlockSize) = 0;
    virtual void processEffect (juce::AudioBuffer<float>& buffer, int numSamples, const Transport::Block& block) noexcept = 0;

private:
    std::array<std::atomic<float>, maxParams> params {};
    std::atomic<bool> enabled { true };

    // audio thread
    double preparedRate = 0.0;
    juce::AudioBuffer<float> dry;   // for bypass crossfades
    juce::SmoothedValue<float> wetAmount;
};

/** Effect type ids in menu order, and their display names ("EQ Three", ...). */
const juce::StringArray& getEffectTypes();
juce::String getEffectDisplayName (const juce::String& type);

/** Parameter infos for a type (empty for an unknown type). */
const std::vector<ParamInfo>& getEffectParamInfos (const juce::String& type);

/** Creates an effect with default parameters (not yet prepared), or nullptr for an unknown type. */
std::unique_ptr<AudioEffect> createEffect (const juce::String& type);

/** Immutable effect chain snapshot for one track (shared instances: an effect keeps its state
    when the chain around it is rebuilt). */
struct EffectChain
{
    std::vector<std::shared_ptr<AudioEffect>> effects;
};

} // namespace daw
