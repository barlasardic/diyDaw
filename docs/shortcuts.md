🇬🇧 English | 🇹🇷 [Türkçe](tr/kisayollar.md)

# diyDaw Shortcuts

All keyboard and mouse shortcuts. “Ctrl” means the Control key on Linux. For detailed
explanations, see the [user guide](user-guide.md).

## General

| Shortcut | Action |
| --- | --- |
| **Space** | Play / stop |
| **Tab** | Arrangement ↔ Mixer |
| **Shift+Tab** | Switch the bottom panel between Clip ↔ Device |
| **Ctrl+Z** | Undo |
| **Ctrl+Shift+Z** / **Ctrl+Y** | Redo |
| **Ctrl+N** | New project |
| **Ctrl+O** | Open project |
| **Ctrl+S** | Save |
| **Ctrl+Shift+S** | Save as |
| **Ctrl+Shift+R** | Export audio |
| **Ctrl+T** | New audio track |
| **Ctrl+Shift+T** | New MIDI track with synth |
| **Ctrl+Alt+T** | New drum track with drum rack |

## Transport and position

| Shortcut | Action |
| --- | --- |
| Stop button (while playing) | Stop |
| Stop button (while stopped) | Return to the start (clip start in Pattern mode) |
| Drag vertically in the BPM box | Change tempo |
| Double-click the BPM box | Reset to 120 BPM |
| Click / drag the position bar | Jump to a position / scrub (snaps to sixteenth notes) |
| … + **Alt** | Move freely without snapping |
| **PAT** / **SONG** | Pattern mode / Song mode |

## Arrangement

| Shortcut | Action |
| --- | --- |
| Click the time ruler | Move the playhead (snaps to the grid; **Alt** disables snapping) |
| Drag vertically on the time ruler | Zoom |
| **Ctrl + wheel** | Zoom around the pointer |
| **Wheel** | Scroll vertically if tracks do not fit, otherwise scroll in time |
| **Shift + wheel** | Scroll in time |
| Click empty space | Move the playhead, clear clip selection, select the track |
| Click a clip | Select the clip |
| Drag a clip's body | Move it, including to another track of the same type; **Alt** disables snapping |
| Drag a clip's edge | Trim its start / end |
| **Escape** (while dragging) | Cancel |
| **Delete** / **Backspace** | Delete the selected clip |
| **Ctrl+D** | Duplicate the selected clip immediately after itself |
| **Ctrl+E** | Split the selected clip at the playhead |
| **Ctrl+L** | Toggle looping for the selected clip |
| Double-click a MIDI clip | Open it in the piano roll |
| Double-click empty space on a MIDI track | Create and open a one-bar MIDI clip |
| Drop an audio file | Add a clip to the track (drop below all tracks to create a new track) |

## Track headers and layout

| Shortcut | Action |
| --- | --- |
| Double-click the name | Rename |
| Numbered button | Enable / disable the track (mute) |
| **S** | Solo |
| **A** | Show / hide the automation lane |
| Double-click the slider | Reset to 0 dB |
| Drag the track's bottom edge | Change track height |
| Double-click the bottom edge | Restore the default height |
| Drag the header / timeline boundary | Change header column width |
| Drag the bottom panel divider | Change bottom panel height |
| Double-click the bottom panel divider | Close / open the bottom panel |

## Piano roll

| Shortcut | Action |
| --- | --- |
| Double-click empty space (and drag) | Add a note |
| Click a note | Select it |
| **Shift** / **Ctrl** + click | Add to / remove from the selection |
| Drag from empty space | Marquee selection |
| **Ctrl+A** | Select all |
| Drag notes | Move in time and pitch; **Alt** disables snapping |
| Drag a note's right edge | Resize all selected notes |
| **Delete** / **Backspace** | Delete selected notes |
| **Right-click** | Delete a note |
| **Right-drag** | Eraser: delete notes under the pointer |
| **Ctrl+D** | Duplicate the selection immediately after itself |
| **↑ / ↓** | Transpose by a semitone |
| **Shift + ↑ / ↓** | Transpose by an octave |
| **← / →** | Move by one grid step |
| Drag in the velocity lane | Change velocity |
| Click a piano key | Preview the note |
| Click / drag the ruler | Move the playhead |
| Drag the orange loop marker | Change loop length |
| **Wheel** | Scroll in pitch |
| **Shift + wheel** | Scroll in time |
| **Ctrl + wheel** | Zoom |
| **Alt + wheel** / **Ctrl + Shift + wheel** | Key and note height (vertical zoom) |
| **−** / **+** (top-left corner) | Key and note height |
| Drag the keyboard's right edge | Keyboard width |
| **Escape** (while dragging) | Cancel |

## Automation lane

| Shortcut | Action |
| --- | --- |
| Click empty space (and drag) | Add a point |
| Drag a point | Move it without crossing its neighbors; **Alt** disables time snapping |
| **Shift** + drag | Change only the value |
| Double-click / right-click a point | Delete it |
| **Delete** (with a point selected) | Delete the point |
| **x** button | Delete all automation for that parameter |
| **Escape** (while dragging) | Cancel |

## Live playing (selected MIDI track)

Only active when Ctrl, Alt, and Shift are not held.

| Key | Action |
| --- | --- |
| `A S D F G H J K L ;` | White keys (C, D, E, F, G, A, B, C, D, E) |
| `W E T Y U O P` | Black keys (C#, D#, F#, G#, A#, C#, D#) |
| `Z` / `X` | Octave down / up (initially A = C3 = MIDI 60) |
| `C` / `V` | Velocity −0.1 / +0.1 (initially 0.8) |

## Devices (Device tab)

| Shortcut | Action |
| --- | --- |
| Drag a knob vertically / horizontally | Change its value (shown in a tooltip) |
| Double-click a knob | Restore the default value |
| Round button in an effect header | Enable / disable the effect (bypass) |
| **<** / **>** | Reorder the effect |
| **x** | Delete the effect |
| **+ Add Effect** | Add an effect or plugin |
| Drop-down in the instrument header | Choose Synth / Drum Rack / plugin instrument |
| Click a drum pad | Select and preview the pad |
| Drop a file on a pad | Load a sample into the pad |
| **Open Editor** | Open the plugin's own editor window |
| **Master** button / click the mixer master strip | Show master effects |
