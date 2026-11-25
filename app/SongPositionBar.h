#pragma once

#include "Project.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** FL Studio-style song position bar: a thin strip spanning the whole song (or, in pattern
    mode, the pattern), filled up to the playhead, with a handle you can grab. Clicking jumps,
    dragging scrubs (the playhead follows the mouse, snapped to 16ths; Alt = free).

    Message thread only. */
class SongPositionBar : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    explicit SongPositionBar (Project& project);

    /** The beat range the bar spans. Song mode: [0, end of song]; pattern mode: the clip. */
    void setRange (double startBeat, double endBeat, bool isPattern);

    /** Called ~30 Hz: moves the handle. */
    void refresh();

    static constexpr int preferredHeight = 18;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> trackArea() const;
    float beatToX (double beat) const;
    double xToBeat (float x) const;
    void seekTo (const juce::MouseEvent&);

    Project& project;
    double rangeStart = 0.0, rangeEnd = 16.0;
    bool pattern = false;
    double shownBeat = -1.0;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongPositionBar)
};

} // namespace daw
