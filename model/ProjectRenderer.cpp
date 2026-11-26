#include "ProjectRenderer.h"

#include <cmath>
#include <random>

namespace daw
{

ProjectRenderer::ProjectRenderer (Project& source, const ExportSettings& s)
    : settings (s), copy (engine, source.getSamplePool())
{
    copy.setPluginHost (source.getPluginHost()); // the copy loads its own plugin instances
    copy.copyFrom (source);

    engine.prepare (settings.sampleRate, blockSize);
    engine.getMetronome().setEnabled (settings.includeMetronome);

    const auto beats = juce::jmax (0.0, settings.endBeat - settings.startBeat);
    lengthInSamples = (int64_t) std::llround (copy.beatsToSeconds (beats) * settings.sampleRate);
}

ProjectRenderer::~ProjectRenderer() = default;

double ProjectRenderer::getProjectEndBeat (const Project& project)
{
    auto end = 0.0;

    for (const auto& track : project.getState())
        for (const auto& clip : track)
            if (Project::isClip (clip))
                end = juce::jmax (end, Project::getClipEnd (clip));

    return end;
}

void ProjectRenderer::rewind()
{
    auto& transport = engine.getTransport();
    transport.stop();
    engine.prepare (settings.sampleRate, blockSize);

    // Seek first so automation is evaluated at the start position during the warm-up.
    transport.setPositionBeats (settings.startBeat);

    // Faders smooth in from silence after prepare(); run the stopped engine for longer than
    // the 20 ms smoothing time so the export starts at full level, not with a fade-in.
    juce::AudioBuffer<float> warmUp (2, blockSize);
    for (int done = 0; done < (int) (settings.sampleRate * 0.05); done += blockSize)
        engine.render (warmUp, blockSize);

    transport.setPositionBeats (settings.startBeat);
    transport.play();
}

template <typename Consumer>
bool ProjectRenderer::renderBlocks (Progress& progress, double progressScale, double progressOffset, Consumer&& consume)
{
    rewind();

    juce::AudioBuffer<float> block (2, blockSize);

    // Fixed tail length, or the auto-tail limit (it may end earlier at silence).
    const auto total = lengthInSamples + getMaxTailSamples();
    const auto silenceGain = juce::Decibels::decibelsToGain (ExportSettings::silenceThresholdDb);
    const auto silenceNeeded = (int64_t) (0.25 * settings.sampleRate);
    int64_t silentFor = 0;

    auto reportedEnd = false;

    for (int64_t pos = 0, n = 0; pos < total; pos += n)
    {
        // At the end of the range: stop, so notes end and clips stop, and let effects ring out.
        if (pos == lengthInSamples)
            engine.getTransport().stop();

        n = juce::jmin ((int64_t) blockSize, total - pos);
        if (pos < lengthInSamples)
            n = juce::jmin (n, lengthInSamples - pos); // the stop lands on the exact sample

        const auto numSamples = (int) n;
        engine.render (block, numSamples);

        if (settings.autoTail && pos >= lengthInSamples)
        {
            const auto peak = juce::jmax (block.getMagnitude (0, 0, numSamples), block.getMagnitude (1, 0, numSamples));
            silentFor = peak < silenceGain ? silentFor + n : 0;
        }

        consume (block, pos, numSamples);

        const auto fraction = juce::jmin (1.0, (double) (pos + n) / (double) total);
        reportedEnd = fraction >= 1.0;

        if (progress && ! progress (progressOffset + progressScale * fraction))
            return false;

        if (settings.autoTail && silentFor >= silenceNeeded)
            break;
    }

    if (progress && ! reportedEnd) // e.g. the auto tail ended early
        progress (progressOffset + progressScale);

    return true;
}

int64_t ProjectRenderer::getMaxTailSamples() const noexcept
{
    const auto seconds = settings.autoTail ? ExportSettings::maxAutoTailSeconds : juce::jmax (0.0, settings.tailSeconds);
    return (int64_t) std::llround (seconds * settings.sampleRate);
}

juce::AudioBuffer<float> ProjectRenderer::render (Progress progress)
{
    // Sized for the longest possible tail once, trimmed to what was rendered at the end.
    juce::AudioBuffer<float> result (2, (int) (lengthInSamples + getMaxTailSamples()));
    result.clear();
    int64_t rendered = 0;

    renderBlocks (progress, 1.0, 0.0, [&result, &rendered] (const juce::AudioBuffer<float>& block, int64_t pos, int n)
    {
        for (int ch = 0; ch < 2; ++ch)
            result.copyFrom (ch, (int) pos, block, ch, 0, n);

        rendered = pos + n;
    });

    if (rendered < result.getNumSamples())
        result.setSize (2, (int) juce::jmax (rendered, lengthInSamples), true, false, true);

    return result;
}

juce::Result ProjectRenderer::renderToFile (const juce::File& file, Progress progress)
{
    if (lengthInSamples <= 0)
        return juce::Result::fail ("Nothing to export: the range is empty.");

    if (settings.bitDepth != 16 && settings.bitDepth != 24 && settings.bitDepth != 32)
        return juce::Result::fail ("Unsupported bit depth: " + juce::String (settings.bitDepth));

    // Pass 1 (only when normalising): find the peak.
    auto gain = 1.0f;

    if (settings.normalise)
    {
        auto peak = 0.0f;
        const auto ok = renderBlocks (progress, 0.5, 0.0, [&peak] (const juce::AudioBuffer<float>& block, int64_t, int n)
        {
            for (int ch = 0; ch < 2; ++ch)
                peak = juce::jmax (peak, block.getMagnitude (ch, 0, n));
        });

        if (! ok)
            return juce::Result::fail ("Export cancelled.");

        if (peak > 0.0f)
            gain = juce::Decibels::decibelsToGain (normaliseTargetDb) / peak;
    }

    if (! file.getParentDirectory().createDirectory())
        return juce::Result::fail ("Couldn't create " + file.getParentDirectory().getFullPathName());

    juce::TemporaryFile temp (file);
    std::unique_ptr<juce::OutputStream> stream = temp.getFile().createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail ("Couldn't write to " + file.getParentDirectory().getFullPathName());

    using Format = juce::AudioFormatWriterOptions::SampleFormat;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (settings.sampleRate)
                             .withNumChannels (2)
                             .withBitsPerSample (settings.bitDepth)
                             .withSampleFormat (settings.bitDepth == 32 ? Format::floatingPoint : Format::integral);

    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor (stream, options);
    if (writer == nullptr)
        return juce::Result::fail ("Couldn't create a WAV writer for these settings.");

    // TPDF dither at 16 bit: the sum of two uniform +-0.5 LSB noises.
    const auto dither = settings.bitDepth == 16;
    const auto lsb = 1.0f / 32768.0f;
    std::minstd_rand rng (12345);
    std::uniform_real_distribution<float> uniform (-0.5f, 0.5f);

    auto writeOk = true;
    const auto rendered = renderBlocks (progress, settings.normalise ? 0.5 : 1.0, settings.normalise ? 0.5 : 0.0,
                                        [&] (juce::AudioBuffer<float>& block, int64_t, int n)
    {
        if (! writeOk)
            return;

        block.applyGain (0, n, gain);

        if (dither)
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* data = block.getWritePointer (ch);
                for (int i = 0; i < n; ++i)
                    data[i] += (uniform (rng) + uniform (rng)) * lsb;
            }

        writeOk = writer->writeFromAudioSampleBuffer (block, 0, n);
    });

    writer.reset(); // flushes and closes the stream

    if (! rendered)
        return juce::Result::fail ("Export cancelled.");

    if (! writeOk)
        return juce::Result::fail ("Writing the audio data failed (disk full?).");

    if (! temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Couldn't replace " + file.getFullPathName());

    return juce::Result::ok();
}

} // namespace daw
