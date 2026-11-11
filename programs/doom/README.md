# Doom (1993) for Minimal-OS

The original Doom - id Software's 1993 engine, as id released it under the
GNU GPL - running in a MOSdesk window, with its picture drawn through
FarcadeGL.

In Minimal-OS: `make mosdesk-doom` builds the desktop ISO with Doom on it.
After setup it is in the launcher as **Doom** (`0:/programs/Doom.run`, with
its data in `0:/programs/Doom/`).

## Playing

| Key | Action |
| --- | --- |
| Arrows | move and turn |
| Ctrl | fire |
| Space | open doors, press switches |
| Shift (held) | run |
| Alt (held) + arrows, or `,` / `.` | strafe |
| 1 - 7 | weapons |
| Tab | automap |
| Escape | menu (new game, save, load, options, quit) |
| F2 / F3 | save / load |
| Alt + Enter | full screen on / off |

| Mouse | Action |
| --- | --- |
| left / right | turn |
| forward / back | walk |
| left button | fire |
| right button (held) | strafe with the mouse |
| middle button | walk forward |

That is the 1993 layout; the buttons can be changed in `default.cfg`
(`mouseb_fire`, `mouseb_strafe`, `mouseb_forward`) and the speed in the
Options menu. While a level is played the window keeps the mouse to itself
and hides the cursor (pointer lock); Escape (the menu), pausing or
switching to another window gives the cursor back.

The window can be resized; the picture keeps Doom's 4:3 shape.

Settings and savegames are kept in `0:/programs/Doom/` (`default.cfg`,
`savegames/`).

## Game data

Doom needs an IWAD, the file with the levels, graphics and sounds. The
engine is free software; id's game data is not, so Minimal-OS does not
ship it:

- **Freedoom: Phase 1** (`freedoom1.wad`) is bundled by `make mosdesk-doom`.
  It is a complete, free (BSD-licensed) set of levels and graphics made for
  the Doom engine by the Freedoom project, <https://freedoom.github.io>.
- **Your own copy of Doom**: `make mosdesk-doom DOOM_WAD=/path/to/DOOM.WAD`
  puts it on the ISO instead, or copy it into `0:/programs/Doom/` later.
  `doom.wad` (registered), `doomu.wad` (Ultimate Doom) and `doom1.wad`
  (the shareware episode) are found by name, then Freedoom, then the Doom II
  family (`doom2.wad`, `plutonia.wad`, `tnt.wad`, `freedoom2.wad`).
- From the terminal, a file on the command line wins:
  `run 0:/programs/Doom.run 0:/users/me/doom.wad`. Other Doom options pass
  through, e.g. `-warp 1 3` or `-skill 4`.

## How it is put together

| Part | What |
| --- | --- |
| `src/` | The engine: [doomgeneric](https://github.com/ozkl/doomgeneric), which is Chocolate Doom (a faithful port of id's released source) made portable. GPL version 2 or later, see `COPYING`. |
| `main.c` | The Minimal-OS platform layer: window, keyboard, mouse, clock, and the picture through FarcadeGL. |
| `sound.c` | Sound effects through a Minimal-OS audio stream. |
| `libc/`, `include/` | The C library parts the engine needs that the SDK does not have (stdio files, printf/scanf, ctype, a little math), on top of the SDK's headers. |
| `tests/host/` | A host test (below). |

**The picture.** Doom draws its 320x200 frame in software, exactly as in
1993 - that is what makes it Doom. Each frame is uploaded as a FarcadeGL
texture and drawn as one quad by a small FGSL shader, scaled to the window
with Doom's chunky pixels kept sharp. On a graphics card that runs
FarcadeGL in hardware the scaling is done there.

**Changes to the engine** (all marked "Minimal-OS" in the source):
the frame is 320x200 (`doomgeneric.h`) so the GPU does the scaling; the
config file is read and written, which doomgeneric had left out; quitting
from the menu ends the program; settings go to the game's folder; the zone
memory is 16 MiB (Freedoom's levels are larger than the originals).

**Keys.** Doom uses Ctrl, Shift and Alt as keys of their own. The Minimal-OS
USB keyboard driver sends them as key events (HID codes 0xE0-0xE7) for that.

**Sound effects.** Doom's effects are short 8-bit recordings (11025 Hz
mostly). `sound.c` plays up to 16 at once with Doom's volume and stereo
panning, mixing them into one 22050 Hz stereo audio stream
(`mos_audio_stream_*`, see the SDK's `docs/audio.md`); the kernel mixes that
with whatever else is playing. Each game tic tops the stream up to about
70 ms ahead. Without a sound card Doom says so at start and plays silently.

**Mouse.** mosWM's pointer lock (`mos_window_set_pointer_lock`) sends how
far the mouse moved instead of where the cursor is; `main.c` adds up the
motion of a tic and hands it to Doom as one mouse event, as the DOS
version read the mouse once per tic.

**Not yet:** music. Doom's songs are MUS scores played on an OPL2 FM chip
(AdLib, Sound Blaster) or a MIDI synthesizer; playing them needs one of
those synthesized in software first.

## Host test

`tests/host/run.sh <iwad>` builds the engine and this C library as a
freestanding Linux program (no host C library: `tests/host/sdk/` maps the
Minimal-OS calls to Linux system calls), plays a scripted stretch - title,
menu, new game, walking, firing, the automap, quit - on a simulated clock,
writes screenshots as PPM files, and runs a second time with the config
the first run saved. `sound.c` is in it too, writing into a stand-in audio
stream (`tests/host/sdk/audio.h`) that counts what comes out.
