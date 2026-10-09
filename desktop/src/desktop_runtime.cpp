/*****************************************************************************
**  THUG desktop -- settings and the Vita debug helpers' desktop stand-ins  **
**  desktop/src/desktop_runtime.cpp                                         **
*****************************************************************************/

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "desktop_config.h"

// ---------------------------------------------------------------------------
// thug_desktop.ini, next to the executable. Written with defaults if absent.
// ---------------------------------------------------------------------------

static const char *s_default_ini =
	"; Tony Hawk's Underground -- desktop settings\n"
	"[display]\n"
	"; window size; 0 = your desktop's resolution\n"
	"width=0\n"
	"height=0\n"
	"; 0 = window, 1 = borderless fullscreen, 2 = exclusive fullscreen (Alt+Enter toggles)\n"
	"fullscreen=1\n"
	"; 3D render size; 0 = match the window\n"
	"render_width=0\n"
	"render_height=0\n"
	"; anti-aliasing samples: 0, 2, 4 or 8\n"
	"msaa=4\n"
	"vsync=1\n"
	"\n"
	"[paths]\n"
	"; folder that holds the game's \"data\" folder; empty = this folder\n"
	"data=\n"
	"\n"
	"[debug]\n"
	"dump_shaders=0\n";

static DesktopConfig s_cfg;
static bool s_loaded = false;

static void trim( char *s )
{
	char *e = s + strlen( s );
	while( e > s && isspace( (unsigned char)e[-1] )) *--e = 0;
	char *b = s;
	while( *b && isspace( (unsigned char)*b )) ++b;
	if( b != s ) memmove( s, b, strlen( b ) + 1 );
}

static void parse( FILE *f )
{
	char line[1200];
	while( fgets( line, sizeof( line ), f ))
	{
		trim( line );
		if( !line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[' )
			continue;
		char *eq = strchr( line, '=' );
		if( !eq ) continue;
		*eq = 0;
		char *k = line, *v = eq + 1;
		// Inline comment: " ; ..." (a path can hold ';' without a space before it).
		for( char *c = v; *c; ++c )
			if(( *c == ';' || *c == '#' ) && ( c == v || isspace( (unsigned char)c[-1] )))
			{
				*c = 0;
				break;
			}
		trim( k );
		trim( v );
		const int iv = atoi( v );
		if( !strcmp( k, "width" ))              s_cfg.window_w = iv;
		else if( !strcmp( k, "height" ))        s_cfg.window_h = iv;
		else if( !strcmp( k, "fullscreen" ))    s_cfg.fullscreen = iv;
		else if( !strcmp( k, "render_width" ))  s_cfg.render_w = iv;
		else if( !strcmp( k, "render_height" )) s_cfg.render_h = iv;
		else if( !strcmp( k, "msaa" ))          s_cfg.msaa = iv;
		else if( !strcmp( k, "vsync" ))         s_cfg.vsync = iv;
		else if( !strcmp( k, "dump_shaders" ))  s_cfg.dump_shaders = iv;
		else if( !strcmp( k, "data" ))          snprintf( s_cfg.data_root, sizeof( s_cfg.data_root ), "%s", v );
	}
}

const DesktopConfig &desktop_config( void )
{
	if( s_loaded )
		return s_cfg;
	s_loaded = true;
	memset( &s_cfg, 0, sizeof( s_cfg ));
	s_cfg.fullscreen = 1;
	s_cfg.msaa = 4;
	s_cfg.vsync = 1;

	char path[1200];
	char *base = SDL_GetBasePath();
	snprintf( path, sizeof( path ), "%sthug_desktop.ini", base ? base : "" );
	SDL_free( base );
	const char *env = getenv( "THUG_DESKTOP_INI" );
	if( env && *env )
		snprintf( path, sizeof( path ), "%s", env );
	FILE *f = fopen( path, "r" );
	if( !f )
	{
		f = fopen( path, "w" );
		if( f ) { fputs( s_default_ini, f ); fclose( f ); }
		f = fopen( path, "r" );
	}
	if( f )
	{
		parse( f );
		fclose( f );
	}
	const char *data = getenv( "THUG_DATA" );
	if( data && *data )
		snprintf( s_cfg.data_root, sizeof( s_cfg.data_root ), "%s", data );
	// 0 (or nonsense) = the desktop's size; the window code also keeps a
	// window inside the screen.
	if( s_cfg.window_w < 320 || s_cfg.window_h < 200 )
		s_cfg.window_w = s_cfg.window_h = 0;
	return s_cfg;
}

// ---------------------------------------------------------------------------
// Vita-only debug tools (TCP debug server, profiler, allocation and GL
// spies): not built on desktop. The engine still calls their entry points.
// ---------------------------------------------------------------------------

extern "C"
{
typedef struct VitaDbgPad VitaDbgPad;
void vita_dbgsrv_start( void ) {}
void vita_dbgsrv_frame( void ) {}
int  vita_dbgsrv_take_command( char *, int ) { return 0; }
int  vita_dbgsrv_pad( VitaDbgPad * ) { return 0; }
void vita_dbgsrv_set_busy( int ) {}
void vita_glspy_bilan( void ) {}
int  vita_glspy_ouverte = 0, vita_glspy_phase = 0;
// Same budget as the Vita build (vita/src/vita_runtime.cpp): the park editor
// sizes its pieces against it.
int  _newlib_heap_size_user = 192 * 1024 * 1024;
// vitaGL texture upload pool: the engine only asks for its alignment.
uint8_t *vgl_reserve_data_pool( uint32_t ) { return NULL; }
void vita_ecran_boot_tot( void ) {}
int  vita_ecran_boot( void ) { return 0; }
void vita_memspy_niveau( void ) {}
void vita_prof_enable( int ) {}
int  vita_prof_dump( unsigned int *, unsigned long long *, unsigned int *, int ) { return 0; }
}

// Engine global defined in Sys/Win32/WinMain.cpp on PC (vita/src/vita_runtime.cpp on Vita).
bool g_hasJustEnteredNetworkGame = false;

extern "C" unsigned long crc32( unsigned long crc, const unsigned char *buf, unsigned int len )
{
	if( !buf )
		return 0;
	crc = ~crc & 0xFFFFFFFFul;
	for( unsigned int i = 0; i < len; ++i )
	{
		crc ^= buf[i];
		for( int k = 0; k < 8; ++k )
			crc = ( crc >> 1 ) ^ ( 0xEDB88320ul & ( 0ul - ( crc & 1 )));
	}
	return ~crc & 0xFFFFFFFFul;
}

extern "C" void desktop_fatal( const char *msg )
{
	fprintf( stderr, "%s\n", msg );
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, "Tony Hawk's Underground", msg, NULL );
	exit( 1 );
}
