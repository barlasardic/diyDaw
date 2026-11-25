#pragma once

#include "Instrument.h"
#include "ParamInfo.h"

#include <array>
#include <atomic>

namespace daw
{

/** Polyphonic subtractive synth (Ableton "Analog"-style, simplified).

    Per voice: two band-limited oscillators (saw / square / triangle / sine), oscillator mix,
    a resonant low-pass filter with its own ADSR and key tracking, and an amp ADSR.
    16 voices; a new note steals a voice already playing the same pitch, else a free voice,
    else the oldest released voice, else the oldest voice (with a short fade, never a click).

    Parameters are described by getParamInfos() so the model can store them by id and the UI
    can build controls generically. setParam/getParam are thread-safe (atomics); the audio
    thread smooths continuous parameters (cutoff, volume, mix) so changes never zipper. */
class Synth : public Instrument
{
public:
    enum Param
    {
        osc1Wave,        // 0 saw, 1 square, 2 triangle, 3 sine (stepped)
        osc2Wave,        // same
        osc2Semitones,   // -24..+24 (stepped)
        osc2Detune,      // cents -50..+50
        oscMix,          // 0 = only osc1, 1 = only osc2
        filterCutoff,    // Hz, 20..20000
        filterResonance, // 0..1 (1 = just below self-oscillation)
        filterEnvAmount, // -1..+1, scaled to +-6 octaves of cutoff modulation
        filterKeyTrack,  // 0..1 (1 = cutoff follows the note pitch 1:1)
        filterAttack, filterDecay, filterSustain, filterRelease, // s, s, 0..1, s
        ampAttack, ampDecay, ampSustain, ampRelease,             // s, s, 0..1, s
        velocitySens,    // 0..1: how much velocity scales the amplitude
        volume,          // dB, -60..+6
        numParams
    };

    using ParamInfo = daw::ParamInfo;

    static const std::array<ParamInfo, numParams>& getParamInfos();

    static constexpr int numVoices = 16;

    Synth();
    ~Synth() override;

    // ---- any thread -----------------------------------------------------------
    void setParam (int index, float value) noexcept;  // clamped (and rounded if stepped)
    float getParam (int index) const noexcept;

    // ---- Instrument (audio thread) --------------------------------------------
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (juce::AudioBuffer<float>& out, int numSamples,
                  const NoteEvent* events, int numEvents) noexcept override;
    void allNotesOff() noexcept override;
    void reset() noexcept override;

    /** Number of voices currently producing sound (for tests and UI). */
    int getNumActiveVoices() const noexcept;

private:
    struct Voice;
    struct Impl;

    std::array<std::atomic<float>, numParams> params;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (Synth)
};

} // namespace daw
