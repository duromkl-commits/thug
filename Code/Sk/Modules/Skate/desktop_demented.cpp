/*****************************************************************************
**  THUG desktop -- Demented difficulty                                     **
**  Code/Sk/Modules/Skate/desktop_demented.cpp                              **
**                                                                          **
**  A story run with a random dealt skater: no saving, starter tricks      **
**  locked, missing goal tricks slotted in for you, and modifiers picked   **
**  on its menu (Perma Bail, Zero, Lives). A small run clock bottom right;  **
**  a run summary when it ends. Nothing here is saved: the run lives in     **
**  memory and ends at the main menu.                                       **
*****************************************************************************/

#ifdef THUG_DESKTOP

#include <core/defines.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <vector>
#include <string>

#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/component.h>
#include <gel/scripting/array.h>
#include <gel/scripting/checksum.h>
#include <gel/scripting/symboltable.h>
#include <gel/scripting/symboltype.h>
#include <gel/components/trickcomponent.h>
#include <gfx/2D/ScreenElemMan.h>
#include <gfx/2D/ScreenElement2.h>
#include <gfx/2D/TextElement.h>
#include <sk/modules/skate/skate.h>
#include <sk/objects/moviecam.h>
#include <sk/modules/skate/gamemode.h>
#include <sk/modules/skate/goalmanager.h>
#include <sk/modules/frontend/frontend.h>
#include <sk/objects/skater.h>
#include <sk/objects/skatercareer.h>
#include <sk/objects/skaterprofile.h>
#include <sk/objects/playerprofilemanager.h>
#include <sk/objects/skatertricks.h>
#include <sys/timer.h>
#include <gel/object/compositeobjectmanager.h>
#include <sk/objects/restart.h>
#include "vita_log.h"

bool DesktopThugZero( void );

static void chaos_tout_finir( void );	// Chaos, below

