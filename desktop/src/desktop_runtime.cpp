/*****************************************************************************
**  THUG desktop -- settings and the Vita debug helpers' desktop stand-ins  **
**  desktop/src/desktop_runtime.cpp                                         **
*****************************************************************************/

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <string>
#include "vita_log.h"

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
	"; picture shape: 4:3 (like the PS2/Xbox originals), 16:9 (the Vita's), or auto = your screen's\n"
	"aspect=auto\n"
	"; anti-aliasing samples: 0, 2, 4 or 8\n"
	"msaa=4\n"
	"vsync=1\n"
	"; refresh rate: auto = if your display's rate isn't a multiple of 60 (144, 165 Hz), run it at one\n"
	"; that is (120 Hz) while the game is open, so every frame shows for the same time. Borderless keeps\n"
	"; its borderless window: the desktop switches, and Windows switches it back when the game closes.\n"
	"; 0 = leave the display alone; or a number, e.g. 120\n"
	"refresh_rate=auto\n"
	"\n"
	"[graphics]\n"
	"; ambient occlusion (darkens corners and contact points): 0 = off, 1 = on, 2 = show only the occlusion (test view)\n"
	"ssao=1\n"
	"; how dark it gets (0.0 - 1.0) and how far it reaches, in inches\n"
	"ssao_strength=0.8\n"
	"ssao_radius=18\n"
	"; glow around bright areas: 0 = off, 1 = on; how strong (0.0 - 1.0); brightness where it starts (0.0 - 1.0)\n"
	"bloom=1\n"
	"bloom_strength=0.15\n"
	"bloom_threshold=0.8\n"
	"; distance haze, coloured like the level's sky: 0 = off, 1 = on; how thick far away (0.0 - 1.0);\n"
	"; distance in inches where it reaches half of that (THUG levels: 8000 = about 200 m)\n"
	"fog=1\n"
	"fog_strength=0.25\n"
	"fog_distance=8000\n"
	"; depth of field: blurs what's far behind your skater: 0 = off, 1 = on; how much (0.0 - 1.0)\n"
	"dof=1\n"
	"dof_strength=0.45\n"
	"; PS2-style dithering over the whole picture (4x4 Bayer, like Aseprite's ordered dither): 0 = off, 1 = on;\n"
	"; colour bits per channel it dithers down to (5 = the PS2's 16-bit colour, 6 = subtler, 8 = none)\n"
	"ps2_dither=1\n"
	"dither_bits=5\n"
	"; softens the finished picture like the PS2 on a TV (0.0 - 1.0); also hides most of the dither weave\n"
	"soften=1.0\n"
	"; a touch of the PS2 video-out look: blacks and whites slightly softened: 0 = off, 1 = on\n"
	"tv_levels=1\n"
	"; colour strength: 1.0 = as rendered\n"
	"saturation=1.0\n"
	"; ghosting: a little of the last frame left over in each new one, like GTA III or Bully (0.0 - 0.8)\n"
	"ghosting=0.2\n"
	"; overscan: black border round the picture like the PS2 on a TV capture, share of the width each side (0.0 - 0.15)\n"
	"overscan=0.04\n"
	"; skater shadow edge blur (0 = hard like the Xbox, 2.5 = default soft)\n"
	"shadow_softness=2.5\n"
	"\n"
	"[gameplay]\n"
	"; on foot, the skater leans into running and into turns (American Wasteland style): 0 = off, 1 = default, 2 = double\n"
	"walk_lean=1.0\n"
	"; camera weight: a dip on hard landings and a shake on bails: 0 = off, 1 = default, 2 = double\n"
	"camera_shake=1.0\n"
	"; the view widens by this many degrees at top skating speed: 0 = off\n"
	"camera_fov_push=4.0\n"
	"; on foot, the camera bobs a little with each step: 0 = off, 1 = default, 2 = double\n"
	"head_bob=1.0\n"
	"\n"
	"[difficulty]\n"
	"; goal score targets and time limits: 1.0 = as shipped; score_scale=1.5 wants half as many points\n"
	"; again, time_scale=0.8 gives a fifth less time\n"
	"score_scale=1.0\n"
	"time_scale=1.0\n"
	"; prestige (new game+): each time you beat the story, point targets go up by prestige_points_percent\n"
	"; of the original (10 = +10% a level: level 10 = 2x, level 100 = 11x), with no ceiling unless\n"
	"; prestige_points_cap is set (e.g. 3 = never past 3x); time limits go down by\n"
	"; prestige_time_per_level (0.02 = -2%) down to prestige_time_min. Goals with stops or a route\n"
	"; (tours, H.O.R.S.E., moving score spots) are left alone.\n"
	"; The level is kept in thug_prestige.txt; delete it or set prestige=0 to stop.\n"
	"prestige=1\n"
	"prestige_points_percent=10\n"
	"prestige_points_cap=0\n"
	"prestige_time_per_level=0.02\n"
	"prestige_time_min=0.8\n"
	"\n"
	"[audio]\n"
	"; voice acting: 0 = off, 1 = on\n"
	"voices=1\n"
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
		else if( !strcmp( k, "refresh_rate" ))  s_cfg.refresh_rate = SDL_strncasecmp( v, "auto", 4 ) ? iv : -1;
		else if( !strcmp( k, "aspect" ))        s_cfg.aspect = !strcmp( v, "4:3" ) ? 43 : !strcmp( v, "16:9" ) ? 169 : 0;
		else if( !strcmp( k, "ssao" ))          s_cfg.ssao = iv;
		else if( !strcmp( k, "ssao_strength" )) s_cfg.ssao_strength = (float)atof( v );
		else if( !strcmp( k, "ssao_radius" ))   s_cfg.ssao_radius = (float)atof( v );
		else if( !strcmp( k, "bloom" ))         s_cfg.bloom = iv;
		else if( !strcmp( k, "bloom_strength" )) s_cfg.bloom_strength = (float)atof( v );
		else if( !strcmp( k, "bloom_threshold" )) s_cfg.bloom_threshold = (float)atof( v );
		else if( !strcmp( k, "fog" ))           s_cfg.fog = iv;
		else if( !strcmp( k, "fog_strength" ))  s_cfg.fog_strength = (float)atof( v );
		else if( !strcmp( k, "fog_distance" ))  s_cfg.fog_distance = (float)atof( v );
		else if( !strcmp( k, "dof" ))           s_cfg.dof = iv;
		else if( !strcmp( k, "dof_strength" ))  s_cfg.dof_strength = (float)atof( v );
		else if( !strcmp( k, "ps2_dither" ))    s_cfg.ps2_dither = iv;
		else if( !strcmp( k, "dither_bits" ))   s_cfg.dither_bits = iv;
		else if( !strcmp( k, "saturation" ))    s_cfg.saturation = (float)atof( v );
		else if( !strcmp( k, "tv_levels" ))     s_cfg.tv_levels = iv;
		else if( !strcmp( k, "soften" ))        s_cfg.soften = (float)atof( v );
		else if( !strcmp( k, "ghosting" ))      s_cfg.ghosting = (float)atof( v );
		else if( !strcmp( k, "walk_lean" ))     s_cfg.walk_lean = (float)atof( v );
		else if( !strcmp( k, "camera_shake" ))  s_cfg.camera_shake = (float)atof( v );
		else if( !strcmp( k, "camera_fov_push" )) s_cfg.camera_fov_push = (float)atof( v );
		else if( !strcmp( k, "head_bob" ))      s_cfg.head_bob = (float)atof( v );
		else if( !strcmp( k, "score_scale" ))   s_cfg.score_scale = (float)atof( v );
		else if( !strcmp( k, "time_scale" ))    s_cfg.time_scale = (float)atof( v );
		else if( !strcmp( k, "prestige" ))      s_cfg.prestige = iv;
		else if( !strcmp( k, "prestige_points_percent" ))   s_cfg.prestige_points_percent = (float)atof( v );
		else if( !strcmp( k, "prestige_points_cap" ))       s_cfg.prestige_points_cap = (float)atof( v );
		else if( !strcmp( k, "prestige_time_per_level" ))   s_cfg.prestige_time_per_level = (float)atof( v );
		else if( !strcmp( k, "prestige_time_min" ))         s_cfg.prestige_time_min = (float)atof( v );
		else if( !strcmp( k, "overscan" ))      s_cfg.overscan = (float)atof( v );
		else if( !strcmp( k, "shadow_softness" )) s_cfg.shadow_softness = (float)atof( v );
		else if( !strcmp( k, "voices" ))        s_cfg.voices = iv;
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
	s_cfg.refresh_rate = -1;
	s_cfg.ssao = 1;
	s_cfg.ssao_strength = 0.8f;
	s_cfg.ssao_radius = 18.0f;
	s_cfg.bloom = 1;
	s_cfg.bloom_strength = 0.15f;
	s_cfg.bloom_threshold = 0.8f;
	s_cfg.fog = 1;
	s_cfg.fog_strength = 0.25f;
	s_cfg.fog_distance = 8000.0f;
	s_cfg.dof = 1;
	s_cfg.dof_strength = 0.45f;
	s_cfg.ps2_dither = 1;
	s_cfg.overscan = 0.04f;
	s_cfg.dither_bits = 5;
	s_cfg.soften = 1.0f;
	s_cfg.tv_levels = 1;
	s_cfg.saturation = 1.0f;
	s_cfg.ghosting = 0.2f;
	s_cfg.shadow_softness = 2.5f;
	s_cfg.voices = 1;
	s_cfg.walk_lean = 1.0f;
	s_cfg.camera_shake = 1.0f;
	s_cfg.camera_fov_push = 4.0f;
	s_cfg.head_bob = 1.0f;
	s_cfg.score_scale = 1.0f;
	s_cfg.time_scale = 1.0f;
	s_cfg.prestige = 1;
	s_cfg.prestige_points_percent = 10.0f;
	s_cfg.prestige_points_cap = 0.0f;
	s_cfg.prestige_time_per_level = 0.02f;
	s_cfg.prestige_time_min = 0.8f;

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

