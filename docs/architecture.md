🇬🇧 English | 🇹🇷 [Türkçe](tr/mimari.md)

# diyDaw Architecture

This document explains diyDaw's internals: its code layers, thread responsibilities, how a
mouse gesture reaches the speakers, and the audio engine's real-time rules. It is intended
for anyone working on the code. For a short technical overview, also see
[AGENTS.md](../AGENTS.md).

**Contents**

1. [Overview](#1-overview)
2. [Layers and directories](#2-layers-and-directories)
3. [Threads and real-time rules](#3-threads-and-real-time-rules)
4. [Data flow: from mouse gesture to speakers](#4-data-flow-from-mouse-gesture-to-speakers)
5. [Audio engine (engine/)](#5-audio-engine-engine)
6. [Document model (model/)](#6-document-model-model)
7. [Plugin hosting](#7-plugin-hosting)
8. [Offline rendering (export)](#8-offline-rendering-export)
9. [User interface (app/)](#9-user-interface-app)
10. [Tests](#10-tests)
11. [Design decisions and rationale](#11-design-decisions-and-rationale)

---

## 1. Overview

```
┌───────────────────────────────── app/ (JUCE GUI) ─────────────────────────────────┐
│  MainComponent ─ TransportBar, SongPositionBar, ArrangementView, Mixer, DetailView │
│                  ExportDialog, PluginManager, PluginWindows                       │
└───────────────┬───────────────────────────────────────────────────▲───────────────┘
                │ undoable edit commands                            │ reads ValueTree,
                ▼                                                   │ refreshes via listeners
┌───────────────────────────────── model/ (headless) ───────────────────────────────┐
│  Project (ValueTree + UndoManager)   SamplePool   PluginHost   ProjectRenderer     │
└───────────────┬───────────────────────────────────────────────────────────────────┘
                │ lock-free snapshots (RealtimeSwap) and atomic values
                ▼
┌───────────────────────────────── engine/ (real-time) ─────────────────────────────┐
│  Engine ─ Transport, AudioTrack ×16 (clips, instrument, effects, automation),      │
│           master effects, master fader, Metronome                                  │
└───────────────┬───────────────────────────────────────────────────────────────────┘
                │ Engine::render()
                ▼
        juce::AudioDeviceManager → ALSA / PipeWire / JACK → audio device
```

The three layers have distinct responsibilities:

- **engine/** only produces audio. It knows nothing about the UI or files; immutable, prepared
  data structures tell it what to play.
- **model/** is the project's single source of truth. Every edit goes through it, is undoable
  where supported, and is automatically sent to the engine.
- **app/** displays state and issues commands. It does not write directly to engine state or
  the project tree.

## 2. Layers and directories

```
diyDaw/
├── engine/            real-time audio engine (JUCE: juce_audio_basics, juce_audio_formats)
│   ├── Engine         complete engine: track pool, master, metronome, render loop
│   ├── Transport      musical clock: position in beats, tempo, loop
│   ├── AudioTrack     one channel: clips, instrument, effects, automation, fader/pan, meter
│   ├── Clip           ClipList, MidiSequence: immutable snapshots supplied to a track
│   ├── Instrument     note-driven sound generator interface (NoteEvent)
│   ├── Synth          built-in synth
│   ├── DrumRack       16-pad sampler
│   ├── BuiltinSamples drum sounds synthesized in code
│   ├── AudioEffect    effect interface (bypass crossfade, parameters)
│   ├── Effects        EQ Three, Compressor, Reverb, Delay
│   ├── Automation     engine-side automation envelopes
│   ├── Metronome      click generator
│   ├── SampleData     decoded audio file + waveform summary
│   ├── PeakMeter      level measurement (audio thread → UI)
│   ├── RealtimeSwap   lock-free object handover
│   └── ParamInfo      parameter definition (ID, name, range, default)
├── model/             headless document model
│   ├── Project        ValueTree document, UndoManager, engine sync, file I/O
│   ├── SamplePool     cache with background audio decoding
│   ├── ProjectRenderer offline rendering (export)
│   ├── PluginHost     VST3/LV2 scanning and loading
│   └── PluginDevices  adapters from plugins to the engine's effect/instrument interfaces
├── app/               JUCE GUI application
│   ├── arrangement/   timeline, ruler, track headers, clip painting, automation lanes
│   ├── clipeditor/    piano roll
│   ├── devices/       synth panel, drum rack panel, device chain
│   └── plugins/       Plugin Manager, plugin windows
├── tests/             headless unit tests (+ tests/plugins: test plugins)
├── examples/          example projects
└── external/JUCE      JUCE 9 (Git submodule)
```

Dependencies flow in one direction: `app → model → engine`. The engine never knows about the
model or UI, and the model never knows about the app UI. One dependency caveat: plugin hosting
uses `juce_audio_processors` in the model. That module depends on GUI code, but the model does
not create GUI components.

## 3. Threads and real-time rules

| Thread | Owner | Responsibilities |
| --- | --- | --- |
| **Message thread** | JUCE | UI, all model edits, saving/loading, plugin creation, plugin scanning, garbage collection |
| **Audio thread** | Audio driver | `Engine::render()` and everything it calls |
| **Decoder threads** (2) | `SamplePool` | Read audio files in the background |
| **MIDI input thread** | JUCE | Receive MIDI keyboard messages and write to the live-note queue |
| **Export thread** | `ExportJob` | Render a copy of the project offline |

### Audio-thread rules

The audio device requests new audio every few milliseconds. If the audio thread misses its
deadline, you hear a dropout. Inside `Engine::render()`:

- **No allocation** (`new`, `std::vector::push_back`, constructing `juce::String`, etc.). Size
  everything during `prepare()`.
- **No locks** (`std::mutex`, `CriticalSection`). No waiting.
- **No file or console I/O.**
- **No exceptions.**

Cross-thread communication uses only three mechanisms:

1. **`std::atomic`** for individual values (fader, pan, mute, synth parameters, tempo, playback state).
2. **`daw::RealtimeSwap<T>`** for handing over large immutable objects (clip lists, effect chains,
   automation, drum kits, plugin instruments).
3. **`daw::PeakMeter`** for levels (the audio thread writes; the UI reads and resets).

Parameter changes that could click (gain, pan, mute, effect bypass, filter cutoff, etc.) are
spread over a few milliseconds using `juce::SmoothedValue`.

### How RealtimeSwap works

```
message thread                         audio thread
──────────────                         ────────────
publish(new)  ──► pending ──┐
                            │  acquire() at block start:
                            └──► if pending, swap with active,
                                 put the old object in the retired queue
collectGarbage() ◄── retired queue (SPSC, 64 slots)
   deletes old objects
```

- The message thread submits a new object with `publish()`. If the audio thread has not yet
  seen the previous pending object, that object can be deleted immediately.
- The audio thread calls `acquire()` at the start of each block and uses the returned pointer
  throughout that block.
- The replaced object is not deleted there. It goes into a single-producer/single-consumer
  queue, and the message thread deletes it with `collectGarbage()` from the UI timer, 30 times
  per second.

The audio thread therefore never allocates or frees memory. Even when a `std::shared_ptr`
reference count reaches zero, the resulting deletion always happens on the message thread.

## 4. Data flow: from mouse gesture to speakers

Example: a user drags and drops a clip in the arrangement.

1. **UI** (`ArrangementView`): during the drag, it leaves the model untouched and draws a ghost
   preview. On mouse release:
   ```cpp
   project.beginTransaction ("Move clip");
   project.moveClip (clip, targetTrack, newStartBeat);
   ```
2. **Model** (`Project::moveClip`): edits the ValueTree through `UndoManager` and applies the
   Ableton-style overlap rule (`clearRegion` trims, splits, or deletes underlying clips).
   All changes belong to the same transaction, so one Ctrl+Z undoes them all.
3. **Model listener** (`Project::valueTreePropertyChanged` / `ChildAdded` / `ChildRemoved`): calls
   `syncTrack()` for the affected track. This builds a new immutable `ClipList` from all its
   clips, sorted by start time. MIDI notes are copied into a `MidiSequence`; audio clips share
   their `SampleData` through `shared_ptr`.
4. **Engine** (`AudioTrack::setClips` → `RealtimeSwap::publish`): the new list reaches the audio
   thread at the start of the next audio block.
5. **UI** listens to the same ValueTree and repaints.

Undo follows the same path: `UndoManager` restores the tree, the listener updates the engine,
and the UI repaints. There is no separate engine implementation of undo.

The rules are:

- The UI **never** writes directly to the ValueTree or calls engine setters such as
  `AudioTrack::setGainDb`. Everything goes through `Project` methods.
- Each user gesture starts with **one** `beginTransaction()`. For sliders and toggles,
  `bindSliderToProject` / `bindToggleToProject` in `app/ProjectControls.h` handle this:
  a transaction starts when dragging begins, and `UndoManager` coalesces consecutive changes
  to the same property into one step.

## 5. Audio engine (engine/)

### Engine

`Engine` is the complete audio engine, independent of the audio device. The app calls it from
the device callback; tests and export call it directly.

- **Track pool:** 16 `AudioTrack` instances are created in advance. Adding a track increments a
  counter without allocating on the audio thread.
- **`prepare(sampleRate, maxBlockSize)`:** sizes everything when the device opens, with audio
  stopped. Also prepares effects and plugins again.
- **`render(out, numSamples)`:** splits the requested block into chunks of at most **256 samples**
  (`automationBlockSize`, the automation update interval). For each chunk:

```
Transport::advance(n)  →  Block { playing, startBeat, beatsPerSample, bpm, sampleRate, numSamples }
       │                    (shortened at loop end; position wraps to loop start)
       ▼
for each track (or only the isolated track in Pattern mode):
    AudioTrack::process(bus, block, anySolo)   ── adds to bus
       ▼
master effects → master gain (smoothed) → metronome → master meter → output
```

### Transport

- Position is a `double` in **beats**, preserving musical position when the tempo changes.
- Each block starts **exactly where the previous one ended** (`positionBeats = block.endBeat()`),
  so `[start, end)` interval checks neither miss nor double-count events.
- UI seek requests (`setPositionBeats`) take effect at the next block boundary.
- **Looping:** `setLoop(start, end)` + `setLoopEnabled(true)`. If a block would cross the loop
  end, `advance()` cuts it exactly there and wraps the position to the loop start.
  `Engine::render` calls `advance()` again for the remainder, making loops sample-accurate.

Beat-to-sample conversion uses a 1/1000-sample tolerance to absorb floating-point error:
an event falls at sample `ceil((beat - blockStart) / beatsPerSample - 0.001)`. Clip edges, MIDI
notes, and the metronome all use this rule, making output independent of block size. Tests
verify it with blocks of 1, 7, 64, 512, and 4096 samples.

### AudioTrack

A track processes each block in this order:

1. **Automation:** write each envelope's value at the playhead to its target parameter, even
   while stopped.
2. **Audio clips:** add overlapping clip audio to a scratch buffer, using linear interpolation
   for sample-rate differences. Apply 2 ms fades at clip edges. Looping clips wrap the sample.
3. **Instrument** (MIDI tracks): build the block's `NoteEvent` list from MIDI clips and the
   live-note queue, then process the instrument (see below).
4. **Effect chain:** process each effect in place, in order.
5. **Fader, pan, mute/solo:** apply constant-power panning and smooth values with `SmoothedValue`.
6. **Meter and summing:** add the result to the master bus.

#### MIDI timing

On each block, `AudioTrack::renderInstrument`:

- Finds note starts within the block. A MIDI clip's content time is
  `offsetBeats + (t - clipStart)`. Looping content wraps at `loopLength`, and notes extending
  past the loop end are cut there.
- **Tracks sounding notes itself:** stores when each pitch should end. Notes cannot get stuck
  if a clip is deleted, a note edited, playback stopped, or the playhead moved. Stopping or
  seeking turns off all sequenced notes.
- Places the previous note's note-off at its own end when the same pitch repeats rapidly,
  never before that note's own note-on.
- Accepts **live notes** (computer keyboard, MIDI keyboard, piano-roll previews) through
  `pushLiveNote()` into a lock-free FIFO. Producers use a mutex among themselves; the audio
  thread does not. Live events are added at block start **in arrival order**. Sequenced events
  are sorted by sample position, with note-offs before note-ons at the same position, using
  an allocation-free insertion sort.

`InstrumentType` selects the track's instrument: `none`, `synth`, `drumRack`, or `plugin`.
A synth and drum rack are ready on every track. A plugin instrument arrives through a separate
`RealtimeSwap` slot. Changing the type resets the old instrument on the audio thread.

### Synth

A 16-voice subtractive synth (`engine/Synth`):

- Oscillators: band-limited saw and square using PolyBLEP, triangle using PolyBLAMP, and sine.
- Filter: a resonant low-pass filter using a TPT (topology-preserving transform) structure;
  recalculated every 16 samples using cutoff, envelope amount, and key tracking.
- Envelopes: linear-segment ADSR; attack and release start from the current level to avoid clicks.
- Voice stealing: reuse a voice playing the same pitch, otherwise a free voice, otherwise the
  oldest released voice, otherwise the oldest voice. A stolen voice fades out over 4 ms.
- Cutoff, volume, and mix are smoothed. Parameters are atomic and described by a `ParamInfo` list.

### DrumRack and built-in sounds

- 16 pads on notes 36–51, one-shot playback, 32 voices, choke groups.
- Per-pad gain, pan, tune (resampling), decay, and choke parameters.
- The drum kit (`DrumRack::Kit`, 16 `SampleData` objects) arrives through `RealtimeSwap`. When
  a kit changes, a voice whose sample is absent from the new kit fades without touching old
  memory: each voice keeps a pre-copied 5 ms lookahead buffer of its sample.
- `BuiltinSamples` synthesizes kicks, snares, hi-hats, toms, and other sounds in code, using
  deterministic, fixed-seed noise. `SamplePool::get("builtin:kick")` generates a sound on its
  first request.

### Effects

The `AudioEffect` base class:

- Stores parameters in an atomic array. `setParam` clamps to the range and rounds stepped values.
- Handles bypass in `process()`: enabling or disabling an effect crossfades between dry and
  processed audio over 10 ms. Fully bypassed effects are not processed. Re-enabling an effect
  after a long bypass clears its old tail, such as reverb.
- Records the prepared sample rate in `prepare()`. During a device change, it skips processing
  a block at a different rate as a safeguard.

An `EffectChain` is a list of `shared_ptr` instances. The model keeps **one engine instance per
effect DEVICE**. Rebuilding the chain after an addition, deletion, or reorder reuses those
instances, so adding another effect does not cut off an existing reverb tail.

Built-in effects: EQ Three (RBJ biquads with coefficients smoothed in 32-sample sub-blocks),
Compressor (stereo-linked, dB-domain peak detector, quadratic knee), Reverb (Freeverb-style,
scaled to sample rate, with pre-delay), and Delay (fractional delay, filtered feedback,
ping-pong, smooth time changes).

### Automation (engine side)

Each track receives its `AutomationList` through `RealtimeSwap`. Each `AutomationLane` has a
target (track gain, track pan, instrument parameter, or a parameter on a specific effect
instance) and sorted points. `AudioTrack::applyAutomation` uses a binary search to find the
value at the block's starting beat and writes it to the target. This overrides the value
stored in the model; when an envelope is removed, the model sends the parameter's own value
to the engine again.

## 6. Document model (model/)

### Project

`Project` owns a `juce::ValueTree` and a `juce::UndoManager`. Tree structure (see
[file-format.md](file-format.md) for details):

```
PROJECT { version, bpm, beatsPerBar, masterGain }
├── TRACK { kind, name, colour, engineIndex, gain, pan, mute, solo, height }
│   ├── CLIP (audio) { file, start, length, offset, loop }
│   ├── CLIP (midi)  { start, length, offsetBeats, loop, loopLength }
│   │   └── NOTE { pitch, start, length, velocity }
│   ├── DEVICE (instrument: synth | drumrack | pluginInstrument)
│   ├── DEVICE (effect: eq | compressor | reverb | delay | plugin) ...
│   └── AUTOMATION { target }
│       └── POINT { beat, value }
└── MASTER
    └── DEVICE (effect) ...
```

Key method groups:

| Group | Examples |
| --- | --- |
| Document | `newDocument`, `saveTo`, `loadFrom`, `copyFrom`, `createSnapshot`, `hasUnsavedChanges` |
| Track | `addTrack`, `setTrackName`, `setTrackGain/Pan/Mute/Solo`, `setTrackHeight`, `getNumTracks`, `getTrack` |
| Clip | `addClip`, `addMidiClip`, `moveClip`, `trimClipStart/End`, `splitClip`, `duplicateClip`, `deleteClip`, `setClipLooping`, `setClipLoopLength` |
| Note | `addNote`, `moveNote`, `setNoteLength`, `setNoteVelocity`, `deleteNote` |
| Device | `setDeviceParam`, `addEffect`, `removeEffect`, `moveEffect`, `setEffectEnabled`, `setInstrument`, `setPadSample` |
| Plugin | `setPluginHost`, `addPluginEffect`, `setPluginInstrument`, `getPluginInstance`, `getPluginError` |
| Automation | `getAutomationTargets`, `getOrCreateAutomationLane`, `addAutomationPoint`, `moveAutomationPoint`, `deleteAutomationPoint`, `removeAutomationLane` |

### Synchronization

`Project` listens to its own tree and updates the relevant engine state according to the node
that changed:

| Change | Method called | Sent to the engine |
| --- | --- | --- |
| CLIP / NOTE | `syncTrack` | New `ClipList` |
| TRACK gain/pan/mute/solo | `syncTrackMixer` | Atomic values |
| Instrument DEVICE added/removed | `syncDevice` | Instrument type, parameters, plugin slot, drum kit |
| Effect DEVICE added/removed/reordered | `syncEffects` | New `EffectChain` |
| DEVICE parameter | (direct) | `setParam` on the relevant instance |
| `pad<N>File` | `syncDrumKit` | New `DrumRack::Kit` |
| AUTOMATION / POINT | `syncAutomation` | New `AutomationList` + model values for parameters whose automation was removed |
| PROJECT bpm / beatsPerBar / masterGain | (direct) | Transport / Engine |

The `engineIndex` property links project tracks to engine tracks. It is runtime information,
not stored in the file. Loading a project resets the engine's track pool and reconnects tracks
in order.

Loading (`replaceContents`) sanitizes file data: converts XML strings to numbers and booleans,
clamps out-of-range values, discards clips of the wrong track type and unknown devices, adds
a missing synth, and makes device `uid` values unique. The rest of the code can then rely on
the tree's structure.

### SamplePool

- Caches audio files by absolute path.
- `loadAsync()` decodes files in a two-thread pool and returns the result on the message thread.
  Requests for the same file share one decode; callbacks are always asynchronous. If the pool
  is destroyed, a `weak_ptr` allows pending jobs to be safely ignored.
- When a sample arrives, `onSampleAdded` makes the project resynchronize all tracks so clips
  waiting for a sample can begin playing.
- Computes a min/max waveform summary in 256-sample blocks for each `SampleData`. The UI uses
  this summary to draw the waveform.

### Undo details

- Call `beginTransaction()` once at the start of each user gesture.
- Non-undoable changes (made with a `nullptr` UndoManager): adding tracks, track height,
  `engineIndex`, `uid` repairs, and capturing plugin state before deletion.
- Plugins: just before deleting a plugin DEVICE, its current state is written to the DEVICE's
  `state` property. When undo restores the DEVICE, a new plugin instance opens with that state.

## 7. Plugin hosting

```
PluginHost (model/)                            PluginDevices (model/)
├── AudioPluginFormatManager                   ├── PluginEffect : AudioEffect
│     app: formats with editors                │     processBlock(stereo buffer, empty MIDI)
│     tests: headless formats                  └── PluginInstrument : Instrument
├── KnownPluginList (catalog)                        NoteEvent → MidiBuffer (preallocated)
├── scanning: file by file on the message thread via a timer,
│             with a dead-man's-pedal file to blacklist crashing plugins
└── createInstance (synchronous)
```

- A plugin DEVICE stores `description` (JUCE `PluginDescription` XML) and `state` (the plugin's
  own state, base64-encoded).
- The model creates effect plugins in `syncEffects` and instrument plugins in `syncDevice`,
  restores saved state, and calls `prepareToPlay` with the engine's prepared sample rate and
  block size.
- State lives inside the plugin. During save and export, `Project::createSnapshot()` writes
  every plugin's state to a **copy** of the tree. Writing to the live tree would pollute undo history.
- When a parameter changes in a plugin's own UI, an `AudioProcessorListener` sets the project's
  atomic unsaved-changes flag.
- If loading fails, the DEVICE remains in the tree but is absent from the engine. Its error
  message is available through `getPluginError()`.
- On shutdown, plugin instances are destroyed before the `PluginHost` that loaded them.
  `MainComponent`'s member order guarantees this.
- JUCE details: call `releaseResources()` before preparing again because some formats skip
  `prepareToPlay` when they see no change. LV2 applies parameter changes in the next `processBlock`.

## 8. Offline rendering (export)

`ProjectRenderer`:

1. Creates its own `Engine` and `Project` on the message thread, then loads the source project's
   `createSnapshot()` copy. Plugins are recreated with saved state; samples share the same
   `SamplePool`.
2. Rendering can then run on the background export thread because it no longer modifies shared
   state. The live engine can keep playing.
3. Before starting, runs the stopped engine for 50 ms to let fader smoothing reach its targets.
   Otherwise, the export would start with a fade-in.
4. Renders the requested range, stops the transport at the range end, then continues rendering
   the **effect tail** for a fixed duration or until silence.
5. If normalization is enabled, makes two passes: first finding the peak, then writing with the
   required gain. Adds TPDF dither for 16-bit output. Writes a temporary file first and replaces
   the destination only after success.

## 9. User interface (app/)

| Component | Responsibility |
| --- | --- |
| `MainComponent` | Audio device, engine, project, plugin host; file operations; shortcuts; live-note routing; Pattern mode; 30 Hz timer (meters, playhead, garbage collection) |
| `TransportBar` | Tempo, metronome, PAT/SONG, play/stop, position, adding tracks |
| `SongPositionBar` | Song position bar |
| `ArrangementView` | Timeline: track lanes, clips, drag previews, file drops, automation lanes, variable-height layout |
| `TrackHeader`, `TimeRuler`, `ClipPainter`, `TimelineGeometry`, `AutomationLane` | Arrangement building blocks |
| `DetailView` | Bottom panel: Clip and Device tabs, Master button |
| `PianoRoll` | MIDI note editor |
| `DeviceChain`, `SynthPanel`, `DrumRackPanel` | Device chain and panels |
| `TrackStrip`, `MasterStrip`, `LevelMeter` | Mixer |
| `ExportDialog` | Export dialog and background job |
| `PluginManager`, `PluginWindows` | Plugin Manager and editor windows |
| `DawLookAndFeel` | Dark theme, knob/slider painting, automation color |

UI components follow the model in two ways. `ValueTree::Listener` handles structural changes
such as adding or removing tracks and devices. A 30 Hz timer reads values such as fader and
knob positions without overwriting a control being dragged. Automated parameters display the
engine's current value rather than the model's stored value.

Some rebuilds are deferred with `MessageManager::callAsync` and `SafePointer` to avoid deleting
a component inside its own callback, such as an effect's “x” button deleting its panel.

## 10. Tests

- `juce::UnitTest` classes in `tests/` use the "diyDaw" category and run in the `DawEngineTests`
  console executable. It creates a `ScopedJuceInitialiser_GUI` because plugins need a
  `MessageManager`.
- Tests are headless: they call `render()` / `renderOffline()` directly and compare output
  sample by sample. They use in-memory virtual samples instead of audio files.
- Coverage (roughly 430 test sections): transport, metronome, clip playback, project editing
  rules, persistence, export, synth, MIDI timing, live notes, effects, effect chains and loops,
  automation, master and effect tails, drum rack, drum tracks, and VST3/LV2 plugins.
- `tests/plugins` builds two small test plugins; plugin tests scan them through `DAW_TEST_PLUGIN_DIR`.
- Every engine or model behavior change needs a test in `tests/`.

## 11. Design decisions and rationale

| Decision | Rationale |
| --- | --- |
| Position as a `double` in beats | Preserves musical position on tempo changes; MIDI and the grid naturally use beats |
| Each block starts at the previous block's end | Avoids cumulative rounding errors, missed events, and double triggers |
| Immutable `ClipList` snapshots per track | The audio thread reads a consistent list without locks; any edit complexity becomes one pointer swap |
| Fixed pool of 16 tracks | Adding a track does not allocate on the audio thread |
| ValueTree + UndoManager | XML persistence, undo, and change notifications come from one structure |
| Preview while dragging, one edit on release | Overlap handling does not repeatedly cut other clips during a drag; one undo step |
| One engine instance per effect DEVICE | Preserves internal state (tails, envelopes) when the chain changes |
| Export with a separate engine and project copy | Leaves live playback unaffected and permits safe background rendering |
| Drum sounds synthesized in code | Avoids external sample licensing concerns and keeps the repository and project files small |
| Locally built test plugins | Tests do not depend on which plugins are installed on the system |