namespace
{
static uint32 crc( const char *t ) { return Script::GenerateCRC( t ); }

// Our own generator: the C library's rand() gets reseeded by other code
// (two runs in a row were dealt the same skater). Seeded once from the
// wall clock and the high-res timer, then never touched by anyone else.
static uint32 hasard( uint32 n )
{
	static uint64 x = 0;
	if( !x )
	{
		x = (uint64)time( NULL ) * 0x9E3779B97F4A7C15ull ^ (uint64)Tmr::GetTime() * 0xBF58476D1CE4E5B9ull ^ 0xD1B54A32D192ED03ull;
		if( !x ) x = 1;
	}
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	return n ? (uint32)( ( x >> 16 ) % n ) : 0;
}

enum { MOD_PERMA, MOD_ZERO, MOD_LIVES, MOD_TIME, MOD_FRAGILE, MOD_AUTO, MOD_MIRROR, MOD_CHAOS, MOD_SURPRISE,
       MOD_R_MONDE, MOD_R_COULEURS, MOD_R_VOIX, MOD_R_SONS, MOD_R_MOTS, MOD_R_NOMS, NB_MODS };
static const char *s_mod_noms[NB_MODS] = { "Perma Bail", "Zero", "Lives", "Time Extend", "Fragile", "Auto", "Mirror", "Chaos", "Surprise",
                                           "Random World", "Random Colours", "Random Voices", "Sound Salad", "Word Salad", "Random Names" };
static bool s_arme[NB_MODS] = { false, false, true, false, false, false, false, false, false, false, false, false, false, false };

// Randomizers (Marcus's GTA SA randomizer list): one seed per chapter, so a
// thing keeps its random replacement until the next chapter.
static uint32 s_rando_graine = 1;
static uint32 s_niv_log = 0;	// level of the last goal dump (reset each run)
static uint32 melanger( uint32 x )
{
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
	return x;
}

// Auto, Mirror and Chaos state (reset with each run)
static float s_reve_t = 0.0f;		// Chaos: time for its slow waves
static float s_flip_t = 0.0f, s_flip_suivant = 90.0f;
static bool  s_flip = false;		// Chaos: the world is mirrored right now
static bool  s_inverse = false;		// Chaos: steering reversed (world as it is)
static bool  s_menu = false;		// a menu or dialog is up (Mirror leaves the pad alone)

// Chaos: names that get swapped for each other, the same way all run
static const char *s_noms_chaos[] = { "Eric", "Mindy", "Kyle", "Paulie", "Chad", "Tony", "Bob", "Andrew",
	"Bam", "Rodney", "Paul", "Arto", "Mike", "Jamie", "Rune", "Steve", "Mark", "Geoff", "Kareem", "Stevie",
	"Bucky", "Elissa", "Peralta", "Muska", "Eddie", "Iggy", "Stacey", "Ryan", "Daewon", "Ali", "Johnny", "Gene", "Rob", "Ace", "Peter",
	// the added skaters (skater_roster.py) and the team / contest names
	"Skeezo", "Ralphie", "Bender", "Jordan", "Stacy", "Todd", "Jim", "Andre", "Smiley", "Dan", "Cooper", "Clyde",
	"Christina", "Nate", "Diego", "Tom", "Charlie", "Udo", "Smitty", "Natlie", "Perry", "Twiggy", "Denny", "Kozar",
	"Hough", "Furlow", "Scotty", "Sean", "Barak", "Shane", "Ricky", "Robby", "Alf", "Ian", "Michael", "Kevin",
	"Christian", "Natas" };
static const int NB_NOMS_CHAOS = (int)( sizeof( s_noms_chaos ) / sizeof( s_noms_chaos[0] ));
typedef char verif_noms_chaos[ NB_NOMS_CHAOS <= 128 ? 1 : -1 ];
static int s_perm_noms[128];
static uint32 s_graine_chaos = 1;

// Time Extend: seconds on the clock at the start, per goal, per chapter, after a lost life.
// Guesses, to tune once it has been played.
static const float TE_DEPART = 300.0f, TE_CHAPITRE = 120.0f, TE_RECHARGE = 60.0f;
static const float TE_GOAL_MIN = 45.0f, TE_GOAL_MAX = 120.0f, TE_GOAL_SANS = 60.0f;	// the menu's choices

struct Run
{
	bool	actif;
	bool	mods[NB_MODS];
	uint32	skater;			// dealt profile name
	char	nom[64];		// its display name
	int		vies;
	int		chapitre_max;
	float	t;				// skating time, pauses left out
	float	reste;			// Time Extend: seconds left on the clock
	float	gagne_temps;	// Time Extend: seconds earned
	int		bails, gagnes, perdus;
	bool	fini;			// summary shown, waiting to go back to the menu
	bool	dialogue;		// the "You've been dealt" box was shown
	bool	hud;
	int		stats_custom[10];	// the created skater's own, put back after
	bool	stats_gardees;
	// starter tricks: (key combo, trick) pairs and (special slot, trick)
	std::vector<uint32> depart_cles, depart_tricks;
};
static Run s_run;

static const char *s_stats[10] = { "air", "run", "ollie", "speed", "spin", "switch",
	"flip_speed", "rail_balance", "lip_balance", "manual_balance" };

static Front::CScreenElement *element( uint32 id )
{
	Front::CScreenElementPtr p = Front::CScreenElementManager::Instance()->GetElement( id );
	return p ? p.Convert() : NULL;
}

// a small one-line notice top right (desktop_toast in gamemenu.qb)
static void panneau( const char *t, const char *son = NULL, int delai = 0, int r = 128, int g = 128, int b = 128 )
{
	Script::CStruct p;
	p.AddString( "text", t );
	if( son ) p.AddChecksum( "sound", Script::GenerateCRC( son ));
	if( delai ) p.AddInteger( "delay", delai );
	Script::CArray *c = new Script::CArray;
	c->SetSizeAndType( 4, ESYMBOLTYPE_INTEGER );
	c->SetInteger( 0, r ); c->SetInteger( 1, g ); c->SetInteger( 2, b ); c->SetInteger( 3, 110 );
	p.AddArrayPointer( "rgba", c );
	Script::SpawnScript( "desktop_demented_msg", &p );
}

static void temps( char *o, int n, float s )
{
	const int h = (int)( s / 3600.0f ), m = (int)( s / 60.0f ) % 60, sec = (int)s % 60;
	if( h ) snprintf( o, n, "%d:%02d:%02d", h, m, sec );
	else snprintf( o, n, "%d:%02d", m, sec );
}

static Obj::CPlayerProfileManager *ppm( void ) { return Mdl::Skate::Instance()->GetPlayerProfileManager(); }

// --- trick lookups -------------------------------------------------------------------

// The trick at key combo 'cle' in a mapping (top level first, then the
// structures it names, as the game resolves them).
static uint32 trick_a( Script::CStruct *p_map, uint32 cle, int niv = 0 )
{
	if( !p_map || niv > 8 ) return 0;
	uint32 t = 0;
	if( p_map->GetChecksum( cle, &t, Script::NO_ASSERT )) return t;
	for( Script::CComponent *c = p_map->GetNextComponent( NULL ); c; c = p_map->GetNextComponent( c ))
	{
		Script::CStruct *sous = NULL;
		if( c->mType == ESYMBOLTYPE_STRUCTURE ) sous = c->mpStructure;
		else if( c->mType == ESYMBOLTYPE_NAME && !c->mNameChecksum ) sous = Script::GetStructure( c->mChecksum, Script::NO_ASSERT );
		if( sous && ( t = trick_a( sous, cle, niv + 1 ))) return t;
	}
	return 0;
}

// The key combo a trick sits on in a mapping, 0 if none.
static uint32 cle_de( Script::CStruct *p_map, uint32 trick, int niv = 0 )
{
	if( !p_map || niv > 8 ) return 0;
	for( Script::CComponent *c = p_map->GetNextComponent( NULL ); c; c = p_map->GetNextComponent( c ))
		if( c->mType == ESYMBOLTYPE_NAME && c->mNameChecksum && c->mChecksum == trick ) return c->mNameChecksum;
	for( Script::CComponent *c = p_map->GetNextComponent( NULL ); c; c = p_map->GetNextComponent( c ))
	{
		Script::CStruct *sous = NULL;
		if( c->mType == ESYMBOLTYPE_STRUCTURE ) sous = c->mpStructure;
		else if( c->mType == ESYMBOLTYPE_NAME && !c->mNameChecksum ) sous = Script::GetStructure( c->mChecksum, Script::NO_ASSERT );
		uint32 k;
		if( sous && ( k = cle_de( sous, trick, niv + 1 ))) return k;
	}
	return 0;
}

// Regular slot families: flips (Square), grabs (Circle), lips (Triangle).
static const char *s_dirs[] = { "U", "D", "L", "R", "UL", "UR", "DL", "DR" };
static void famille_cles( int f, std::vector<uint32> &o )
{
	static const char *pre[3] = { "Air_Square", "Air_Circle", "Lip_Triangle" };
	static const char *dbl[3] = { "Square", "Circle", NULL };
	char t[48];
	for( int i = 0; i < 8; ++i ) { snprintf( t, sizeof( t ), "%s%s", pre[f], s_dirs[i] ); o.push_back( crc( t )); }
	if( dbl[f] )
	{
		static const char *dd[4] = { "U_U", "D_D", "L_L", "R_R" };
		for( int i = 0; i < 4; ++i ) { snprintf( t, sizeof( t ), "Air_%s_%s", dd[i], dbl[f] ); o.push_back( crc( t )); }
	}
}

static int famille_de_cle( uint32 cle )
{
	for( int f = 0; f < 3; ++f )
	{
		std::vector<uint32> v; famille_cles( f, v );
		for( uint32 c : v ) if( c == cle ) return f;
	}
	return -1;
}

// Which regular family a trick belongs to, from the pros' default lists.
static int famille_de_trick( uint32 trick )
{
	static const char *defs[] = { "CustomTricks_default", "HawkTricks_default", "CaballeroTricks_default",
		"CampbellTricks_default", "GlifbergTricks_default", "KostonTricks_default", "LasekTricks_default",
		"MargeraTricks_default", "MullenTricks_default", "MuskaTricks_default", "ReynoldsTricks_default",
		"RowleyTricks_default", "SteamerTricks_default", "ThomasTricks_default", "SaariTricks_default",
		"VallelyTricks_default", "RodriguezTricks_default", "BumTricks_default",
		"VertLip", "StreetLip", "HawkLip", "FreestyleFlips", "AllAroundGrabs" };
	for( const char *d : defs )
	{
		Script::CStruct *s = Script::GetStructure( crc( d ), Script::NO_ASSERT );
		const uint32 k = s ? cle_de( s, trick ) : 0;
		const int f = k ? famille_de_cle( k ) : -1;
		if( f >= 0 ) return f;
	}
	return -1;
}

static const char *s_sp_types[4] = { "goal_special_tricks_air", "goal_special_tricks_lip",
	"goal_special_tricks_grind", "goal_special_tricks_manual" };

// A special trick's slot type, from any skater that has it (air if none).
static int type_special( uint32 trick )
{
	Obj::CPlayerProfileManager *pm = ppm();
	const int n = (int)pm->GetNumProfileTemplates();
	for( int i = 0; i < n; ++i )
	{
		Obj::CSkaterProfile *p = pm->GetProfileTemplateByIndex( i );
		if( !p ) continue;
		const int ns = (int)p->GetNumSpecialTrickSlots();
		for( int s = 0; s < ns; ++s )
		{
			Obj::SSpecialTrickInfo inf = p->GetSpecialTrickInfo( s );
			if( inf.m_TrickName != trick || inf.IsUnassigned()) continue;
			for( int t = 0; t < 4; ++t )
			{
				Script::CStruct *st = Script::GetStructure( crc( s_sp_types[t] ), Script::NO_ASSERT );
				if( st && st->ContainsComponentNamed( inf.m_TrickSlot )) return t;
				if( st && st->ContainsFlag( inf.m_TrickSlot )) return t;
			}
		}
	}
	return 0;
}

static void maj_tricks( void )
{
	Obj::CSkater *sk = Mdl::Skate::Instance()->GetSkater( 0 );
	Obj::CTrickComponent *tc = sk ? GetTrickComponentFromObject( sk ) : NULL;
	if( tc ) tc->UpdateTrickMappings( Mdl::Skate::Instance()->GetCurrentProfile());
}

static bool est_depart( uint32 cle, uint32 trick )
{
	for( size_t i = 0; i < s_run.depart_cles.size(); ++i )
		if( s_run.depart_cles[i] == cle && s_run.depart_tricks[i] == trick ) return true;
	return false;
}

static void note_depart( void )
{
	s_run.depart_cles.clear(); s_run.depart_tricks.clear();
	Obj::CSkaterProfile *p = Mdl::Skate::Instance()->GetCurrentProfile();
	Script::CStruct *map = p->GetTrickMapping( crc( "trick_mapping" ));
	for( int f = 0; f < 3; ++f )
	{
		std::vector<uint32> v; famille_cles( f, v );
		for( uint32 c : v )
		{
			const uint32 t = trick_a( map, c );
			if( t ) { s_run.depart_cles.push_back( c ); s_run.depart_tricks.push_back( t ); }
		}
	}
	const int ns = (int)p->GetNumSpecialTrickSlots();
	for( int s = 0; s < ns; ++s )
	{
		Obj::SSpecialTrickInfo inf = p->GetSpecialTrickInfo( s );
		if( !inf.IsUnassigned()) { s_run.depart_cles.push_back( 0x80000000u | s ); s_run.depart_tricks.push_back( inf.m_TrickName ); }
	}
}

// --- run start / end ------------------------------------------------------------------

static void stats_rendre( void )
{
	if( !s_run.stats_gardees ) return;
	s_run.stats_gardees = false;
	Obj::CSkaterProfile *c = ppm()->GetProfileTemplate( crc( "custom" ));
	if( !c ) return;
	for( int i = 0; i < 10; ++i ) c->GetInfo()->AddInteger( crc( s_stats[i] ), s_run.stats_custom[i] );
}

static bool s_hud_neuf = true;	// stars to rebuild (lives HUD)

static void hud_off( void )
{
	if( s_run.hud ) Script::RunScript( "desktop_demented_hud_off" );
	s_run.hud = false;
}

static void fin_run( void )
{
	if( !s_run.actif ) return;
	hud_off();
	chaos_tout_finir();
	ppm()->DesktopEndLend();
	stats_rendre();
	s_run.actif = false;
	VLOG( "DSK", "demented: run closed" );
}

static void resume( char *o, int n, bool gagne )
{
	char t[32], mods[200] = "";
	temps( t, sizeof( t ), s_run.t );
	for( int i = 0; i < NB_MODS; ++i )
		if( s_run.mods[i] && i != MOD_LIVES )
		{
			const size_t l = strlen( mods );
			snprintf( mods + l, sizeof( mods ) - l, "%s%s", l ? ", " : "", s_mod_noms[i] );
		}
	char vies[96] = "";
	if( s_run.mods[MOD_LIVES] ) snprintf( vies, sizeof( vies ), "\\c0Lives left: \\c2%d\\n", s_run.vies );
	if( s_run.mods[MOD_TIME] )
	{
		char g[32]; temps( g, sizeof( g ), s_run.gagne_temps );
		const size_t l = strlen( vies );
		snprintf( vies + l, sizeof( vies ) - l, "\\c0Time extended: \\c2%s\\n", g );
	}
	snprintf( o, n,
		"\\c0Skater: \\c2%s\\n"
		"\\c0Time: \\c2%s\\n"
		"\\c0Chapter: \\c2%d\\n"
		"\\c0Goals beaten: \\c2%d\\n"
		"\\c0Goals failed: \\c2%d\\n"
		"\\c0Bails: \\c2%d\\n"
		"%s"
		"\\c0Modifiers: \\c2%s",
		s_run.nom, t, s_run.chapitre_max + 1, s_run.gagnes, s_run.perdus, s_run.bails, vies,
		mods[0] ? mods : "None" );
	(void)gagne;
}

static void game_over( const char *pourquoi )
{
	if( !s_run.actif || s_run.fini ) return;
	s_run.fini = true;
	char t[600];
	resume( t, sizeof( t ), false );
	Script::CStruct p;
	p.AddString( "title", "RUN OVER" );
	p.AddString( "reason", pourquoi );
	p.AddString( "text", t );
	Script::SpawnScript( "desktop_demented_end", &p );
	VLOG( "DSK", "demented: run over (%s), %.0f s, %d goals", pourquoi, s_run.t, s_run.gagnes );
}

static void temps_plus( float s, const char *quoi )
{
	if( !s_run.mods[MOD_TIME] || s_run.fini ) return;
	s_run.reste += s;
	s_run.gagne_temps += s;
	char t[64], c[32];
	temps( c, sizeof( c ), s );
	snprintf( t, sizeof( t ), "+%s %s", c, quoi );
	panneau( t, "HUD_jumpgap", 0, 70, 110, 128 );
}

} // namespace

// --- engine hooks --------------------------------------------------------------------

bool DesktopDemented( void )
{
	return s_run.actif;
}

// --- Mirror and Chaos, for the renderer and the pad ------------------------------

