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
	"\n"
	"[graphics]\n"
	"; ambient occlusion (darkens corners and contact points): 0 = off, 1 = on, 2 = show only the occlusion (test view)\n"
	"ssao=1\n"
	"; how dark it gets (0.0 - 1.0) and how far it reaches, in inches\n"
	"ssao_strength=0.8\n"
	"ssao_radius=28\n"
	"; skater shadow edge blur (0 = hard like the Xbox, 2.5 = default soft)\n"
	"shadow_softness=2.5\n"
	"; shadows of buildings and objects from the sun: 0 = off everywhere, 1 = on in the levels listed under [sun]\n"
	"sun_shadows=1\n"
	"\n"
	"[sun]\n"
	"; one line per level: sun_<level>=heading pitch strength, e.g. sun_nj=50 330 0.7\n"
	"; levels without a line have no sun shadows. Set them in game with the keyboard:\n"
	";   F5 shadows on/off for this level   F6/F7 turn the sun   F8/F9 raise/lower it\n"
	";   F10/F11 lighter/darker (hold Shift for finer steps). Every change is saved here.\n"
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
static char s_ini_path[1200];
static void sun_parse( const char *k, const char *v );

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
		else if( !strcmp( k, "aspect" ))        s_cfg.aspect = !strcmp( v, "4:3" ) ? 43 : !strcmp( v, "16:9" ) ? 169 : 0;
		else if( !strcmp( k, "ssao" ))          s_cfg.ssao = iv;
		else if( !strcmp( k, "ssao_strength" )) s_cfg.ssao_strength = (float)atof( v );
		else if( !strcmp( k, "ssao_radius" ))   s_cfg.ssao_radius = (float)atof( v );
		else if( !strcmp( k, "shadow_softness" )) s_cfg.shadow_softness = (float)atof( v );
		else if( !strcmp( k, "sun_shadows" ))   s_cfg.sun_shadows = iv;
		else if( !strcmp( k, "sun_strength" ))  s_cfg.sun_strength = (float)atof( v );
		else if( !strcmp( k, "voices" ))        s_cfg.voices = iv;
		else if( !strcmp( k, "dump_shaders" ))  s_cfg.dump_shaders = iv;
		else if( !strncmp( k, "sun_", 4 ) && strcmp( k, "sun_shadows" ) && strcmp( k, "sun_strength" ))
			sun_parse( k + 4, v );
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
	s_cfg.ssao = 1;
	s_cfg.ssao_strength = 0.8f;
	s_cfg.ssao_radius = 28.0f;
	s_cfg.shadow_softness = 2.5f;
	s_cfg.voices = 1;
	s_cfg.sun_shadows = 1;
	s_cfg.sun_strength = 0.7f;	// strength of a level switched on in game

	char path[1200];
	char *base = SDL_GetBasePath();
	snprintf( path, sizeof( path ), "%sthug_desktop.ini", base ? base : "" );
	SDL_free( base );
	const char *env = getenv( "THUG_DESKTOP_INI" );
	if( env && *env )
		snprintf( path, sizeof( path ), "%s", env );
	snprintf( s_ini_path, sizeof( s_ini_path ), "%s", path );
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

// ---------------------------------------------------------------------------
// Per-level sun ([sun] in the ini). THUG's lighting is baked into the vertex
// colours, shadows included, so a sun shadow only looks right where its
// direction matches the baked one: each level is set by hand, in game.
// ---------------------------------------------------------------------------

struct SSoleil { unsigned crc; char nom[24]; float heading, pitch, force; bool on; };
static SSoleil s_sol[64];
static int s_sol_n = 0;
static unsigned s_sol_niveau = 0;	// level being drawn (desktop_sun_level)

static unsigned crc_thps( const char *p )
{
	unsigned c = 0xffffffffu;
	for( ; *p; ++p )
	{
		c ^= (unsigned char)tolower( (unsigned char)*p );
		for( int k = 0; k < 8; ++k )
			c = ( c >> 1 ) ^ (( c & 1 ) ? 0xedb88320u : 0u );
	}
	return c;
}

static SSoleil *sol_trouver( unsigned crc )
{
	for( int i = 0; i < s_sol_n; ++i )
		if( s_sol[i].crc == crc )
			return &s_sol[i];
	return NULL;
}

static void sun_parse( const char *k, const char *v )
{
	char load[40];
	snprintf( load, sizeof( load ), "load_%s", k );
	SSoleil *p = sol_trouver( crc_thps( load ));
	if( !p )
	{
		if( s_sol_n >= 64 ) return;
		p = &s_sol[s_sol_n++];
	}
	memset( p, 0, sizeof( *p ));
	p->crc = crc_thps( load );
	snprintf( p->nom, sizeof( p->nom ), "%s", k );
	p->force = 0.7f;
	p->pitch = 330.0f;
	const int n = sscanf( v, "%f %f %f", &p->heading, &p->pitch, &p->force );
	p->on = ( n >= 2 ) && ( p->force > 0.0f );
}

