#include "SampleData.h"

namespace daw
{

std::unique_ptr<SampleData> loadSample (const juce::File& file, juce::AudioFormatManager& formats)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    // Cap at 10 minutes for now; long files will be streamed from disk in a later milestone.
    const auto maxFrames = (juce::int64) (reader->sampleRate * 600.0);
    const auto numFrames = (int) juce::jmin (reader->lengthInSamples, maxFrames);
    const auto numChannels = (int) juce::jlimit (1u, 2u, reader->numChannels);

    auto data = std::make_unique<SampleData>();
    data->sampleRate = reader->sampleRate;
    data->name = file.getFileNameWithoutExtension();
    data->buffer.setSize (numChannels, numFrames);

    if (! reader->read (&data->buffer, 0, numFrames, 0, true, numChannels > 1))
        return nullptr;

    return data;
}

} // namespace daw