extern "C" int desktop_mirror_world( void )
{
	if( !s_run.actif || !s_run.skater || s_run.fini ) return 0;
	return ( s_run.mods[MOD_MIRROR] != ( s_run.mods[MOD_CHAOS] && s_flip )) ? 1 : 0;
}

// Left and right swapped on the pad too, so left still goes left on screen;
// menus keep their own directions.
extern "C" int desktop_mirror_input( void )
{
	return 0;	// the pad stays as it is: trick prompts must match (Marcus)
}

// Steering only (SkaterCorePhysicsComponent, WalkComponent): left goes left
// on a mirrored screen; tricks and balance keep the pad's own directions.
extern "C" int desktop_mirror_steer( void )
{
	if( !s_run.actif || !s_run.skater || s_run.fini ) return 0;
	return ( desktop_mirror_world() != 0 ) != ( s_run.mods[MOD_CHAOS] && s_inverse ) ? 1 : 0;
}

static bool chaos_on( void ) { return s_run.actif && s_run.skater && !s_run.fini && s_run.mods[MOD_CHAOS]; }

// --- Randomizers ---------------------------------------------------------------------

static bool rando_on( int m ) { return s_run.actif && !s_run.fini && s_run.mods[m]; }

// Random Colours (p_NxSprite.cpp, p_NxFont.cpp): text and coloured HUD
// pieces get another hue, same brightness, so dark stays dark and nothing
// is tinted across the screen. 0 = off.
extern "C" unsigned int desktop_rando_couleurs( void )
{
	return rando_on( MOD_R_COULEURS ) ? s_rando_graine : 0;
}

// HUD sprites: only in play, not in a menu (pictures there stay as they are)
extern "C" unsigned int desktop_rando_hud( void )
{
	return ( rando_on( MOD_R_COULEURS ) && !s_menu ) ? s_rando_graine : 0;
}

extern "C" void desktop_rando_teinte( unsigned int cle, float *r, float *g, float *b )
{
	const float mx = *r > *g ? ( *r > *b ? *r : *b ) : ( *g > *b ? *g : *b );
	const float mn = *r < *g ? ( *r < *b ? *r : *b ) : ( *g < *b ? *g : *b );
	if( mx < 0.2f ) return;	// black outlines, shadows, dark panels
	float sat = mx > 0.0f ? ( mx - mn ) / mx : 0.0f;
	if( sat < 0.6f ) sat = 0.6f;
	const uint32 h = melanger( cle ^ s_rando_graine );
	const float hue = (float)( h % 360u ) / 60.0f;
	const int i = (int)hue;
	const float f = hue - (float)i, p = mx * ( 1.0f - sat ), q = mx * ( 1.0f - sat * f ), t = mx * ( 1.0f - sat * ( 1.0f - f ));
	switch( i % 6 )
	{
		case 0: *r = mx; *g = t;  *b = p;  break;
		case 1: *r = q;  *g = mx; *b = p;  break;
		case 2: *r = p;  *g = mx; *b = t;  break;
		case 3: *r = p;  *g = q;  *b = mx; break;
		case 4: *r = t;  *g = p;  *b = mx; break;
		default: *r = mx; *g = p; *b = q;  break;
	}
}

// The goal givers' stars (Element3d.cpp): red, or with Random Colours a hue
// of their own (not the lives overlay's). Returns the seed, 0 = stay red.
extern "C" unsigned int desktop_rando_etoile( unsigned char *r, unsigned char *g, unsigned char *b )
{
	if( !rando_on( MOD_R_COULEURS ) || !s_rando_graine ) return 0;
	float fr = 1.0f, fg = 0.0f, fb = 0.0f;
	desktop_rando_teinte( 0x6a09e667, &fr, &fg, &fb );
	*r = (unsigned char)( fr * 128.0f ); *g = (unsigned char)( fg * 128.0f ); *b = (unsigned char)( fb * 128.0f );
	if( !*r && !*g && !*b ) *r = 1;
	return s_rando_graine;
}

// Random Voices (p_music.cpp): voice streams swapped for another of about
// the same length. 0 = off.
extern "C" unsigned int desktop_rando_voix( void )
{
	return rando_on( MOD_R_VOIX ) ? s_rando_graine : 0;
}

// --- Chaos: curveballs ---------------------------------------------------------------
// At random times (8 to 40 s apart, the clock stopped in menus and cutscenes)
// something changes for a random while (10 to 30 s), sometimes two at once.
// Cheats are faked through CSkaterCareer::GetCheat (desktop_chaos_cheat);
// the body ones need the real flags, put back when they end.
enum { EV_MOON, EV_HEAVY, EV_FLIP, EV_REVERSE, EV_SLOW, EV_FAST, EV_STATS_0, EV_STATS_MAX, EV_STATS_RND,
       EV_KID, EV_BIGHEAD, EV_GORILLA, EV_INVISIBLE, EV_SPECIAL, EV_PERFECT, EV_TELEPORT, EV_NAMES, EV_SCRAMBLE, EV_SONS, NB_EV };
static const char *s_ev_noms[NB_EV] = { "MOON GRAVITY", "HEAVY GRAVITY", "THE WORLD FLIPS", "STEERING REVERSED",
	"SLOW MOTION", "FAST FORWARD", "STATS DRAINED", "STATS MAXED", "STATS AT %d", "KID MODE", "BIG HEAD",
	"GORILLA ARMS", "INVISIBLE", "ALWAYS SPECIAL", "PERFECT BALANCE", "TELEPORTED", "WHO'S WHO?", "WORD SALAD", "SOUND SALAD" };
// 0: never (Marcus: no moon gravity, and nothing that just helps the player: maxed stats, always special, perfect balance)
static const int s_ev_poids[NB_EV] = { 0, 10, 6, 7, 6, 6, 5, 0, 5, 5, 5, 4, 4, 0, 0, 6, 4, 7, 7 };
static float s_ev_reste[NB_EV];		// seconds left, 0 = off
static float s_ev_prochain = 20.0f;
static float s_ev_stat = 0.0f;
static bool  s_ev_corps = false;	// the body flags were changed (refresh owed)
// on-screen text: every text element redoes itself when this changes
// (CTextElement::update); Word Salad bumps it fast at first, then slower
static int   s_txt_gen = 1;
static float s_salade_t = 0.0f, s_salade_pas = 0.1f;
static uint32 s_salade_graine = 1;
static uint32 s_son_graine = 1;

static const char *ev_drapeau( int e )
{
	switch( e )
	{
		case EV_KID: return "CHEAT_KID";
		case EV_BIGHEAD: return "CHEAT_BIGHEAD";
		case EV_GORILLA: return "CHEAT_GORILLA";
		case EV_INVISIBLE: return "CHEAT_INVISIBLE";
		default: return NULL;
	}
}

static void ev_corps( void )
{
	Obj::CSkaterCareer *car = Mdl::Skate::Instance()->GetCareer();
	if( !car ) return;
	for( int e = EV_KID; e <= EV_INVISIBLE; ++e )
	{
		const int f = Script::GetInteger( crc( ev_drapeau( e )));
		if( s_ev_reste[e] > 0.0f ) car->SetGlobalFlag( f ); else car->UnSetGlobalFlag( f );
	}
	if( Mdl::Skate::Instance()->GetLocalSkater()) Script::RunScript( "refresh_skater_model_for_cheats" );
	s_ev_corps = true;
}

static void ev_stats( void )
{
	float v = 0.0f;
	if( s_ev_reste[EV_STATS_0] > 0.0f ) v = 0.01f;
	else if( s_ev_reste[EV_STATS_MAX] > 0.0f ) v = 13.0f;
	else if( s_ev_reste[EV_STATS_RND] > 0.0f ) v = s_ev_stat;
	Mdl::Skate::Instance()->SetStatOverride( v );
}

static void ev_temps( void )
{
	if( s_ev_reste[EV_SLOW] > 0.0f ) Tmr::SetSlomo( 0.55f );
	else if( s_ev_reste[EV_FAST] > 0.0f ) Tmr::SetSlomo( 1.4f );
	else Tmr::SetSlomo( 1.0f );
}

static void ev_fin( int e )
{
	if( s_ev_reste[e] <= 0.0f ) return;
	s_ev_reste[e] = 0.0f;
	if( e == EV_FLIP ) s_flip = false;
	if( e == EV_REVERSE ) s_inverse = false;
	if( e == EV_SCRAMBLE ) ++s_txt_gen;	// back to plain text
	if( e >= EV_STATS_0 && e <= EV_STATS_RND ) ev_stats();
	if( e == EV_SLOW || e == EV_FAST ) ev_temps();
	if( ev_drapeau( e )) ev_corps();
}

static void chaos_tout_finir( void )
{
	bool rien = !s_flip && !s_inverse;
	for( int e = 0; e < NB_EV; ++e ) if( s_ev_reste[e] > 0.0f ) { rien = false; break; }
	if( rien && !s_ev_corps ) return;
	for( int e = 0; e < NB_EV; ++e ) ev_fin( e );
	s_flip = s_inverse = false;
	++s_txt_gen;
	Mdl::Skate::Instance()->SetStatOverride( 0.0f );
	Tmr::SetSlomo( 1.0f );
	if( s_ev_corps ) { ev_corps(); s_ev_corps = false; }
}

static void ev_teleport( void )
{
	std::vector<int> v;
	int n = -1;
	while(( n = Obj::GetRestartNode( 0, 0, n )) != -1 && v.size() < 200 ) v.push_back( n );
	if( v.empty()) return;
	Script::CStruct p;
	p.AddInteger( "node_number", v[hasard( (uint32)v.size())] );
	Script::RunScript( "desktop_demented_teleport", &p );
}

