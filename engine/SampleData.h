#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <vector>

namespace daw
{

/** An immutable, fully decoded audio file held in memory, plus a waveform overview. */
struct SampleData
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
    juce::String name;

    /** Mono min/max overview for drawing: peaks[i] covers frames [i*framesPerPeak, (i+1)*framesPerPeak). */
    static constexpr int framesPerPeak = 256;
    std::vector<juce::Range<float>> peaks;

    int getNumFrames() const noexcept { return buffer.getNumSamples(); }
    double getLengthSeconds() const noexcept { return buffer.getNumSamples() / sampleRate; }

    /** (Re)builds 'peaks' from 'buffer'. Call after filling the buffer. */
    void computePeaks();
};

/** Decodes a whole file into memory. Returns nullptr if the file can't be read.
    Never call on the audio thread (allocates and does file I/O). Thread-safe otherwise,
    as long as 'formats' isn't modified concurrently. */
std::unique_ptr<SampleData> loadSample (const juce::File& file, juce::AudioFormatManager& formats);

} // namespace daw
