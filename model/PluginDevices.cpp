#include "PluginDevices.h"

namespace daw
{

namespace
{
    constexpr int midiBytesToReserve = 16384; // plenty for one block of notes, no allocation later

    void addEvents (juce::MidiBuffer& midi, const NoteEvent* events, int numEvents, bool allNotesOff) noexcept
    {
        midi.clear();

        if (allNotesOff)
            midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);

        for (int i = 0; i < numEvents; ++i)
        {
            const auto& e = events[i];
            const auto message = e.isNoteOn ? juce::MidiMessage::noteOn (1, e.pitch, juce::jlimit (0.0f, 1.0f, e.velocity))
                                            : juce::MidiMessage::noteOff (1, e.pitch);
            midi.addEvent (message, e.sampleOffset);
        }
    }
}

int configureStereoBuses (juce::AudioPluginInstance& plugin)
{
    auto layout = plugin.getBusesLayout();

    if (! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();

    if (plugin.checkBusesLayoutSupported (layout))
        plugin.setBusesLayout (layout);

    return juce::jmax (2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels());
}

// ---- PluginEffect -----------------------------------------------------------------

PluginEffect::PluginEffect (std::unique_ptr<juce::AudioPluginInstance> p) : plugin (std::move (p))
{
    configureStereoBuses (*plugin);
    initialiseParams();
}

PluginEffect::~PluginEffect()
{
    plugin->releaseResources();
}

const std::vector<ParamInfo>& PluginEffect::getParamInfos() const noexcept
{
    static const std::vector<ParamInfo> none;
    return none;
}

void PluginEffect::prepareEffect (double sampleRate, int maxBlockSize)
{
    // Release first: some formats skip prepareToPlay() when they believe nothing changed, and
    // prepareToPlay() sets the rate and block size itself.
    plugin->releaseResources();
    const auto channels = configureStereoBuses (*plugin);
    plugin->prepareToPlay (sampleRate, maxBlockSize);
    plugin->setRateAndBufferSizeDetails (sampleRate, maxBlockSize); // LV2 doesn't record it itself
    scratch.setSize (channels, maxBlockSize, false, true, false);
    midi.ensureSize (midiBytesToReserve);
}

void PluginEffect::reset() noexcept
{
    plugin->reset();
}

void PluginEffect::processEffect (juce::AudioBuffer<float>& buffer, int numSamples, const Transport::Block&) noexcept
{
    if (numSamples > scratch.getNumSamples())
        return;

    const auto ins = plugin->getTotalNumInputChannels();
    const auto outs = plugin->getTotalNumOutputChannels();

    for (int ch = 0; ch < scratch.getNumChannels(); ++ch)
    {
        if (ch < ins)
            scratch.copyFrom (ch, 0, buffer, juce::jmin (ch, 1), 0, numSamples); // mono input gets the left side
        else
            scratch.clear (ch, 0, numSamples);
    }

    juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), scratch.getNumChannels(), numSamples);
    midi.clear();
    plugin->processBlock (view, midi);

    if (outs <= 0)
        return;

    for (int ch = 0; ch < 2; ++ch)
        buffer.copyFrom (ch, 0, scratch, juce::jmin (ch, outs - 1), 0, numSamples); // mono output to both
}

// ---- PluginInstrument -------------------------------------------------------------

PluginInstrument::PluginInstrument (std::unique_ptr<juce::AudioPluginInstance> p) : plugin (std::move (p))
{
    configureStereoBuses (*plugin);
}

PluginInstrument::~PluginInstrument()
{
    plugin->releaseResources();
}

void PluginInstrument::prepare (double sampleRate, int maxBlockSize)
{
    // Release first: some formats skip prepareToPlay() when they believe nothing changed, and
    // prepareToPlay() sets the rate and block size itself.
    plugin->releaseResources();
    const auto channels = configureStereoBuses (*plugin);
    plugin->prepareToPlay (sampleRate, maxBlockSize);
    plugin->setRateAndBufferSizeDetails (sampleRate, maxBlockSize); // LV2 doesn't record it itself
    scratch.setSize (channels, maxBlockSize, false, true, false);
    midi.ensureSize (midiBytesToReserve);
}

void PluginInstrument::reset() noexcept
{
    plugin->reset();
    pendingAllNotesOff.store (true);
}

void PluginInstrument::process (juce::AudioBuffer<float>& out, int numSamples, const NoteEvent* events, int numEvents) noexcept
{
    if (numSamples > scratch.getNumSamples())
        return;

    addEvents (midi, events, numEvents, pendingAllNotesOff.exchange (false));

    scratch.clear (0, numSamples);
    juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), scratch.getNumChannels(), numSamples);
    plugin->processBlock (view, midi);

    const auto outs = plugin->getTotalNumOutputChannels();
    if (outs <= 0)
        return;

    for (int ch = 0; ch < juce::jmin (2, out.getNumChannels()); ++ch)
        out.addFrom (ch, 0, scratch, juce::jmin (ch, outs - 1), 0, numSamples);
}

} // namespace daw