static void ev_lancer( Game::CGoalManager *gm )
{
	const bool goal = gm && gm->GetNumActiveGoals() > 0;
	int total = 0, poids[NB_EV];
	for( int e = 0; e < NB_EV; ++e )
	{
		poids[e] = s_ev_poids[e];
		if( s_ev_reste[e] > 0.0f ) poids[e] = 0;
		if( e == EV_TELEPORT && goal ) poids[e] = 0;	// a race or a combo line doesn't survive it
		if(( e == EV_SLOW && s_ev_reste[EV_FAST] > 0.0f ) || ( e == EV_FAST && s_ev_reste[EV_SLOW] > 0.0f )) poids[e] = 0;
		if( e >= EV_STATS_0 && e <= EV_STATS_RND && ( s_ev_reste[EV_STATS_0] > 0.0f || s_ev_reste[EV_STATS_MAX] > 0.0f || s_ev_reste[EV_STATS_RND] > 0.0f )) poids[e] = 0;
		total += poids[e];
	}
	if( total <= 0 ) return;
	int r = (int)hasard( total ), e = 0;
	while( r >= poids[e] ) r -= poids[e++];
	char t[48];
	snprintf( t, sizeof( t ), "CHAOS: %s", s_ev_noms[e] );
	const float duree = 10.0f + hasard( 21 );
	switch( e )
	{
		case EV_TELEPORT: ev_teleport(); break;
		case EV_NAMES:
			for( int i = NB_NOMS_CHAOS - 1; i > 0; --i ) { const int k = (int)hasard( i + 1 ), x = s_perm_noms[i]; s_perm_noms[i] = s_perm_noms[k]; s_perm_noms[k] = x; }
			s_graine_chaos = hasard( 0x7fffffff ) | 1;
			++s_txt_gen;
			break;
		default:
			s_ev_reste[e] = duree;
			if( e == EV_FLIP ) s_flip = true;
			if( e == EV_REVERSE ) s_inverse = true;
			if( e == EV_SONS ) s_son_graine = hasard( 0x7fffffff ) | 1;
			if( e == EV_SCRAMBLE ) { s_salade_t = 0.0f; s_salade_pas = 0.08f; s_salade_graine = hasard( 0x7fffffff ) | 1; ++s_txt_gen; }
			if( e == EV_STATS_RND ) { s_ev_stat = (float)hasard( 11 ); if( s_ev_stat == 0.0f ) s_ev_stat = 0.01f; snprintf( t, sizeof( t ), "CHAOS: STATS AT %d", (int)s_ev_stat ); }
			if( e >= EV_STATS_0 && e <= EV_STATS_RND ) ev_stats();
			if( e == EV_SLOW || e == EV_FAST ) ev_temps();
			if( ev_drapeau( e )) ev_corps();
			break;
	}
	VLOG( "DSK", "demented chaos: %s (%.0f s)", t, duree );	// no on-screen label: you work it out (Marcus)
}

static void chaos_maj( float dt, bool pause, bool film )
{
	if( pause || film )
	{
		// slow/fast motion would run the menu and the cutscene too
		if( s_ev_reste[EV_SLOW] > 0.0f || s_ev_reste[EV_FAST] > 0.0f ) Tmr::SetSlomo( 1.0f );
		return;
	}
	ev_temps();
	if( s_ev_reste[EV_SCRAMBLE] > 0.0f )
	{
		s_salade_t += dt;
		if( s_salade_t >= s_salade_pas )
		{
			s_salade_t = 0.0f;
			s_salade_pas = s_salade_pas * 1.3f > 3.0f ? 3.0f : s_salade_pas * 1.3f;
			s_salade_graine = hasard( 0x7fffffff ) | 1;
			++s_txt_gen;
		}
	}
	for( int e = 0; e < NB_EV; ++e )
		if( s_ev_reste[e] > 0.0f )
		{
			s_ev_reste[e] -= dt;
			if( s_ev_reste[e] <= 0.0f ) { s_ev_reste[e] = 0.01f; ev_fin( e ); }
		}
	if( s_menu ) return;
	s_ev_prochain -= dt;
	if( s_ev_prochain > 0.0f ) return;
	s_ev_prochain = 8.0f + hasard( 33 );
	Game::CGoalManager *gm = Game::GetGoalManager();
	ev_lancer( gm );
	if( hasard( 100 ) < 25 ) ev_lancer( gm );	// a double
}

// CSkaterCareer::GetCheat (desktop_extras_glue.cpp desktop_chaos_cheat)
bool desktop_demented_cheat( uint32 cheat )
{
	if( !chaos_on()) return false;
	if( s_ev_reste[EV_MOON] > 0.0f && cheat == crc( "CHEAT_MOON" )) return true;
	if( s_ev_reste[EV_SPECIAL] > 0.0f && cheat == crc( "CHEAT_ALWAYS_SPECIAL" )) return true;
	if( s_ev_reste[EV_PERFECT] > 0.0f && ( cheat == crc( "CHEAT_PERFECT_RAIL" ) || cheat == crc( "CHEAT_PERFECT_MANUAL" ))) return true;
	return false;
}

extern "C" int desktop_chaos_gen( void ) { return s_txt_gen; }

// soundfx.cpp PlaySound: Sound Salad's swap seed, 0 when off
extern "C" unsigned int desktop_chaos_son_graine( void )
{
	if( chaos_on() && s_ev_reste[EV_SONS] > 0.0f ) return s_son_graine;
	// the Sound Salad randomizer: in play only, menus keep their sounds
	if( rando_on( MOD_R_SONS ) && !s_menu ) return melanger( s_rando_graine ^ 0x50a15a1du ) | 1;
	return 0;
}

// Word Salad: words of a text change places, and their letters get shuffled
static int salade( char *t, uint32 graine )
{
	const int n = (int)strlen( t );
	std::vector<int> deb, lon;
	for( int i = 0; i < n; )
	{
		if( t[i] == '\\' ) { i += 3; continue; }
		if( !(( t[i] >= 'A' && t[i] <= 'Z' ) || ( t[i] >= 'a' && t[i] <= 'z' ))) { ++i; continue; }
		const int d = i;
		while( i < n && (( t[i] >= 'A' && t[i] <= 'Z' ) || ( t[i] >= 'a' && t[i] <= 'z' ))) ++i;
		deb.push_back( d ); lon.push_back( i - d );
	}
	if( deb.empty()) return 0;
	uint32 h = graine;
	for( int i = 0; i < n; ++i ) h = ( h ^ (uint8)t[i] ) * 16777619u;
	struct R { uint32 x; uint32 operator()( uint32 m ) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return m ? x % m : 0; } } r = { h | 1 };
	std::string src( t );
	// the words, shuffled, put back into the slots of the same length
	std::vector<int> ordre( deb.size());
	for( size_t i = 0; i < ordre.size(); ++i ) ordre[i] = (int)i;
	for( int i = (int)ordre.size() - 1; i > 0; --i ) { const int k = (int)r( i + 1 ), x = ordre[i]; ordre[i] = ordre[k]; ordre[k] = x; }
	// rebuilt: the separators where they were, the words in the new order
	std::string o;
	int p = 0;
	for( size_t i = 0; i < deb.size(); ++i )
	{
		o.append( src, p, deb[i] - p );
		std::string w = src.substr( deb[ordre[i]], lon[ordre[i]] );
		// and its letters shuffled, two out of three times
		if( w.size() >= 3 && r( 3 ) != 0 )
			for( int k = (int)w.size() - 1; k > 0; --k ) { const int q = (int)r( k + 1 ); const char c = w[k]; w[k] = w[q]; w[q] = c; }
		o += w;
		p = deb[i] + lon[i];
	}
	o.append( src, p, std::string::npos );
	memcpy( t, o.c_str(), n );	// same letters, same length
	return 1;
}

// SkaterCorePhysicsComponent: air gravity multiplier
extern "C" float desktop_demented_gravity( void )
{
	return ( chaos_on() && s_ev_reste[EV_HEAVY] > 0.0f ) ? 1.7f : 1.0f;
}

// Chaos: the field of view breathes.
extern "C" float desktop_chaos_fov( void )
{
	return 1.0f;	// Marcus: no screen effects, gameplay curveballs instead
}

// Chaos: a slow wave over the picture and colours drifting round the wheel.
extern "C" int desktop_chaos_dream( float *amp, float *t, float *teinte )
{
	return 0;	// Marcus: no wobble or hue drift
	*amp = 0.0035f * ( 0.6f + 0.4f * sinf( s_reve_t * 0.21f ));
	*t = s_reve_t;
	*teinte = 0.45f * sinf( s_reve_t * 0.11f );
	return 1;
}

extern "C" int desktop_noms_extra_count( int famille );		// custom_music.cpp: data\custom\(last-)names.txt
extern "C" const char *desktop_noms_extra( int famille, int i );

// Random Names: last names it knows (not "Way" or "Song": ordinary words)
static const char *s_noms_famille[] = { "Hawk", "Sparrow", "Margera", "Mullen", "Burnquist", "Vallely", "Reynolds",
	"Glifberg", "Saari", "Thomas", "Campbell", "Rowley", "Lasek", "Koston", "Muska", "Peralta", "Hosoi", "Caballero",
	"Rodriguez", "Steamer", "Sheckler", "Dyrdek", "Templeton", "Gonzalez", "Moore", "Sanchez", "Turbo", "Simmons", "Williams", "Gonzales", "Boulala", "Stanley", "Frehley", "Criss",
	"Flores", "Meronek", "Conley", "Kirks", "Russell", "Grebe", "Albright", "Wiser", "Kostelnik", "Gaskell", "Kopp",
	"Ikeda", "Kondo", "Okui", "Bremner", "Buchli", "Clements", "Fawler", "Slade", "Harlow", "Hancock", "Thornton",
	"Bartlett", "Kang", "Kelly", "Carrera", "Qbert", "Kaupas" };
