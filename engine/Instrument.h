#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace daw
{

/** One note event inside an audio block, at a sample-accurate position. */
struct NoteEvent
{
    int sampleOffset = 0;   // 0 <= sampleOffset < numSamples of the block
    int pitch = 60;         // MIDI note number 0..127
    float velocity = 0.0f;  // 0..1 (note-on); ignored for note-off
    bool isNoteOn = true;
};

/** A sound generator played by notes (a MIDI track's device).

    Threading: prepare() is called with the audio thread stopped (or from the thread that
    renders, before rendering); process(), allNotesOff() and reset() are audio-thread and
    must be real-time safe. Parameter access (in subclasses) must be atomic. */
class Instrument
{
public:
    virtual ~Instrument() = default;

    virtual void prepare (double sampleRate, int maxBlockSize) = 0;

    /** Renders numSamples and *adds* them into out's first two channels. 'events' are sorted
        by sampleOffset; each takes effect exactly at its offset. */
    virtual void process (juce::AudioBuffer<float>& out, int numSamples,
                          const NoteEvent* events, int numEvents) noexcept = 0;

    /** Releases every sounding note (they fade out with their normal release). */
    virtual void allNotesOff() noexcept = 0;

    /** Silences everything immediately (e.g. on a transport jump). */
    virtual void reset() noexcept = 0;
};

} // namespace daw
