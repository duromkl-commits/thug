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
aspect=auto        ; 4:3 (PS2/Xbox shape), 16:9 (Vita shape), auto = from your screen

[graphics]
ssao=1              ; ambient occlusion: darkens corners and contact points (2 = show only the occlusion, as a test)
ssao_strength=0.8   ; 0.0 - 1.0
ssao_radius=18      ; reach, in inches of game world
bloom=1             ; glow around bright areas
bloom_strength=0.15 ; 0.0 - 1.0
bloom_threshold=0.8 ; 0.0 - 1.0, brightness where the glow starts
fog=1               ; distance haze in the colour of the level's sky (none indoors)
fog_strength=0.25   ; 0.0 - 1.0, how thick it gets far away
fog_distance=8000   ; inches to reach half of that (8000 = about 200 m)
dof=1               ; depth of field: blurs what's far behind your skater
dof_strength=0.45   ; 0.0 - 1.0
ps2_dither=1        ; 4x4 Bayer dithering over the whole picture (PS2 16-bit colour)
dither_bits=5       ; bits per colour channel it dithers to: 5 = PS2, 6 = subtler, 8 = none
soften=1.0          ; 0.0 - 1.0, TV-like softness over the finished picture (hides most of the dither)
tv_levels=1         ; a touch of the PS2 video-out look: blacks and whites slightly softened
saturation=1.0      ; colour strength, 1.0 = as rendered
ghosting=0.2        ; 0.0 - 0.8, last frame left over in each new one (GTA III / Bully trails)
overscan=0.04       ; black border round the picture like the PS2 output (0.0 - 0.15)
shadow_softness=2.5 ; skater shadow edge blur (0 = hard, like the Xbox)

[gameplay]
walk_lean=1.0       ; on foot, lean with the stick into running and turns (American Wasteland style); 0 = off, 2 = double

[paths]
data=               ; folder that contains "data"; empty = next to the .exe
```

## Smooth motion

The game runs at 60 frames a second, like the consoles. That divides evenly
into 60, 120 and 240 Hz displays (59/119 Hz too), and there vsync shows
every frame for the same time. On 144 or 165 Hz there is no even split: some
frames stay up longer than others, and fast movement judders. Set the
monitor to 120 Hz in Windows display settings, or use G-Sync/FreeSync, for
smooth motion. thug.log's `frame pacing` line shows what the game picked.

## Custom soundtrack

Put MP3, OGG, FLAC or WAV files in the `custom_music` folder next to
`thug.exe` (created on first launch; subfolders work too). On the next launch
they join the playlist as a fourth genre, **Custom**, in Options > Sound
Options > Playlist: each song can be switched on or off, the Custom heading
toggles them all, and they shuffle in with the game's songs.

- The name shown is the **album artist** (the artist when a file has none)
  and the title, from the file's tags. Untagged files use
  `Artist - Title.mp3` file names. Accented letters lose their accent; other
  non-Latin characters are dropped, since the game's fonts can't draw them.
- The game keeps the playlist as 128 on/off switches: the stock songs
  take most of them (76 in the PC version), so roughly 50 custom songs fit. Extra files are skipped (thug.log says
  how many).
- Songs are ordered by file path. Adding or removing files shifts the saved
  on/off switches of the custom songs after them.
- M4A/AAC and WMA aren't supported.

## Controls

The game runs with PS2 controls and PS2 button icons: triangle backs out of
menus, spine transfer is R2 (or L2) alone. Old `thug_desktop.ini` files keep
working; missing `[graphics]` keys use the defaults above.

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
- The game is paced to 60 fps whatever the monitor refresh rate.
- Cutscenes are letterboxed 16:9 inside the 4:3 picture, like the PS2.
- Voice acting plays from `data/streams/pcm/pcm.wad`.
- No videos yet, no network play (System Link is removed from the menu).
