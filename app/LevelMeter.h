#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace daw
{

/** Vertical stereo peak meter (two bars side by side).
    Message thread only. Feed it ~30 times per second with setLevels(). */
class LevelMeter : public juce::Component
{
public:
    LevelMeter();

    /** Linear peak levels since the last call (0 = silence, 1 = 0 dBFS, may exceed 1). */
    void setLevels (float leftLinear, float rightLinear);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override; // clicking clears the clip indicators

    static constexpr float minDb = -60.0f;
    static constexpr float maxDb = 6.0f;

    /** Maps a dB value to 0..1 along the meter height (non-linear: more resolution near the top). */
    static float dbToProportion (float db) noexcept;

private:
    struct Channel
    {
        float displayDb = minDb;   // falls back smoothly
        float holdDb = minDb;      // peak-hold line
        int holdFrames = 0;
        bool clipped = false;
    };

    Channel channels[2];

    // Pixel positions from the last paint(), so setLevels() can skip redundant repaints.
    int lastDisplayPixel[2] { -1, -1 };
    int lastHoldPixel[2] { -1, -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

} // namespace daw
