#include "SampleData.h"

namespace daw
{

void SampleData::computePeaks()
{
    const auto frames = buffer.getNumSamples();
    const auto channels = buffer.getNumChannels();
    const auto numPeaks = (frames + framesPerPeak - 1) / framesPerPeak;

    peaks.assign ((size_t) numPeaks, {});

    for (int p = 0; p < numPeaks; ++p)
    {
        const auto start = p * framesPerPeak;
        const auto count = juce::jmin (framesPerPeak, frames - start);
        auto lo = 0.0f, hi = 0.0f;

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto r = juce::FloatVectorOperations::findMinAndMax (buffer.getReadPointer (ch, start), count);
            lo = juce::jmin (lo, r.getStart());
            hi = juce::jmax (hi, r.getEnd());
        }

        peaks[(size_t) p] = { lo, hi };
    }
}

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

    data->computePeaks();
    return data;
}

} // namespace daw
