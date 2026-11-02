/*****************************************************************************
**  THUG desktop -- achievements                                            **
**                                                                          **
**  A list of goals across every mode, unlocked from stats the game reports **
**  (Code/Sk/Modules/Skate/desktop_life_glue.cpp and desktop_modes_glue.cpp) **
**  and saved in thug_achievements.txt next to the exe, apart from story    **
**  saves. Shown with a pop-up when unlocked and in Pause > Extras.         **
*****************************************************************************/

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <deque>

#include "achievements.h"

namespace
{

struct Ach { const char *cle, *nom, *desc; int stat; float seuil; bool montre_progres; };

static const Ach s_ach[] = {
	{ "first_combo",   "First Steps",         "Land a combo",                                   ACH_COMBO_BEST,   1, false },
	{ "combo_10k",     "Five Figures",        "Land a 10,000 point combo",                      ACH_COMBO_BEST,   10000, false },
	{ "combo_100k",    "Six Figures",         "Land a 100,000 point combo",                     ACH_COMBO_BEST,   100000, false },
	{ "combo_500k",    "Half a Million",      "Land a 500,000 point combo",                     ACH_COMBO_BEST,   500000, false },
	{ "combo_1m",      "Millionaire",         "Land a 1,000,000 point combo",                   ACH_COMBO_BEST,   1000000, false },
	{ "combo_5m",      "Combo God",           "Land a 5,000,000 point combo",                   ACH_COMBO_BEST,   5000000, false },
	{ "tricks_10",     "Ten Tricks",          "10 tricks in one combo",                         ACH_COMBO_TRICKS, 10, false },
	{ "tricks_25",     "Quarter Century",     "25 tricks in one combo",                         ACH_COMBO_TRICKS, 25, false },
	{ "tricks_50",     "Fifty Deep",          "50 tricks in one combo",                         ACH_COMBO_TRICKS, 50, false },
	{ "mult_20",       "Times Twenty",        "Reach a x20 multiplier",                         ACH_COMBO_MULT,   20, false },
	{ "mult_50",       "Times Fifty",         "Reach a x50 multiplier",                         ACH_COMBO_MULT,   50, false },
	{ "time_30",       "Long Haul",           "Keep a combo going for 30 seconds",              ACH_COMBO_TIME,   30, false },
	{ "time_60",       "Endurance",           "Keep a combo going for 60 seconds",              ACH_COMBO_TIME,   60, false },
	{ "air_5",         "Hang Time",           "5 seconds in the air in one combo",              ACH_AIR_TIME,     5, false },
	{ "grind_20",      "Grind Line",          "20 seconds grinding in one combo",               ACH_GRIND_TIME,   20, false },
	{ "manual_30",     "Balance Beam",        "30 seconds manualling in one combo",             ACH_MANUAL_TIME,  30, false },
	{ "bail_1",        "Slammed",             "Bail",                                           ACH_BAILS,        1, false },
	{ "bail_100",      "Hospital Bills",      "Bail 100 times",                                 ACH_BAILS,        100, true },
	{ "goal_1",        "First Goal",          "Win a goal",                                     ACH_GOALS,        1, false },
	{ "goal_25",       "Goal Getter",         "Win 25 goals",                                   ACH_GOALS,        25, true },
	{ "goal_50",       "Goal Machine",        "Win 50 goals",                                   ACH_GOALS,        50, true },
	{ "goal_100",      "Century",             "Win 100 goals",                                  ACH_GOALS,        100, true },
	{ "story",         "Underground Legend",  "Beat Story Mode",                                ACH_STORY,        1, false },
	{ "zero_10",       "Zero Hero",           "Win 10 goals in THUG Zero",                      ACH_ZERO_GOALS,   10, true },
	{ "zero_story",    "From Zero",           "Beat Story Mode in THUG Zero",                   ACH_ZERO_STORY,   1, false },
	{ "classic_run",   "Old School",          "Finish a classic run",                           ACH_CL_RUNS,      1, false },
	{ "classic_skate", "S-K-A-T-E",           "Collect S-K-A-T-E in a classic run",             ACH_CL_SKATE,     1, false },
	{ "classic_combo", "C-O-M-B-O",           "Collect C-O-M-B-O in a classic run",             ACH_CL_COMBO,     1, false },
	{ "classic_tape",  "Tape Hunter",         "Find the secret tape in a classic run",          ACH_CL_TAPE,      1, false },
	{ "classic_sick",  "Sick",                "Get the Sick score in a classic run",            ACH_CL_SICK,      1, false },
	{ "classic_all",   "Perfect Run",         "All 7 goals in one classic run",                 ACH_CL_ALL,       7, false },
	{ "ga_10",         "On the Clock",        "Win 10 goals with the goal attack timer on",     ACH_GA_GOALS,     10, true },
	{ "ga_pb",         "Personal Best",       "Beat a goal attack split",                       ACH_GA_PB,        1, false },
	{ "photo_1",       "Say Cheese",          "Take a photo",                                   ACH_PHOTOS,       1, false },
	{ "photo_25",      "Photographer",        "Take 25 photos",                                 ACH_PHOTOS,       25, true },
	{ "life_cash",     "Paid",                "Have $1,000 in Skater Life",                     ACH_LIFE_CASH,    1000, false },
	{ "life_out",      "Lights Out",          "Pass out in Skater Life",                        ACH_LIFE_BLACKOUT, 1, false },
	{ "chaos_10",      "Chaos Theory",        "Skate through 10 chaos rounds",                  ACH_CHAOS,        10, true },
	{ "fp_50k",        "Eyes Only",           "Land a 50,000 point combo in first person",      ACH_FP_COMBO,     50000, false },
	{ "slowmo_10",     "Bullet Time",         "Land 10 combos in slow motion practice",         ACH_SLOWMO_COMBOS, 10, true },
	{ "points_10m",    "Ten Million",         "Land 10,000,000 points in total",                ACH_POINTS,       10000000, true },
	{ "playtime_10h",  "Dedicated",           "Skate for 10 hours",                             ACH_PLAYTIME,     36000, true },
};
enum { NB = sizeof( s_ach ) / sizeof( s_ach[0] ) };

static const char *s_stats[ACH_NB_STATS] = {
	"combo_best", "combo_tricks", "combo_mult", "combo_time", "air_time", "grind_time", "manual_time",
	"bails", "goals", "story", "zero_goals", "zero_story", "cl_runs", "cl_skate", "cl_combo", "cl_tape",
	"cl_sick", "cl_all", "ga_goals", "ga_pb", "photos", "life_cash", "life_blackout", "chaos", "fp_combo",
	"slowmo_combos", "points", "playtime" };

static float s_val[ACH_NB_STATS];
static bool  s_fait[NB];
static bool  s_lu = false, s_sale = false;
static std::deque<int> s_file;

static std::string chemin( void )
{
	char *base = SDL_GetBasePath();
	std::string p = std::string( base ? base : "" ) + "thug_achievements.txt";
	SDL_free( base );
	return p;
}

static void lit( void )
{
	if( s_lu ) return;
	s_lu = true;
	FILE *f = fopen( chemin().c_str(), "r" );
	if( !f ) return;
	char l[256];
	while( fgets( l, sizeof( l ), f ))
	{
		char *eg = strchr( l, '=' );
		if( l[0] == '#' || !eg ) continue;
		*eg = 0;
		const char *v = eg + 1;
		if( !strncmp( l, "stat.", 5 ))
		{
			for( int i = 0; i < ACH_NB_STATS; ++i ) if( !strcmp( l + 5, s_stats[i] )) s_val[i] = (float)atof( v );
		}
		else if( !strncmp( l, "got.", 4 ))
		{
			for( int i = 0; i < NB; ++i ) if( !strcmp( l + 4, s_ach[i].cle )) s_fait[i] = atoi( v ) != 0;
		}
	}
	fclose( f );
}

static void verifie( int stat )
{
	for( int i = 0; i < NB; ++i )
		if( s_ach[i].stat == stat && !s_fait[i] && s_val[stat] >= s_ach[i].seuil )
		{
			s_fait[i] = true;
			s_file.push_back( i );
			s_sale = true;
		}
}

} // namespace

