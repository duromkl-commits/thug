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
