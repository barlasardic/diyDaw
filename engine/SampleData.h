#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>

namespace daw
{

/** An immutable, fully decoded audio file held in memory. */
struct SampleData
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    juce::String name;

    int getNumFrames() const noexcept { return buffer.getNumSamples(); }
    double getLengthSeconds() const noexcept { return buffer.getNumSamples() / sampleRate; }
};

/** Decodes a whole file into memory. Returns nullptr if the file can't be read.
    Message thread / background thread only (allocates and does file I/O). */
std::unique_ptr<SampleData> loadSample (const juce::File& file, juce::AudioFormatManager& formats);

} // namespace daw
