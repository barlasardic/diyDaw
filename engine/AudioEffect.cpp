#include "AudioEffect.h"
#include "Effects.h"

#include <cmath>

namespace daw
{

void AudioEffect::initialiseParams()
{
    const auto& infos = getParamInfos();
    jassert ((int) infos.size() <= maxParams);

    for (size_t i = 0; i < infos.size() && i < params.size(); ++i)
        params[i].store (infos[i].defaultValue);
}

void AudioEffect::setParam (int index, float value) noexcept
{
    const auto& infos = getParamInfos();
    if (index < 0 || index >= (int) infos.size() || index >= maxParams)
        return;

    const auto& info = infos[(size_t) index];
    value = juce::jlimit (info.minValue, info.maxValue, value);
    if (info.stepped)
        value = std::round (value);

    params[(size_t) index].store (value);
}

float AudioEffect::getParam (int index) const noexcept
{
    if (index < 0 || index >= (int) getParamInfos().size() || index >= maxParams)
        return 0.0f;

    return params[(size_t) index].load();
}

void AudioEffect::prepare (double sampleRate, int maxBlockSize)
{
    prepareEffect (sampleRate, maxBlockSize);
    dry.setSize (2, maxBlockSize, false, false, true);
    wetAmount.reset (sampleRate, 0.01);
    wetAmount.setCurrentAndTargetValue (isEnabled() ? 1.0f : 0.0f);
    reset();
    preparedRate = sampleRate;
}

void AudioEffect::process (juce::AudioBuffer<float>& buffer, int numSamples, const Transport::Block& block) noexcept
{
    if (! juce::exactlyEqual (block.sampleRate, preparedRate) || numSamples > dry.getNumSamples()
        || buffer.getNumChannels() < 2)
        return;

    const auto wasBypassed = ! wetAmount.isSmoothing() && wetAmount.getTargetValue() <= 0.0f;
    wetAmount.setTargetValue (isEnabled() ? 1.0f : 0.0f);

    if (! wetAmount.isSmoothing())
    {
        if (wetAmount.getTargetValue() <= 0.0f)
            return; // fully bypassed

        processEffect (buffer, numSamples, block);
        return;
    }

    if (wasBypassed)
        reset(); // coming back from bypass: don't resume a stale tail

    // Crossfade between dry and processed signal while switching.
    for (int ch = 0; ch < 2; ++ch)
        dry.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    processEffect (buffer, numSamples, block);

    auto* l = buffer.getWritePointer (0);
    auto* r = buffer.getWritePointer (1);
    const auto* dl = dry.getReadPointer (0);
    const auto* dr = dry.getReadPointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto w = wetAmount.getNextValue();
        l[i] = dl[i] + w * (l[i] - dl[i]);
        r[i] = dr[i] + w * (r[i] - dr[i]);
    }
}

// ---- registry -----------------------------------------------------------------------

const juce::StringArray& getEffectTypes()
{
    static const juce::StringArray types { "eq", "compressor", "reverb", "delay" };
    return types;
}

juce::String getEffectDisplayName (const juce::String& type)
{
    if (type == "eq")         return "EQ Three";
    if (type == "compressor") return "Compressor";
    if (type == "reverb")     return "Reverb";
    if (type == "delay")      return "Delay";
    if (type == "synth")      return "Synth";
    return type;
}

const std::vector<ParamInfo>& getEffectParamInfos (const juce::String& type)
{
    static const std::vector<ParamInfo> none;
    if (type == "eq")         return EqEffect::infos();
    if (type == "compressor") return CompressorEffect::infos();
    if (type == "reverb")     return ReverbEffect::infos();
    if (type == "delay")      return DelayEffect::infos();
    return none;
}

std::unique_ptr<AudioEffect> createEffect (const juce::String& type)
{
    if (type == "eq")         return std::make_unique<EqEffect>();
    if (type == "compressor") return std::make_unique<CompressorEffect>();
    if (type == "reverb")     return std::make_unique<ReverbEffect>();
    if (type == "delay")      return std::make_unique<DelayEffect>();
    return nullptr;
}

} // namespace daw
