/*****************************************************************************
**  THUG desktop -- the Pro Skater 4 career                                 **
**  Code/Sk/Modules/Skate/desktop_th4_glue.cpp                              **
**                                                                          **
**  Story Options > PRO SKATER 4 plays the converted THPS4 levels' own      **
**  goals in career mode. Its progress is kept apart from THUG's story:     **
**  on the way in, the goal manager's memory (beaten goals, points, cash)   **
**  is set aside and the PS4 career's is loaded from thug_ps4_career.dat    **
**  next to the exe; on the way back to the main menu it is saved there and **
**  THUG's is put back. So Start New Story or loading a story save never    **
**  touches it, and story saves never get PS4 goals in them (Save Game and  **
**  the after-goal save prompt write the PS4 file instead while it runs).   **
**                                                                          **
**  Levels unlock with pro points (one per goal), as in THPS4: 8 per level, **
**  in THPS4's order (College, San Francisco, Alcatraz, Kona, Shipyard,     **
**  London, Zoo). THPS4 sold Carnival and Chicago in its secrets shop; here **
**  they come after the Zoo, 8 points each. The level select lists only     **
**  the THPS4 levels while the PS4 career runs, and THUG's story doesn't    **
**  list them at all. Global flags 500-508 mark the unlocked levels (only   **
**  set while the PS4 career runs).                                         **
*****************************************************************************/

#ifdef THUG_DESKTOP

#include <core/defines.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/checksum.h>
#include <gel/scripting/utils.h>
#include <sk/modules/skate/skate.h>
#include <sk/modules/skate/goalmanager.h>
#include <sk/objects/skatercareer.h>

#include "desktop_extras.h"
#include "vita_log.h"

namespace
{

static const char *s_fichier = "thug_ps4_career.dat";
static const char *s_dossiers[] = { "Sch", "SF2", "Alc", "Kon", "Jnk", "Lon", "Zoo", "Cnv", "Hof" };
static const char *s_noms[] = { "College", "San Francisco", "Alcatraz", "Kona", "Shipyard", "London", "Zoo", "Carnival", "Chicago" };
enum { NB = sizeof( s_dossiers ) / sizeof( s_dossiers[0] ), POINTS_NIVEAU = 8, DRAPEAU = 500, DRAPEAU_ZERO = 511 };

static bool               s_actif = false;
static Script::CStruct   *sp_histoire = NULL;	// THUG's story, set aside
static bool               s_zero = false;		// THUG Zero flag, set aside
static int                s_ouverts = 0;
static bool               s_a_sauver = false;
// THUG's story keeps its progress in career global flags too: kept aside on
// the way in and put back on the way out, so nothing the THPS4 scripts set
// leaks into the story (Marcus saw story goals vanish after a PS4 visit)
enum { NB_DRAPEAUX = 512 };
static bool               s_drapeaux[NB_DRAPEAUX];

static int niveau( uint32 load_script )
{
	char t[32];
	for( int i = 0; i < NB; ++i )
	{
		snprintf( t, sizeof( t ), "load_TH4_%s", s_dossiers[i] );
		if( Script::GenerateCRC( t ) == load_script ) return i;
	}
	return -1;
}

static int ouverts( void )
{
	Game::CGoalManager *gm = Game::GetGoalManager();
	const int n = 1 + ( gm ? gm->GetTotalNumberOfGoalPointsEarned() : 0 ) / POINTS_NIVEAU;
	return n < NB ? n : NB;
}

static void drapeaux( int n )
{
	Obj::CSkaterCareer *c = Mdl::Skate::Instance()->GetCareer();
	for( int i = 0; i < NB; ++i )
		if( i < n ) c->SetGlobalFlag( DRAPEAU + i ); else c->UnSetGlobalFlag( DRAPEAU + i );
}

static void sauve( void )
{
	Game::CGoalManager *gm = Game::GetGoalManager();
	if( !gm ) return;
	Script::CStruct s;
	gm->DesktopExport( &s );
	const uint32 n = Script::CalculateBufferSize( &s, 4 * 1024 * 1024 );
	uint8 *b = (uint8 *)malloc( n );
	if( !b ) return;
	const uint32 w = Script::WriteToBuffer( &s, b, n, Script::NO_ASSERT );
	const int ok = w ? extras_file_write( s_fichier, b, w ) : 0;
	free( b );
	VLOG( "PS4", "career %s (%u bytes, %d pro points)", ok ? "saved" : "NOT saved", w, gm->GetTotalNumberOfGoalPointsEarned());
}

static void entre( void )
{
	if( s_actif ) return;
	Game::CGoalManager *gm = Game::GetGoalManager();
	if( !gm ) return;
	sp_histoire = new Script::CStruct;
	gm->DesktopExport( sp_histoire );
	Obj::CSkaterCareer *c = Mdl::Skate::Instance()->GetCareer();
	s_zero = c->GetGlobalFlag( DRAPEAU_ZERO );
	for( int i = 0; i < NB_DRAPEAUX; ++i ) s_drapeaux[i] = c->GetGlobalFlag( i );
	c->UnSetGlobalFlag( DRAPEAU_ZERO );

	unsigned n = 0;
	uint8 *b = (uint8 *)extras_file_read( s_fichier, &n );
	Script::CStruct s;
	if( b ) Script::ReadFromBuffer( &s, b );
	free( b );
	gm->DesktopImport( b ? &s : NULL );
	s_actif = true;
	s_ouverts = ouverts();
	drapeaux( s_ouverts );
	VLOG( "PS4", "career in: %s, %d pro points, %d levels open", b ? "loaded" : "new", gm->GetTotalNumberOfGoalPointsEarned(), s_ouverts );
}

static void sort( void )
{
	if( !s_actif ) return;
	sauve();
	Game::CGoalManager *gm = Game::GetGoalManager();
	if( gm ) gm->DesktopImport( sp_histoire );
	delete sp_histoire;
	sp_histoire = NULL;
	drapeaux( 0 );
	Obj::CSkaterCareer *c = Mdl::Skate::Instance()->GetCareer();
	int change = 0;
	for( int i = 0; i < NB_DRAPEAUX; ++i )
	{
		if( i >= DRAPEAU && i < DRAPEAU + NB ) continue;
		const bool v = c->GetGlobalFlag( i );
		if( i != DRAPEAU_ZERO && v != s_drapeaux[i] )
		{
			if( change++ < 40 ) VLOG( "PS4", "career out: global flag %d was %d, now %d (put back)", i, s_drapeaux[i], v );
		}
		if( s_drapeaux[i] ) c->SetGlobalFlag( i ); else c->UnSetGlobalFlag( i );
	}
	if( s_zero ) c->SetGlobalFlag( DRAPEAU_ZERO );
	s_actif = false;
	s_a_sauver = false;
	VLOG( "PS4", "career out: THUG story put back" );
}

} // namespace

