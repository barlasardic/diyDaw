#pragma once

#include "SampleData.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace daw
{

/** Everything needed to draw one audio clip. */
struct ClipDrawInfo
{
    juce::String name;
    juce::Colour colour;                        // the track's colour
    std::shared_ptr<const SampleData> sample;   // nullptr while the file is still decoding
    double offsetSeconds = 0.0;                 // audio time at the clip's left edge
    double lengthSeconds = 0.0;                 // audio time spanned by the clip's width
    bool looping = false;                       // audio wraps to the sample start when it runs out
    bool selected = false;
    bool missing = false;                       // sample == nullptr because the file couldn't be loaded
};

/** Ableton-style clip: a title bar in the track colour with the clip name, and a body
    showing the waveform. 'bounds' is the full clip rectangle (it may extend outside the
    graphics context's clip region; only the visible part needs to be drawn).
    Thread: message thread (paint). */
void paintClip (juce::Graphics& g, juce::Rectangle<float> bounds, const ClipDrawInfo& info);

/** Height of the clip title bar in pixels. */
inline constexpr float clipTitleHeight = 15.0f;

} // namespace daw
