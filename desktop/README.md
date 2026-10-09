# THUG on Windows (desktop build)

This runs the Vita port's engine on a PC: same game code, with the Vita's
graphics, input, audio and file system replaced by SDL2 and OpenGL.

## Install

1. Extract the **Xbox USA** disc with [extract-xiso](https://github.com/XboxDev/extract-xiso).
   You get a `data` folder.
2. Put `thug.exe` and `SDL2.dll` in one folder, and the `data` folder next to them:

   ```
   Tony Hawk's Underground Remastered\
       thug.exe
       SDL2.dll
       data\
   ```

3. Run `thug.exe`. The first run writes `thug_desktop.ini` next to it, and the
   game log goes to `thug.log`.

If the game files aren't found, a message says where it looked.

## Settings (`thug_desktop.ini`)

```ini
[display]
width=0             ; window size; 0 = your desktop resolution
height=0
fullscreen=1        ; 0 = window, 1 = borderless fullscreen, 2 = exclusive (Alt+Enter toggles)
render_width=0      ; internal resolution; 0 = match the window
render_height=0
msaa=4              ; antialiasing samples (0 = off)
vsync=1

[paths]
data=               ; folder that contains "data"; empty = next to the .exe
```

## Controls

| Xbox pad | Keyboard | Game (PS2 name) |
|---|---|---|
| Left stick / D-pad | WASD / arrows | Steer, walk, balance |
| Right stick | IJKL | Camera |
| A | Space | ✕ Ollie |
| X | Left Shift | □ Flip |
| B | Left Ctrl / Backspace | ○ Grab |
| Y | F | △ Grind |
| LB / RB | Q / E | L1 / R1 Spin |
| LT / RT | Z / C | L2 / R2 Nollie / Switch, revert |
| Start | Enter / Esc | Start |
| Back | Tab | Select |

## Building

32-bit only (the engine assumes 32-bit pointers).

- **Windows .exe from Linux:** install `mingw-w64`, `cmake`, `ninja`, get
  `SDL2-devel-2.x.y-mingw.tar.gz` from the SDL releases, then
  `SDL2_MINGW=<SDL2-2.x.y/i686-w64-mingw32> desktop/tools/build_mingw32.sh`.
  GitHub Actions does the same (`.github/workflows/desktop-windows.yml`).
- **Linux test build:** `SDL2_ROOT=<32-bit SDL2> desktop/tools/build_linux32.sh`.

Both build from a copy of the sources with case-fixed include links
(`desktop/tools/case_shadow.py`), because the engine was written on Windows.

## What's different from the Vita build

- The Vita's direct-to-GPU draw paths (`p_gxm_desktop.inc`) are plain OpenGL.
- Cg shaders are translated to GLSL at load time (`src/cg_to_glsl.cpp`).
- The game draws into an offscreen 960x544-shaped screen scaled to the render
  size, then letterboxed into the window.
- No videos yet, no network play.
