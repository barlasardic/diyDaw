#pragma once

#include "SampleData.h"

#include <memory>

namespace daw
{

/** Drum sounds synthesised in code (no external files, no licensing questions), used for
    the default drum kit. They are addressed with SamplePool keys "builtin:<name>".

    Names, in default-kit pad order (pad 1 = C1 = MIDI 36, General MIDI-like):
      kick, rim, snare, clap, shaker, lowTom, closedHat, midTom,
      cowbell, highTom, openHat, conga, zap, crash, sub, ride

    Any thread (pure functions; allocation is fine - never called on the audio thread). */
const juce::StringArray& getBuiltinSampleNames();

/** Renders a builtin sound (mono, 'sampleRate'), or nullptr for an unknown name.
    Deterministic: the same name always gives the same audio. */
std::shared_ptr<const SampleData> createBuiltinSample (const juce::String& name, double sampleRate = 48000.0);

inline juce::String builtinSampleKey (const juce::String& name) { return "builtin:" + name; }
inline bool isBuiltinSampleKey (const juce::String& key)          { return key.startsWith ("builtin:"); }
inline juce::String builtinSampleName (const juce::String& key)   { return key.fromFirstOccurrenceOf ("builtin:", false, false); }

} // namespace daw
