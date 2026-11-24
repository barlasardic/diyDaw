#include "LevelMeter.h"
#include "DawLookAndFeel.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace daw
{

namespace
{
    constexpr int updateRateHz = 30;
    constexpr float releaseDbPerSecond = 24.0f;
    constexpr float releaseDbPerUpdate = releaseDbPerSecond / (float) updateRateHz;
    constexpr int holdUpdates = updateRateHz; // one second at 30 Hz

    constexpr float midDb = -12.0f;
    constexpr float highDb = -3.0f;

    constexpr int barGapPixels = 1;
    constexpr int maxClipHeight = 5;
    constexpr int clipBarGapPixels = 2;

    int clipHeightFor (int meterHeight)
    {
        return juce::jlimit (2, maxClipHeight, meterHeight / 8);
    }

    /** Column of one channel, below its clip indicator. */
    juce::Rectangle<int> barBounds (int width, int height, int channel)
    {
        const auto barW = juce::jmax (1, (width - barGapPixels) / 2);
        const auto x = channel == 0 ? 0 : barW + barGapPixels;
        const auto w = channel == 0 ? barW : juce::jmax (1, width - x);
        const auto top = clipHeightFor (height) + clipBarGapPixels;

        return { x, top, w, juce::jmax (0, height - top) };
    }

    juce::Rectangle<int> clipBounds (int width, int height, int channel)
    {
        const auto bar = barBounds (width, height, channel);
        return { bar.getX(), 0, bar.getWidth(), clipHeightFor (height) };
    }

    /** Y position of a dB level inside a bar (grows upwards). */
    int pixelForDb (float db, juce::Rectangle<int> bar)
    {
        const auto proportion = LevelMeter::dbToProportion (db);
        return bar.getBottom() - juce::roundToInt (proportion * (float) bar.getHeight());
    }
}

LevelMeter::LevelMeter()
{
    setOpaque (true);
    setMouseClickGrabsKeyboardFocus (false);
}

float LevelMeter::dbToProportion (float db) noexcept
{
    const auto clamped = juce::jlimit (minDb, maxDb, db);

    // Piecewise-linear so the top half of the meter covers -12..+6 dB.
    if (clamped <= midDb)
        return 0.5f * (clamped - minDb) / (midDb - minDb);

    return 0.5f + 0.5f * (clamped - midDb) / (maxDb - midDb);
}

void LevelMeter::setLevels (float leftLinear, float rightLinear)
{
    const float inputs[2] = { leftLinear, rightLinear };
    bool clipChanged = false;

    for (int i = 0; i < 2; ++i)
    {
        auto& channel = channels[i];
        const auto levelDb = juce::Decibels::gainToDecibels (inputs[i], minDb);

        if (inputs[i] > 1.0f && ! channel.clipped)
        {
            channel.clipped = true;
            clipChanged = true;
        }

        // Instant attack, steady release.
        channel.displayDb = levelDb >= channel.displayDb
                                ? levelDb
                                : juce::jmax (minDb, channel.displayDb - releaseDbPerUpdate);

        // Peak hold: sit on the peak for a second, then fall at the release rate.
        if (levelDb >= channel.holdDb)
        {
            channel.holdDb = levelDb;
            channel.holdFrames = holdUpdates;
        }
        else if (channel.holdFrames > 0)
        {
            --channel.holdFrames;
        }
        else
        {
            channel.holdDb = juce::jmax (minDb, channel.holdDb - releaseDbPerUpdate);
        }

        channel.holdDb = juce::jmax (channel.holdDb, channel.displayDb);
    }

    // Only ask for a repaint when a bar or hold line would move by a pixel.
    bool needsRepaint = clipChanged;
    const auto width = getWidth();
    const auto height = getHeight();

    for (int i = 0; i < 2; ++i)
    {
        const auto bar = barBounds (width, height, i);

        if (pixelForDb (channels[i].displayDb, bar) != lastDisplayPixel[i]
            || pixelForDb (channels[i].holdDb, bar) != lastHoldPixel[i])
            needsRepaint = true;
    }

    if (needsRepaint)
        repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);

    const auto width = getWidth();
    const auto height = getHeight();

    for (int i = 0; i < 2; ++i)
    {
        const auto bar = barBounds (width, height, i);
        const auto clip = clipBounds (width, height, i);
        auto& channel = channels[i];

        // Clip indicator.
        g.setColour (channel.clipped ? colours::meterHigh : colours::outline);
        g.fillRect (clip);
        g.setColour (channel.clipped ? colours::meterHigh.brighter (0.35f) : colours::outline);
        g.drawRect (clip, 1);

        const auto levelY = pixelForDb (channel.displayDb, bar);
        const auto holdY = pixelForDb (channel.holdDb, bar);

        // Remember what this paint shows, even if the meter is currently too small to draw.
        lastDisplayPixel[i] = levelY;
        lastHoldPixel[i] = holdY;

        if (bar.isEmpty())
            continue;

        g.setColour (colours::background);
        g.fillRect (bar);

        // Colours are tied to fixed dB ranges, so they don't stretch with the level.
        struct Segment
        {
            float lowDb, highDb;
            juce::Colour colour;
        };

        const Segment segments[] = {
            { minDb, midDb, colours::meterLow },
            { midDb, highDb, colours::meterMid },
            { highDb, maxDb, colours::meterHigh }
        };

        for (const auto& segment : segments)
        {
            const auto top = juce::jmax (pixelForDb (segment.highDb, bar), levelY);
            const auto bottom = juce::jmin (pixelForDb (segment.lowDb, bar), bar.getBottom());

            if (bottom > top)
            {
                g.setColour (segment.colour);
                g.fillRect (juce::Rectangle<int> (bar.getX(), top, bar.getWidth(), bottom - top));
            }
        }

        // Peak-hold line.
        if (channel.holdDb > minDb)
        {
            const auto lineY = juce::jlimit (bar.getY(), bar.getBottom() - 1, holdY);
            g.setColour (colours::text.withAlpha (0.85f));
            g.fillRect (juce::Rectangle<int> (bar.getX() + 1, lineY,
                                              juce::jmax (0, bar.getWidth() - 2), 1));
        }

        g.setColour (colours::outline);
        g.drawRect (bar, 1);
    }
}

void LevelMeter::mouseDown (const juce::MouseEvent&)
{
    bool hadClip = false;

    for (auto& channel : channels)
    {
        hadClip = hadClip || channel.clipped;
        channel.clipped = false;
    }

    if (hadClip)
        repaint();
}

} // namespace daw