static const int NB_NOMS_FAMILLE = (int)( sizeof( s_noms_famille ) / sizeof( s_noms_famille[0] ));

// Chaos: people's names swapped for each other (the same swap all run), and
// now and then a word with two letters traded. Same input, same output, so
// text redrawn every frame doesn't flicker.
extern "C" int desktop_chaos_text( const char *in, char *out, int n )
{
	if( !in || n < 2 ) return 0;
	// Word Salad randomizer: words shuffled, one seed per chapter, in play
	// only (a paused menu is built while the game is paused, and stays plain)
	const bool mots = rando_on( MOD_R_MOTS ) && !Mdl::FrontEnd::Instance()->GamePaused();
	// Random Names randomizer: every known name becomes one from the pool
	// (the stock list + data\custom\names.txt), one seed per chapter
	const bool noms = rando_on( MOD_R_NOMS );
	const bool chaos = chaos_on();
	if( !chaos && !noms )
	{
		if( !mots ) return 0;
		strncpy( out, in, n - 1 );
		out[n - 1] = 0;
		return salade( out, melanger( s_rando_graine ^ 0x3c6e1a5bu ) | 1 );
	}
	int o = 0, change = 0;
	const char *p = in;
	while( *p && o < n - 1 )
	{
		if( *p == '\\' )
		{
			for( int k = 0; k < 3 && *p && o < n - 1; ++k ) out[o++] = *p++;
			continue;
		}
		if( !(( *p >= 'A' && *p <= 'Z' ) || ( *p >= 'a' && *p <= 'z' )))
		{
			out[o++] = *p++;
			continue;
		}
		const char *d = p;
		while(( *p >= 'A' && *p <= 'Z' ) || ( *p >= 'a' && *p <= 'z' )) ++p;
		const int l = (int)( p - d );
		const char *rempl = NULL;
		bool majuscules = l > 1;
		for( int k = 1; k < l; ++k ) if( d[k] >= 'a' ) majuscules = false;
		for( int i = 0; i < NB_NOMS_CHAOS && !rempl; ++i )
		{
			const char *nm = s_noms_chaos[i];
			if( (int)strlen( nm ) != l ) continue;
			// Random Names: Peralta and Muska are last names, they get one
			bool famille = false;
			for( int j = 0; noms && j < NB_NOMS_FAMILLE && !famille; ++j ) famille = !strcmp( nm, s_noms_famille[j] );
			if( famille ) continue;
			bool ok = true;
			for( int k = 0; k < l && ok; ++k )
			{
				char c = d[k];
				if( majuscules && c >= 'A' && c <= 'Z' && k > 0 ) c = (char)( c - 'A' + 'a' );
				ok = c == nm[k];
			}
			if( ok )
			{
				if( noms )
				{
					const int nx = desktop_noms_extra_count( 0 );
					const uint32 h = melanger((uint32)i * 0x9E3779B1u ^ s_rando_graine ^ 0x1f83d9abu );
					const uint32 k = h % (uint32)( NB_NOMS_CHAOS + nx );
					rempl = k < (uint32)NB_NOMS_CHAOS ? s_noms_chaos[k] : desktop_noms_extra( 0, (int)k - NB_NOMS_CHAOS );
					if( !rempl ) rempl = s_noms_chaos[i];
				}
				else rempl = s_noms_chaos[s_perm_noms[i]];
			}
		}
		for( int i = 0; noms && i < NB_NOMS_FAMILLE && !rempl; ++i )
		{
			const char *nm = s_noms_famille[i];
			if( (int)strlen( nm ) != l ) continue;
			bool ok = true;
			for( int k = 0; k < l && ok; ++k )
			{
				char c = d[k];
				if( majuscules && c >= 'A' && c <= 'Z' && k > 0 ) c = (char)( c - 'A' + 'a' );
				ok = c == nm[k];
			}
			if( !ok ) continue;
			const int nx = desktop_noms_extra_count( 1 );
			const uint32 h = melanger((uint32)i * 0x85ebca6bu ^ s_rando_graine ^ 0x5be0cd19u );
			const uint32 k = h % (uint32)( NB_NOMS_FAMILLE + nx );
			rempl = k < (uint32)NB_NOMS_FAMILLE ? s_noms_famille[k] : desktop_noms_extra( 1, (int)k - NB_NOMS_FAMILLE );
			if( !rempl ) rempl = nm;
		}
		if( rempl )
		{
			for( int k = 0; rempl[k] && o < n - 1; ++k )
			{
				char c = rempl[k];
				if( majuscules && c >= 'a' && c <= 'z' ) c = (char)( c - 'a' + 'A' );
				out[o++] = c;
			}
			change = 1;
			continue;
		}
		uint32 h = s_graine_chaos;
		for( int k = 0; k < l; ++k ) h = ( h ^ (uint8)d[k] ) * 16777619u;
		const int debut = o;
		for( int k = 0; k < l && o < n - 1; ++k ) out[o++] = d[k];
		if( chaos && l >= 5 && ( h % 100 ) < 5 && o - debut == l )
		{
			const int a = 1 + (int)(( h >> 8 ) % ( l - 2 )), b = a + 1;
			const char x = out[debut + a]; out[debut + a] = out[debut + b]; out[debut + b] = x;
			change = 1;
		}
	}
	out[o] = 0;
	if( s_ev_reste[EV_SCRAMBLE] > 0.0f ) { if( salade( out, s_salade_graine )) change = 1; }
	else if( mots && salade( out, melanger( s_rando_graine ^ 0x3c6e1a5bu ) | 1 )) change = 1;
	return change;
}

void desktop_demented_bail( void )
{
	if( !s_run.actif || s_run.fini ) return;
	Mdl::Skate *sk = Mdl::Skate::Instance();
	if( !sk->GetGameMode()->IsTrue( CRCD( 0x1ded1ea4, "is_career" ))) return;
	++s_run.bails;
	if( s_run.mods[MOD_PERMA] ) { game_over( "You bailed." ); return; }
	// Fragile (Marcus): every bail costs a life.
	if( s_run.mods[MOD_FRAGILE] )
	{
		--s_run.vies;
		if( s_run.vies <= 0 ) { s_run.vies = 0; game_over( "Out of lives." ); return; }
		panneau( "-1 LIFE", NULL, 0, 128, 50, 40 );
	}
}

// goal_time: the goal's own time limit in seconds, 0 when it has none
void desktop_demented_goal( bool won, int goal_time )
{
	if( !s_run.actif || s_run.fini ) return;
	if( won )
	{
		++s_run.gagnes;
		float s = TE_GOAL_SANS;
		if( goal_time > 0 )
		{
			s = 30.0f + goal_time * 0.5f;
			if( s < TE_GOAL_MIN ) s = TE_GOAL_MIN;
			if( s > TE_GOAL_MAX ) s = TE_GOAL_MAX;
		}
		temps_plus( (float)(int)s, "TIME EXTEND" );
		return;
	}
	++s_run.perdus;
	if( !s_run.mods[MOD_LIVES] ) return;
	--s_run.vies;
	if( s_run.vies <= 0 ) { s_run.vies = 0; game_over( "Out of lives." ); return; }
	char t[64];
	snprintf( t, sizeof( t ), "-1 LIFE" );
	panneau( t, NULL, 0, 128, 50, 40 );
}

// Binding a trick in Demented: never over a starter trick.
bool desktop_demented_bind_ok( bool special, int index, uint32 key_combo )
{
	if( !s_run.actif ) return true;
	Obj::CSkaterProfile *p = Mdl::Skate::Instance()->GetCurrentProfile();
	if( special )
	{
		if( index < 0 || index >= (int)p->GetNumSpecialTrickSlots()) return true;
		Obj::SSpecialTrickInfo inf = p->GetSpecialTrickInfo( index );
		if( inf.IsUnassigned()) return true;
		return !est_depart( 0x80000000u | index, inf.m_TrickName );
	}
	const uint32 t = trick_a( p->GetTrickMapping( crc( "trick_mapping" )), key_combo );
	return !t || !est_depart( key_combo, t );
}

