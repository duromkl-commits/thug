/*****************************************************************************
**  THUG desktop -- engine-side glue                                        **
**  Code/Sk/Modules/Skate/desktop_extras_glue.cpp                           **
**                                                                          **
**  The per-frame desktop updates (DesktopGlueUpdate, from the skate        **
**  module's logic task), Demented's faked cheats and THUG Zero (Twisted).  **
**  The pause-menu Extras (input display, combo stats, chaos lite, first    **
**  person, photo, classic, goal attack, practice) and the achievements     **
**  were removed at Marcus's request on 2026-11-09.                         **
*****************************************************************************/

#ifdef THUG_DESKTOP

#include <core/defines.h>

#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/checksum.h>
#include <gel/scripting/symboltable.h>
#include <sk/modules/skate/skate.h>
#include <sk/modules/skate/gamemode.h>
#include <sk/objects/skatercareer.h>

namespace Mdl
{
void DesktopTH4Update( Skate &mdl );
void DesktopDementedUpdate( Skate &mdl );
void DesktopPortraitsUpdate( Skate &mdl );

// Each frame, from Skate::s_logic_code.
void DesktopGlueUpdate( Skate &mdl )
{
	DesktopTH4Update( mdl );
	DesktopDementedUpdate( mdl );
	DesktopPortraitsUpdate( mdl );
}
} // namespace Mdl

// CSkaterCareer::GetCheat: true while Demented's Chaos fakes that cheat.
bool desktop_demented_cheat( uint32 cheat );	// desktop_demented.cpp
bool desktop_chaos_cheat( uint32 cheat )
{
	return desktop_demented_cheat( cheat );
}

// CGoal::Win: the Pro Skater 4 career's own bookkeeping.
void desktop_th4_goal_won( void );	// desktop_th4_glue.cpp
void desktop_goal_won( uint32 goal, const char *nom )
{
	(void)goal; (void)nom;
	desktop_th4_goal_won();
}

extern "C" int desktop_air_flip_bail( void );	// trickcomponent.cpp
extern "C" void desktop_air_trick_mark( void );	// trickcomponent.cpp

namespace CFuncs
{

// --- THUG Zero ----------------------------------------------------------------
//
// A story with every stat at 0 and Sick goals. The story menu's "THUG Zero"
// arms it, the new story's set_career_difficulty sets global flag 511 (saved
// with the story; THUG's own flags stop at 461 and reset_career leaves it).

static bool s_zero_arme = false;

// @script | DesktopZeroArm | desktop: on arms THUG Zero for the next new story, off disarms
// @script | DesktopAirTrickMark | desktop: a flip or grab trick started (THUG2 flips/rolls need one)
bool ScriptDesktopAirTrickMark( Script::CStruct *pParams, Script::CScript *pScript )
{
	desktop_air_trick_mark();
	return true;
}

// @script | DesktopAirFlipBail | desktop: true when a flip/roll is landed before it comes round
bool ScriptDesktopAirFlipBail( Script::CStruct *pParams, Script::CScript *pScript )
{
	return desktop_air_flip_bail() != 0;
}

bool ScriptDesktopZeroArm( Script::CStruct *pParams, Script::CScript *pScript )
{
	s_zero_arme = pParams->ContainsFlag( Script::GenerateCRC( "on" ));
	return true;
}

// @script | DesktopZeroArmed | desktop: true while THUG Zero is armed
bool ScriptDesktopZeroArmed( Script::CStruct *pParams, Script::CScript *pScript )
{
	return s_zero_arme;
}

} // namespace CFuncs

bool DesktopThugZero( void )
{
	Mdl::Skate *sk = Mdl::Skate::Instance();
	if( !sk ) return false;
	Game::CGameMode *m = sk->GetGameMode();
	if( !m || !m->IsTrue( CRCD( 0x1ded1ea4, "is_career" ))) return false;
	Obj::CSkaterCareer *c = sk->GetCareer();
	return c && c->GetGlobalFlag( 511 );
}

// Main menu / front end: the skater rolling around behind the menus takes
// no pad input; nor does the skater of a level being left (Marcus: no manuals or tricks on the startup screen).
bool DesktopFrontEndSkater( void )
{
	Mdl::Skate *sk = Mdl::Skate::Instance();
	if( !sk ) return false;
	// Also between change_level and the new level's HideLoadingScreen
	// (gameflow resets is_changing_levels there): the pause menu is gone
	// and the old level keeps running for a few frames (Marcus: he could
	// still move the skater just before the loading screen).
	if( Script::GetInteger( CRCD( 0xe997db9f, "is_changing_levels" )))
		return true;
	Game::CGameMode *m = sk->GetGameMode();
	return m && m->IsFrontEnd();
}

#endif // THUG_DESKTOP
