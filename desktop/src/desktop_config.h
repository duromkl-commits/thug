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
	char data_root[1024];		// folder holding "data" (default: the exe's folder)
};

const DesktopConfig &desktop_config( void );
// 1 when the game runs in 4:3 like the PS2/Xbox originals, 0 for the Vita's 16:9.
extern "C" int desktop_ecran_43( void );

#endif
