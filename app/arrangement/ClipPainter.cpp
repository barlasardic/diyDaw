#include "ClipPainter.h"
#include "DawLookAndFeel.h"

#include <cmath>

namespace daw
{

namespace
{
    constexpr float cornerSize = 2.0f;

    /** Expands [lo, hi] with the peaks bins covering a frame range. */
    void accumulateBins (const std::vector<juce::Range<float>>& peaks,
                         double startSeconds, double endSeconds,
                         double sampleRate, int framesPerPeak,
                         float& lo, float& hi)
    {
        const auto numBins = (int) peaks.size();

        if (numBins <= 0 || endSeconds <= startSeconds || sampleRate <= 0.0)
            return;

        const auto firstFrame = (int) std::floor (startSeconds * sampleRate);
        const auto lastFrame = (int) std::ceil (endSeconds * sampleRate);

        auto firstBin = firstFrame / framesPerPeak;
        auto lastBin = (lastFrame + framesPerPeak - 1) / framesPerPeak;

        firstBin = juce::jlimit (0, numBins - 1, firstBin);
        // Always cover at least one bin, however far in we are zoomed.
        lastBin = juce::jlimit (firstBin + 1, numBins, lastBin);

        for (int bin = firstBin; bin < lastBin; ++bin)
        {
            lo = juce::jmin (lo, peaks[(size_t) bin].getStart());
            hi = juce::jmax (hi, peaks[(size_t) bin].getEnd());
        }
    }
}

namespace
{
    /** Ableton-style MIDI clip overview: notes as small bars, pitch range fitted to the body. */
    void paintNotes (juce::Graphics& g, juce::Rectangle<float> body, const ClipDrawInfo& info)
    {
        const auto& clip = info.midiClip;
        int lowest = 128, highest = -1;

        for (const auto& note : clip)
        {
            const auto p = (int) note.getProperty ("pitch");
            lowest = juce::jmin (lowest, p);
            highest = juce::jmax (highest, p);
        }

        if (highest < 0 || info.lengthBeats <= 0.0)
            return;

        // At least an octave of range so a single note isn't a full-height block.
        const auto centre = (lowest + highest) / 2;
        lowest = juce::jmin (lowest, centre - 6);
        highest = juce::jmax (highest, centre + 6);

        const auto area = body.reduced (0.0f, 3.0f);
        const auto rowHeight = area.getHeight() / (float) (highest - lowest + 1);
        const auto noteHeight = juce::jlimit (1.0f, 6.0f, rowHeight - (rowHeight > 3.0f ? 1.0f : 0.0f));
        const auto pxPerBeat = body.getWidth() / (float) info.lengthBeats;
        const auto loop = info.looping && info.loopLengthBeats > 0.0;
        const auto visible = g.getClipBounds().toFloat().getIntersection (body);

        g.setColour (info.colour.brighter (0.35f));

        // Content beat c appears at timeline beat (c - offset) + k * loopLength.
        const auto firstCycle = loop ? std::floor (info.offsetBeats / info.loopLengthBeats) : 0.0;
        const auto lastCycle = loop ? std::floor ((info.offsetBeats + info.lengthBeats) / info.loopLengthBeats) : 0.0;

        for (auto k = firstCycle; k <= lastCycle; k += 1.0)
        {
            const auto cycleStart = loop ? k * info.loopLengthBeats : 0.0;

            for (const auto& note : clip)
            {
                const auto start = (double) note.getProperty ("start");
                auto length = (double) note.getProperty ("length");
                if (loop)
                {
                    if (start >= info.loopLengthBeats)
                        continue;
                    length = juce::jmin (length, info.loopLengthBeats - start);
                }

                const auto t0 = juce::jmax (0.0, cycleStart + start - info.offsetBeats);
                const auto t1 = juce::jmin (info.lengthBeats, cycleStart + start + length - info.offsetBeats);
                if (t1 <= t0)
                    continue;

                const auto x0 = body.getX() + (float) t0 * pxPerBeat;
                const auto w = juce::jmax (1.0f, (float) (t1 - t0) * pxPerBeat - 1.0f);
                if (x0 > visible.getRight() || x0 + w < visible.getX())
                    continue;

                const auto row = highest - (int) note.getProperty ("pitch");
                g.fillRect (x0, area.getY() + (float) row * rowHeight + (rowHeight - noteHeight) * 0.5f, w, noteHeight);
            }
        }

        // Loop boundaries as faint lines.
        if (loop)
        {
            g.setColour (juce::Colours::white.withAlpha (0.18f));
            for (auto k = firstCycle + 1.0; k <= lastCycle; k += 1.0)
            {
                const auto x = body.getX() + (float) ((k * info.loopLengthBeats - info.offsetBeats) * pxPerBeat);
                if (x > body.getX() && x < body.getRight())
                    g.fillRect (x, body.getY(), 1.0f, body.getHeight());
            }
        }
    }
}

void paintClip (juce::Graphics& g, juce::Rectangle<float> bounds, const ClipDrawInfo& info)
{
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return;

    const auto visible = bounds.getIntersection (g.getClipBounds().toFloat());

    if (visible.isEmpty())
        return;

    const auto titleHeight = juce::jmin (clipTitleHeight, bounds.getHeight());
    auto body = bounds.withTrimmedTop (titleHeight);

    // Title bar in the full track colour, body darkened so the waveform reads clearly.
    g.setColour (info.colour);
    g.fillRoundedRectangle (bounds, cornerSize);

    if (body.getHeight() > 0.0f)
    {
        juce::Path bodyPath;
        bodyPath.addRoundedRectangle (body.getX(), body.getY(), body.getWidth(), body.getHeight(),
                                      cornerSize, cornerSize, false, false, true, true);
        g.setColour (info.colour.withMultipliedSaturation (0.55f).withMultipliedBrightness (0.45f));
        g.fillPath (bodyPath);
    }

    if (body.getHeight() > 2.0f)
    {
        const auto* sample = info.sample.get();

        if (info.midiClip.isValid())
        {
            paintNotes (g, body, info);
        }
        else if (sample == nullptr)
        {
            const auto loadingArea = body.reduced (4.0f, 0.0f);

            if (loadingArea.getWidth() > 1.0f)
            {
                g.setColour (info.missing ? juce::Colour (0xffff6b6b) : juce::Colours::white.withAlpha (0.65f));
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (info.missing ? "Missing file" : "Loading...", loadingArea,
                            juce::Justification::centredLeft, true);
            }
        }
        else if (! sample->peaks.empty()
                 && sample->getNumFrames() > 0
                 && sample->getLengthSeconds() > 0.0
                 && info.lengthSeconds > 0.0)
        {
            const auto sampleLength = sample->getLengthSeconds();
            const auto sampleRate = sample->sampleRate;
            const auto framesPerPeak = SampleData::framesPerPeak;
            const auto& peaks = sample->peaks;

            const auto centreY = body.getCentreY();
            const auto scaleY = juce::jmax (1.0f, body.getHeight() * 0.5f - 1.0f);

            const auto firstX = (int) std::floor (visible.getX());
            const auto lastX = (int) std::ceil (visible.getRight());

            juce::Path waveform;

            const auto timeAtX = [&] (double x)
            {
                return info.offsetSeconds + (x - (double) bounds.getX()) / (double) bounds.getWidth()
                                                * info.lengthSeconds;
            };

            for (int px = firstX; px < lastX; ++px)
            {
                const auto x0 = (double) px;
                const auto t0 = timeAtX (x0);
                const auto t1 = timeAtX (x0 + 1.0);

                if (t1 <= t0)
                    continue;

                float lo = 2.0f, hi = -2.0f;

                if (info.looping)
                {
                    if (t1 - t0 >= sampleLength)
                    {
                        accumulateBins (peaks, 0.0, sampleLength, sampleRate, framesPerPeak, lo, hi);
                    }
                    else
                    {
                        auto start = std::fmod (t0, sampleLength);
                        if (start < 0.0)
                            start += sampleLength;

                        const auto end = start + (t1 - t0);

                        if (end <= sampleLength)
                        {
                            accumulateBins (peaks, start, end, sampleRate, framesPerPeak, lo, hi);
                        }
                        else
                        {
                            accumulateBins (peaks, start, sampleLength, sampleRate, framesPerPeak, lo, hi);
                            accumulateBins (peaks, 0.0, end - sampleLength, sampleRate, framesPerPeak, lo, hi);
                        }
                    }
                }
                else
                {
                    const auto start = juce::jmax (0.0, t0);
                    const auto end = juce::jmin (t1, sampleLength);

                    if (end > start)
                        accumulateBins (peaks, start, end, sampleRate, framesPerPeak, lo, hi);
                }

                if (hi < lo) // nothing of the sample is under this column
                    continue;

                const auto top = centreY - juce::jlimit (-1.0f, 1.0f, hi) * scaleY;
                auto bottom = centreY - juce::jlimit (-1.0f, 1.0f, lo) * scaleY;

                if (bottom - top < 1.0f)
                    bottom = top + 1.0f;

                const auto x = (float) px + 0.5f;
                waveform.startNewSubPath (x, top);
                waveform.lineTo (x, bottom);
            }

            if (! waveform.isEmpty())
            {
                g.setColour (info.selected ? juce::Colours::white.withAlpha (0.92f)
                                           : info.colour.brighter (0.8f).withAlpha (0.9f));
                g.strokePath (waveform, juce::PathStrokeType (1.0f));
            }

            // Dashed marks where the audio wraps back to the sample start.
            if (info.looping)
            {
                const auto spacingPixels = sampleLength / info.lengthSeconds * (double) bounds.getWidth();

                if (spacingPixels >= 4.0)
                {
                    const float dashes[] = { 2.0f, 3.0f };
                    const auto y0 = body.getY() + 2.0f;
                    const auto y1 = body.getBottom() - 2.0f;

                    g.setColour (juce::Colours::white.withAlpha (0.3f));

                    for (auto wrap = std::ceil (info.offsetSeconds / sampleLength);; ++wrap)
                    {
                        const auto at = wrap * sampleLength - info.offsetSeconds;

                        if (at > info.lengthSeconds)
                            break;

                        if (at > 0.0)
                        {
                            const auto x = bounds.getX()
                                           + (float) (at / info.lengthSeconds) * bounds.getWidth();

                            if (x > visible.getX() && x < visible.getRight())
                                g.drawDashedLine ({ x, y0, x, y1 }, dashes, 2, 1.0f);
                        }
                    }
                }
            }
        }
    }

    // Clip name in the title bar.
    const auto titleArea = bounds.withHeight (titleHeight).reduced (4.0f, 0.0f);

    if (titleArea.getWidth() > 1.0f && titleArea.getHeight() > 1.0f)
    {
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (info.name, titleArea, juce::Justification::centredLeft, true);
    }

    if (bounds.getWidth() > 2.0f && bounds.getHeight() > 2.0f)
    {
        g.setColour (info.selected ? juce::Colours::white.withAlpha (0.92f) : colours::outline);
        g.drawRoundedRectangle (bounds.reduced (info.selected ? 0.75f : 0.5f), cornerSize,
                                info.selected ? 1.5f : 1.0f);
    }
}

} // namespace daw