// Level checksums are "load_<name>" (levels.q): the names are listed to save
// a level switched on in game under a readable key.
static const char *s_noms_niveaux[] = {
	"nj", "ny", "fl", "sd", "hi", "vc", "sj", "ru", "au", "se", "sc", "dj", "ph", "vn",
	"hn", "sc2", "www", "skateshop", "cas", "boardshop", "sk5ed", "sk5ed_gameplay",
	"test", "testlevel", "default", NULL };

static void sun_sauver( const SSoleil *p )
{
	if( !s_ini_path[0] )
		return;
	std::string sortie, cle = std::string( "sun_" ) + p->nom;
	char ligne[128];
	snprintf( ligne, sizeof( ligne ), "%s=%.0f %.0f %.2f\n", cle.c_str(), p->heading, p->pitch,
	          p->on ? p->force : 0.0f );
	bool fait = false;
	FILE *f = fopen( s_ini_path, "r" );
	if( f )
	{
		char l[1200];
		while( fgets( l, sizeof( l ), f ))
		{
			char k[64] = "";
			sscanf( l, " %63[^= \t]", k );
			if( !fait && cle == k )
			{
				sortie += ligne;
				fait = true;
			}
			else
				sortie += l;
		}
		fclose( f );
	}
	if( !fait )
	{
		if( sortie.find( "[sun]" ) == std::string::npos )
			sortie += "\n[sun]\n";
		sortie += ligne;
	}
	f = fopen( s_ini_path, "w" );
	if( f )
	{
		fputs( sortie.c_str(), f );
		fclose( f );
	}
}

extern "C" float desktop_sun_strength( void )
{
	return desktop_config().sun_shadows ? 1.0f : 0.0f;
}

// 1 and the level's sun when it has shadows on; 0 otherwise. *p_cree is set
// when a key press just switched the level on and wants the game's own
// direction as a starting point (desktop_sun_seed).
extern "C" int desktop_sun_level( unsigned level, float *heading, float *pitch, float *force )
{
	desktop_config();
	s_sol_niveau = level;
	const SSoleil *p = sol_trouver( level );
	if( !p || !p->on || !desktop_config().sun_shadows )
		return 0;
	*heading = p->heading;
	*pitch = p->pitch;
	*force = p->force;
	return 1;
}

static float s_graine_h = 60.0f, s_graine_p = 330.0f;
extern "C" void desktop_sun_seed( float heading, float pitch )
{
	s_graine_h = heading;
	s_graine_p = pitch;
}

static float tourner( float a, float d )
{
	a = fmodf( a + d, 360.0f );
	return a < 0.0f ? a + 360.0f : a;
}

extern "C" void desktop_sun_key( int touche, int maj )
{
	if( !s_sol_niveau || !desktop_config().sun_shadows )
		return;
	SSoleil *p = sol_trouver( s_sol_niveau );
	if( !p )
	{
		if( touche != 5 || s_sol_n >= 64 )
			return;
		p = &s_sol[s_sol_n++];
		memset( p, 0, sizeof( *p ));
		p->crc = s_sol_niveau;
		snprintf( p->nom, sizeof( p->nom ), "%08x", s_sol_niveau );
		for( int i = 0; s_noms_niveaux[i]; ++i )
		{
			char load[40];
			snprintf( load, sizeof( load ), "load_%s", s_noms_niveaux[i] );
			if( crc_thps( load ) == s_sol_niveau )
				snprintf( p->nom, sizeof( p->nom ), "%s", s_noms_niveaux[i] );
		}
		p->heading = s_graine_h;
		p->pitch = s_graine_p;
		p->force = desktop_config().sun_strength;
		p->on = false;
	}
	const float pas = maj ? 1.0f : 5.0f;
	switch( touche )
	{
		case 5:  p->on = !p->on; if( p->force <= 0.0f ) p->force = desktop_config().sun_strength; break;
		case 6:  p->heading = tourner( p->heading, -pas ); break;
		case 7:  p->heading = tourner( p->heading, pas ); break;
		// Pitch 330 = 30 degrees up (levels.q); 270 = straight overhead.
		case 8:  p->pitch = tourner( p->pitch, -pas ); if( p->pitch < 275.0f ) p->pitch = 275.0f; break;
		case 9:  p->pitch = tourner( p->pitch, pas );  if( p->pitch > 355.0f || p->pitch < 270.0f ) p->pitch = 355.0f; break;
		case 10: p->force -= maj ? 0.02f : 0.1f; if( p->force < 0.05f ) p->force = 0.05f; break;
		case 11: p->force += maj ? 0.02f : 0.1f; if( p->force > 1.0f ) p->force = 1.0f; break;
		default: return;
	}
	if( touche != 5 )
		p->on = true;
	sun_sauver( p );
	SDL_Log( "sun %s: %s, heading %.0f pitch %.0f strength %.2f", p->nom, p->on ? "on" : "off",
	         p->heading, p->pitch, p->force );
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
