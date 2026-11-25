#include "AudioTrack.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace daw
{

void AudioTrack::prepare (double sampleRate, int maxBlockSize)
{
    synth.prepare (sampleRate, maxBlockSize);
    synth.reset();

    if (auto* chain = effects.acquire()) // audio is stopped while preparing
        for (auto& e : chain->effects)
            e->prepare (sampleRate, maxBlockSize);
    noteEndBeat.fill (-1.0);
    noteOnIndex.fill (-1);
    numEvents = 0;
    expectedStartBeat = std::numeric_limits<double>::quiet_NaN();

    scratch.setSize (2, maxBlockSize, false, false, true);
    leftGain.reset (sampleRate, 0.02);
    rightGain.reset (sampleRate, 0.02);
    leftGain.setCurrentAndTargetValue (0.0f);
    rightGain.setCurrentAndTargetValue (0.0f);
}

void AudioTrack::renderClip (const Clip& clip, const Transport::Block& block, int n) noexcept
{
    const auto* s = clip.sample.get();
    if (s == nullptr || s->getNumFrames() < 2 || clip.lengthBeats <= 0.0)
        return;

    // Sample index (within this block) at which a beat position is reached. The tolerance
    // keeps edges that land exactly on a sample stable against floating-point drift.
    const auto indexOf = [&block, n] (double beat)
    {
        const auto x = std::ceil ((beat - block.startBeat) / block.beatsPerSample - 1.0e-3);
        return (int) juce::jlimit (0.0, (double) n, x);
    };

    const auto first = indexOf (clip.startBeat);
    const auto last = indexOf (clip.endBeat());
    if (first >= last)
        return;

    const auto frames = s->getNumFrames();
    const auto secondsPerBeat = 60.0 / block.bpm;
    const auto secondsPerSample = 1.0 / block.sampleRate;
    const auto clipSeconds = clip.lengthBeats * secondsPerBeat;

    // Time since the clip started, at sample 'first'.
    auto elapsed = juce::jmax (0.0, (block.beatAt (first) - clip.startBeat) * secondsPerBeat);

    const auto* srcL = s->buffer.getReadPointer (0);
    const auto* srcR = s->buffer.getReadPointer (juce::jmin (1, s->buffer.getNumChannels() - 1));
    auto* dstL = scratch.getWritePointer (0);
    auto* dstR = scratch.getWritePointer (1);

    for (int i = first; i < last; ++i, elapsed += secondsPerSample)
    {
        auto pos = (elapsed + clip.offsetSeconds) * s->sampleRate;

        if (clip.looping)
        {
            pos = std::fmod (pos, (double) frames);
            if (pos < 0.0)
                pos += frames;
        }
        else if (pos >= frames - 1)
            break;
        else if (pos < 0.0)
            continue;

        const auto fade = juce::jmin (1.0, elapsed / clipFadeSeconds, (clipSeconds - elapsed) / clipFadeSeconds);
        const auto g = clip.gain * (float) juce::jmax (0.0, fade);

        const auto i0 = (int) pos;
        const auto i1 = i0 + 1 < frames ? i0 + 1 : 0; // wraps only when looping
        const auto frac = (float) (pos - i0);

        dstL[i] += g * (srcL[i0] + frac * (srcL[i1] - srcL[i0]));
        dstR[i] += g * (srcR[i0] + frac * (srcR[i1] - srcR[i0]));
    }
}

void AudioTrack::process (juce::AudioBuffer<float>& mixBus, const Transport::Block& block, bool anyTrackSoloed) noexcept
{
    const auto n = juce::jmin (block.numSamples, scratch.getNumSamples());
    scratch.clear (0, n);

    const auto* list = clips.acquire();

    if (list != nullptr && block.playing && block.beatsPerSample > 0.0)
    {
        const auto blockEnd = block.endBeat();

        for (const auto& clip : list->clips)
        {
            if (clip.startBeat >= blockEnd)
                break; // sorted: nothing later can overlap this block

            if (clip.endBeat() > block.startBeat && ! clip.isMidi())
                renderClip (clip, block, n);
        }
    }

    if (instrumentEnabled.load (std::memory_order_relaxed))
        renderInstrument (list, block, n);

    if (auto* chain = effects.acquire())
        for (auto& e : chain->effects)
            e->process (scratch, n, block);

    // Fader + pan + mute/solo, smoothed to avoid zipper noise and clicks.
    const auto db = gainDb.load (std::memory_order_relaxed);
    const auto silent = mute.load (std::memory_order_relaxed)
                     || (anyTrackSoloed && ! solo.load (std::memory_order_relaxed))
                     || db <= minGainDb;

    const auto g = silent ? 0.0f : juce::Decibels::decibelsToGain (db);
    const auto angle = (pan.load (std::memory_order_relaxed) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    leftGain.setTargetValue (g * std::cos (angle));
    rightGain.setTargetValue (g * std::sin (angle));

    leftGain.applyGain (scratch.getWritePointer (0), n);
    rightGain.applyGain (scratch.getWritePointer (1), n);

    meter.push (scratch, n);

    for (int ch = 0; ch < juce::jmin (2, mixBus.getNumChannels()); ++ch)
        mixBus.addFrom (ch, 0, scratch, ch, 0, n);
}

// ---- MIDI --------------------------------------------------------------------------

void AudioTrack::pushLiveNote (int pitch, float velocity, bool isNoteOn)
{
    if (pitch < 0 || pitch > 127)
        return;

    const std::scoped_lock lock (liveMutex);
    const auto scope = liveFifo.write (1);

    if (scope.blockSize1 > 0)
        liveQueue[(size_t) scope.startIndex1] = { 0, pitch, juce::jlimit (0.0f, 1.0f, velocity), isNoteOn };
}

void AudioTrack::addEvent (int offset, int pitch, float velocity, bool isNoteOn) noexcept
{
    if (numEvents < maxEventsPerBlock)
        events[(size_t) numEvents++] = { offset, pitch, velocity, isNoteOn };
}

void AudioTrack::collectMidiClip (const Clip& clip, const Transport::Block& block, int n) noexcept
{
    const auto& notes = clip.notes->notes;
    if (notes.empty())
        return;

    // A note belongs to the block whose sample index (same rounding as audio clips) it lands on,
    // so search one sample beyond the block on both sides and keep only indices in [0, n).
    const auto t0 = juce::jmax (block.startBeat - block.beatsPerSample, clip.startBeat);
    const auto t1 = juce::jmin (block.endBeat() + block.beatsPerSample, clip.endBeat());
    if (t1 <= t0)
        return;

    const auto indexOf = [&block] (double beat)
    {
        return (int) std::ceil ((beat - block.startBeat) / block.beatsPerSample - 1.0e-3);
    };

    // Plays the notes whose content start lies in [c0, c1); 'shift' maps content to timeline,
    // 'cutAt' is the content position where notes are cut (loop end), if any.
    const auto playRange = [&] (double c0, double c1, double shift, double cutAt)
    {
        auto it = std::lower_bound (notes.begin(), notes.end(), c0,
                                    [] (const MidiNote& note, double c) { return note.startBeat < c; });

        for (; it != notes.end() && it->startBeat < c1; ++it)
        {
            const auto& note = *it;
            if (note.pitch < 0 || note.pitch > 127 || note.lengthBeats <= 0.0)
                continue;

            const auto onBeat = note.startBeat + shift;
            const auto offBeat = juce::jmin (juce::jmin (note.startBeat + note.lengthBeats, cutAt) + shift, clip.endBeat());
            const auto index = indexOf (onBeat);
            if (index < 0 || index >= n)
                continue; // played by the previous or next block

            const auto p = (size_t) note.pitch;

            if (noteEndBeat[p] >= 0.0)
            {
                // The previous note of this pitch ends at its own end, or at this onset if it
                // would still be sounding - and never before its own note-on.
                const auto earliest = noteOnIndex[p] >= 0 ? noteOnIndex[p] + 1 : 0;
                const auto offIndex = juce::jlimit (juce::jmin (earliest, index), index, indexOf (noteEndBeat[p]));
                addEvent (offIndex, note.pitch, 0.0f, false);
            }

            addEvent (index, note.pitch, note.velocity, true);
            noteEndBeat[p] = offBeat;
            noteOnIndex[p] = index;
        }
    };

    const auto u0 = clip.offsetBeats + (t0 - clip.startBeat); // unwrapped content positions
    const auto u1 = clip.offsetBeats + (t1 - clip.startBeat);
    const auto shift = clip.startBeat - clip.offsetBeats;

    if (! clip.looping || clip.loopLengthBeats <= 0.0)
    {
        playRange (u0, u1, shift, std::numeric_limits<double>::max());
        return;
    }

    const auto len = clip.loopLengthBeats;
    for (auto m = std::floor (u0 / len); m * len < u1; m += 1.0)
    {
        const auto loopStart = m * len;
        playRange (juce::jmax (u0, loopStart) - loopStart, juce::jmin (u1, loopStart + len) - loopStart,
                   shift + loopStart, len);
    }
}

void AudioTrack::renderInstrument (const ClipList* list, const Transport::Block& block, int n) noexcept
{
    numEvents = 0;
    noteOnIndex.fill (-1);

    // A stop, a jump or a tempo/position discontinuity ends every sequenced note.
    const auto continuous = block.playing && juce::exactlyEqual (block.startBeat, expectedStartBeat);
    expectedStartBeat = block.playing ? block.endBeat() : std::numeric_limits<double>::quiet_NaN();

    if (! continuous)
    {
        for (int p = 0; p < 128; ++p)
            if (noteEndBeat[(size_t) p] >= 0.0)
            {
                addEvent (0, p, 0.0f, false);
                noteEndBeat[(size_t) p] = -1.0;
            }
    }

    if (block.playing && block.beatsPerSample > 0.0 && list != nullptr)
    {
        const auto blockEnd = block.endBeat();

        for (const auto& clip : list->clips)
        {
            if (clip.startBeat >= blockEnd)
                break;

            if (clip.isMidi() && clip.endBeat() > block.startBeat)
                collectMidiClip (clip, block, n);
        }

        // End the notes that finish inside this block. A note-off never comes before its own
        // note-on: a note shorter than a sample ends one sample later (or in the next block).
        for (int p = 0; p < 128; ++p)
        {
            const auto end = noteEndBeat[(size_t) p];
            if (end < 0.0 || end >= blockEnd)
                continue;

            auto index = (int) juce::jlimit (0.0, (double) n,
                                             std::ceil ((end - block.startBeat) / block.beatsPerSample - 1.0e-3));
            if (index <= noteOnIndex[(size_t) p])
                index = noteOnIndex[(size_t) p] + 1;

            if (index >= n)
            {
                noteEndBeat[(size_t) p] = blockEnd; // ends at sample 0 of the next block
                continue;
            }

            addEvent (index, p, 0.0f, false);
            noteEndBeat[(size_t) p] = -1.0;
        }
    }

    // Sequenced events: stable insertion sort (no allocation, and the list is short and nearly
    // sorted) by offset; at equal offsets note-offs go first so a repeated note retriggers.
    const auto before = [] (const NoteEvent& a, const NoteEvent& b)
    {
        if (a.sampleOffset != b.sampleOffset)
            return a.sampleOffset < b.sampleOffset;
        return ! a.isNoteOn && b.isNoteOn;
    };

    for (int i = 1; i < numEvents; ++i)
    {
        const auto e = events[(size_t) i];
        auto j = i;
        for (; j > 0 && before (e, events[(size_t) (j - 1)]); --j)
            events[(size_t) j] = events[(size_t) (j - 1)];
        events[(size_t) j] = e;
    }

    // Live notes play at the start of the block, after the sequencer's offset-0 events and in
    // *arrival order*: a quick click sends note-on then note-off within one block, and
    // reordering those (offs first) would leave the note hanging.
    {
        auto insertAt = 0;
        while (insertAt < numEvents && events[(size_t) insertAt].sampleOffset == 0)
            ++insertAt;

        const auto scope = liveFifo.read (liveFifo.getNumReady());
        const auto numLive = juce::jmin (scope.blockSize1 + scope.blockSize2, maxEventsPerBlock - numEvents);

        // Make room after the offset-0 group, then copy the live events in order.
        for (int i = numEvents - 1; i >= insertAt; --i)
            events[(size_t) (i + numLive)] = events[(size_t) i];

        for (int i = 0; i < numLive; ++i)
        {
            const auto index = i < scope.blockSize1 ? scope.startIndex1 + i : scope.startIndex2 + (i - scope.blockSize1);
            const auto& live = liveQueue[(size_t) index];
            events[(size_t) (insertAt + i)] = { 0, live.pitch, live.velocity, live.isNoteOn };
        }

        numEvents += numLive;
    }

    synth.process (scratch, n, events.data(), numEvents);
}

} // namespace daw
