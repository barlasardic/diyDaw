#include "SongPositionBar.h"
#include "DawLookAndFeel.h"

#include <cmath>

namespace daw
{

SongPositionBar::SongPositionBar (Project& p) : project (p)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Song position: click to jump, drag to scrub (Alt = no snap)");
}

void SongPositionBar::setRange (double startBeat, double endBeat, bool isPattern)
{
    endBeat = juce::jmax (startBeat + 1.0, endBeat);

    if (! juce::exactlyEqual (startBeat, rangeStart) || ! juce::exactlyEqual (endBeat, rangeEnd) || isPattern != pattern)
    {
        rangeStart = startBeat;
        rangeEnd = endBeat;
        pattern = isPattern;
        repaint();
    }
}

void SongPositionBar::refresh()
{
    const auto beat = project.getEngine().getTransport().getPositionBeats();
    if (std::abs (beatToX ((float) beat) - beatToX ((float) shownBeat)) >= 0.5f)
    {
        shownBeat = beat;
        repaint();
    }
}

juce::Rectangle<float> SongPositionBar::trackArea() const
{
    return getLocalBounds().toFloat().reduced (8.0f, 0.0f).withTrimmedLeft (52.0f);
}

float SongPositionBar::beatToX (double beat) const
{
    const auto area = trackArea();
    const auto t = (beat - rangeStart) / (rangeEnd - rangeStart);
    return area.getX() + (float) juce::jlimit (0.0, 1.0, t) * area.getWidth();
}

double SongPositionBar::xToBeat (float x) const
{
    const auto area = trackArea();
    const auto t = juce::jlimit (0.0f, 1.0f, (x - area.getX()) / juce::jmax (1.0f, area.getWidth()));
    return rangeStart + t * (rangeEnd - rangeStart);
}

void SongPositionBar::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);

    const auto bounds = getLocalBounds().toFloat();
    const auto area = trackArea();
    const auto beatsPerBar = juce::jmax (1, project.getEngine().getTransport().getBeatsPerBar());

    // Mode label on the left.
    g.setColour (pattern ? colours::accent : colours::textDim);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText (pattern ? "PATTERN" : "SONG", bounds.withWidth (56.0f).withTrimmedLeft (6.0f),
                juce::Justification::centredLeft);

    // Groove.
    const auto groove = area.withSizeKeepingCentre (area.getWidth(), 6.0f);
    g.setColour (colours::panelRaised);
    g.fillRoundedRectangle (groove, 3.0f);

    // Bar ticks (thinned out so they never crowd).
    const auto bars = (rangeEnd - rangeStart) / beatsPerBar;
    auto step = 1;
    while (bars / step * 6.0 > area.getWidth())
        step *= 2;

    g.setColour (colours::outline.brighter (0.2f));
    const auto firstBar = (int) std::ceil (rangeStart / beatsPerBar);
    for (auto bar = firstBar; bar * beatsPerBar <= rangeEnd; bar += step)
        g.fillRect (beatToX (bar * beatsPerBar), groove.getBottom() + 1.0f, 1.0f, 3.0f);

    // Progress.
    const auto x = beatToX (shownBeat);
    g.setColour ((pattern ? colours::accent : colours::play).withAlpha (0.55f));
    g.fillRoundedRectangle (groove.withRight (x), 3.0f);

    // Handle.
    const auto handle = juce::Rectangle<float> (12.0f, (float) getHeight() - 4.0f).withCentre ({ x, bounds.getCentreY() });
    g.setColour (dragging ? colours::text : colours::text.darker (0.15f));
    g.fillRoundedRectangle (handle, 2.0f);
    g.setColour (colours::background);
    g.fillRect (handle.withSizeKeepingCentre (1.0f, handle.getHeight() - 6.0f));

    g.setColour (colours::outline);
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void SongPositionBar::seekTo (const juce::MouseEvent& e)
{
    auto beat = xToBeat (e.position.x);
    if (! e.mods.isAltDown())
        beat = std::round (beat * 4.0) / 4.0;

    beat = juce::jlimit (rangeStart, rangeEnd, beat);
    project.getEngine().getTransport().setPositionBeats (beat);
    shownBeat = beat;
    repaint();
}

void SongPositionBar::mouseDown (const juce::MouseEvent& e)
{
    dragging = true;
    seekTo (e);
}

void SongPositionBar::mouseDrag (const juce::MouseEvent& e)
{
    seekTo (e);
}

void SongPositionBar::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

} // namespace daw