bool desktop_th4_active( void ) { return s_actif; }

// GoalManager_LevelLoad: back in the main menu (or any non-THPS4 level),
// the PS4 career ends. Before the goal manager reads its memory.
void desktop_th4_level_load( void )
{
	if( s_actif && niveau( Mdl::Skate::Instance()->m_cur_level ) < 0 ) sort();
}

// A story about to be reset or loaded (GoalManager_ResetCareer, a story
// save read): the PS4 career, if still running, is saved and set aside first.
void desktop_th4_leave( void )
{
	sort();
}

// GoalManager_LevelUnload: the level's goals were just written to memory.
void desktop_th4_level_unload( void )
{
	if( s_actif ) sauve();
}

// A goal was won (CGoal::Win). Saved and checked for unlocks once no goal
// is running.
void desktop_th4_goal_won( void )
{
	if( s_actif ) s_a_sauver = true;
}

namespace Mdl
{
void DesktopTH4Update( Skate &mdl )
{
	if( !s_actif || !s_a_sauver ) return;
	Game::CGoalManager *gm = Game::GetGoalManager();
	if( !gm || gm->GetNumActiveGoals() > 0 ) return;
	s_a_sauver = false;
	gm->LevelUnload();	// the goals' state into memory
	sauve();
	const int n = ouverts();
	for( int i = s_ouverts; i < n; ++i )
	{
		char t[96];
		snprintf( t, sizeof( t ), "%s unlocked!\\nChange Level from the pause menu.", s_noms[i] );
		Script::CStruct p;
		p.AddString( "text", t );
		Script::SpawnScript( "desktop_th4_toast", &p );
	}
	if( n != s_ouverts ) drapeaux( n );
	s_ouverts = n;
}
} // namespace Mdl

namespace CFuncs
{

// @script | DesktopTH4Career | desktop: on = the Pro Skater 4 career starts
// (THUG's story set aside, the PS4 one loaded); save = writes the PS4 save
bool ScriptDesktopTH4Career( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( pParams->ContainsFlag( Script::GenerateCRC( "on" ))) entre();
	else if( pParams->ContainsFlag( Script::GenerateCRC( "save" )) && s_actif )
	{
		Game::CGoalManager *gm = Game::GetGoalManager();
		if( gm ) gm->LevelUnload();
		sauve();
	}
	return true;
}

// @script | DesktopTH4Active | desktop: true while the Pro Skater 4 career runs
bool ScriptDesktopTH4Active( Script::CStruct *pParams, Script::CScript *pScript )
{
	return s_actif;
}

// @script | DesktopTH4Level | desktop: true when level (a load script) is a
// THPS4 level; adds th4_flag, the global flag set while it is unlocked
bool ScriptDesktopTH4Level( Script::CStruct *pParams, Script::CScript *pScript )
{
	uint32 l = 0;
	pParams->GetChecksum( Script::GenerateCRC( "level" ), &l, Script::NO_ASSERT );
	const int i = niveau( l );
	if( i < 0 ) return false;
	pScript->GetParams()->AddInteger( "th4_flag", DRAPEAU + i );
	return true;
}

} // namespace CFuncs

#endif
