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
#include <vector>

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
#include "vita_log.h"

bool DesktopThugZero( void );

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

enum { MOD_PERMA, MOD_ZERO, MOD_LIVES, MOD_TIME, MOD_FRAGILE, NB_MODS };
static const char *s_mod_noms[NB_MODS] = { "Perma Bail", "Zero", "Lives", "Time Extend", "Fragile" };
static bool s_arme[NB_MODS] = { false, false, true, false, false };

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

static void hud_off( void )
{
	if( s_run.hud ) Script::RunScript( "desktop_demented_hud_off" );
	s_run.hud = false;
}

static void fin_run( void )
{
	if( !s_run.actif ) return;
	hud_off();
	ppm()->DesktopEndLend();
	stats_rendre();
	s_run.actif = false;
	VLOG( "DSK", "demented: run closed" );
}

static void resume( char *o, int n, bool gagne )
{
	char t[32], mods[64] = "";
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
		static uint32 niv_log = 0;
		static float t_niv = 0.0f;
		if( niveau != niv_log ) { niv_log = niveau; t_niv = 0.0f; }
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
		s_run.reste -= dt;
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
	if( film ) { hud_off(); return; }

	// Run clock bottom right, with lives when they're on.
	if( !element( crc( "desktop_demented_hud" )))
	{
		Script::RunScript( "desktop_demented_hud_create" );
		s_run.hud = element( crc( "desktop_demented_hud" )) != NULL;
	}
	char t[48], c[32];
	const bool compte = s_run.mods[MOD_TIME];
	temps( c, sizeof( c ), compte ? s_run.reste + 0.999f : s_run.t );
	Front::CTextElement *e = (Front::CTextElement *)element( crc( "desktop_demented_time" ));
	if( e )
	{
		e->SetText( c );
		// Time Extend: brighter, then red over the last 15 seconds
		Image::RGBA col( 128, 128, 128, compte ? 100 : 70 );
		if( compte && s_run.reste < 15.0f ) col = Image::RGBA( 128, 24, 16, 120 );
		e->SetRGBA( col );
	}
	if( s_run.mods[MOD_LIVES] ) snprintf( t, sizeof( t ), "LIVES %d", s_run.vies );
	else snprintf( t, sizeof( t ), " " );
	e = (Front::CTextElement *)element( crc( "desktop_demented_lives" ));
	if( e ) e->SetText( t );
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

// @script | DesktopDementedToggle | desktop: flips Demented modifier i (0 Perma Bail, 1 Zero, 2 Lives, 3 Time Extend, 4 Fragile)
bool ScriptDesktopDementedToggle( Script::CStruct *pParams, Script::CScript *pScript )
{
	int i = -1;
	pParams->GetInteger( crc( "i" ), &i, Script::NO_ASSERT );
	if( i >= 0 && i < NB_MODS && i != MOD_LIVES ) s_arme[i] = !s_arme[i];	// lives are always on
	return true;
}

// @script | DesktopDementedText | desktop: adds text0..text3 ("Perma Bail: Off" ...)
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
	VLOG( "DSK", "demented: new run, perma bail %d, zero %d, lives %d, time extend %d, fragile %d", s_run.mods[0], s_run.mods[1], s_run.mods[2], s_run.mods[3], s_run.mods[4] );
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

	// The dealt skater starts with its own stats: they go through the
	// created skater, whose figures the story lends to whoever is played.
	Obj::CSkaterProfile *c = pm->GetProfileTemplate( CRCD( 0xa7be964, "custom" ));
	if( c )
	{
		for( int i = 0; i < 10; ++i )
		{
			int v0 = 0, vp = 0;
			c->GetInfo()->GetInteger( crc( s_stats[i] ), &v0, Script::NO_ASSERT );
			pro->GetInfo()->GetInteger( crc( s_stats[i] ), &vp, Script::NO_ASSERT );
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

// @script | DesktopDementedName | desktop: adds dealt_name (the dealt skater's name)
bool ScriptDesktopDementedName( Script::CStruct *pParams, Script::CScript *pScript )
{
	pScript->GetParams()->AddString( "dealt_name", s_run.nom );
	return true;
}

// @script | DesktopTricksViewOnly | desktop: edit tricks menus are read only (Demented, or a goal running)
bool ScriptDesktopTricksViewOnly( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( s_run.actif ) return true;
	Game::CGoalManager *gm = Game::GetGoalManager();
	return gm && gm->GetNumActiveGoals() > 0;
}

// @script | DesktopDementedChapter | desktop: chapter title shown; a new chapter gives a life
bool ScriptDesktopDementedChapter( Script::CStruct *pParams, Script::CScript *pScript )
{
	int ch = 0;
	if( !s_run.actif || !pParams->GetInteger( crc( "chapter" ), &ch, Script::NO_ASSERT )) return true;
	if( s_run.chapitre_max < 0 ) { s_run.chapitre_max = ch; return true; }
	if( ch <= s_run.chapitre_max ) return true;
	s_run.chapitre_max = ch;
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
