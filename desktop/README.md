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
refresh_rate=auto   ; fullscreen: on a 144/165 Hz display, switch to 120 Hz while playing (0 = leave the display alone, or a rate like 120)
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
camera_shake=1.0    ; camera dips on hard landings and shakes on bails; 0 = off, 2 = double
camera_fov_push=4.0 ; the view widens by this many degrees at top skating speed; 0 = off
head_bob=1.0        ; on foot, the camera bobs a little with each step; 0 = off, 2 = double

[difficulty]
score_scale=1.0     ; goal score targets x this (1.5 = half as many points again)
time_scale=1.0      ; goal time limits x this (0.8 = a fifth less time)
prestige=1          ; new game+: each time you beat the story, goals get a bit harder
prestige_points_per_level=0.10 ; +10% points per prestige level...
prestige_points_max=2.0        ; ...up to 2x the original
prestige_time_per_level=0.02   ; -2% time per prestige level...
prestige_time_min=0.8          ; ...down to 80% of the original

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

`refresh_rate=auto` (the default) does that switch for you: in fullscreen,
when the display runs at a rate 60 doesn't divide into, the game takes the
screen in exclusive fullscreen at the same resolution and the highest rate
that is a multiple of 60 (120 Hz on a 144 Hz monitor). Windows puts the
display back when you Alt+Tab out or quit; expect a brief black flash
each time. `refresh_rate=0` keeps borderless at the desktop's rate.

## Difficulty and prestige

`score_scale` and `time_scale` change story goals' point targets and
time limits (goals with a point target of 1,000 or more; counters such as
"collect 5" stay as they are). The goal's text shows the new figure.

Prestige is a new game+: when the story's ending plays, the prestige level
goes up by one (kept in `thug_prestige.txt`), and point targets grow by
`prestige_points_per_level` per level (+10%), up to `prestige_points_max`
(2x) of the original, on top of `score_scale`; time limits shrink by
`prestige_time_per_level` (-2%) down to `prestige_time_min` (80%). Level 10
with the defaults: 2x the points in 80% of the time. Goals built around stops or a route (tours, H.O.R.S.E., moving score
spots like Chad Muska's SUV) keep their own figures. Delete
`thug_prestige.txt` to go back to level 0.

## Custom soundtrack

Put MP3, OGG, FLAC or WAV files in the `custom_music` folder next to
`thug.exe` (created on first launch; subfolders work too). On the next launch
they join the playlist in Options > Sound Options > Playlist, each under the
genre heading its **genre tag** fits: Punk (punk, pop punk, hardcore, ska,
emo), Hip Hop (hip hop, rap, trap, grime, drill) or Rock/Other (everything
else, and songs without a genre tag). They switch on and off like the game's
own songs and shuffle in with them.

- The name shown is the **album artist** (the artist when a file has none)
  and the title, from the file's tags. Untagged files use
  `Artist - Title.mp3` file names. Accented letters lose their accent; other
  non-Latin characters are dropped, since the game's fonts can't draw them.
- The playlist is in band order, like the game's own list: custom songs are
  sorted in among the stock ones by album artist (case and punctuation
  ignored), then title.
- Up to 512 songs in total: the game's 76 plus about 436 of yours. Extra
  files are skipped (thug.log says how many).
- Which songs are switched on or off is kept by song name in
  `thug_playlist.txt` next to `thug.exe`, so adding or removing files doesn't
  shuffle your choices. Delete it to switch everything back on.
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