namespace Mdl
{
void DesktopDementedUpdate( Skate &mdl )
{
	if( !s_run.actif ) return;
	Game::CGameMode *mode = mdl.GetGameMode();
	const uint32 niveau = mdl.m_cur_level;
	const bool menu = niveau == CRCD( 0x9f2bafb7, "load_skateshop" ), cas = niveau == crc( "load_cas" );
	const bool histoire = mode && !mode->IsFrontEnd() && mode->IsTrue( CRCD( 0x1ded1ea4, "is_career" ));
	// Until the deal (after the intro, in the create-a-skater level) the run
	// only exists in the menus; anywhere else (a loaded story) it's gone.
	if( !s_run.skater ) { if( !menu && !cas ) fin_run(); return; }
	// Back at the main menu, or out of the story: the run is over.
	if( menu || ( !histoire && !cas )) { fin_run(); return; }
	if( cas ) return;
	// diagnostics: what the goal manager holds 10 s into each level
	{
		static float t_niv = 0.0f;
		if( niveau != s_niv_log ) { s_niv_log = niveau; t_niv = 0.0f; }
		else if( t_niv >= 0.0f )
		{
			t_niv += Tmr::FrameLength();
			if( t_niv > 10.0f )
			{
				t_niv = -1.0f;
				Game::CGoalManager *gm = Game::GetGoalManager();
				int n = gm ? gm->GetNumGoals() : -1, verr = 0;
				for( int i = 0; gm && i < n; ++i ) { Game::CGoal *g = gm->GetGoalByIndex( i ); if( g && g->IsLocked()) ++verr; }
				VLOG( "DSK", "demented: level %08x, %d goals (%d locked, %d active), skater %08x", niveau, n, verr,
					gm ? gm->GetNumActiveGoals() : -1, s_run.skater );
				// each goal: why it might not be offered (giver missing, locked, ...)
				for( int i = 0; gm && i < n; ++i )
				{
					Game::CGoal *g = gm->GetGoalByIndex( i );
					if( !g ) continue;
					uint32 trig = 0;
					if( g->GetParams()) g->GetParams()->GetChecksum( crc( "trigger_obj_id" ), &trig, Script::NO_ASSERT );
					const bool vivant = trig && Obj::CCompositeObjectManager::Instance()->GetObjectByID( trig ) != NULL;
					VLOG( "DSK", "  goal %s: locked %d active %d won %d mini %d giver %s %s", Script::FindChecksumName( g->GetGoalId()),
						g->IsLocked(), g->IsActive(), g->HasWonGoal(), g->IsMinigame(), trig ? Script::FindChecksumName( trig ) : "-",
						trig ? ( vivant ? "here" : "MISSING" ) : "" );
				}
			}
		}
	}
	const bool pause = Mdl::FrontEnd::Instance()->GamePaused();
	// cutscenes and goal intro cams run through the movie managers: they don't
	// count, and the clock is hidden while they play
	const bool film = ( mdl.GetMovieManager() && mdl.GetMovieManager()->IsRolling())
		|| ( mdl.GetObjectAnimManager() && mdl.GetObjectAnimManager()->IsRolling());
	float dt = Tmr::FrameLength();
	if( dt > 0.1f ) dt = 0.1f;	// a load or hitch is not skating time
	if( !pause && !s_run.fini ) s_run.t += dt;
	if( !pause && !film && !s_run.fini && s_run.mods[MOD_TIME] )
	{
		const float avant = s_run.reste;
		s_run.reste -= dt;
		// once the clock is red (under 15 s): the goal timers' beep, once a second
		if( s_run.reste > 0.0f && s_run.reste < 15.0f && (int)avant > (int)s_run.reste )
			Script::RunScript( CRCD( 0xcfc7c3ad, "timer_runout_beep" ));
		if( s_run.reste <= 0.0f )
		{
			s_run.reste = 0.0f;
			if( s_run.mods[MOD_LIVES] && s_run.vies > 1 )
			{
				--s_run.vies;
				s_run.reste = TE_RECHARGE;
				char t[64];
				snprintf( t, sizeof( t ), "OUT OF TIME -1 LIFE" );
				panneau( t, "GoalFail", 0, 128, 50, 40 );
			}
			else { if( s_run.mods[MOD_LIVES] ) s_run.vies = 0; game_over( "Out of time." ); }
		}
	}
	if( !pause ) s_reve_t += dt;
	s_menu = pause || element( crc( "current_menu_anchor" )) || element( crc( "dialog_box_anchor" ))
		|| element( crc( "goal_start_dialog" )) || element( crc( "speech_box_anchor" ));
	// Chaos: curveballs at random times
	if( s_run.mods[MOD_CHAOS] && !s_run.fini ) chaos_maj( dt, pause, film );
	else chaos_tout_finir();
	if( film ) { hud_off(); return; }

	// Run clock bottom right, with lives when they're on.
	if( !element( crc( "desktop_demented_hud" )))
	{
		Script::RunScript( "desktop_demented_hud_create" );
		s_run.hud = element( crc( "desktop_demented_hud" )) != NULL;
		s_hud_neuf = true;
	}
	char t[48], c[32];
	const bool compte = s_run.mods[MOD_TIME];
	temps( c, sizeof( c ), compte ? s_run.reste + 0.999f : s_run.t );
	Front::CTextElement *e = (Front::CTextElement *)element( crc( "desktop_demented_time" ));
	if( e )
	{
		e->SetText( compte ? c : " " );	// only with Time Extend (Marcus)
		// Time Extend: brighter, then red over the last 15 seconds
		Image::RGBA col( 128, 128, 128, compte ? 100 : 70 );
		if( compte && s_run.reste < 15.0f ) col = Image::RGBA( 128, 24, 16, 120 );
		e->SetRGBA( col );
	}
	// Lives: the goal givers' spinning star, one per life, stacked up the
	// left side from the bottom (Marcus), 9 at most. Rebuilt only when the
	// count or the colour changes (Mirror is handled by the projection).
	{
		enum { ETOILES_MAX = 9 };
		static int s_n = -1;
		static int s_miroir = -1;
		static uint32 s_coul = 0xffffffff;
		const int n = s_run.mods[MOD_LIVES] ? ( s_run.vies > 0 ? s_run.vies : 0 ) : 0;
		const int miroir = 0;
		const bool hud = s_run.hud;
		// olive green (Marcus's swatch, about 60,71,23 on the star's body),
		// a random hue with Random Colours
		float r = 0.40f, g = 0.47f, b = 0.15f;
		if( rando_on( MOD_R_COULEURS )) { r = 1.0f; g = 0.0f; b = 0.0f; desktop_rando_teinte( 0x510e527f, &r, &g, &b ); }
		const uint32 coul = ((uint32)( r * 128.0f ) << 16 ) | ((uint32)( g * 128.0f ) << 8 ) | (uint32)( b * 128.0f );
		if( s_hud_neuf || n != s_n || miroir != s_miroir || coul != s_coul )
		{
			for( int i = 0; i < ETOILES_MAX; ++i )
			{
				char id[40];
				snprintf( id, sizeof( id ), "desktop_demented_star%d", i );
				Script::CStruct p;
				p.AddChecksum( "id", crc( id ));
				if( i < n && hud )
				{
					// Inside the TV border (2D y runs -16..464 on screen), a few
					// units from its left and bottom edges, then upwards.
					p.AddPair( "pos", 22.0f, 450.0f - 25.0f * (float)i );
					Script::CArray *a = new Script::CArray;
					a->SetSizeAndType( 4, ESYMBOLTYPE_INTEGER );
					a->SetInteger( 0, (int)( r * 128.0f )); a->SetInteger( 1, (int)( g * 128.0f ));
					a->SetInteger( 2, (int)( b * 128.0f )); a->SetInteger( 3, 128 );
					p.AddArrayPointer( "rgba", a );
					p.AddFloat( "AngleY", (float)i * 0.6f );	// not all facing the same way
				}
				Script::RunScript( "desktop_demented_star", &p );
			}
			s_hud_neuf = false; s_n = n; s_miroir = miroir; s_coul = coul;
		}
	}
}
} // namespace Mdl

// --- script functions ----------------------------------------------------------------

