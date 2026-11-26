#pragma once

#include "Instrument.h"
#include "ParamInfo.h"
#include "RealtimeSwap.h"
#include "SampleData.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace daw
{

/** Ableton-style drum rack (basic): 16 sample pads played by MIDI notes 36..51 (C1..D#2;
    pad 0 = note 36).

    Pads are one-shots: a note-on starts the pad's sample from the beginning (note-offs are
    ignored); the sound ends when the sample ends or when its decay envelope fades it out.
    Per pad: gain (dB), pan, tune (semitones, resampled), decay (time to fade to silence,
    exponential-ish; max = no fade), choke group (1..8: a pad cuts every other sounding pad
    of the same group with a ~5 ms fade; 0 = none). Velocity scales the level (linear).

    Polyphony: 32 voices; retriggering a pad keeps its previous hit ringing (until choke or
    voice stealing). Stealing takes the oldest voice with a short fade, never clicks.

    Threading: setKit()/collectGarbage() message thread; setParam/getParam any thread;
    prepare/process/allNotesOff/reset as in Instrument. */
class DrumRack : public Instrument
{
public:
    static constexpr int numPads = 16;
    static constexpr int firstNote = 36;
    static constexpr int numVoices = 32;

    enum PadParam { gain, pan, tune, decay, choke, numPadParams };
    static constexpr int numParams = numPads * numPadParams;

    /** numParams entries: pad-major, ids "pad<N><Param>" with N = 1..16 ("pad1Gain", "pad1Pan",
        "pad1Tune", "pad1Decay", "pad1Choke", "pad2Gain", ...); names "Gain", "Pan", ... */
    static const std::vector<ParamInfo>& getParamInfos();
    static constexpr int paramIndex (int pad, PadParam p) noexcept { return pad * numPadParams + p; }

    /** The samples on the pads (nullptr = empty pad). */
    struct Kit
    {
        std::array<std::shared_ptr<const SampleData>, numPads> samples;
    };

    DrumRack();
    ~DrumRack() override;

    // ---- message thread ---------------------------------------------------------
    void setKit (std::unique_ptr<Kit> kit);
    void collectGarbage();

    // ---- Instrument -----------------------------------------------------------------
    void setParam (int index, float value) noexcept override;
    float getParam (int index) const noexcept override;
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::AudioBuffer<float>& out, int numSamples,
                  const NoteEvent* events, int numEvents) noexcept override;
    void allNotesOff() noexcept override;
    void reset() noexcept override;

    int getNumActiveVoices() const noexcept;

private:
    struct Impl;
    std::array<std::atomic<float>, numParams> params;
    RealtimeSwap<Kit> kit;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (DrumRack)
};

} // namespace daw
