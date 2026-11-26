#include "TestPlugins.h"

#include <array>
#include <cmath>

namespace
{
template <typename Derived>
class TestPluginBase : public juce::AudioProcessor
{
public:
    using juce::AudioProcessor::AudioProcessor;

    const juce::String getName() const override              { return JucePlugin_Name; }
    double getTailLengthSeconds() const override             { return 0.0; }
    int getNumPrograms() override                            { return 1; }
    int getCurrentProgram() override                         { return 0; }
    void setCurrentProgram (int) override                    {}
    const juce::String getProgramName (int) override         { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    bool hasEditor() const override                          { return true; }
    juce::AudioProcessorEditor* createEditor() override      { return new juce::GenericAudioProcessorEditor (*this); }
    void releaseResources() override                         {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && (layouts.getMainInputChannelSet().isDisabled()
                || layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo());
    }

    void getStateInformation (juce::MemoryBlock& data) override
    {
        juce::MemoryOutputStream out (data, false);
        for (auto* p : getParameters())
            out.writeFloat (p->getValue());
    }

    void setStateInformation (const void* data, int size) override
    {
        juce::MemoryInputStream in (data, (size_t) size, false);
        for (auto* p : getParameters())
            if (! in.isExhausted())
                p->setValueNotifyingHost (in.readFloat());
    }
};

class TestGain final : public TestPluginBase<TestGain>
{
public:
    TestGain()
        : TestPluginBase (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo())
                                           .withOutput ("Out", juce::AudioChannelSet::stereo()))
    {
        addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 2.0f, 0.5f));
    }

    using juce::AudioProcessor::processBlock;

    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    void prepareToPlay (double, int) override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain (gain->get());
    }

private:
    juce::AudioParameterFloat* gain = nullptr;
};

class TestSynth final : public TestPluginBase<TestSynth>
{
public:
    TestSynth() : TestPluginBase (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo()))
    {
        addParameter (level = new juce::AudioParameterFloat (juce::ParameterID { "level", 1 }, "Level", 0.0f, 1.0f, 1.0f));
    }

    using juce::AudioProcessor::processBlock;

    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }

    void prepareToPlay (double sr, int) override
    {
        sampleRate = sr;
        for (auto& v : voices) v = {};
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        auto it = midi.begin();

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            for (; it != midi.end() && (*it).samplePosition <= i; ++it)
            {
                const auto m = (*it).getMessage();
                auto& v = voices[(size_t) m.getNoteNumber()];
                if (m.isNoteOn())
                    v = { true, 0.0, juce::MidiMessage::getMidiNoteInHertz (m.getNoteNumber()) / sampleRate, m.getFloatVelocity() };
                else if (m.isNoteOff())
                    v.on = false;
                else if (m.isAllNotesOff() || m.isAllSoundOff())
                    for (auto& each : voices) each.on = false;
            }

            float s = 0.0f;
            for (auto& v : voices)
                if (v.on)
                {
                    s += 0.25f * v.velocity * (float) std::sin (2.0 * juce::MathConstants<double>::pi * v.phase);
                    v.phase = std::fmod (v.phase + v.delta, 1.0);
                }

            s *= level->get();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, i, s);
        }
    }

private:
    struct Voice { bool on = false; double phase = 0.0, delta = 0.0; float velocity = 0.0f; };
    std::array<Voice, 128> voices {};
    double sampleRate = 44100.0;
    juce::AudioParameterFloat* level = nullptr;
};
} // namespace

juce::AudioProcessor* createTestGainPlugin()  { return new TestGain(); }
juce::AudioProcessor* createTestSynthPlugin() { return new TestSynth(); }