extern "C" void ach_add( int stat, float v )
{
	lit();
	if( stat < 0 || stat >= ACH_NB_STATS || v == 0.0f ) return;
	s_val[stat] += v;
	s_sale = true;
	verifie( stat );
}

extern "C" void ach_max( int stat, float v )
{
	lit();
	if( stat < 0 || stat >= ACH_NB_STATS || v <= s_val[stat] ) return;
	s_val[stat] = v;
	s_sale = true;
	verifie( stat );
}

extern "C" int ach_count( void ) { return NB; }

extern "C" int ach_unlocked_count( void )
{
	lit();
	int n = 0;
	for( int i = 0; i < NB; ++i ) if( s_fait[i] ) ++n;
	return n;
}

extern "C" void ach_line( int i, char *out, int n )
{
	lit();
	if( i < 0 || i >= NB ) { snprintf( out, n, " " ); return; }
	const Ach &a = s_ach[i];
	if( s_fait[i] )
		snprintf( out, n, "\\c4%s\\c0 - %s", a.nom, a.desc );
	else if( a.montre_progres )
		snprintf( out, n, "%s - %s (%d/%d)", a.nom, a.desc, (int)s_val[a.stat], (int)a.seuil );
	else
		snprintf( out, n, "%s - %s", a.nom, a.desc );
}

extern "C" int ach_pop( char *name, int n_name, char *desc, int n_desc )
{
	if( s_file.empty()) return 0;
	const int i = s_file.front();
	s_file.pop_front();
	snprintf( name, n_name, "%s", s_ach[i].nom );
	snprintf( desc, n_desc, "%s", s_ach[i].desc );
	return 1;
}

extern "C" void ach_flush( void )
{
	if( !s_sale ) return;
	s_sale = false;
	FILE *f = fopen( chemin().c_str(), "w" );
	if( !f ) return;
	fprintf( f, "# THUG desktop achievements\n" );
	for( int i = 0; i < ACH_NB_STATS; ++i ) fprintf( f, "stat.%s=%.2f\n", s_stats[i], s_val[i] );
	for( int i = 0; i < NB; ++i ) fprintf( f, "got.%s=%d\n", s_ach[i].cle, s_fait[i] ? 1 : 0 );
	fclose( f );
}