extern "C" float desktop_shadow_softness( void )
{
	const float v = desktop_config().shadow_softness;
	return v < 0.0f ? 0.0f : v > 8.0f ? 8.0f : v;
}

extern "C" float desktop_walk_lean( void )
{
	const float l = desktop_config().walk_lean;
	return ( l < 0.0f ) ? 0.0f : ( l > 3.0f ) ? 3.0f : l;
}

extern "C" float desktop_camera_shake( void )
{
	const float v = desktop_config().camera_shake;
	return ( v < 0.0f ) ? 0.0f : ( v > 3.0f ) ? 3.0f : v;
}

extern "C" float desktop_camera_fov_push( void )
{
	const float v = desktop_config().camera_fov_push;
	return ( v < 0.0f ) ? 0.0f : ( v > 15.0f ) ? 15.0f : v;
}

extern "C" float desktop_head_bob( void )
{
	const float v = desktop_config().head_bob;
	return ( v < 0.0f ) ? 0.0f : ( v > 3.0f ) ? 3.0f : v;
}

// --- difficulty and prestige ---------------------------------------------------

static int s_prestige = -1;

static std::string prestige_path( void )
{
	char *base = SDL_GetBasePath();
	std::string p = std::string( base ? base : "" ) + "thug_prestige.txt";
	SDL_free( base );
	return p;
}

