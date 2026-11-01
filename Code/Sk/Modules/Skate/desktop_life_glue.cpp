/*****************************************************************************
**  THUG desktop -- Skater Life mode, engine side                           **
**  Code/Sk/Modules/Skate/desktop_life_glue.cpp                             **
**                                                                          **
**  The simulation is desktop/src/skater_life.cpp. This file runs it from   **
**  the skate module's logic task, keeps its HUD (built by the patched-in   **
**  script desktop_life_hud_create) up to date, shows its messages and      **
**  gives the scripts their DesktopLife* functions.                         **
*****************************************************************************/

#ifdef THUG_DESKTOP

#include <core/defines.h>
#include <stdio.h>
#include <string.h>

#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/checksum.h>
#include <gel/music/music.h>
#include <gfx/2D/ScreenElemMan.h>
#include <gfx/2D/ScreenElement2.h>
#include <gfx/2D/TextElement.h>
#include <sk/modules/skate/skate.h>
#include <sk/modules/skate/gamemode.h>
#include <sk/modules/frontend/frontend.h>
#include <sk/objects/skater.h>
#include <sys/timer.h>

#include "skater_life.h"

namespace
{

static bool s_hud = false;
static bool s_radio_lancee = false;	// the Sims radio has taken over the music
static char s_txt_horloge[64] = "";
static char s_txt_cash[32] = "";
static char s_txt_gain[32] = "";

static Front::CScreenElement *element( uint32 id )
{
	Front::CScreenElementPtr p = Front::CScreenElementManager::Instance()->GetElement( id );
	return p ? p.Convert() : NULL;
}

static void texte( uint32 id, char *dernier, int n, const char *t )
{
	if( !strncmp( dernier, t, n )) return;
	Front::CTextElement *p = (Front::CTextElement *)element( id );
	if( !p ) return;
	p->SetText( t );
	snprintf( dernier, n, "%s", t );
}

// Red under 25, yellow around 50, green from 75 (PS2-range RGBA, 128 = full).
static Image::RGBA couleur( float v )
{
	float r, g, b = 20.0f;
	if( v < 50.0f )	{ const float t = v / 50.0f; r = 125.0f; g = 25.0f + t * 85.0f; }
	else			{ const float t = ( v - 50.0f ) / 50.0f; r = 125.0f - t * 85.0f; g = 110.0f; }
	return Image::RGBA((uint8)r, (uint8)g, (uint8)b, 128 );
}

static void met_a_jour_hud( void )
{
	char t[64];
	life_clock_text( t, sizeof( t ));
	texte( Script::GenerateCRC( "desktop_life_clock" ), s_txt_horloge, sizeof( s_txt_horloge ), t );
	snprintf( t, sizeof( t ), "$%d", life_cash());
	texte( Script::GenerateCRC( "desktop_life_cash" ), s_txt_cash, sizeof( s_txt_cash ), t );

	for( int i = 0; i < life_num_needs(); ++i )
	{
		snprintf( t, sizeof( t ), "desktop_life_bar%d", i );
		Front::CScreenElement *p = element( Script::GenerateCRC( t ));
		if( !p ) continue;
		const float v = life_need( i );
		p->SetScale( 10.5f * ( v < 1.0f ? 0.01f : v / 100.0f ), 1.0f, false, Front::CScreenElement::FORCE_INSTANT );
		p->SetRGBA( couleur( v ));
	}

	float age = 99.0f;
	const int gain = life_gain( &age );
	Front::CScreenElement *g = element( Script::GenerateCRC( "desktop_life_gain" ));
	if( g )
	{
		snprintf( t, sizeof( t ), "+$%d", gain );
		texte( Script::GenerateCRC( "desktop_life_gain" ), s_txt_gain, sizeof( s_txt_gain ), t );
		float a = age < 1.5f ? 1.0f : ( age > 2.5f ? 0.0f : 2.5f - age );
		g->SetAlpha( a, Front::CScreenElement::FORCE_INSTANT );
	}
}

static void detruit_hud( void )
{
	if( !s_hud ) return;
	s_hud = false;
	s_txt_horloge[0] = s_txt_cash[0] = s_txt_gain[0] = 0;
	Script::RunScript( "desktop_life_hud_off" );
}

} // namespace

