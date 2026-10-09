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
	int  dump_shaders;
	int  aspect;			// 0 = auto (from the desktop), 43 = 4:3, 169 = 16:9
	int  ssao;			// ambient occlusion on the 3D image
	float ssao_strength;		// 0..1, how dark creases get
	float ssao_radius;		// world inches
	float shadow_softness;		// skater shadow edge blur, shadow-map texels (0 = hard)
	int  sun_shadows;		// shadows of the level cast by the time-of-day sun
	float sun_strength;		// 0..1, starting strength of a level switched on in game
	int  voices;			// voice acting streams (pcm.wad)
	char data_root[1024];		// folder holding "data" (default: the exe's folder)
};

const DesktopConfig &desktop_config( void );
// 1 when the game runs in 4:3 like the PS2/Xbox originals, 0 for the Vita's 16:9.
extern "C" int desktop_ecran_43( void );
extern "C" float desktop_shadow_softness( void );
extern "C" int desktop_voices( void );
extern "C" float desktop_sun_strength( void );	// 0 = sun shadows off everywhere
extern "C" int  desktop_sun_level( unsigned level, float *heading, float *pitch, float *strength );
extern "C" void desktop_sun_seed( float heading, float pitch );
extern "C" void desktop_sun_key( int fkey, int shift );
extern "C" int  desktop_sun_auto_request( void );
extern "C" int  desktop_sun_show_baked( void );
extern "C" void desktop_sun_set_level( unsigned level, float heading, float pitch );

#endif