namespace CFuncs
{

// @script | DesktopDemented | desktop: true while a Demented run is going
bool ScriptDesktopDemented( Script::CStruct *pParams, Script::CScript *pScript )
{
	return s_run.actif;
}

// @script | DesktopDementedToggle | desktop: flips Demented modifier i (0 Perma Bail, 1 Zero, 2 Lives, 3 Time Extend, 4 Fragile, 5 Auto, 6 Mirror, 7 Chaos, 8 Surprise)
bool ScriptDesktopDementedToggle( Script::CStruct *pParams, Script::CScript *pScript )
{
	int i = -1;
	pParams->GetInteger( crc( "i" ), &i, Script::NO_ASSERT );
	if( i == MOD_AUTO ) return true;	// Auto removed (Marcus)
	int v = -1;
	if( pParams->GetInteger( crc( "set" ), &v, Script::NO_ASSERT ) && i >= 0 && i < NB_MODS && i != MOD_LIVES )
		s_arme[i] = v != 0;		// SURPRISE ME! / Start Run
	else if( i >= 0 && i < NB_MODS && i != MOD_LIVES ) s_arme[i] = !s_arme[i];	// lives are always on
	return true;
}

// @script | DesktopDementedText | desktop: adds text0..text8 ("Perma Bail: Off" ...)
bool ScriptDesktopDementedText( Script::CStruct *pParams, Script::CScript *pScript )
{
	for( int i = 0; i < NB_MODS; ++i )
	{
		char n[16], t[64];
		snprintf( n, sizeof( n ), "text%d", i );
		snprintf( t, sizeof( t ), "%s: %s", s_mod_noms[i], s_arme[i] ? "On" : "Off" );
		pScript->GetParams()->AddString( n, t );
	}
	return true;
}

// @script | DesktopDementedStart | desktop: a new story is a Demented run (set_career_difficulty)
bool ScriptDesktopDementedStart( Script::CStruct *pParams, Script::CScript *pScript )
{
	fin_run();
	s_run.actif = true;
	for( int i = 0; i < NB_MODS; ++i ) s_run.mods[i] = s_arme[i];
	s_run.mods[MOD_LIVES] = true;	// not a modifier (Marcus): every run has lives
	s_run.mods[MOD_AUTO] = false;	// removed (Marcus)
	if( s_run.mods[MOD_SURPRISE] )
	{
		// Surprise: every other modifier rolled, odds by how much it hurts
		static const int chance[NB_MODS] = { 12, 30, 100, 40, 35, 0, 30, 30, 0, 50, 40, 40, 35, 25, 40 };
		for( int i = 0; i < NB_MODS; ++i )
			if( i != MOD_LIVES && i != MOD_SURPRISE ) s_run.mods[i] = (int)hasard( 100 ) < chance[i];
		if( s_run.mods[MOD_PERMA] ) s_run.mods[MOD_FRAGILE] = false;
	}
	s_reve_t = 0.0f; s_flip_t = 0.0f; s_flip_suivant = 60.0f + hasard( 90 ); s_flip = false;
	chaos_tout_finir();
	s_ev_prochain = 15.0f + hasard( 30 );
	s_graine_chaos = hasard( 0x7fffffff ) | 1;
	s_niv_log = 0;
	s_rando_graine = hasard( 0x7fffffff ) | 1;
	for( int i = 0; i < NB_NOMS_CHAOS; ++i ) s_perm_noms[i] = i;
	for( int i = NB_NOMS_CHAOS - 1; i > 0; --i ) { const int k = (int)hasard( i + 1 ), x = s_perm_noms[i]; s_perm_noms[i] = s_perm_noms[k]; s_perm_noms[k] = x; }
	s_run.skater = 0;
	s_run.nom[0] = 0;
	s_run.vies = 3;
	s_run.chapitre_max = -1;
	s_run.t = 0.0f;
	s_run.reste = TE_DEPART;
	s_run.gagne_temps = 0.0f;
	s_run.bails = s_run.gagnes = s_run.perdus = 0;
	s_run.fini = false;
	s_run.dialogue = false;
	s_run.depart_cles.clear(); s_run.depart_tricks.clear();
	Obj::CSkaterCareer *car = Mdl::Skate::Instance()->GetCareer();
	if( car )
	{
		if( s_run.mods[MOD_ZERO] ) car->SetGlobalFlag( 511 ); else car->UnSetGlobalFlag( 511 );
	}
	VLOG( "DSK", "demented: new run, perma %d zero %d time %d fragile %d auto %d mirror %d chaos %d surprise %d",
		s_run.mods[MOD_PERMA], s_run.mods[MOD_ZERO], s_run.mods[MOD_TIME], s_run.mods[MOD_FRAGILE],
		s_run.mods[MOD_AUTO], s_run.mods[MOD_MIRROR], s_run.mods[MOD_CHAOS], s_run.mods[MOD_SURPRISE] );
	return true;
}

// @script | DesktopDementedStop | desktop: any other new story ends a Demented run
bool ScriptDesktopDementedStop( Script::CStruct *pParams, Script::CScript *pScript )
{
	fin_run();
	return true;
}

// @script | DesktopDementedNeedsDeal | desktop: true until the run's skater is dealt
bool ScriptDesktopDementedNeedsDeal( Script::CStruct *pParams, Script::CScript *pScript )
{
	return s_run.actif && !s_run.skater;
}

bool ScriptDesktopSelectSkaterLent( uint32 name );	// skfuncs.cpp

// @script | DesktopDementedDeal | desktop: deals the run a random skater (any but the created one)
bool ScriptDesktopDementedDeal( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( !s_run.actif || s_run.skater ) return false;
	Obj::CPlayerProfileManager *pm = ppm();
	std::vector<Obj::CSkaterProfile *> v;
	const int n = (int)pm->GetNumProfileTemplates();
	for( int i = 0; i < n; ++i )
	{
		Obj::CSkaterProfile *p = pm->GetProfileTemplateByIndex( i );
		if( !p || !p->IsPro()) continue;
		const uint32 nom = p->GetSkaterNameChecksum();
		if( nom == CRCD( 0xa7be964, "custom" ) || nom == CRCD( 0x061a741e, "ped" )) continue;
		const char *d = NULL;
		if( !p->GetInfo()->GetString( CRCD( 0x2ab66cb8, "display_name" ), &d, Script::NO_ASSERT ) || !d ) continue;
		v.push_back( p );
	}
	if( v.empty()) { VLOG( "DSK", "demented: no skater to deal" ); return false; }
	Obj::CSkaterProfile *pro = v[hasard( (uint32)v.size())];
	const char *d = "";
	pro->GetInfo()->GetString( CRCD( 0x2ab66cb8, "display_name" ), &d, Script::NO_ASSERT );
	snprintf( s_run.nom, sizeof( s_run.nom ), "%s", d ? d : "" );

	// The dealt skater gets random stats: they go through the created
	// skater, whose figures the story lends to whoever is played.
	Obj::CSkaterProfile *c = pm->GetProfileTemplate( CRCD( 0xa7be964, "custom" ));
	if( c )
	{
		for( int i = 0; i < 10; ++i )
		{
			int v0 = 0;
			c->GetInfo()->GetInteger( crc( s_stats[i] ), &v0, Script::NO_ASSERT );
			// Random 0-10 (Marcus): not the dealt skater's own figures.
			const int vp = (int)hasard( 11 );
			s_run.stats_custom[i] = v0;
			c->GetInfo()->AddInteger( crc( s_stats[i] ), vp );
		}
		s_run.stats_gardees = true;
	}
	s_run.skater = pro->GetSkaterNameChecksum();
	ScriptDesktopSelectSkaterLent( s_run.skater );
	note_depart();
	pScript->GetParams()->AddString( "dealt_name", s_run.nom );
	VLOG( "DSK", "demented: dealt %s (%d to pick from), %d starter tricks", s_run.nom, (int)v.size(), (int)s_run.depart_cles.size());
	return true;
}

// @script | DesktopDementedNeedsDialog | desktop: a Demented run whose "You've been dealt" box hasn't shown
bool ScriptDesktopDementedNeedsDialog( Script::CStruct *pParams, Script::CScript *pScript )
{
	return s_run.actif && !s_run.dialogue;
}

// @script | DesktopDementedDialogShown | desktop: marks the dealt box as shown
bool ScriptDesktopDementedDialogShown( Script::CStruct *pParams, Script::CScript *pScript )
{
	s_run.dialogue = true;
	return true;
}

static const char *s_stats_noms[10] = { "Air", "Hang Time", "Ollie", "Speed", "Spin", "Switch",
	"Flip Speed", "Rail Balance", "Lip Balance", "Manual Balance" };

static int stat_run( int i )
{
	if( s_run.mods[MOD_ZERO] ) return 0;
	Obj::CSkaterProfile *c = ppm()->GetProfileTemplate( crc( "custom" ));
	int v = 0;
	if( c ) c->GetInfo()->GetInteger( crc( s_stats[i] ), &v, Script::NO_ASSERT );
	return v;
}

static void liste_mods( char *o, int n )
{
	o[0] = 0;
	for( int i = 0; i < NB_MODS; ++i )
		if( s_run.mods[i] && i != MOD_LIVES )
		{
			const size_t l = strlen( o );
			snprintf( o + l, n - l, "%s%s", l ? ", " : "", s_mod_noms[i] );
		}
	if( !o[0] ) snprintf( o, n, "None" );
}

// @script | DesktopDementedName | desktop: adds dealt_name and dealt_text (the box shown in the room)
bool ScriptDesktopDementedName( Script::CStruct *pParams, Script::CScript *pScript )
{
	pScript->GetParams()->AddString( "dealt_name", s_run.nom );
	char mods[200], t[700];
	liste_mods( mods, sizeof( mods ));
	snprintf( t, sizeof( t ),
		"You've been dealt:\\n\\c2%s\\n\\n"
		"\\c0Difficulty: \\c2Sick\\n"
		"\\c0Modifiers: \\c2%s\\n"
		"\\c0Air %d  Hang %d  Ollie %d  Speed %d  Spin %d\\n"
		"Switch %d  Flip %d  Rail %d  Lip %d  Manual %d\\n\\n"
		"No changing skater, no editing your starter tricks, no saving.",
		s_run.nom, mods, stat_run( 0 ), stat_run( 1 ), stat_run( 2 ), stat_run( 3 ), stat_run( 4 ),
		stat_run( 5 ), stat_run( 6 ), stat_run( 7 ), stat_run( 8 ), stat_run( 9 ));
	pScript->GetParams()->AddString( "dealt_text", t );
	return true;
}

// @script | DesktopDementedStatRows | desktop: the View Stats panel, one desktop_demented_stat_row / desktop_demented_info_row per line
bool ScriptDesktopDementedStatRows( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( !s_run.actif ) return false;
	struct L { static void ligne( const char *g, const char *d )
	{
		Script::CStruct p;
		if( !g ) p.AddChecksum( NONAME, crc( "sep" ));
		else { p.AddString( "text", g ); p.AddString( "value", d ); }
		Script::RunScript( crc( "desktop_demented_stat_row" ), &p );
	} };
	char v[64], g[48];
	snprintf( g, sizeof( g ), " SKATER:" );
	L::ligne( g, s_run.nom );
	L::ligne( " DIFFICULTY:", "SICK" );
	L::ligne( NULL, NULL );
	for( int i = 0; i < 10; ++i )
	{
		snprintf( g, sizeof( g ), " %s:", s_stats_noms[i] );
		snprintf( v, sizeof( v ), "%d / 10", stat_run( i ));
		L::ligne( g, v );
	}
	L::ligne( NULL, NULL );
	if( s_run.mods[MOD_LIVES] ) { snprintf( v, sizeof( v ), "%d", s_run.vies ); L::ligne( " LIVES:", v ); }
	char c[32]; temps( c, sizeof( c ), s_run.t );
	L::ligne( " RUN TIME:", c );
	if( s_run.mods[MOD_TIME] ) { temps( c, sizeof( c ), s_run.reste ); L::ligne( " TIME LEFT:", c ); }
	snprintf( v, sizeof( v ), "%d", s_run.gagnes ); L::ligne( " GOALS BEATEN:", v );
	snprintf( v, sizeof( v ), "%d", s_run.perdus ); L::ligne( " GOALS FAILED:", v );
	snprintf( v, sizeof( v ), "%d", s_run.bails ); L::ligne( " BAILS:", v );
	L::ligne( NULL, NULL );
	int n = 0;
	for( int i = 0; i < NB_MODS; ++i )
		if( s_run.mods[i] && i != MOD_LIVES && i != MOD_SURPRISE )
		{
			L::ligne( n ? " " : " MODIFIERS:", s_mod_noms[i] );
			++n;
		}
	if( !n ) L::ligne( " MODIFIERS:", "NONE" );
	if( s_run.mods[MOD_SURPRISE] ) L::ligne( " ROLLED BY:", "SURPRISE ME!" );
	return true;
}

// @script | DesktopTricksViewOnly | desktop: edit tricks menus are read only (Demented, or a goal running)
bool ScriptDesktopTricksViewOnly( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( s_run.actif ) return true;
	Game::CGoalManager *gm = Game::GetGoalManager();
	return gm && gm->GetNumActiveGoals() > 0;
}

// @script | DesktopRandoTod | desktop: Random World. Called first thing in
// script_change_tod: the time of day it was asked for becomes another one
// (the same one for the whole chapter). No weather: the port draws none
// (Marcus didn't want rain/snow), so the rainy presets are left out and
// set_tod_rainoff runs after, in case the original was a rainy one.
bool ScriptDesktopRandoTod( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( !rando_on( MOD_R_MONDE ) || !pScript || !pScript->GetParams()) return true;
	Script::CStruct *p = pScript->GetParams();
	uint32 a = 0;
	if( !p->GetChecksum( crc( "tod_action" ), &a, Script::NO_ASSERT )) return true;
	static const char *orig[] = { "set_tod_startup", "set_tod_day", "set_tod_morning", "set_tod_evening", "set_tod_night",
		"set_tod_nj_fog", "set_tod_nj_morning", "set_tod_nj_evening", "set_tod_nj_night", "set_tod_vc_evening",
		"set_tod_ru_day", "set_tod_ru_day_snow", "set_tod_ru_night", "set_tod_ru_night_snow", "set_tod_scj" };
	bool trouve = false;
	for( unsigned i = 0; i < sizeof( orig ) / sizeof( orig[0] ); ++i ) if( crc( orig[i] ) == a ) { trouve = true; break; }
	if( !trouve ) return true;	// night vision, KISS and other special looks stay
	static const char *cible[] = { "set_tod_startup", "set_tod_morning", "set_tod_evening", "set_tod_night", "set_tod_nj_morning", "set_tod_nj_evening" };
	static const int poids[] = { 30, 15, 20, 20, 7, 8 };
	int total = 0;
	for( unsigned i = 0; i < 6; ++i ) total += poids[i];
	int r = (int)( melanger( a ^ s_rando_graine ) % (uint32)total ), k = 0;
	while( r >= poids[k] ) r -= poids[k++];
	p->AddChecksum( "tod_action", crc( cible[k] ));
	p->AddChecksum( "tod_action2", crc( "set_tod_rainoff" ));
	VLOG( "DSK", "random world: %08x -> %s", a, cible[k] );
	return true;
}

// @script | DesktopDementedChapter | desktop: chapter title shown; a new chapter gives a life
bool ScriptDesktopDementedChapter( Script::CStruct *pParams, Script::CScript *pScript )
{
	int ch = 0;
	if( !s_run.actif || !pParams->GetInteger( crc( "chapter" ), &ch, Script::NO_ASSERT )) return true;
	if( s_run.chapitre_max < 0 ) { s_run.chapitre_max = ch; return true; }
	if( ch <= s_run.chapitre_max ) return true;
	s_run.chapitre_max = ch;
	s_rando_graine = hasard( 0x7fffffff ) | 1;	// randomizers re-roll each chapter
	++s_txt_gen;	// Word Salad: on-screen text re-shuffled with the new seed
	if( s_run.mods[MOD_LIVES] && !s_run.fini )
	{
		++s_run.vies;
		char t[64];
		snprintf( t, sizeof( t ), "+1 LIFE EARNED" );
		panneau( t, "GoalGood02", 0, 70, 128, 60 );
	}
	if( s_run.mods[MOD_TIME] && !s_run.fini )
	{
		// after the life notice, so the two don't land on top of each other
		s_run.reste += TE_CHAPITRE;
		s_run.gagne_temps += TE_CHAPITRE;
		char t[64], c[32];
		temps( c, sizeof( c ), TE_CHAPITRE );
		snprintf( t, sizeof( t ), "+%s TIME EXTEND", c );
		panneau( t, "HUD_jumpgap", s_run.mods[MOD_LIVES] ? 200 : 0, 70, 110, 128 );
	}
	return true;
}

// @script | DesktopDementedWon | desktop: the story's end in a Demented run; adds text for the summary
bool ScriptDesktopDementedWon( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( !s_run.actif ) return false;
	s_run.fini = true;
	char t[600];
	resume( t, sizeof( t ), true );
	pScript->GetParams()->AddString( "text", t );
	VLOG( "DSK", "demented: story beaten in %.0f s", s_run.t );
	return true;
}

// @script | DesktopDementedEndRun | desktop: closes the run (back to the main menu)
bool ScriptDesktopDementedEndRun( Script::CStruct *pParams, Script::CScript *pScript )
{
	fin_run();
	return true;
}

// @script | DesktopDementedSlotTricks | desktop: puts a goal's missing required_tricks in: a
// random unused slot of the right kind, else a random one of that kind
bool ScriptDesktopDementedSlotTricks( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( !s_run.actif ) return false;
	Script::CArray *a = NULL;
	if( !pParams->GetArray( crc( "required_tricks" ), &a, Script::NO_ASSERT ) || !a ) return false;
	Obj::CSkaterProfile *p = Mdl::Skate::Instance()->GetCurrentProfile();
	Script::CStruct *map = p->GetTrickMapping( crc( "trick_mapping" ));
	bool change = false;
	std::vector<uint32> pris;	// slots filled for this goal, not reused for its other tricks
	for( uint32 i = 0; i < a->GetSize(); ++i )
	{
		const uint32 trick = a->GetChecksum( i );
		if( !trick || cle_de( map, trick )) continue;
		const int ns = (int)p->GetNumSpecialTrickSlots();
		bool deja = false;
		for( int s = 0; s < ns && !deja; ++s ) deja = p->GetSpecialTrickInfo( s ).m_TrickName == trick;
		if( deja ) continue;
		const int f = famille_de_trick( trick );
		if( f >= 0 )
		{
			std::vector<uint32> v, libres; famille_cles( f, v );
			std::vector<uint32> ok;
			for( uint32 c : v )
			{
				bool p2 = false; for( uint32 x : pris ) p2 |= x == c;
				if( p2 ) continue;
				ok.push_back( c );
				if( !trick_a( map, c )) libres.push_back( c );
			}
			if( ok.empty()) continue;
			const uint32 c = libres.empty() ? ok[hasard( (uint32)ok.size())] : libres[hasard( (uint32)libres.size())];
			map->RemoveComponent( c );
			map->AddChecksum( c, trick );
			pris.push_back( c );
			change = true;
			VLOG( "DSK", "demented: goal trick %08x slotted on %08x (%s)", trick, c, libres.empty() ? "replaced" : "free" );
			continue;
		}
		// A special: an empty special slot, else a random one.
		if( ns <= 0 ) { VLOG( "DSK", "demented: goal trick %08x needs a special slot, none", trick ); continue; }
		std::vector<int> libres, ok;
		for( int s = 0; s < ns; ++s )
		{
			bool p2 = false; for( uint32 x : pris ) p2 |= x == ( 0x80000000u | s );
			if( p2 ) continue;
			ok.push_back( s );
			if( p->GetSpecialTrickInfo( s ).IsUnassigned()) libres.push_back( s );
		}
		if( ok.empty()) continue;
		const int s = libres.empty() ? ok[hasard( (uint32)ok.size())] : libres[hasard( (uint32)libres.size())];
		// a key combo of its kind that no other special uses
		Script::CStruct *st = Script::GetStructure( crc( s_sp_types[type_special( trick )] ), Script::NO_ASSERT );
		uint32 combo = 0;
		for( Script::CComponent *c = st ? st->GetNextComponent( NULL ) : NULL; c && !combo; c = st->GetNextComponent( c ))
		{
			const uint32 k = c->mNameChecksum ? c->mNameChecksum : c->mChecksum;
			bool utilise = false;
			for( int o = 0; o < ns; ++o ) if( o != s && p->GetSpecialTrickInfo( o ).m_TrickSlot == k ) utilise = true;
			if( !utilise ) combo = k;
		}
		if( !combo ) combo = p->GetSpecialTrickInfo( s ).m_TrickSlot;
		if( !combo ) continue;
		Obj::SSpecialTrickInfo inf;
		inf.m_TrickSlot = combo;
		inf.m_TrickName = trick;
		inf.m_isCat = false;
		p->SetSpecialTrickInfo( s, inf );
		pris.push_back( 0x80000000u | s );
		change = true;
		VLOG( "DSK", "demented: goal special %08x slotted in special slot %d (%s)", trick, s, libres.empty() ? "replaced" : "free" );
	}
	if( change )
	{
		maj_tricks();
		panneau( "Missing trick slotted in" );
	}
	return true;
}

} // namespace CFuncs

#endif // THUG_DESKTOP
