# diyDaw — agent guide

An Ableton-style DAW in C++20 + JUCE 9.0.6 (git submodule at `external/JUCE`, do not edit it;
fresh clones need `git submodule update --init`).

## Layout
- `engine/` — device-independent audio engine (namespace `daw`). Only depends on
  `juce_audio_basics` / `juce_audio_formats`. Never include GUI or device headers here.
- `app/` — JUCE GUI application (transport bar, mixer strips, meters, look-and-feel).
- `tests/` — headless `juce::UnitTest` tests, category `"diyDaw"`, run by `DawEngineTests`.

## Build
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug            # full app (needs libasound2-dev)
cmake -S . -B build-noaudio -G Ninja -DDAW_AUDIO_BACKENDS=OFF    # compile-check without ALSA/JACK
cmake --build build --target DawEngineTests && ./build/DawEngineTests_artefacts/Debug/DawEngineTests
```

Visual check of the UI without opening a window:
```
./build-noaudio/DiyDaw_artefacts/Debug/diyDaw --snapshot ui.png 1100x640
```

## Real-time rules (audio thread = Engine::render and everything it calls)
- No allocation, no locks, no file/console I/O, no exceptions, no waiting.
- Message thread ⇄ audio thread communication only through `std::atomic`,
  `daw::RealtimeSwap<T>` (object hand-over) or `daw::PeakMeter` (levels).
- Parameter changes that would click (gain, pan, mute) go through `juce::SmoothedValue`.

## JUCE 9 notes
- Check `external/JUCE/BREAKING_CHANGES.md` and the module headers instead of
  relying on memory of older JUCE versions.
- Use `juce::FontOptions` to construct fonts (the `Font(float)` constructors are deprecated).

## Style
- JUCE style: 4-space indent, space before call parentheses `foo (x)`, braces on new lines.
- Keep classes small with a short doc comment saying which thread may call what.
- Every engine behaviour change needs a test in `tests/`.
