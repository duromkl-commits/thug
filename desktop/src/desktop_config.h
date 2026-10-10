// THUG desktop -- settings read from thug_desktop.ini next to the game.
#ifndef THUG_DESKTOP_CONFIG_H
#define THUG_DESKTOP_CONFIG_H

struct DesktopConfig
{
	int  window_w, window_h;
	int  fullscreen;		// 0 window, 1 borderless desktop, 2 exclusive
	int  render_w, render_h;	// 0 = follow the window
	int  msaa;			// 0/1 off, 2, 4, 8
	int  vsync;
	int  refresh_rate;		// fullscreen refresh: -1 auto (a multiple of 60), 0 the desktop's, else Hz
	int  dump_shaders;
	int  aspect;			// 0 = auto (from the desktop), 43 = 4:3, 169 = 16:9
	int  ssao;			// ambient occlusion on the 3D image
	float ssao_strength;		// 0..1, how dark creases get
	float ssao_radius;		// world inches
	int  bloom;			// glow around bright areas
	float bloom_strength;		// 0..1
	float bloom_threshold;		// 0..1, brightness where glow starts
	int  fog;			// distance haze in the sky's colour
	float fog_strength;		// 0..1, haze at the far end
	float fog_distance;		// inches to half of fog_strength
	int   dof;			// depth of field: blur well behind the skater
	float dof_strength;		// 0-1, blur mix at its farthest
	int   ps2_dither;		// PS2 16-bit frame buffer dither over the whole picture
	int   dither_bits;		// colour bits per channel after the dither (5 = PS2 16-bit)
	float saturation;		// colour strength, 1 = as rendered
	int   tv_levels;		// PS2 video-out levels: black 16, white 235, lifted mids
	float soften;			// 0-1, blur over the finished picture (TV softness)
	float ghosting;			// 0-0.8, share of the last frame mixed into each new one
	float overscan;			// black border each side, fraction of the width (PS2 look)
	float shadow_softness;		// skater shadow edge blur, shadow-map texels (0 = hard)
	int  voices;			// voice acting streams (pcm.wad)
	float walk_lean;		// on-foot body lean into speed and turns (0 = off, 1 = default)
	float camera_shake;		// camera dip on hard landings, shake on bails (0 = off, 1 = default)
	float camera_fov_push;		// degrees the view widens at top skating speed (0 = off)
	float head_bob;			// on-foot camera bob with each step (0 = off, 1 = default)
	float score_scale;		// goal score targets x this (1 = as shipped)
	float time_scale;		// goal time limits x this (1 = as shipped)
	int  prestige;			// 1: beating the story raises the prestige level (thug_prestige.txt)
	float prestige_points_per_level;	// per prestige level: score targets +this share
	float prestige_points_max;	// prestige never takes score targets past this multiple
	float prestige_time_per_level;	// per prestige level: time limits -this share
	float prestige_time_min;	// prestige never takes time limits under this multiple
	char data_root[1024];		// folder holding "data" (default: the exe's folder)
};

const DesktopConfig &desktop_config( void );
// 1 when the game runs in 4:3 like the PS2/Xbox originals, 0 for the Vita's 16:9.
extern "C" int desktop_ecran_43( void );
extern "C" float desktop_shadow_softness( void );
extern "C" int desktop_voices( void );
extern "C" float desktop_walk_lean( void );
extern "C" float desktop_camera_shake( void );
extern "C" float desktop_camera_fov_push( void );
extern "C" float desktop_head_bob( void );
extern "C" float desktop_goal_score_scale( void );
extern "C" float desktop_goal_time_scale( void );
extern "C" void desktop_prestige_story_beaten( void );

#endif