static int prestige_level( void )
{
	if( s_prestige >= 0 )
		return s_prestige;
	s_prestige = 0;
	FILE *f = fopen( prestige_path().c_str(), "r" );
	if( f )
	{
		char l[64];
		while( fgets( l, sizeof( l ), f ))
			if( isdigit( (unsigned char)l[0] )) { s_prestige = atoi( l ); break; }
		fclose( f );
	}
	if( s_prestige < 0 ) s_prestige = 0;
	if( s_prestige > 0 )
		VLOG( "GAME", "prestige level %d", s_prestige );
	return s_prestige;
}

static float clampf( float v, float lo, float hi ) { return v < lo ? lo : v > hi ? hi : v; }

extern "C" float desktop_goal_score_scale( void )
{
	const DesktopConfig &c = desktop_config();
	float s = clampf( c.score_scale, 0.1f, 20.0f );
	if( c.prestige )
	{
		// Linear, +10% of the original a level by default, no ceiling unless
		// prestige_points_cap is set. (Compounding made level 10 ~9x.)
		float p = 1.0f + clampf( c.prestige_points_percent, 0.0f, 10000.0f ) / 100.0f * (float)prestige_level();
		if( c.prestige_points_cap >= 1.0f && p > c.prestige_points_cap )
			p = c.prestige_points_cap;
		s *= p;
	}
	return s < 0.1f ? 0.1f : s;
}

extern "C" float desktop_goal_time_scale( void )
{
	const DesktopConfig &c = desktop_config();
	// Prestige trims time gently (-2% a level, never under 80%): route goals
	// (Muska's drive between stops, tours) are skipped in Goal.cpp anyway.
	float t = clampf( c.time_scale, 0.1f, 10.0f );
	if( c.prestige )
	{
		const float mn = clampf( c.prestige_time_min, 0.3f, 1.0f );
		t *= clampf( 1.0f - clampf( c.prestige_time_per_level, 0.0f, 0.5f ) * prestige_level(), mn, 1.0f );
	}
	return clampf( t, 0.05f, 10.0f );
}

// The story's ending (script HI_Endgame_show_messages_spawned) started.
extern "C" void desktop_prestige_story_beaten( void )
{
	static bool s_deja = false;
	if( s_deja || !desktop_config().prestige )
		return;
	s_deja = true;
	const int n = prestige_level() + 1;
	FILE *f = fopen( prestige_path().c_str(), "w" );
	if( f )
	{
		fprintf( f, "%d\n; prestige level: raised each time the story is beaten (thug_desktop.ini [difficulty])\n", n );
		fclose( f );
	}
	s_prestige = n;
	VLOG( "GAME", "story beaten: prestige level %d (scores x%.2f, time x%.2f)", n,
	      desktop_goal_score_scale(), desktop_goal_time_scale());
}

extern "C" int desktop_voices( void )
{
	return desktop_config().voices;
}

extern "C" int desktop_ecran_43( void )
{
	static int s_43 = -1;
	if( s_43 >= 0 )
		return s_43;
	const DesktopConfig &cfg = desktop_config();
	if( cfg.aspect == 43 )
		s_43 = 1;
	else if( cfg.aspect == 169 )
		s_43 = 0;
	else
	{
		// auto: 4:3 on a screen narrower than 16:10.
		SDL_InitSubSystem( SDL_INIT_VIDEO );
		SDL_DisplayMode m;
		s_43 = ( SDL_GetDesktopDisplayMode( 0, &m ) == 0 && m.h > 0 && m.w * 10 < m.h * 16 ) ? 1 : 0;
	}
	return s_43;
}
