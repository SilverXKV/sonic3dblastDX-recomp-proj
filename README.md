# Sonic 3D Blast: Director's Cut - native Windows recompile

A native Windows port of the **Sonic 3D Blast: Director's Cut** ROM hack. The game's 68000 code is statically translated to C and compiled together with a small hardware layer (VDP, YM2612, SN76489, Z80), so it runs as an ordinary `.exe`, with a **native 16:9 mode**.

**No ROM and no game data are included.** You need your own copy of the Director's Cut RO., The translated code is generated from it on your own machine if you are building it yourself.

## Features

- 4:3 (original picture) or 16:9 (native widescreen): more of the level, enemies and objects are ACTUALLY drawn at the sides.
- Options window with different settings for the game.
- Keyboard and XInput gamepad support.
- Settings are saved in `s3d.ini` next to the exe.


## What you need (to run the built EXE)

- Windows 10 or 11
- **Your own ROM**: Sonic 3D Blast: Director's Cut, MD5 `742dd5c98d143ff6716800dee6a25dc5`.

## What you need (to build this yourself)
### (Skip if you have the .exe)

- Windows 10 or 11
- **Your own ROM**: Sonic 3D Blast: Director's Cut, MD5 `742dd5c98d143ff6716800dee6a25dc5`.
- [Python 3](https://www.python.org/downloads/) (standard library only, no packages to install)
- A C compiler, either:
  - Visual Studio or Build Tools with **Desktop development with C++**, or
  - MinGW-w64 `gcc` on your `PATH` (for example w64devkit or MSYS2)

## Build it with your own ROM
### (Skip if you have the .exe)

1. **Clone or download** this repository.
2. **Generate the translated code** from your ROM. Open a Command Prompt in the repository folder and run:

   ```bat
   set S3D_ROM=C:\path\to\your\rom.md
   python tools\gen.py generated
   ```

   This reads your ROM and writes the C files into a new `generated\` folder (a few seconds). It prints `emit errors 0` and `wrote 13 files` when it worked. The `generated\` folder is derived from the ROM, so it is not part of this repository and must not be committed or shared.
3. **Build the exe**:

   ```bat
   build.bat
   ```

   `build.bat` uses `gcc` if it is on your `PATH`, otherwise it finds Visual Studio on its own. It produces `Sonic 3D Blast - Directors Cut Recompiled.exe`.

## Run it

Double-click the exe. It looks for the ROM next to it under one of these names: `sonic3dblast_dc.md`, `Sonic 3D Blast - Director's Cut (World) (Unl).md`, `rom.md` or `rom.bin` (in the same folder). if none is found it asks you to pick the file. You can also pass the path:

```bat
"Sonic 3D Blast - Directors Cut Recompiled.exe" "C:\path\to\rom.md"
```

Command-line options: `--4:3` or `--16:9` force the display mode for that run, `--pal` uses 50 Hz timing.

## Controls and hotkeys

Defaults (all rebindable in **Options > Controls**):

| Action | Keys |
| --- | --- |
| D-pad | Arrow keys or WASD |
| A / B / C | Z / X / C or J / K / L |
| Start | Enter or Space |
| Fast forward (hold) | Tab |

Function hotkeys:

| Key | Does |
| --- | --- |
| F8 | Open Options on the Controls tab |
| F9 | Switch 4:3 / 16:9 |
| F11 or Alt+Enter | Full screen |
| Esc | Quit |

A change of display mode made while a stage is running is applied when the next stage starts. XInput gamepads are picked up automatically.

## How it works

- `tools\` translates the ROM's 68000 code to C (`gen.py` drives `analyze.py`, `emit.py`, `m68k.py` and friends). `tools\patches.py` lists the changes made to the translated code for the widescreen mode (wider tile streaming and object activation windows, a larger tile ring in a second video-memory bank, HUD placement, special stage handling). They do nothing in 4:3 mode, which is pixel-identical to the unmodified translation.
- `src\` is the hardware layer and the Windows front end: `hw.c` (memory map, VDP, Z80 bus), `render.c` (scanline renderer), `widescreen.c`, `audio.c` (YM2612 without PM-LFO / SSG-EG, SN76489), `z80.c` (interpreter for the sound driver) and `win32_main.c` (window, input, audio output, Options window).

## Notes

- Sound is an emulation of the YM2612 and SN76489 driven by the game's own Z80 sound driver; it is close but not cycle-exact.
- This is an unofficial fan project and is not affiliated with SEGA. Sonic 3D Blast and the Director's Cut ROM hack belong to their respective owners.
