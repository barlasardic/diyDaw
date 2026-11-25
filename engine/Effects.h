#pragma once

#include "AudioEffect.h"

namespace daw
{

/** Three-band EQ: low shelf, peaking mid, high shelf (RBJ biquads), plus output gain. */
class EqEffect : public AudioEffect
{
public:
    enum Param { lowFreq, lowGain, midFreq, midGain, midQ, highFreq, highGain, output, numParams };

    EqEffect();
    ~EqEffect() override;
    const char* getType() const noexcept override { return "eq"; }
    const std::vector<ParamInfo>& getParamInfos() const noexcept override { return infos(); }
    static const std::vector<ParamInfo>& infos();
    void reset() noexcept override;

private:
    void prepareEffect (double sampleRate, int maxBlockSize) override;
    void processEffect (juce::AudioBuffer<float>&, int, const Transport::Block&) noexcept override;
    struct State;
    std::unique_ptr<State> state;
};

/** Feed-forward stereo-linked compressor with soft knee, makeup gain and dry/wet mix. */
class CompressorEffect : public AudioEffect
{
public:
    enum Param { threshold, ratio, attack, release, knee, makeup, mix, numParams };

    CompressorEffect();
    ~CompressorEffect() override;
    const char* getType() const noexcept override { return "compressor"; }
    const std::vector<ParamInfo>& getParamInfos() const noexcept override { return infos(); }
    static const std::vector<ParamInfo>& infos();
    void reset() noexcept override;

    /** Current gain reduction in dB (>= 0), for a meter. Any thread. */
    float getGainReductionDb() const noexcept { return gainReductionDb.load(); }

private:
    void prepareEffect (double sampleRate, int maxBlockSize) override;
    void processEffect (juce::AudioBuffer<float>&, int, const Transport::Block&) noexcept override;
    struct State;
    std::unique_ptr<State> state;
    std::atomic<float> gainReductionDb { 0.0f };
};

/** Algorithmic stereo reverb (Freeverb-style or better) with pre-delay, size, damping,
    width and dry/wet. */
class ReverbEffect : public AudioEffect
{
public:
    enum Param { size, damping, predelay, width, mix, numParams };

    ReverbEffect();
    ~ReverbEffect() override;
    const char* getType() const noexcept override { return "reverb"; }
    const std::vector<ParamInfo>& getParamInfos() const noexcept override { return infos(); }
    static const std::vector<ParamInfo>& infos();
    void reset() noexcept override;

private:
    void prepareEffect (double sampleRate, int maxBlockSize) override;
    void processEffect (juce::AudioBuffer<float>&, int, const Transport::Block&) noexcept override;
    struct State;
    std::unique_ptr<State> state;
};

/** Stereo delay: tempo-synced (note values) or free time in ms, feedback with low/high cut
    filters in the loop, optional ping-pong, dry/wet. */
class DelayEffect : public AudioEffect
{
public:
    enum Param { sync, syncDivision, timeMs, feedback, pingPong, lowCut, highCut, mix, numParams };

    /** syncDivision values: 0 = 1/16, 1 = 1/8, 2 = 1/8 dotted, 3 = 1/4, 4 = 1/4 dotted, 5 = 1/2, 6 = 1 bar. */
    static double divisionInBeats (int division, int beatsPerBar) noexcept;

    DelayEffect();
    ~DelayEffect() override;
    const char* getType() const noexcept override { return "delay"; }
    const std::vector<ParamInfo>& getParamInfos() const noexcept override { return infos(); }
    static const std::vector<ParamInfo>& infos();
    void reset() noexcept override;

private:
    void prepareEffect (double sampleRate, int maxBlockSize) override;
    void processEffect (juce::AudioBuffer<float>&, int, const Transport::Block&) noexcept override;
    struct State;
    std::unique_ptr<State> state;
};

} // namespace daw
