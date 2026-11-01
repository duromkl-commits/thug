/*****************************************************************************
**  THUG desktop -- pause-menu Extras toggles                               **
**                                                                          **
**  Input display, combo stats and chaos mode, on or off, kept in           **
**  thug_extras.txt next to the exe. The features themselves live in        **
**  Code/Sk/Modules/Skate/desktop_life_glue.cpp.                            **
*****************************************************************************/

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "desktop_extras.h"

namespace
{
static const char *s_noms[NB_EXTRAS] = { "InputDisplay", "ComboStats", "ChaosMode" };
static int  s_val[NB_EXTRAS] = { 0, 0, 0 };
static bool s_lu = false;

static std::string chemin( void )
{
	char *base = SDL_GetBasePath();
	std::string p = std::string( base ? base : "" ) + "thug_extras.txt";
	SDL_free( base );
	return p;
}

static void lit( void )
{
	if( s_lu ) return;
	s_lu = true;
	FILE *f = fopen( chemin().c_str(), "r" );
	if( !f ) return;
	char l[128];
	while( fgets( l, sizeof( l ), f ))
		for( int i = 0; i < NB_EXTRAS; ++i )
		{
			const size_t n = strlen( s_noms[i] );
			if( !strncmp( l, s_noms[i], n ) && l[n] == '=' ) s_val[i] = l[n + 1] == '1';
		}
	fclose( f );
}

static void ecrit( void )
{
	FILE *f = fopen( chemin().c_str(), "w" );
	if( !f ) return;
	fprintf( f, "# THUG desktop - pause menu Extras\n" );
	for( int i = 0; i < NB_EXTRAS; ++i ) fprintf( f, "%s=%d\n", s_noms[i], s_val[i] );
	fclose( f );
}
}

extern "C" int extras_get( int i )
{
	lit();
	return ( i >= 0 && i < NB_EXTRAS ) ? s_val[i] : 0;
}

extern "C" void extras_toggle( int i )
{
	lit();
	if( i < 0 || i >= NB_EXTRAS ) return;
	s_val[i] = !s_val[i];
	ecrit();
}

extern "C" const char *extras_name( int i )
{
	static const char *s_txt[NB_EXTRAS] = { "Input Display", "Combo Stats", "Chaos Mode" };
	return ( i >= 0 && i < NB_EXTRAS ) ? s_txt[i] : "";
}
