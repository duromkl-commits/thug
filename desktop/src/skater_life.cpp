/*****************************************************************************
**  THUG desktop -- Skater Life mode                                        **
**  desktop/src/skater_life.cpp                                             **
**                                                                          **
**  A Sims-style survival layer over free skate: five needs (hunger,        **
**  energy, hygiene, bladder, fun), cash earned from landed combos, a game  **
**  clock (1 real second = 1 game minute) and daily bills. This file is     **
**  the simulation, the save file and the Sims radio; the engine side       **
**  (HUD, menus, sounds) is Code/Sk/Modules/Skate/desktop_life_glue.cpp and **
**  the scripts patched in by custom_music.cpp.                             **
**                                                                          **
**  Sims music and sounds come from the player's own Sims install, copied   **
**  to Data\sims\Music and Data\sims\Sound.                                 **
*****************************************************************************/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>

#include "custom_music.h"
#include "skater_life.h"
#include "vita_log.h"

extern "C" void desktop_host_path( const char *vita_path, char *out, size_t out_size );

namespace
{

enum { FAIM, ENERGIE, HYGIENE, VESSIE, FUN, NB_BESOINS };

static const char *s_noms[NB_BESOINS] = { "Hunger", "Energy", "Hygiene", "Bladder", "Fun" };

// Loss per game hour while awake.
static const float s_perte[NB_BESOINS] = { 5.0f, 4.0f, 3.0f, 8.0f, 6.0f };

static const int   CASH_DEPART	= 100;
static const int   LOYER		= 40;		// bills, every morning at 9
static const float SOMMEIL_H	= 12.5f;	// energy per hour asleep

struct Etat
{
	float besoin[NB_BESOINS];
	int   cash;
	double minutes;			// game minutes since day 1, 00:00
	int   jour_facture;		// last day the bills were paid
	float affame_h;			// hours spent at 0 hunger
	int   station;			// -2 Sims mix, -1 THUG soundtrack, 0.. a Sims station
};

static Etat  s_e;
static bool  s_actif   = false;
static bool  s_charge  = false;
static int   s_retenue = 0;		// events held while an action's fade runs
static bool  s_alerte[NB_BESOINS];	// warning shown, until the need recovers
static bool  s_puant_dit = false;

struct Evenement { std::string texte, son; int genre; };
static std::deque<Evenement> s_file;

static int   s_gain = 0;
static float s_gain_age = 99.0f;

static std::vector<std::string> s_stations;		// folder names under Stations
static bool  s_stations_vues = false;

// --- save file ---------------------------------------------------------------

static std::string chemin_sauvegarde( void )
{
	char *base = SDL_GetBasePath();
	std::string p = std::string( base ? base : "" ) + "skater_life.txt";
	SDL_free( base );
	return p;
}

static void neuf( void )
{
	for( int i = 0; i < NB_BESOINS; ++i ) s_e.besoin[i] = 80.0f;
	s_e.cash = CASH_DEPART;
	s_e.minutes = 8 * 60;		// day 1, 8:00 AM
	s_e.jour_facture = 1;
	s_e.affame_h = 0.0f;
	s_e.station = -2;
	for( int i = 0; i < NB_BESOINS; ++i ) s_alerte[i] = false;
	s_puant_dit = false;
}

static void sauve( void )
{
	FILE *f = fopen( chemin_sauvegarde().c_str(), "w" );
	if( !f ) return;
	fprintf( f, "# THUG desktop - Skater Life save\n" );
	for( int i = 0; i < NB_BESOINS; ++i ) fprintf( f, "%s=%.2f\n", s_noms[i], s_e.besoin[i] );
	fprintf( f, "Cash=%d\nMinutes=%.1f\nBillsDay=%d\nStarving=%.2f\nStation=%d\n",
		s_e.cash, s_e.minutes, s_e.jour_facture, s_e.affame_h, s_e.station );
	fclose( f );
}

static void charge( void )
{
	neuf();
	FILE *f = fopen( chemin_sauvegarde().c_str(), "r" );
	if( !f ) return;
	char l[256];
	while( fgets( l, sizeof( l ), f ))
	{
		char *eg = strchr( l, '=' );
		if( !eg || l[0] == '#' ) continue;
		*eg = 0;
		const char *v = eg + 1;
		for( int i = 0; i < NB_BESOINS; ++i )
			if( !strcmp( l, s_noms[i] )) s_e.besoin[i] = (float)atof( v );
		if( !strcmp( l, "Cash" )) s_e.cash = atoi( v );
		else if( !strcmp( l, "Minutes" )) s_e.minutes = atof( v );
		else if( !strcmp( l, "BillsDay" )) s_e.jour_facture = atoi( v );
		else if( !strcmp( l, "Starving" )) s_e.affame_h = (float)atof( v );
		else if( !strcmp( l, "Station" )) s_e.station = atoi( v );
	}
	fclose( f );
	for( int i = 0; i < NB_BESOINS; ++i )
	{
		if( !( s_e.besoin[i] >= 0.0f )) s_e.besoin[i] = 0.0f;
		if( s_e.besoin[i] > 100.0f ) s_e.besoin[i] = 100.0f;
		s_alerte[i] = s_e.besoin[i] < 30.0f;
	}
	if( s_e.minutes < 0 ) s_e.minutes = 8 * 60;
}

// --- events --------------------------------------------------------------------

static void annonce( const std::string &texte, const char *son, int genre = LIFE_EVENT_MESSAGE )
{
	Evenement e;
	e.texte = texte;
	e.son = son ? son : "";
	e.genre = genre;
	s_file.push_back( e );
	VLOG( "LIFE", "%s", texte.c_str());
}

static int jour( void ) { return (int)( s_e.minutes / ( 24 * 60 )) + 1; }
static float heure( void ) { return (float)fmod( s_e.minutes, 24 * 60 ) / 60.0f; }

static float borne( float v ) { return v < 0.0f ? 0.0f : ( v > 100.0f ? 100.0f : v ); }

static void ajoute( int i, float d ) { s_e.besoin[i] = borne( s_e.besoin[i] + d ); }

static float humeur( void )
{
	float m = 0.0f;
	for( int i = 0; i < NB_BESOINS; ++i ) m += s_e.besoin[i];
	return m / NB_BESOINS;
}

static void factures( void )
{
	// Bills each morning at 9, from day 2 on.
	while( true )
	{
		const int j = jour();
		const bool passe_9h = heure() >= 9.0f;
		const int du = passe_9h ? j : j - 1;
		if( du <= s_e.jour_facture ) break;
		s_e.jour_facture += 1;
		s_e.cash -= LOYER;
		char t[128];
		if( s_e.cash >= 0 )
			snprintf( t, sizeof( t ), "Bills paid: -$%d", LOYER );
		else
			snprintf( t, sizeof( t ), "Bills: -$%d. You're broke! Land combos to pay it off", LOYER );
		annonce( t, "Get_Bills" );
	}
}

// Advances the clock by 'h' game hours. 'dort': asleep (energy goes up,
// hunger and bladder fall at half rate, hygiene and fun hold).
static void avance( float h, bool dort )
{
	if( h <= 0.0f ) return;
	s_e.minutes += h * 60.0;
	if( dort )
	{
		ajoute( ENERGIE, SOMMEIL_H * h );
		ajoute( FAIM, -s_perte[FAIM] * 0.5f * h );
		ajoute( VESSIE, -s_perte[VESSIE] * 0.5f * h );
	}
	else
	{
		const bool affame = s_e.besoin[FAIM] <= 0.0f;
		for( int i = 0; i < NB_BESOINS; ++i )
			ajoute( i, -s_perte[i] * h * (( i == ENERGIE && affame ) ? 2.0f : 1.0f ));
	}
	factures();
}

static void controle( void )
{
	static const char *s_alertes[NB_BESOINS] = {
		"You're getting hungry. Grab some food (Pause > Skater Life)",
		"You're getting tired. Find somewhere to crash",
		"You smell. Hit the shower",
		"You need to pee. Bad",
		"You're bored. Go land something big" };
	for( int i = 0; i < NB_BESOINS; ++i )
	{
		if( !s_alerte[i] && s_e.besoin[i] < 30.0f )
		{
			s_alerte[i] = true;
			annonce( s_alertes[i], "Ui_Help" );
		}
		else if( s_alerte[i] && s_e.besoin[i] > 40.0f )
			s_alerte[i] = false;
	}
	if( s_e.besoin[HYGIENE] < 15.0f && !s_puant_dit )
	{
		s_puant_dit = true;
		annonce( "You stink. Sponsors are paying half", "UI_ERROR" );
	}
	else if( s_e.besoin[HYGIENE] > 25.0f )
		s_puant_dit = false;

	// Bladder at 0: an accident.
	if( s_e.besoin[VESSIE] <= 0.0f )
	{
		s_e.besoin[VESSIE] = 100.0f;
		s_e.besoin[HYGIENE] = 0.0f;
		ajoute( FUN, -20.0f );
		annonce( "Uh oh... you wet yourself", "sting_socfail1" );
	}
	// Energy at 0: you pass out where you are.
	if( s_e.besoin[ENERGIE] <= 0.0f )
	{
		avance( 6.0f, true );
		annonce( "You passed out from exhaustion and slept on the pavement", "sting_sleep1", LIFE_EVENT_BLACKOUT );
	}
	// Starving for 3 game hours: you faint, and the hospital bills you.
	if( s_e.affame_h >= 3.0f )
	{
		s_e.affame_h = 0.0f;
		s_e.besoin[FAIM] = 50.0f;
		s_e.cash -= 50;
		avance( 6.0f, false );
		s_e.besoin[FAIM] = 50.0f;
		annonce( "You fainted from hunger. Hospital bill: -$50", "sting_danger1", LIFE_EVENT_BLACKOUT );
	}
}

// --- Sims radio ------------------------------------------------------------------

static std::string hote( const char *vita )
{
	char p[1200];
	desktop_host_path( vita, p, sizeof( p ));
	return p;
}

#ifdef _WIN32
static std::wstring large( const std::string &s )
{
	int n = MultiByteToWideChar( CP_UTF8, 0, s.c_str(), -1, NULL, 0 );
	std::wstring w( n > 0 ? n - 1 : 0, 0 );
	if( n > 1 ) MultiByteToWideChar( CP_UTF8, 0, s.c_str(), -1, &w[0], n );
	return w;
}
static std::string etroit( const std::wstring &w )
{
	int n = WideCharToMultiByte( CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL );
	std::string s( n > 0 ? n - 1 : 0, 0 );
	if( n > 1 ) WideCharToMultiByte( CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL );
	return s;
}
#endif

// Names in a folder: sub-folders, or files with an audio extension.
static std::vector<std::string> liste( const std::string &dossier, bool dossiers )
{
	std::vector<std::string> r;
#ifdef _WIN32
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW( large( dossier + "\\*" ).c_str(), &d );
	if( h == INVALID_HANDLE_VALUE ) return r;
	do
	{
		const std::string nom = etroit( d.cFileName );
		if( nom == "." || nom == ".." ) continue;
		const bool est_dossier = ( d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0;
		if( est_dossier == dossiers ) r.push_back( nom );
	}
	while( FindNextFileW( h, &d ));
	FindClose( h );
#else
	DIR *d = opendir( dossier.c_str());
	if( !d ) return r;
	while( struct dirent *e = readdir( d ))
	{
		const std::string nom = e->d_name;
		if( nom == "." || nom == ".." ) continue;
		struct stat st;
		if( stat(( dossier + "/" + nom ).c_str(), &st )) continue;
		if( S_ISDIR( st.st_mode ) == dossiers ) r.push_back( nom );
	}
	closedir( d );
#endif
	if( !dossiers )
	{
		std::vector<std::string> a;
		for( size_t i = 0; i < r.size(); ++i )
		{
			std::string x = r[i];
			for( size_t k = 0; k < x.size(); ++k ) x[k] = (char)tolower((unsigned char)x[k] );
			if( x.size() > 4 && ( x.compare( x.size() - 4, 4, ".mp3" ) == 0 || x.compare( x.size() - 4, 4, ".wav" ) == 0
				|| x.compare( x.size() - 4, 4, ".ogg" ) == 0 ))
				a.push_back( r[i] );
		}
		r.swap( a );
	}
	std::sort( r.begin(), r.end());
	return r;
}

#ifdef _WIN32
static const char SEP = '\\';
#else
static const char SEP = '/';
#endif

static std::string dossier_stations( void )
{
	return hote( "ux0:data/thug/Data/sims/Music/Stations" );
}

static void voit_stations( void )
{
	if( s_stations_vues ) return;
	s_stations_vues = true;
	const std::string base = dossier_stations();
	std::vector<std::string> d = liste( base, true );
	for( size_t i = 0; i < d.size(); ++i )
		if( !liste( base + SEP + d[i], false ).empty()) s_stations.push_back( d[i] );
	VLOG( "LIFE", "Sims radio: %d stations in %s", (int)s_stations.size(), base.c_str());
}

// Display names for the Sims 1 station folders.
static std::string nom_station( const std::string &dossier )
{
	static const struct { const char *d, *n; } s_n[] = {
		{ "Beach", "Beach" }, { "Classica", "Classical" }, { "Country", "Country" }, { "CountryD", "Country Dance" },
		{ "Disco", "Disco" }, { "EZ", "Easy Listening" }, { "EZExp", "Easy Listening Deluxe" }, { "Latin", "Latin" },
		{ "Rap", "Rap" }, { "Rave", "Rave" }, { "Rock", "Rock" }, { "Superstar", "Superstar" },
		{ "UnlCheap", "Unleashed Band" }, { "UnlSpeak", "Zydeco" } };
	for( size_t i = 0; i < sizeof( s_n ) / sizeof( s_n[0] ); ++i )
		if( !strcasecmp( dossier.c_str(), s_n[i].d )) return s_n[i].n;
	return dossier;
}

static std::string s_station_txt;
static int s_derniere = -1;		// last song, not repeated twice running

} // namespace

// --- C API ---------------------------------------------------------------------

extern "C" int life_on( void ) { return s_actif ? 1 : 0; }

extern "C" void life_start( void )
{
	if( !s_charge ) { charge(); s_charge = true; }
	voit_stations();
	if( s_e.station >= (int)s_stations.size()) s_e.station = -2;
	if( s_e.station == -2 && s_stations.empty()) s_e.station = -1;
	s_actif = true;
	s_retenue = 0;
	VLOG( "LIFE", "Skater Life on: day %d, $%d", jour(), s_e.cash );
}

extern "C" void life_stop( void )
{
	if( !s_actif ) return;
	sauve();
	s_actif = false;
	s_file.clear();
	VLOG( "LIFE", "Skater Life off" );
}

extern "C" void life_tick( float secondes )
{
	if( !s_actif || secondes <= 0.0f || secondes > 1.0f ) return;
	const float h = secondes / 60.0f;		// 1 real second = 1 game minute
	const int heure_avant = (int)( s_e.minutes / 60.0 );
	if( s_e.besoin[FAIM] <= 0.0f ) s_e.affame_h += h;
	else s_e.affame_h = 0.0f;
	avance( h, false );
	controle();
	s_gain_age += secondes;
	if((int)( s_e.minutes / 60.0 ) != heure_avant ) sauve();	// once per game hour
}

extern "C" int life_landed( int score )
{
	if( !s_actif || score <= 0 ) return 0;
	ajoute( FUN, 2.0f + std::min( 28.0f, score / 2000.0f ));
	ajoute( ENERGIE, -std::min( 3.0f, score / 20000.0f ));
	float mult = 0.5f + humeur() / 100.0f;
	if( s_e.besoin[HYGIENE] < 15.0f ) mult *= 0.5f;
	const int gain = (int)( std::min( 250.0f, score / 1000.0f ) * mult );
	if( gain > 0 )
	{
		s_e.cash += gain;
		s_gain = gain;
		s_gain_age = 0.0f;
	}
	return gain;
}

extern "C" void life_bailed( void )
{
	if( !s_actif ) return;
	ajoute( FUN, -3.0f );
	ajoute( HYGIENE, -4.0f );
	ajoute( ENERGIE, -2.0f );
}

static bool paie( int prix )
{
	if( s_e.cash >= prix ) { s_e.cash -= prix; return true; }
	char t[96];
	snprintf( t, sizeof( t ), "You can't afford that ($%d). Land some combos", prix );
	annonce( t, "UI_ERROR" );
	return false;
}

extern "C" int life_action( const char *a, char *son, int n_son, float *fondu )
{
	if( son && n_son > 0 ) son[0] = 0;
	if( fondu ) *fondu = 0.0f;
	if( !s_actif || !a ) return 0;
	const char *s = NULL;
	float f = 0.0f;
	char t[128] = "";
	if( !strcmp( a, "snack" ))
	{
		if( !paie( 5 )) return 0;
		avance( 10.0f / 60.0f, false );
		ajoute( FAIM, 25.0f );
		s = "Food_Snack_Eat1"; f = 1.5f;
		snprintf( t, sizeof( t ), "Ate a snack. -$5" );
	}
	else if( !strcmp( a, "meal" ))
	{
		if( !paie( 15 )) return 0;
		avance( 0.75f, false );
		ajoute( FAIM, 65.0f );
		ajoute( FUN, 5.0f );
		s = "Meal_Sloppy_Eater1"; f = 2.0f;
		snprintf( t, sizeof( t ), "Had a proper meal. -$15" );
	}
	else if( !strcmp( a, "coffee" ))
	{
		if( !paie( 3 )) return 0;
		avance( 5.0f / 60.0f, false );
		ajoute( ENERGIE, 15.0f );
		ajoute( VESSIE, -12.0f );
		s = "COFFEE_SLURP1"; f = 1.0f;
		snprintf( t, sizeof( t ), "Coffee. -$3" );
	}
	else if( !strcmp( a, "shower" ))
	{
		avance( 0.5f, false );
		s_e.besoin[HYGIENE] = 100.0f;
		s = "SHOWER1_SWATERON"; f = 3.0f;
		snprintf( t, sizeof( t ), "Squeaky clean" );
	}
	else if( !strcmp( a, "toilet" ))
	{
		avance( 5.0f / 60.0f, false );
		s_e.besoin[VESSIE] = 100.0f;
		s = "Toilet_Flush_Cheap"; f = 2.5f;
		snprintf( t, sizeof( t ), "Much better" );
	}
	else if( !strcmp( a, "sleep" ))
	{
		float h = ( 100.0f - s_e.besoin[ENERGIE] ) / SOMMEIL_H;
		h = h < 1.0f ? 1.0f : ceilf( h );
		avance( h, true );
		s = "sting_sleep1"; f = 4.0f;
		snprintf( t, sizeof( t ), "Slept %d hours", (int)h );
	}
	else if( !strcmp( a, "radio" ))
	{
		// THUG soundtrack -> Sims mix -> each Sims station -> THUG soundtrack
		if( s_e.station == -1 ) s_e.station = s_stations.empty() ? -1 : -2;
		else if( s_e.station == -2 ) s_e.station = 0;
		else if( ++s_e.station >= (int)s_stations.size()) s_e.station = -1;
		s_derniere = -1;
		s = "stereo_stations_switch1";
		snprintf( t, sizeof( t ), "Radio: %s", life_station_name());
	}
	else if( !strcmp( a, "newlife" ))
	{
		const int st = s_e.station;
		neuf();
		s_e.station = st;
		s = "sting_econsuc1"; f = 2.0f;
		snprintf( t, sizeof( t ), "A fresh start. Day 1, $%d", CASH_DEPART );
	}
	else
		return 0;
	controle();
	sauve();
	if( t[0] ) annonce( t, NULL );
	if( s && son && n_son > 0 ) snprintf( son, n_son, "%s", s );
	if( fondu ) *fondu = f;
	return 1;
}

extern "C" void life_hold( int on )
{
	s_retenue += on ? 1 : -1;
	if( s_retenue < 0 ) s_retenue = 0;
}

extern "C" int life_pop_event( char *texte, int n_texte, char *son, int n_son, int *genre )
{
	if( s_file.empty() || s_retenue ) return 0;
	const Evenement e = s_file.front();
	s_file.pop_front();
	snprintf( texte, n_texte, "%s", e.texte.c_str());
	snprintf( son, n_son, "%s", e.son.c_str());
	*genre = e.genre;
	return 1;
}

extern "C" float life_need( int i ) { return ( i >= 0 && i < NB_BESOINS ) ? s_e.besoin[i] : 0.0f; }
extern "C" const char *life_need_name( int i ) { return ( i >= 0 && i < NB_BESOINS ) ? s_noms[i] : ""; }
extern "C" int life_num_needs( void ) { return NB_BESOINS; }
extern "C" int life_cash( void ) { return s_e.cash; }
extern "C" float life_mood( void ) { return humeur(); }

extern "C" void life_clock_text( char *out, int n )
{
	const int m = (int)fmod( s_e.minutes, 24 * 60 );
	int h = m / 60;
	const char *ap = h < 12 ? "AM" : "PM";
	h %= 12;
	if( h == 0 ) h = 12;
	snprintf( out, n, "Day %d  %d:%02d %s", jour(), h, m % 60, ap );
}

extern "C" int life_gain( float *age )
{
	if( age ) *age = s_gain_age;
	return s_gain;
}

extern "C" const char *life_station_name( void )
{
	if( s_e.station == -1 ) return "THUG Soundtrack";
	if( s_e.station == -2 ) return "Sims Mix";
	if( s_e.station >= 0 && s_e.station < (int)s_stations.size())
	{
		s_station_txt = nom_station( s_stations[s_e.station] );
		return s_station_txt.c_str();
	}
	return "THUG Soundtrack";
}

extern "C" int life_radio_active( void )
{
	return ( s_actif && s_e.station != -1 && !s_stations.empty()) ? 1 : 0;
}

// Opens a random song of the current station (any station for the mix) in
// the custom music decoder.
extern "C" int life_radio_open( void )
{
	if( s_stations.empty()) return 0;
	int st = s_e.station;
	if( st < 0 || st >= (int)s_stations.size()) st = rand() % (int)s_stations.size();
	const std::string d = dossier_stations() + SEP + s_stations[st];
	std::vector<std::string> f = liste( d, false );
	if( f.empty()) return 0;
	int k = rand() % (int)f.size();
	if( f.size() > 1 && k == s_derniere ) k = ( k + 1 ) % (int)f.size();
	s_derniere = k;
	const std::string p = d + SEP + f[k];
#ifdef _WIN32
	const int ok = cm_open( large( p ).c_str());
#else
	const int ok = cm_open( p.c_str());
#endif
	VLOG( "LIFE", "Sims radio %s: %s %s", nom_station( s_stations[st] ).c_str(), f[k].c_str(), ok ? "" : "(could not open)" );
	return ok;
}
