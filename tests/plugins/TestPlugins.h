#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** Tiny plugins built only to test diyDaw's plugin hosting (VST3 + LV2):
    - "diyDaw Test Gain": stereo effect, one "gain" parameter (linear 0..2, default 0.5).
    - "diyDaw Test Synth": instrument, plays a sine per MIDI note (level 0.25 * velocity),
      one "level" parameter (0..1, default 1); reports its received note count in its state.
    Both save/restore their parameters through getStateInformation / setStateInformation. */
juce::AudioProcessor* createTestGainPlugin();
juce::AudioProcessor* createTestSynthPlugin();
