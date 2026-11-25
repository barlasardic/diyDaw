# diyDaw — agent guide

An Ableton-style DAW in C++20 + JUCE 9.0.6 (git submodule at `external/JUCE`, do not edit it;
fresh clones need `git submodule update --init`).

## Layout
- `engine/` — device-independent real-time audio engine (namespace `daw`). Only depends on
  `juce_audio_basics` / `juce_audio_formats`. Never include GUI or device headers here.
  Tracks play immutable `ClipList` snapshots (`engine/Clip.h`).
- `model/` — headless document model: `Project` (ValueTree of PROJECT/TRACK/CLIP +
  UndoManager; owns tempo, master and track mixer values and clips; pushes everything to the
  engine on every change; saves/loads `.diydaw` XML files) and `SamplePool` (background
  decoding + cache). No GUI code here either.
- `app/` — JUCE GUI application (transport bar, mixer strips, meters, look-and-feel).
- `app/arrangement/` — arrangement view: `TimelineGeometry` (beats<->pixels, grid, snap),
  `TimeRuler`, `TrackHeader`, `ClipPainter`, `ArrangementView`.
- `app/DetailView` — Ableton's bottom panel: Clip tab (`app/clipeditor/PianoRoll`) and Device
  tab (`app/devices/SynthPanel`). `app/ExportDialog` — Export Audio dialog + background job.
- `examples/` — small projects for snapshots and manual testing (`midi-demo.diydaw`).
- `tests/` — headless `juce::UnitTest` tests, category `"diyDaw"`, run by `DawEngineTests`.

## Data flow
UI gesture -> `Project` edit (undoable) -> ValueTree listener -> new `ClipList` /
mixer atomics -> audio thread. The UI never edits the ValueTree or engine state directly
(no `AudioTrack::setGainDb` etc. from the UI); it reads the ValueTree to draw. Sliders and
toggles use `bindSliderToProject` / `bindToggleToProject` (`app/ProjectControls.h`) so each
gesture is one undo step. Loading a file replaces the tree's children, so views must sync
their per-track components from ValueTree child-added/removed callbacks.

## MIDI
- Track kinds: audio and MIDI (`TRACK kind`). A MIDI track has one `DEVICE` child (the Synth's
  parameters by `Synth::ParamInfo::id`) and MIDI `CLIP`s with `NOTE` children (content beats).
  Iterate a track's clips with `Project::isClip()` — not every child is a clip.
- Engine: `AudioTrack` turns MIDI clips into sample-accurate `NoteEvent`s for its `Synth`
  (`engine/Instrument.h`); it tracks sounding notes itself so stops, seeks and edits never hang
  notes. Live notes (computer keyboard, MIDI inputs, piano-roll preview) go through
  `AudioTrack::pushLiveNote` to the selected MIDI track.
- Keys: Ctrl+Shift+T new MIDI track; Shift+Tab switches the detail view between Clip and
  Device; A W S E D F T G Y H U J K play notes, Z/X octave, C/V velocity (no modifiers).
- Double-click empty space on a MIDI lane creates a one-bar clip; double-click a clip opens it.

## Effects, loop and pattern mode
- Insert effects (`engine/AudioEffect.h`, `engine/Effects.h`: EQ Three, Compressor, Reverb,
  Delay) are effect `DEVICE`s on any track, after the instrument, before the fader. The model
  keeps one engine instance per DEVICE (`Project::getEffectInstance`) and republishes an
  `EffectChain` snapshot on every chain change. New effects are prepared on the message thread.
- `Transport::setLoop/setLoopEnabled`: `advance()` may return a shorter block at the loop end;
  `Engine::render` keeps asking. `Engine::setIsolatedTrack` renders one track only.
- Pattern mode (FL style, PAT/SONG in the transport bar): loops the MIDI clip open in the piano
  roll on its own (loop = clip range, isolated track). `SongPositionBar` under the transport bar
  shows and scrubs the song (or pattern) position; the piano roll ruler also seeks.

## Automation
- `AUTOMATION { target }` lanes with `POINT { beat, value }` on a track; targets are "gain",
  "pan" or "<device uid>:<param id>" (`Project::getAutomationTargets`). Every DEVICE has a
  project-unique `uid`. The engine (`engine/Automation.h`) applies each lane's value at the
  playhead once per block (blocks are at most `Engine::automationBlockSize` samples), also while
  stopped; when a lane goes away the model pushes the parameter's own value again.
- UI: the "A" button on a track header opens an automation sub-lane (target chooser, envelope
  editor). Automated knobs/faders turn red and show the engine's value (`showAutomationState`).

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
Audio file i is placed on track i at bar i+1, so you can check clip/waveform drawing. A
`.diydaw` argument opens that project instead. `--dialog export|pianoroll|synth` snapshots that
panel (piano roll / synth of the project's first MIDI clip / track). Headless bounce:
`diyDaw --export out.wav project.diydaw`.

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
