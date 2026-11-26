#pragma once

#include "AudioEffect.h"
#include "Instrument.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

namespace daw
{

/** Lays the plugin out for stereo where it allows it and returns the channel count its
    processBlock() buffer needs. Message thread / audio stopped. */
int configureStereoBuses (juce::AudioPluginInstance& plugin);

/** A VST3 / LV2 effect plugin as an insert effect (stereo in/out).

    It has no ParamInfos of its own (its parameters live in the plugin and are saved in its
    state blob). prepare() may be called again on a device change; processing is real-time
    safe as long as the plugin is (buffers are sized in prepare). Plugin latency is not
    compensated yet. */
class PluginEffect : public AudioEffect
{
public:
    explicit PluginEffect (std::unique_ptr<juce::AudioPluginInstance> plugin);
    ~PluginEffect() override;

    const char* getType() const noexcept override { return "plugin"; }
    const std::vector<ParamInfo>& getParamInfos() const noexcept override;
    void reset() noexcept override;

    juce::AudioPluginInstance& getPlugin() noexcept { return *plugin; }

private:
    void prepareEffect (double sampleRate, int maxBlockSize) override;
    void processEffect (juce::AudioBuffer<float>&, int numSamples, const Transport::Block&) noexcept override;

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
};

/** A VST3 / LV2 instrument plugin: note events become MIDI messages at their sample offsets.
    setParam/getParam do nothing (see PluginEffect). */
class PluginInstrument : public Instrument
{
public:
    explicit PluginInstrument (std::unique_ptr<juce::AudioPluginInstance> plugin);
    ~PluginInstrument() override;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::AudioBuffer<float>& out, int numSamples, const NoteEvent* events, int numEvents) noexcept override;
    void allNotesOff() noexcept override { pendingAllNotesOff.store (true); }
    void reset() noexcept override;
    void setParam (int, float) noexcept override {}
    float getParam (int) const noexcept override { return 0.0f; }

    juce::AudioPluginInstance& getPlugin() noexcept { return *plugin; }

private:
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    std::atomic<bool> pendingAllNotesOff { false };
};

} // namespace daw
