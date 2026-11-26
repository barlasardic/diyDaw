🇬🇧 English | 🇹🇷 [Türkçe](tr/gelistirme.md)

# Developing diyDaw

This document covers building diyDaw from source, running tests, using helper tools, and working
on the code. See [architecture.md](architecture.md) for the internals and
[file-format.md](file-format.md) for project files.

**Contents**

1. [Requirements](#1-requirements)
2. [Getting the source](#2-getting-the-source)
3. [Building](#3-building)
4. [Running and command-line options](#4-running-and-command-line-options)
5. [Tests](#5-tests)
6. [Checking the UI without a window (snapshots)](#6-checking-the-ui-without-a-window-snapshots)
7. [Coding conventions](#7-coding-conventions)
8. [Example: adding an effect](#8-example-adding-an-effect)
9. [Example: adding a model edit](#9-example-adding-a-model-edit)
10. [Desktop integration](#desktop-integration)
11. [Contributor workflow: parallel contributors and coding agents](#11-contributor-workflow-parallel-contributors-and-coding-agents)
12. [Common build problems](#12-common-build-problems)

---

## 1. Requirements

- **Linux** (tested on Ubuntu 22.04 / 24.04 and derivatives)
- **C++20** compiler: GCC 11+ or Clang 14+
- **CMake** 3.22+ and **Ninja**
- **Python 3** (optional, for some helper scripts)
- JUCE's Linux dependencies:

```bash
sudo apt install build-essential cmake ninja-build \
    libasound2-dev libjack-jackd2-dev \
    libfreetype-dev libfontconfig1-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
    libxinerama-dev libxrandr-dev libxrender-dev libxi-dev \
    libegl-dev libglu1-mesa-dev mesa-common-dev
```

`libcurl` and `webkit2gtk` are not required: diyDaw builds with `JUCE_USE_CURL=0` and
`JUCE_WEB_BROWSER=0`. On older systems, use `libfreetype6-dev` instead of `libfreetype-dev`.

## 2. Getting the source

JUCE 9.0.6 is a Git submodule at `external/JUCE`:

```bash
git clone --recursive https://github.com/barlasardic/diyDaw.git
# Or, if you cloned without submodules:
git submodule update --init
```

Do not edit files inside `external/JUCE`.

## 3. Building

```bash
# Full Debug build: app, tests, and test plugins
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The first build takes a few minutes because it also builds JUCE. Subsequent builds compile
only changed files.

### CMake options

| Option | Default | Description |
| --- | --- | --- |
| `DAW_BUILD_APP` | ON | Build the GUI application |
| `DAW_BUILD_TESTS` | ON | Build the `DawEngineTests` executable |
| `DAW_BUILD_TEST_PLUGINS` | ON | Build two small VST3/LV2 plugins for plugin tests |
| `DAW_AUDIO_BACKENDS` | ON | ALSA and JACK audio output. `OFF` builds without audio device support, for compile checks on machines without ALSA/JACK development packages |

Examples:

```bash
# Compile check without audio device development packages
cmake -S . -B build-noaudio -G Ninja -DDAW_AUDIO_BACKENDS=OFF

# Tests only, without the GUI app
cmake -S . -B build-tests -G Ninja -DDAW_BUILD_APP=OFF -DDAW_AUDIO_BACKENDS=OFF

# Optimized build
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Targets

| Target | Output |
| --- | --- |
| `DiyDaw` | `build/DiyDaw_artefacts/Debug/diyDaw` |
| `DawEngineTests` | `build/DawEngineTests_artefacts/Debug/DawEngineTests` |
| `DiyDawTestGain_VST3`, `DiyDawTestGain_LV2`, `DiyDawTestSynth_VST3`, `DiyDawTestSynth_LV2` | `build/tests/plugins/DiyDawTest*_artefacts/Debug/{VST3,LV2}/` |

Source lists are maintained manually in `CMakeLists.txt`: `daw_engine` (engine/*.cpp),
`daw_model` (model/*.cpp), tests, and the app. Add new `.cpp` files to the appropriate list.

## 4. Running and command-line options

```bash
./build/DiyDaw_artefacts/Debug/diyDaw                          # Empty project
./build/DiyDaw_artefacts/Debug/diyDaw examples/midi-demo.diydaw # Open a project
```

| Option | Description |
| --- | --- |
| `diyDaw <project.diydaw>` | Start with the given project |
| `--export <output.wav> <project.diydaw>` | Render the entire project to 44.1 kHz / 24-bit WAV without a window, with an automatic effect tail, then exit |
| `--snapshot <output.png> [WxH] [files...]` | Draw the UI to a PNG without a window, then exit (see below) |
| `--dialog <name>` | With `--snapshot`, draw a panel instead of the main window. Names: `export`, `pianoroll`, `synth`, `devices`, `master`, `plugins` |
| `--show-automation` | With `--snapshot`, open automation lanes on tracks that have automation |
| `--piano-rows <px>` / `--piano-keys <px>` | With `--dialog pianoroll`: piano roll row height / keyboard width |
| `--plugin-path <directory>` | Scan that directory for plugins immediately (for snapshots and export) |

Terminal messages saying `Cannot connect to server socket` mean that the JACK server is not
running; they are harmless.

## 5. Tests

```bash
cmake --build build --target DawEngineTests
./build/DawEngineTests_artefacts/Debug/DawEngineTests
```

The runner lists each test section and finishes with `ALL TESTS PASSED` or `TESTS FAILED: N`.
A successful run exits with code 0. You can filter the output to inspect particular tests:

```bash
./build/DawEngineTests_artefacts/Debug/DawEngineTests | grep -A3 "Starting tests in: Drum"
```

### Test files

| File | Coverage |
| --- | --- |
| `TransportTests` | Position, tempo, seeking, block continuity |
| `MetronomeTests` | Click timing independent of block size |
| `TrackTests` | Audio clip playback, fades, loops, gain/pan/mute/solo |
| `ProjectTests` | Clip edits, overlap rules, undo |
| `PersistenceTests` | Save/load, relative paths, malformed files |
| `ExportTests` | Offline rendering, bit depths, normalization, cancellation |
| `SynthTests` | Pitch accuracy, envelopes, filter, voice stealing, block-size independence |
| `MidiTests` | MIDI timing, loops, no stuck notes, model operations |
| `LiveNoteTests` | Live notes, including quick taps |
| `EffectsTests` | EQ, compressor, reverb, delay DSP |
| `DeviceChainTests` | Effect chain model, transport looping, track isolation |
| `AutomationTests` | Automation envelopes, targets, UIDs, persistence |
| `MasterTailTests` | Master effects, export tails |
| `DrumRackTests` | Drum rack DSP, built-in sounds |
| `DrumTrackTests` | Drum tracks, pads, persistence |
| `PluginTests` | VST3 and LV2 scanning, effects, instruments, state, missing plugins |

### Tips for writing tests

- Tests are headless; drive the engine with `Engine::prepare()` and `renderOffline()`.
- Instead of an audio file, create a `SampleData` in memory and insert it into `SamplePool`
  under a virtual path, such as `/virtual/kick.wav`, using `SamplePool::insert()`.
- Compare output sample by sample. Verify timing with different block sizes (1, 7, 64, 512).
- Faders smooth over 20 ms. Measure after the initial transition, or run the engine briefly
  while stopped before measuring.
- Scan `DAW_TEST_PLUGIN_DIR` in plugin tests. LV2 parameter changes take effect in the next
  `processBlock`; render a block before reading plugin state.
- When a test reveals a bug, keep the failing test and fix the bug.

### Test plugins

`tests/plugins/` builds two plugins with JUCE's `juce_add_plugin`:

- **diyDaw Test Gain:** a stereo effect with one `gain` parameter (0–2, default 0.5).
- **diyDaw Test Synth:** an instrument that plays a sine wave for each note at amplitude
  `0.25 × velocity × level`.

Both save their state as parameter values. They are only for testing and are not copied into
plugin directories in your home folder.

## 6. Checking the UI without a window (snapshots)

The app can draw itself to a PNG so you can inspect UI changes without opening a window:

```bash
B=./build/DiyDaw_artefacts/Debug/diyDaw

# Main window, 1280x620, with a project and automation lanes open
$B --show-automation --snapshot main.png 1280x620 examples/midi-demo.diydaw

# Audio files: file i is placed on track i at bar i
$B --snapshot audio.png 1280x560 drums.wav bass.wav

# Panels
$B --dialog pianoroll --snapshot pr.png 1280x320 examples/midi-demo.diydaw
$B --dialog devices   --snapshot dev.png 1280x320 examples/drums-demo.diydaw
$B --dialog master    --snapshot m.png   1280x330 project.diydaw
$B --dialog export    --snapshot e.png   400x400  examples/midi-demo.diydaw
$B --dialog plugins   --snapshot p.png   1000x560 --plugin-path build/tests/plugins
```

The UI timer does not run in snapshot mode, so some timer-driven indicators, such as the
position bar, may appear in their default state. The images in `docs/images` were generated
with these commands.

## 7. Coding conventions

### Formatting

- JUCE style: 4-space indentation, a space before the parenthesis in function calls (`foo (x)`),
  opening braces on a new line.
- Keep classes small. Each should start with a short comment describing which thread may call
  which methods.
- Builds should be **warning-free** with JUCE's recommended warning flags.

### JUCE 9 notes

- Construct fonts with `juce::FontOptions` (`Font(float)` constructors are deprecated).
- Use `juce::exactlyEqual` for intentional floating-point equality comparisons.
- Consult `external/JUCE/BREAKING_CHANGES.md` and module headers rather than relying on memory
  of older JUCE versions.

### Architectural rules

- `engine/` must not include GUI or device headers; it depends only on `juce_audio_basics` and
  `juce_audio_formats`.
- `model/` must not create GUI components.
- The UI must not write directly to the ValueTree or engine. Every edit goes through `Project`
  methods, and each user gesture starts with one `beginTransaction()`.
- No allocation, locks, file/console access, or exceptions on the audio thread (see
  [architecture.md](architecture.md#3-threads-and-real-time-rules)).
- Every engine or model behavior change needs a test in `tests/`.
- Use `Project::isClip()` / `isEffect()` / `isAutomationLane()` when iterating a track's children.
  Use `getNumTracks()` / `getTrack()` instead of iterating PROJECT children (MASTER is also a
  PROJECT child).

## 8. Example: adding an effect

To add a “Chorus” effect:

1. **Engine:** add a `ChorusEffect` class derived from `AudioEffect` to `engine/Effects.h`:
   - `getType()` → `"chorus"`
   - `static const std::vector<ParamInfo>& infos()` and `getParamInfos()` → parameter definitions
     (ID, name, range, default, knob midpoint, stepped flag, unit)
   - `prepareEffect()`: allocate buffers such as delay lines here
   - `processEffect()`: process stereo audio in place; do not allocate; read parameters with
     `getParam()` and smooth changes that could click
   - `reset()`: clear internal state
   - call `initialiseParams()` in the constructor
2. **Registration:** add `"chorus"` to `getEffectTypes()`, `getEffectDisplayName()`,
   `getEffectParamInfos()`, and `createEffect()` in `engine/AudioEffect.cpp`.
3. **Tests:** write DSP tests in `tests/EffectsTests.cpp` or a new file: expected behavior at
   default settings, no NaN/infinity at extremes, and block-size independence.
4. **UI:** no extra work is needed. The device chain panel creates knobs from `ParamInfo`,
   “+ Add Effect” lists `getEffectTypes()`, and automation targets and persistence also work
   automatically through parameter IDs.

## 9. Example: adding a model edit

For example, “change a clip's color”:

1. Add a property ID (`DAW_DECLARE_ID (clipColour)`) and a method to `model/Project.h`:
   `void setClipColour (juce::ValueTree clip, juce::Colour)`.
2. Have the method validate its input and change the tree using `&undoManager` so it is undoable.
3. If the change affects the audio engine, call the relevant synchronization method from
   `valueTreePropertyChanged`. Purely visual changes do not need this.
4. If the property is persistent, convert loaded values to the correct type in `normaliseTypes`.
5. In the UI: `project.beginTransaction ("Change clip colour"); project.setClipColour (...);`
6. Add tests in `tests/` for the change, undo/redo, and save/load.
7. Update [file-format.md](file-format.md).

## Desktop integration

Project files contain XML, so a desktop environment may open them as ordinary XML files, for
example in a browser. To open `.diydaw` files in diyDaw on double-click, add a user-level MIME
type and desktop entry:

`~/.local/share/mime/packages/diydaw.xml`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-diydaw-project">
    <comment>diyDaw project</comment>
    <sub-class-of type="application/xml"/>
    <glob pattern="*.diydaw" weight="90"/>
    <generic-icon name="audio-x-generic"/>
  </mime-type>
</mime-info>
```

`~/.local/share/applications/diydaw.desktop` (replace the path in `Exec` with your build's path):

```ini
[Desktop Entry]
Type=Application
Name=diyDaw
Comment=Ableton-style DAW
Exec="/path/to/diyDaw/build/DiyDaw_artefacts/Debug/diyDaw" %f
Icon=audio-x-generic
Terminal=false
Categories=AudioVideo;Audio;Music;
MimeType=application/x-diydaw-project;
```

Then run:

```bash
update-mime-database ~/.local/share/mime
update-desktop-database ~/.local/share/applications
xdg-mime default diydaw.desktop application/x-diydaw-project
```

## 11. Contributor workflow: parallel contributors and coding agents

When contributors and coding agents work in parallel:

- Define interfaces (headers) and buildable stubs before starting a feature, so contributors
  work against a shared contract.
- Each contributor edits only assigned files and uses a separate build directory covered by
  `.gitignore`, such as `build-contributor-a`.
- Test-focused contributors leave a failing regression test and a clear bug report; the
  contributor responsible for the implementation handles the fix.
- Review changes before integration, especially audio-thread allocations and object lifetimes.
- [AGENTS.md](../AGENTS.md) is the short technical guide; update it when the architecture changes.

## 12. Common build problems

| Error | Solution |
| --- | --- |
| `alsa/asoundlib.h: No such file` | `sudo apt install libasound2-dev` (or use `-DDAW_AUDIO_BACKENDS=OFF`) |
| `jack/jack.h: No such file` | `sudo apt install libjack-jackd2-dev` |
| `X11/extensions/XInput2.h` not found | `sudo apt install libxi-dev` |
| `ft2build.h` not found | `sudo apt install libfreetype-dev libfontconfig1-dev` |
| Empty `external/JUCE` | `git submodule update --init` |
| Test plugins not found | `cmake --build build --target DawEngineTests` also builds the plugins; reconfigure CMake if you changed the build directory |
| "JUCE Assertion failure in juce_MessageListener" in tests | Make sure the test runner starts with `ScopedJuceInitialiser_GUI` (`tests/TestMain.cpp`) |
