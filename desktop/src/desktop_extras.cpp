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
#include <vector>
#include <stdlib.h>

#include "desktop_extras.h"

namespace
{
static const char *s_noms[NB_EXTRAS] = { "InputDisplay", "ComboStats", "ChaosMode", "GoalAttack", "SlowMotion", "PerfectBalance", "TrickReadout" };
static int  s_val[NB_EXTRAS] = { 0, 0, 0, 0, 0, 0, 0 };
static bool s_lu = false;

static std::string chemin( const char *nom = "thug_extras.txt" )
{
	char *base = SDL_GetBasePath();
	std::string p = std::string( base ? base : "" ) + nom;
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
	static const char *s_txt[NB_EXTRAS] = { "Input Display", "Combo Stats", "Chaos Mode", "Goal Attack Timer",
	                                        "Practice: Slow Motion", "Practice: Perfect Balance", "Practice: Trick Readout" };
	return ( i >= 0 && i < NB_EXTRAS ) ? s_txt[i] : "";
}

// --- records -----------------------------------------------------------------

namespace
{
struct Records { std::string fichier; std::vector<std::pair<std::string, float> > v; };
static Records s_rec[2];

static Records &records( const char *file )
{
	for( int i = 0; i < 2; ++i )
		if( s_rec[i].fichier == file ) return s_rec[i];
	Records &r = s_rec[0].fichier.empty() ? s_rec[0] : s_rec[1];
	r.fichier = file;
	r.v.clear();
	FILE *f = fopen( chemin( file ).c_str(), "r" );
	if( f )
	{
		char l[256];
		while( fgets( l, sizeof( l ), f ))
		{
			char *eg = strrchr( l, '=' );
			if( l[0] == '#' || !eg ) continue;
			*eg = 0;
			r.v.push_back( std::make_pair( std::string( l ), (float)atof( eg + 1 )));
		}
		fclose( f );
	}
	return r;
}
}

extern "C" float extras_record_get( const char *file, const char *key, float def )
{
	Records &r = records( file );
	for( size_t i = 0; i < r.v.size(); ++i )
		if( r.v[i].first == key ) return r.v[i].second;
	return def;
}

extern "C" void extras_record_set( const char *file, const char *key, float value )
{
	Records &r = records( file );
	size_t i = 0;
	for( ; i < r.v.size() && r.v[i].first != key; ++i ) {}
	if( i == r.v.size()) r.v.push_back( std::make_pair( std::string( key ), value ));
	else r.v[i].second = value;
	FILE *f = fopen( chemin( file ).c_str(), "w" );
	if( !f ) return;
	fprintf( f, "# THUG desktop records\n" );
	for( size_t k = 0; k < r.v.size(); ++k ) fprintf( f, "%s=%.3f\n", r.v[k].first.c_str(), r.v[k].second );
	fclose( f );
}