namespace Mdl
{

// Each frame, from Skate::s_logic_code.
void DesktopLifeUpdate( Skate &mdl )
{
	if( !life_on())
	{
		detruit_hud();
		s_radio_lancee = false;
		return;
	}
	Game::CGameMode *p_mode = mdl.GetGameMode();
	if( !p_mode || p_mode->IsFrontEnd())
	{
		detruit_hud();
		return;
	}
	// Skater Life is a free skate session; any other game mode ends it.
	if( p_mode->GetNameChecksum() != Script::GenerateCRC( "singlesession" ) && p_mode->GetNameChecksum() != Script::GenerateCRC( "freeskate" ))
	{
		detruit_hud();
		life_stop();
		return;
	}
	if( !mdl.GetLocalSkater())
		return;

	if( !element( Script::GenerateCRC( "desktop_life_hud" )))
	{
		s_hud = false;
		s_txt_horloge[0] = s_txt_cash[0] = s_txt_gain[0] = 0;
		Script::RunScript( "desktop_life_hud_create" );
		if( !element( Script::GenerateCRC( "desktop_life_hud" )))
			return;
		s_hud = true;
		// Switch to the Sims radio now rather than at the end of the song.
		if( !s_radio_lancee && life_radio_active())
			Pcm::SkipMusicTrack();
		s_radio_lancee = true;
	}

	if( !Mdl::FrontEnd::Instance()->GamePaused())
		life_tick( Tmr::FrameLength());

	char texte_ev[160], son[64];
	int genre = 0;
	while( life_pop_event( texte_ev, sizeof( texte_ev ), son, sizeof( son ), &genre ))
	{
		Script::CStruct *p = new Script::CStruct;
		p->AddString( "text", texte_ev );
		if( son[0] ) p->AddChecksum( "sound", Script::GenerateCRC( son ));
		if( genre == LIFE_EVENT_BLACKOUT ) p->AddChecksum( NONAME, Script::GenerateCRC( "blackout" ));
		Script::SpawnScript( "desktop_life_event", p );
		delete p;
	}

	met_a_jour_hud();
}

} // namespace Mdl

// --- script functions (ftables.cpp) ----------------------------------------------

namespace CFuncs
{

// @script | DesktopLifeStart | desktop: turns Skater Life on (the main menu item)
bool ScriptDesktopLifeStart( Script::CStruct *pParams, Script::CScript *pScript )
{
	life_start();
	return true;
}

// @script | DesktopLifeStop | desktop: saves and turns Skater Life off (the main menu)
bool ScriptDesktopLifeStop( Script::CStruct *pParams, Script::CScript *pScript )
{
	life_stop();
	return true;
}

// @script | DesktopLifeOn | desktop: true while Skater Life is on
bool ScriptDesktopLifeOn( Script::CStruct *pParams, Script::CScript *pScript )
{
	return life_on() != 0;
}

// @script | DesktopLifeAction | desktop: a = snack/meal/coffee/shower/toilet/
// sleep/radio/newlife. False when it can't be done; else adds sound (the
// Sims sound to play) and fade (seconds of black screen, 0 for none).
bool ScriptDesktopLifeAction( Script::CStruct *pParams, Script::CScript *pScript )
{
	static const char *s_actions[] = { "snack", "meal", "coffee", "shower", "toilet", "sleep", "radio", "newlife" };
	uint32 a = 0;
	pParams->GetChecksum( Script::GenerateCRC( "a" ), &a, Script::NO_ASSERT );
	const char *nom = NULL;
	for( unsigned i = 0; i < sizeof( s_actions ) / sizeof( s_actions[0] ); ++i )
		if( a == Script::GenerateCRC( s_actions[i] )) nom = s_actions[i];
	if( !nom ) return false;
	char son[64];
	float fondu = 0.0f;
	if( !life_action( nom, son, sizeof( son ), &fondu ))
		return false;
	if( son[0] ) pScript->GetParams()->AddChecksum( "sound", Script::GenerateCRC( son ));
	pScript->GetParams()->AddFloat( "fade", fondu );
	if( !strcmp( nom, "radio" ))
		Pcm::SkipMusicTrack();
	return true;
}

// @script | DesktopLifeHold | desktop: on holds Skater Life messages (during an
// action's black screen), off shows them
bool ScriptDesktopLifeHold( Script::CStruct *pParams, Script::CScript *pScript )
{
	life_hold( pParams->ContainsFlag( Script::GenerateCRC( "on" ) ) ? 1 : 0 );
	return true;
}

// @script | DesktopLifeRadioName | desktop: adds station = the radio's name
bool ScriptDesktopLifeRadioName( Script::CStruct *pParams, Script::CScript *pScript )
{
	pScript->GetParams()->AddString( "station", life_station_name());
	return true;
}

} // namespace CFuncs

#endif // THUG_DESKTOP
