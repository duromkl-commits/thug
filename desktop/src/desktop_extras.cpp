/*****************************************************************************
**  THUG desktop -- whole files next to the exe                            **
**                                                                          **
**  Used by the Pro Skater 4 career save (desktop_th4_glue.cpp). The       **
**  pause-menu Extras toggles that used to live here were removed with     **
**  that menu (Marcus, 2026-11-09).                                        **
*****************************************************************************/

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <stdlib.h>

#include "desktop_extras.h"

namespace
{
static std::string chemin( const char *nom = "thug_extras.txt" )
{
	char *base = SDL_GetBasePath();
	std::string p = std::string( base ? base : "" ) + nom;
	SDL_free( base );
	return p;
}

}

extern "C" void *extras_file_read( const char *file, unsigned *size )
{
	*size = 0;
	FILE *f = fopen( chemin( file ).c_str(), "rb" );
	if( !f ) return NULL;
	fseek( f, 0, SEEK_END );
	const long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	void *d = n > 0 ? malloc( (size_t)n ) : NULL;
	if( d && fread( d, 1, (size_t)n, f ) != (size_t)n ) { free( d ); d = NULL; }
	fclose( f );
	if( d ) *size = (unsigned)n;
	return d;
}

extern "C" int extras_file_write( const char *file, const void *data, unsigned size )
{
	// written beside, then renamed over: a crash mid-write keeps the old one
	const std::string c = chemin( file ), t = c + ".tmp";
	FILE *f = fopen( t.c_str(), "wb" );
	if( !f ) return 0;
	const bool ok = fwrite( data, 1, size, f ) == size;
	if( fclose( f ) != 0 || !ok ) { remove( t.c_str()); return 0; }
	remove( c.c_str());
	return rename( t.c_str(), c.c_str()) == 0;
}

// Controls menu options (Marcus 2026-11-13): 0 = No Air Shuffles,
// 1 = No Flips/Rolls. Kept in thug_controls.txt beside the exe ("01").
static int s_opts[2];
static bool s_opts_lus = false;

static void opts_lire( void )
{
	if( s_opts_lus ) return;
	s_opts_lus = true;
	unsigned n = 0;
	char *d = (char *)extras_file_read( "thug_controls.txt", &n );
	for( unsigned i = 0; d && i < n && i < 2; ++i )
		s_opts[i] = d[i] == '1';
	free( d );
}

extern "C" int desktop_control_opt( int i )
{
	opts_lire();
	return i >= 0 && i < 2 ? s_opts[i] : 0;
}

extern "C" int desktop_control_opt_toggle( int i )
{
	opts_lire();
	if( i < 0 || i >= 2 ) return 0;
	s_opts[i] = !s_opts[i];
	const char t[2] = { (char)( '0' + s_opts[0] ), (char)( '0' + s_opts[1] ) };
	extras_file_write( "thug_controls.txt", t, 2 );
	return s_opts[i];
}
