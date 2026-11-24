#include "TimeRuler.h"
#include "DawLookAndFeel.h"

#include <cmath>

namespace daw
{

TimeRuler::TimeRuler (const TimelineGeometry& g, Transport& t)
    : geometry (g), transport (t)
{
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TimeRuler::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    const auto width = (float) bounds.getWidth();
    const auto height = (float) bounds.getHeight();

    if (width <= 0.0f || height <= 0.0f)
        return;

    g.setColour (colours::panel);
    g.fillRect (bounds);

    // ---- grid ticks and bar numbers ------------------------------------
    const auto step = geometry.gridBeats();
    const auto beatsPerBar = juce::jmax (1, geometry.beatsPerBar);
    const auto endBeat = geometry.xToBeat (width);

    const auto firstIndex = (long long) std::floor (geometry.viewStartBeat / step);
    const auto lastIndex = (long long) std::ceil (endBeat / step);

    // Widen the label interval until bar numbers have room to breathe.
    auto labelStep = 1;
    while ((double) beatsPerBar * geometry.pixelsPerBeat * (double) labelStep < 48.0)
        labelStep *= 2;

    const juce::Font tickFont (juce::FontOptions (10.0f));

    for (auto index = firstIndex; index <= lastIndex; ++index)
    {
        const auto beat = (double) index * step;
        const auto x = geometry.beatToX (beat);

        if (x < -2.0f || x > width + 2.0f)
            continue;

        const auto bar = (long long) std::llround (beat / (double) beatsPerBar);
        const auto isBar = std::abs (beat - (double) bar * (double) beatsPerBar) < 1.0e-6;

        if (isBar)
        {
            g.setColour (colours::textDim.withAlpha (0.8f));
            g.fillRect (x, height - 12.0f, 1.0f, 12.0f);

            const auto barNumber = (int) bar + 1;

            if (barNumber >= 1 && (barNumber - 1) % labelStep == 0)
            {
                g.setColour (colours::textDim);
                g.setFont (tickFont);
                g.drawText (juce::String (barNumber),
                            juce::Rectangle<float> (x + 3.0f, 0.0f,
                                                    juce::jmax (0.0f, width - x - 3.0f), 12.0f),
                            juce::Justification::centredLeft, false);
            }
        }
        else
        {
            const auto onBeat = std::abs (beat - std::round (beat)) < 1.0e-6;
            const auto tickHeight = onBeat ? 6.0f : 4.0f;

            g.setColour (colours::textDim.withAlpha (onBeat ? 0.4f : 0.22f));
            g.fillRect (x, height - tickHeight, 1.0f, tickHeight);
        }
    }

    g.setColour (colours::outline);
    g.fillRect (0.0f, height - 1.0f, width, 1.0f);

    // ---- playhead marker -----------------------------------------------
    const auto playheadX = geometry.beatToX (transport.getPositionBeats());

    if (playheadX >= -6.0f && playheadX <= width + 6.0f)
    {
        juce::Path marker;
        marker.addTriangle (playheadX - 5.0f, 0.0f,
                            playheadX + 5.0f, 0.0f,
                            playheadX, 7.0f);

        g.setColour (colours::accent);
        g.fillPath (marker);
        g.fillRect (playheadX - 0.5f, 7.0f, 1.0f, juce::jmax (0.0f, height - 7.0f));
    }
}

void TimeRuler::seekToX (int x, bool snapToGrid)
{
    auto beat = geometry.xToBeat ((float) x);

    if (snapToGrid)
        beat = geometry.snap (beat);

    transport.setPositionBeats (juce::jmax (0.0, beat));
}

void TimeRuler::mouseDown (const juce::MouseEvent& e)
{
    dragMode = DragMode::undecided;
    dragStart = e.getPosition();
    lastDragY = e.y;
    playheadBeforeDrag = transport.getPositionBeats();
    setMouseCursor (juce::MouseCursor::NormalCursor);

    // Seek immediately so a plain click moves the playhead even if no drag event follows.
    // If the gesture turns out to be a vertical zoom, mouseDrag() restores the playhead.
    seekToX (e.x, ! e.mods.isAltDown());
}

void TimeRuler::mouseDrag (const juce::MouseEvent& e)
{
    const auto dy = e.y - lastDragY;
    lastDragY = e.y;

    if (dragMode == DragMode::undecided)
    {
        if (e.getDistanceFromDragStart() < 4)
            return;

        if (std::abs (e.getDistanceFromDragStartY()) > std::abs (e.getDistanceFromDragStartX()))
        {
            dragMode = DragMode::zoom;
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            transport.setPositionBeats (playheadBeforeDrag);
        }
        else
        {
            dragMode = DragMode::seek;
            setMouseCursor (juce::MouseCursor::NormalCursor);
        }
    }

    if (dragMode == DragMode::zoom)
    {
        if (dy != 0 && onZoom != nullptr)
            onZoom ((float) dragStart.x, std::exp (-(double) dy * 0.01));

        return;
    }

    seekToX (e.x, ! e.mods.isAltDown());
}

} // namespace daw
