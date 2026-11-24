# diyDaw — agent guide

An Ableton-style DAW in C++20 + JUCE 9.0.6 (git submodule at `external/JUCE`, do not edit it;
fresh clones need `git submodule update --init`).

## Layout
- `engine/` — device-independent real-time audio engine (namespace `daw`). Only depends on
  `juce_audio_basics` / `juce_audio_formats`. Never include GUI or device headers here.
  Tracks play immutable `ClipList` snapshots (`engine/Clip.h`).
- `model/` — headless document model: `Project` (ValueTree of TRACK/CLIP + UndoManager,
  pushes ClipList snapshots to the engine on every change) and `SamplePool` (background
  decoding + cache). No GUI code here either.
- `app/` — JUCE GUI application (transport bar, mixer strips, meters, look-and-feel).
- `app/arrangement/` — arrangement view: `TimelineGeometry` (beats<->pixels, grid, snap),
  `TimeRuler`, `TrackHeader`, `ClipPainter`, `ArrangementView`.
- `tests/` — headless `juce::UnitTest` tests, category `"diyDaw"`, run by `DawEngineTests`.

## Data flow
UI gesture -> `Project` edit (undoable) -> ValueTree listener -> new `ClipList` ->
`AudioTrack::setClips` (lock-free) -> audio thread. The UI never edits the ValueTree or the
engine's clip data directly; it reads the ValueTree to draw.

## Arrangement view (interaction spec, Ableton-like)
- Layout: TimeRuler across the top (offset by `TrackHeader::preferredWidth`), then one lane per
  track: TrackHeader on the left, clip lane on the right. Lanes ~64 px tall, vertical scroll
  when they don't fit. Empty space below the last lane is a drop zone that creates a new track.
- Grid: vertical lines from `TimelineGeometry::gridBeats()`; bar lines brighter. Alternate
  bar shading is nice-to-have.
- Playhead: thin line across all lanes at `Transport::getPositionBeats()`. Clicking empty lane
  space moves the playhead there (snapped) and clears the selection.
- Zoom: Ctrl+wheel (anchored at mouse) and vertical drag on the ruler. Scroll: wheel /
  Shift+wheel / horizontal trackpad scroll moves the view in time.
- Clips: drawn with `paintClip()`. Click selects (one selected clip at a time is fine).
  Drag the body to move (snapped to grid, Alt = no snap; can move to another lane).
  Drag within ~6 px of the left/right edge to trim (resize cursor). While dragging, show
  the clip at its new position as a ghost; commit to Project only on mouseUp (one
  `beginTransaction` + one Project call), because Project overwrites clips it lands on.
- Keys (when the view has focus): Delete/Backspace = delete selected; Ctrl+D = duplicate;
  Ctrl+E = split selected clip at the playhead; Ctrl+L = toggle loop on selected clip.
  Space, Tab, Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y / Ctrl+T must keep working (handled by
  MainComponent — don't consume them).
- File drop from the OS onto a lane: `SamplePool::loadAsync`, then `Project::addClip` at the
  snapped drop beat on that lane. Multiple files: place them one after another.
- Follow: while playing, if the playhead leaves the visible area, page the view so it's visible.

## Build
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug            # full app (needs libasound2-dev)
cmake -S . -B build-noaudio -G Ninja -DDAW_AUDIO_BACKENDS=OFF    # compile-check without ALSA/JACK
cmake --build build --target DawEngineTests && ./build/DawEngineTests_artefacts/Debug/DawEngineTests
```

Visual check of the UI without opening a window:
```
./build-noaudio/DiyDaw_artefacts/Debug/diyDaw --snapshot ui.png 1280x560 [a.wav b.wav ...]
```
Audio file i is placed on track i at bar i+1, so you can check clip/waveform drawing.

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
- Every engine or model behaviour change needs a test in `tests/`.
- Several agents work in parallel on disjoint files: only edit the files your task names,
  and use your own build directory (named in your task).
