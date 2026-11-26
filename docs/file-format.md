🇬🇧 English | 🇹🇷 [Türkçe](tr/dosya-formati.md)

# `.diydaw` Project File Format

A diyDaw project is a single UTF-8 **XML** file with the `.diydaw` extension. The file directly
represents the in-memory `juce::ValueTree`: each node is an XML element, and each property is
an XML attribute. This document describes format version **1**.

Files are human-readable and can be edited by hand. The app validates and repairs them on
load (see [Load-time rules](#load-time-rules)).

## Overall structure

```
PROJECT
├── TRACK  (0..16, in playback order)
│   ├── CLIP   (audio clips on audio tracks, MIDI clips on MIDI tracks)
│   │   └── NOTE (MIDI clips only)
│   ├── DEVICE (instrument first on MIDI tracks, then effects; effects only on audio tracks)
│   └── AUTOMATION
│       └── POINT
└── MASTER (always the last child)
    └── DEVICE (master effects)
```

Numbers use a decimal point. `double` values use the shortest representation that reproduces
the exact same bits when read back (for example, `1.8765432109876543`), so saving and loading
does not shift clips. Booleans are written as `1` / `0`.

## PROJECT

| Attribute | Type | Description |
| --- | --- | --- |
| `version` | integer | Format version, currently `1`. Files with a version newer than the app supports are rejected |
| `bpm` | number | Tempo, 20–999 (default 120) |
| `beatsPerBar` | integer | Beats per bar, 1–32 (default 4) |
| `masterGain` | number | Master fader in dB, −60…+6 (default 0) |

## TRACK

| Attribute | Type | Description |
| --- | --- | --- |
| `kind` | `audio` / `midi` | Track type; defaults to `audio` if absent. Drum tracks are also `midi` |
| `name` | text | Display name; an empty name becomes "N Audio" |
| `colour` | ARGB hexadecimal | For example, `ffff8a3d`. Assigned from the palette if absent |
| `gain` | number | Fader in dB, −60…+6 (default 0) |
| `pan` | number | −1 (left) … +1 (right) (default 0) |
| `mute` | 0/1 | 1 = muted |
| `solo` | 0/1 | 1 = solo |
| `height` | integer | Arrangement height in pixels, 40–320 (default 64) |

The `engineIndex` attribute is runtime information and is not written to the file.

## CLIP (audio)

Found on audio tracks. The `kind` attribute is `audio` or absent.

| Attribute | Type | Description |
| --- | --- | --- |
| `kind` | `audio` | |
| `name` | text | Display name (usually the filename) |
| `file` | path | Absolute path to the audio file |
| `relativeFile` | path | Path to the same file relative to the project file's directory (file only; used during loading, then removed) |
| `start` | number | Timeline start in beats (≥ 0) |
| `length` | number | Length in beats (≥ 1/16) |
| `offset` | number | Position of the clip's first sample within the audio file, in **seconds** |
| `loop` | 0/1 | 1 = repeat the audio file throughout the clip |

Audio clips play at their natural speed: one second on the timeline is one second in the file.

## CLIP (MIDI)

Found on MIDI tracks.

| Attribute | Type | Description |
| --- | --- | --- |
| `kind` | `midi` | |
| `name` | text | Display name (default "MIDI Clip") |
| `start` | number | Timeline start in beats |
| `length` | number | Length in beats |
| `offsetBeats` | number | Position of the clip's left edge in content time, in beats |
| `loop` | 0/1 | 1 = repeat the content every `loopLength` beats |
| `loopLength` | number | Loop length in beats (defaults to `length` if absent) |

**Content time:** at timeline position `t`, the content position is
`offsetBeats + (t − start)`. With looping enabled, this wraps at `loopLength` (the content loop
is `[0, loopLength)`), and notes extending past the loop end are cut there. With looping
disabled, content plays straight through.

### NOTE

| Attribute | Type | Description |
| --- | --- | --- |
| `pitch` | integer | MIDI note number, 0–127 (60 = C3) |
| `start` | number | Start in content time, in beats (≥ 0) |
| `length` | number | Length in beats (≥ 1/64) |
| `velocity` | number | 0–1 |

Notes do not need to be sorted in the file.

## DEVICE

Common attributes:

| Attribute | Type | Description |
| --- | --- | --- |
| `type` | text | Device type (see the tables below) |
| `uid` | integer | Project-unique identifier referenced by automation targets |
| `enabled` | 0/1 | Effects only: 0 = bypass (default 1) |
| (parameters) | number | Each parameter is stored as an attribute named after its ID. Missing parameters use their defaults |

On a MIDI track, the first DEVICE is the instrument (`synth`, `drumrack`, or `pluginInstrument`);
subsequent devices on any track are effects, in processing order. Audio tracks and MASTER
have no instrument.

### `synth` parameters

| ID | Range | Default |
| --- | --- | --- |
| `osc1Wave`, `osc2Wave` | 0 = saw, 1 = square, 2 = triangle, 3 = sine | 0 |
| `osc2Semitones` | −24…24 (integer) | 0 |
| `osc2Detune` | −50…50 cents | 7 |
| `oscMix` | 0…1 | 0.5 |
| `filterCutoff` | 20…20000 Hz | 2000 |
| `filterResonance` | 0…1 | 0.2 |
| `filterEnvAmount` | −1…1 | 0.25 |
| `filterKeyTrack` | 0…1 | 0.5 |
| `filterAttack`, `filterDecay`, `filterRelease` | 0.001…10 s | 0.005 / 0.3 / 0.3 |
| `filterSustain` | 0…1 | 0.5 |
| `ampAttack`, `ampDecay`, `ampRelease` | 0.001…10 s | 0.005 / 0.3 / 0.3 |
| `ampSustain` | 0…1 | 0.7 |
| `velocitySens` | 0…1 | 0.8 |
| `volume` | −60…6 dB | 0 |

### `drumrack`

Per pad (N = 1…16; pad 1 = MIDI 36):

| ID | Range | Default |
| --- | --- | --- |
| `padNFile` | text | Pad sample: an absolute audio file path or `builtin:<name>` |
| `padNRelativeFile` | path | File only: path relative to the project directory |
| `padNGain` | −60…6 dB | 0 |
| `padNPan` | −1…1 | 0 |
| `padNTune` | −24…24 semitones (integer) | 0 |
| `padNDecay` | 0.01…10 s | 10 |
| `padNChoke` | 0…8 (0 = no group) | 0 |

Built-in sounds: `builtin:kick`, `rim`, `snare`, `clap`, `shaker`, `lowTom`, `closedHat`, `midTom`,
`cowbell`, `highTom`, `openHat`, `conga`, `zap`, `crash`, `sub`, `ride`. A new drum track assigns
these to pads 1–16 in this order, with `closedHat` and `openHat` in choke group 1. A pad without
a `padNFile` attribute is empty.

### Effects

| `type` | Parameter IDs (range, default) |
| --- | --- |
| `eq` | `lowFreq` (20–1000 Hz, 100), `lowGain` (−18…18 dB, 0), `midFreq` (100–10000 Hz, 1000), `midGain` (−18…18, 0), `midQ` (0.3–8, 1), `highFreq` (1000–20000 Hz, 8000), `highGain` (−18…18, 0), `output` (−18…18, 0) |
| `compressor` | `threshold` (−60…0 dB, −18), `ratio` (1–20, 4), `attack` (0.1–100 ms, 10), `release` (10–1000 ms, 150), `knee` (0–24 dB, 6), `makeup` (0–24 dB, 0), `mix` (0–1, 1) |
| `reverb` | `size` (0–1, 0.5), `damping` (0–1, 0.5), `predelay` (0–200 ms, 10), `width` (0–1, 1), `mix` (0–1, 0.25) |
| `delay` | `sync` (0/1, 1), `syncDivision` (0–6: 1/16, 1/8, dotted 1/8, 1/4, dotted 1/4, 1/2, 1 bar; 3), `timeMs` (1–2000, 375), `feedback` (0–0.95, 0.35), `pingPong` (0/1, 0), `lowCut` (20–2000 Hz, 80), `highCut` (1000–20000 Hz, 8000), `mix` (0–1, 0.3) |

### Plugins: `plugin` and `pluginInstrument`

| Attribute | Description |
| --- | --- |
| `type` | `plugin` (effect) or `pluginInstrument` (MIDI track instrument) |
| `pluginName` | Display name |
| `description` | A single-line JUCE `PluginDescription` XML string (format, file path, unique ID, manufacturer, category, ...). Used to locate and load the plugin |
| `state` | The plugin's own state (`getStateInformation`), encoded as **base64**. Written on save |
| `enabled` | `plugin` only: bypass |

Plugin parameters are not stored as individual attributes; they are all in `state`. If a plugin
cannot be found, its DEVICE is preserved unchanged, including its `description` and last known
`state` when the project is saved again.

## AUTOMATION and POINT

| Node | Attribute | Description |
| --- | --- | --- |
| AUTOMATION | `target` | `gain`, `pan`, or `<device uid>:<parameter ID>` (for example, `1:filterCutoff`, `4:mix`, `7:pad1Gain`) |
| POINT | `beat` | Timeline position in beats (≥ 0) |
| POINT | `value` | Value in the parameter's own units (dB, Hz, 0–1, ...) |

Values interpolate linearly between points. Before the first point, its value applies; after
the last point, its value applies. An AUTOMATION node with no points has no effect on playback.
An AUTOMATION node whose target no longer exists (for example, a deleted device) is preserved
in the file but ignored during playback. Plugin parameters cannot currently be targets.

## MASTER

Always the last child of PROJECT, containing only effect DEVICEs (built-in effects or `plugin`).
If absent, an empty MASTER is created on load.

## File paths

For audio clips and drum pads, both an absolute path (`file` / `padNFile`) and a path relative
to the project directory (`relativeFile` / `padNRelativeFile`) are written. On load:

1. If the file exists at the absolute path, that path is used.
2. Otherwise, the relative path is resolved against the project file's directory. If the file
   exists there, the path is updated.
3. If neither exists, the clip / pad stays silent and appears as "Missing" in the UI.

This lets you move a whole project folder (project file plus nearby audio files) to another
location or computer. Audio files are not copied into the project.

## Load-time rules

The app performs these steps when loading, so handwritten or malformed files can be opened safely:

- Convert XML string values to numbers / booleans.
- Clamp values to their ranges (tempo, gain, pan, note pitch and velocity, device parameters;
  round stepped parameters), and clamp negative starts to 0.
- Discard clips that do not match the track type (such as audio clips on a MIDI track) and
  unrecognized elements.
- Add a synth if a MIDI track has no instrument. If it has several, keep the first and move
  it to the start of the chain. Discard instruments on audio tracks and unknown effect types.
- Reassign missing or duplicate `uid` values.
- Load only the first 16 tracks if there are more than 16.
- Add MASTER if absent, keep the first if there are several, and move it to the end.
- Reject files with a format version newer than the app supports, leaving the current project
  untouched.

## Example

```xml
<?xml version="1.0" encoding="UTF-8"?>
<PROJECT version="1" bpm="110" beatsPerBar="4" masterGain="0">
  <TRACK kind="midi" name="Chords" colour="ffc77aff" gain="-4" pan="0" mute="0" solo="0" height="96">
    <DEVICE type="synth" uid="1" filterCutoff="1200" ampAttack="0.4" ampRelease="0.8"/>
    <DEVICE type="reverb" uid="2" enabled="1" size="0.7" mix="0.3"/>
    <CLIP kind="midi" name="Am F C G" start="0" length="32" offsetBeats="0" loop="1" loopLength="16">
      <NOTE pitch="57" start="0" length="3.5" velocity="0.7"/>
      <NOTE pitch="60" start="0" length="3.5" velocity="0.7"/>
      <NOTE pitch="64" start="0" length="3.5" velocity="0.7"/>
    </CLIP>
    <AUTOMATION target="1:filterCutoff">
      <POINT beat="0" value="300"/>
      <POINT beat="16" value="6000"/>
    </AUTOMATION>
  </TRACK>
  <TRACK kind="midi" name="Drums" colour="ffff8a3d">
    <DEVICE type="drumrack" uid="3" pad1File="builtin:kick" pad3File="builtin:snare"
            pad7File="builtin:closedHat" pad11File="builtin:openHat" pad7Choke="1" pad11Choke="1"/>
    <CLIP kind="midi" name="Beat" start="0" length="4" offsetBeats="0" loop="1" loopLength="4">
      <NOTE pitch="36" start="0" length="0.25" velocity="1"/>
      <NOTE pitch="38" start="1" length="0.25" velocity="0.9"/>
    </CLIP>
  </TRACK>
  <TRACK kind="audio" name="Vocal">
    <CLIP kind="audio" name="vocal" file="/path/to/project/vocal.wav"
          relativeFile="vocal.wav" start="8" length="16" offset="0" loop="0"/>
  </TRACK>
  <MASTER>
    <DEVICE type="compressor" uid="4" enabled="1" threshold="-20" ratio="2"/>
  </MASTER>
</PROJECT>
```

The repository's `examples/midi-demo.diydaw` and `examples/drums-demo.diydaw` are also working,
handwritten examples.

## Version history

| Version | Change |
| --- | --- |
| 1 | Initial format. Later additions (MIDI, devices, automation, MASTER, track height, plugins) are backward-compatible: anything absent from older files takes its default value |
