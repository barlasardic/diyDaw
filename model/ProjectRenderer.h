#pragma once

#include "Project.h"

#include <functional>

namespace daw
{

/** Settings for rendering a project to an audio file. */
struct ExportSettings
{
    double startBeat = 0.0;
    double endBeat = 0.0;          // must be > startBeat
    double sampleRate = 44100.0;
    int bitDepth = 24;             // 16 or 24 (PCM, 16 is TPDF-dithered) or 32 (float)
    bool normalise = false;        // scale so the loudest sample peaks at -0.3 dBFS
    bool includeMetronome = false;
};

/** Offline "bounce" of a project, independent of the live engine and audio device.

    The constructor (message thread) copies the project's current state - tracks, clips,
    mixer and tempo - into a private Engine. After that the renderer touches nothing shared,
    so render()/renderToFile() can run on a background thread while the app keeps playing.
    Destroy it on the message thread.
*/
class ProjectRenderer
{
public:
    /** Returns false to cancel. Called with progress 0..1 (from the rendering thread). */
    using Progress = std::function<bool (double)>;

    ProjectRenderer (Project& source, const ExportSettings& settings);
    ~ProjectRenderer();

    int64_t getLengthInSamples() const noexcept { return lengthInSamples; }

    /** Renders the whole range into memory (stereo, float, no normalising or dither). */
    juce::AudioBuffer<float> render (Progress progress = {});

    /** Renders and writes a stereo WAV file. Writes to a temporary file first, so a failed
        or cancelled export never leaves a half-written file behind. */
    juce::Result renderToFile (const juce::File& file, Progress progress = {});

    /** End of the last clip in the project, in beats (0 if there are none). */
    static double getProjectEndBeat (const Project& project);

    static constexpr float normaliseTargetDb = -0.3f;

private:
    /** Restarts the private engine at the start of the range. */
    void rewind();

    /** Renders the next block (at most blockSize). Returns false if cancelled. */
    template <typename Consumer>
    bool renderBlocks (Progress& progress, double progressScale, double progressOffset, Consumer&& consume);

    static constexpr int blockSize = 1024;

    ExportSettings settings;
    Engine engine;
    Project copy;
    int64_t lengthInSamples = 0;

    JUCE_DECLARE_NON_COPYABLE (ProjectRenderer)
};

} // namespace daw
