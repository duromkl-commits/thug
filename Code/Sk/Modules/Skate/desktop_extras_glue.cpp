/*****************************************************************************
**  THUG desktop -- extras, engine side                                     **
**  Code/Sk/Modules/Skate/desktop_extras_glue.cpp                           **
**                                                                          **
**  DualSense adaptive triggers, the pause-menu Extras (input display,      **
**  combo stats, chaos mode, first person) and THUG Zero. Run each frame    **
**  from the skate module's logic task (DesktopGlueUpdate).                 **
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
#include <sk/objects/skatercareer.h>
#include <sys/timer.h>
#include <math.h>
#include <gel/object/compositeobject.h>
#include <gel/object/compositeobjectmanager.h>
#include <gel/components/pedlogiccomponent.h>
#include <sk/components/skaterphysicscontrolcomponent.h>
#include <sk/engine/feeler.h>
#include <sk/components/skatercorephysicscomponent.h>
#include <sk/components/skaterbalancetrickcomponent.h>
#include <sk/objects/skaterflags.h>
#include <gfx/camera.h>
#include <gfx/nxviewman.h>
#include <gfx/nxviewport.h>
#include <gfx/nxmodel.h>
#include <gfx/2D/Element3d.h>
#include <gel/components/modelcomponent.h>
#include <gel/scripting/array.h>
#include <gel/scripting/symboltype.h>
#include <gel/scripting/symboltable.h>

#include "desktop_pad.h"
#include "desktop_extras.h"
#include "achievements.h"
#include <sk/modules/skate/score.h>
#include <sk/objects/skatercareer.h>
#include <stdlib.h>


namespace
{
static uint32 crc( const char *t ) { return Script::GenerateCRC( t ); }

static Front::CScreenElement *element( uint32 id )
{
	Front::CScreenElementPtr p = Front::CScreenElementManager::Instance()->GetElement( id );
	return p ? p.Convert() : NULL;
}

static void lance( const char *script, Script::CStruct *p = NULL )
{
	Script::SpawnScript( script, p );
}

static void place( uint32 id, float x, float y )
{
	Front::CScreenElement *p = element( id );
	if( p ) p->SetPos( x, y, Front::CScreenElement::FORCE_INSTANT );
}
} // namespace

namespace Mdl
{
void DesktopTriggersUpdate( Skate &mdl );
void DesktopExtrasUpdate( Skate &mdl );
void DesktopModesUpdate( Skate &mdl );

// Each frame, from Skate::s_logic_code.
void DesktopGlueUpdate( Skate &mdl )
{
	DesktopTriggersUpdate( mdl );
	DesktopExtrasUpdate( mdl );
	DesktopModesUpdate( mdl );
}
} // namespace Mdl

// --- DualSense adaptive triggers ------------------------------------------------
//
// Skating: both triggers firm up slightly. In the air R2 (spine transfer,
// revert) has a wall just before the point the game reads it as pressed.
// Grinds, lips and manuals: both triggers stiffen as the balance meter tips.
// On foot, driving, paused or in menus: loose. Sent only when it changes.

extern "C" void desktop_pad_triggers( const unsigned char *droite, const unsigned char *gauche );

namespace
{
static int s_gach_prec = -1;

static void effet( unsigned char *e, int debut, int force )
{
	memset( e, 0, 11 );
	if( force <= 0 ) { e[0] = 0x05; return; }	// no effect
	e[0] = 0x01;								// resistance from 'debut' on
	e[1] = (unsigned char)debut;
	e[2] = (unsigned char)( force > 255 ? 255 : force );
}

static void gachettes( int dd, int fd, int dg, int fg )
{
	const int cle = (( dd & 0xf0 ) << 20 ) ^ (( fd & 0xf8 ) << 12 ) ^ (( dg & 0xf0 ) << 4 ) ^ ( fg & 0xf8 );
	if( cle == s_gach_prec ) return;
	s_gach_prec = cle;
	unsigned char d[11], g[11];
	effet( d, dd, fd );
	effet( g, dg, fg );
	desktop_pad_triggers( d, g );
}
}

namespace Mdl
{
void DesktopTriggersUpdate( Skate &mdl )
{
	Obj::CSkater *sk = mdl.GetLocalSkater();
	Game::CGameMode *mode = mdl.GetGameMode();
	if( !sk || !mode || mode->IsFrontEnd() || Mdl::FrontEnd::Instance()->GamePaused())
	{
		gachettes( 0, 0, 0, 0 );
		return;
	}
	Obj::CSkaterPhysicsControlComponent *pc = GetSkaterPhysicsControlComponentFromObject( sk );
	Obj::CSkaterCorePhysicsComponent *cp = GetSkaterCorePhysicsComponentFromObject( sk );
	if( !pc || !cp || pc->IsDriving() || pc->IsWalking())
	{
		gachettes( 0, 0, 0, 0 );
		return;
	}
	Obj::CSkaterBalanceTrickComponent *bt = GetSkaterBalanceTrickComponentFromObject( sk );
	const Obj::EStateType st = cp->GetState();
	int qui = -1;
	if( st == Obj::RAIL ) qui = 0;
	else if( st == Obj::LIP ) qui = 1;
	else if( st == Obj::GROUND && bt && bt->DoingBalanceTrick()) qui = 2;
	if( qui >= 0 && bt )
	{
		float l = bt->DesktopLean( qui ) / 4096.0f;
		if( l < 0 ) l = -l;
		if( l > 1 ) l = 1;
		const int f = 40 + (int)( l * 200.0f );
		gachettes( 0, f, 0, f );
	}
	else if( st == Obj::AIR || st == Obj::WALL || st == Obj::WALLPLANT )
		gachettes( 80, 230, 0, 25 );
	else
		gachettes( 0, 25, 0, 25 );
}
}


// --- pause-menu Extras: input display, combo stats, chaos mode, first person ----

namespace
{

// Input display: a pad drawn bottom left (lit while held) and the last eight
// presses above it, fighting-game style ("D+X", "UR+O").
static bool s_in_cree = false;
static unsigned s_in_prec = 0;
static char s_in_hist[8][24];

struct BoutonAff { const char *id; unsigned bit; unsigned char r, g, b; };
static const BoutonAff s_btn[] = {
	{ "desktop_in_l2", 0x100, 110, 110, 110 }, { "desktop_in_l1", 0x400, 110, 110, 110 },
	{ "desktop_in_r2", 0x200, 110, 110, 110 }, { "desktop_in_r1", 0x800, 110, 110, 110 },
	{ "desktop_in_up", 0x10, 110, 110, 110 }, { "desktop_in_down", 0x40, 110, 110, 110 },
	{ "desktop_in_left", 0x80, 110, 110, 110 }, { "desktop_in_right", 0x20, 110, 110, 110 },
	{ "desktop_in_tri", 0x1000, 32, 110, 80 }, { "desktop_in_sq", 0x8000, 110, 60, 110 },
	{ "desktop_in_ci", 0x2000, 128, 70, 60 }, { "desktop_in_x", 0x4000, 50, 80, 128 },
	{ "desktop_in_sel", 0x1, 90, 90, 90 }, { "desktop_in_start", 0x8, 90, 90, 90 } };

static void entree_off( void )
{
	if( !s_in_cree ) return;
	s_in_cree = false;
	Script::RunScript( "desktop_input_off" );
}

static void entree_maj( void )
{
	float lx, ly, rx, ry;
	const unsigned b = desktop_pad_raw( &lx, &ly, &rx, &ry );
	if( !element( crc( "desktop_input" )))
	{
		Script::RunScript( "desktop_input_create" );
		for( int i = 0; i < 8; ++i ) s_in_hist[i][0] = 0;
		s_in_cree = element( crc( "desktop_input" )) != NULL;
		if( !s_in_cree ) return;
	}
	for( unsigned i = 0; i < sizeof( s_btn ) / sizeof( s_btn[0] ); ++i )
	{
		Front::CScreenElement *e = element( crc( s_btn[i].id ));
		if( !e ) continue;
		const bool on = ( b & s_btn[i].bit ) != 0;
		e->SetRGBA( on ? Image::RGBA( s_btn[i].r, s_btn[i].g, s_btn[i].b, 128 ) : Image::RGBA( 30, 30, 30, 70 ));
	}
	place( crc( "desktop_in_ls" ), 92.0f + lx * 9.0f, 452.0f + ly * 9.0f );
	place( crc( "desktop_in_rs" ), 122.0f + rx * 9.0f, 452.0f + ry * 9.0f );

	// A new press of a button (not a direction): one history line.
	const unsigned boutons = 0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x100 | 0x200 | 0x400 | 0x800;
	const unsigned neuf = b & ~s_in_prec & boutons;
	s_in_prec = b;
	if( !neuf ) return;
	char dir[4] = "";
	bool u = ( b & 0x10 ) || ly < -0.5f, d = ( b & 0x40 ) || ly > 0.5f;
	bool l = ( b & 0x80 ) || lx < -0.5f, r = ( b & 0x20 ) || lx > 0.5f;
	snprintf( dir, sizeof( dir ), "%s%s", u ? "U" : ( d ? "D" : "" ), l ? "L" : ( r ? "R" : "" ));
	static const struct { unsigned bit; const char *nom; } noms[] = {
		{ 0x4000, "X" }, { 0x8000, "[]" }, { 0x2000, "O" }, { 0x1000, "/\\" },
		{ 0x400, "L1" }, { 0x800, "R1" }, { 0x100, "L2" }, { 0x200, "R2" } };
	char t[24];
	snprintf( t, sizeof( t ), "%s", dir );
	for( unsigned k = 0; k < sizeof( noms ) / sizeof( noms[0] ); ++k )
		if( neuf & noms[k].bit )
		{
			const size_t n = strlen( t );
			snprintf( t + n, sizeof( t ) - n, "%s%s", n ? "+" : "", noms[k].nom );
		}
	for( int i = 7; i > 0; --i ) memcpy( s_in_hist[i], s_in_hist[i - 1], sizeof( s_in_hist[i] ));
	snprintf( s_in_hist[0], sizeof( s_in_hist[0] ), "%s", t );
	for( int i = 0; i < 8; ++i )
	{
		char id[32];
		snprintf( id, sizeof( id ), "desktop_in_h%d", i );
		Front::CTextElement *e = (Front::CTextElement *)element( crc( id ));
		if( e ) e->SetText( s_in_hist[i][0] ? s_in_hist[i] : " " );
	}
}

// Combo stats: where the time went in each combo, shown when it lands.
static float s_c_air = 0, s_c_rail = 0, s_c_lip = 0, s_c_man = 0, s_c_sol = 0;
static float s_c_meilleur = 0;

static void combo_raz( void ) { s_c_air = s_c_rail = s_c_lip = s_c_man = s_c_sol = 0; }

static void combo_maj( Obj::CSkater *sk, float dt )
{
	Mdl::Score *sc = sk->GetScoreObject();
	if( !sc || sc->GetScorePotValue() <= 0 ) return;
	Obj::CSkaterCorePhysicsComponent *cp = GetSkaterCorePhysicsComponentFromObject( sk );
	Obj::CSkaterBalanceTrickComponent *bt = GetSkaterBalanceTrickComponentFromObject( sk );
	Obj::CSkaterPhysicsControlComponent *pc = GetSkaterPhysicsControlComponentFromObject( sk );
	if( !cp ) return;
	const Obj::EStateType st = cp->GetState();
	if( pc && pc->IsWalking()) s_c_sol += dt;
	else if( st == Obj::RAIL ) s_c_rail += dt;
	else if( st == Obj::LIP ) s_c_lip += dt;
	else if( st == Obj::GROUND ) { if( bt && bt->DoingBalanceTrick()) s_c_man += dt; else s_c_sol += dt; }
	else s_c_air += dt;
}

// Chaos mode: a random modifier every 45 s in free skate. Cheats are faked
// through CSkaterCareer::GetCheat; flags the game sets meanwhile are put
// back when the modifier ends, so nothing sticks in a save.
struct Chaos { const char *nom; const char *cheat; float stat; };
static const Chaos s_chaos[] = {
	{ "Moon Gravity", "CHEAT_MOON", 0 }, { "Perfect Rails", "CHEAT_PERFECT_RAIL", 0 },
	{ "Perfect Manuals", "CHEAT_PERFECT_MANUAL", 0 }, { "Always Special", "CHEAT_ALWAYS_SPECIAL", 0 },
	{ "Flame", "CHEAT_FLAME", 0 }, { "Maxed Stats", NULL, 13.0f }, { "Rookie Stats", NULL, 1.0f } };
static int   s_ch_actif = -1;
static float s_ch_t = 0.0f;
static int   s_ch_drapeau = -1;
static bool  s_ch_avant = false;

static void chaos_fin( void )
{
	if( s_ch_actif < 0 ) return;
	const Chaos &c = s_chaos[s_ch_actif];
	if( c.stat > 0.0f ) Mdl::Skate::Instance()->SetStatOverride( 0.0f );
	if( s_ch_drapeau >= 0 )
	{
		Obj::CSkaterCareer *car = Mdl::Skate::Instance()->GetCareer();
		if( car )
		{
			if( s_ch_avant ) car->SetGlobalFlag( s_ch_drapeau ); else car->UnSetGlobalFlag( s_ch_drapeau );
		}
	}
	s_ch_actif = -1;
	s_ch_drapeau = -1;
}

static void chaos_suivant( void )
{
	if( s_ch_actif >= 0 ) ach_add( ACH_CHAOS, 1 );	// a round skated through
	const int n = (int)( sizeof( s_chaos ) / sizeof( s_chaos[0] ));
	const int prec = s_ch_actif;
	chaos_fin();
	int i = rand() % n;
	if( i == prec ) i = ( i + 1 ) % n;
	s_ch_actif = i;
	const Chaos &c = s_chaos[i];
	if( c.cheat )
	{
		s_ch_drapeau = Script::GetInteger( crc( c.cheat ));
		Obj::CSkaterCareer *car = Mdl::Skate::Instance()->GetCareer();
		s_ch_avant = car && car->GetGlobalFlag( s_ch_drapeau );
	}
	if( c.stat > 0.0f ) Mdl::Skate::Instance()->SetStatOverride( c.stat );
	char t[64];
	snprintf( t, sizeof( t ), "CHAOS: %s", c.nom );
	Script::CStruct p;
	p.AddString( "text", t );
	lance( "desktop_chaos_msg", &p );
}

// First person (SELECT outside Skater Life): the camera at the skater's head,
// looking where they're going; the skater's model is hidden.
static bool s_fp = false, s_fp_cache = false;
static Mth::Vector s_fp_dir( 0.0f, 0.0f, 1.0f );
static unsigned s_fp_prec = 0;

static void fp_modele( Obj::CCompositeObject *sk, bool cache )
{
	Obj::CModelComponent *mc = sk ? GetModelComponentFromObject( sk ) : NULL;
	if( mc && mc->GetModel()) mc->GetModel()->Hide( cache );
	s_fp_cache = cache;
}

} // namespace

// CSkaterCareer::GetCheat: true while chaos mode fakes that cheat.
bool desktop_practice_cheat( uint32 cheat );	// desktop_modes_glue.cpp
bool desktop_chaos_cheat( uint32 cheat )
{
	if( desktop_practice_cheat( cheat )) return true;
	return s_ch_actif >= 0 && s_chaos[s_ch_actif].cheat && crc( s_chaos[s_ch_actif].cheat ) == cheat;
}

// Score::Land / Bail: the combo's breakdown.
void desktop_combo_end( int score, bool bail )
{
	const float total = s_c_air + s_c_rail + s_c_lip + s_c_man + s_c_sol;
	// Achievements: none for combos helped by practice or chaos.
	if( bail ) ach_add( ACH_BAILS, 1 );
	else if( score > 0 && !extras_get( EXTRA_SLOWMO ) && !extras_get( EXTRA_PERFECT ) && s_ch_actif < 0 )
	{
		ach_max( ACH_COMBO_BEST, (float)score );
		ach_add( ACH_POINTS, (float)score );
		ach_max( ACH_COMBO_TIME, total );
		ach_max( ACH_AIR_TIME, s_c_air );
		ach_max( ACH_GRIND_TIME, s_c_rail );
		ach_max( ACH_MANUAL_TIME, s_c_man );
		if( s_fp ) ach_max( ACH_FP_COMBO, (float)score );
	}
	if( !bail && extras_get( EXTRA_COMBO_STATS ) && total >= 1.0f )
	{
		const float trucs = total - s_c_sol;
		const bool record = total > s_c_meilleur;
		if( record ) s_c_meilleur = total;
		char t[200];
		snprintf( t, sizeof( t ), "%.1fs combo%s  -  air %.1f  grind %.1f  lip %.1f  manual %.1f  runout %.1f  (%d%% tricks)",
			total, record ? " (best)" : "", s_c_air, s_c_rail, s_c_lip, s_c_man, s_c_sol,
			(int)( 100.0f * trucs / total + 0.5f ));
		Script::CStruct p;
		p.AddString( "text", t );
		lance( "desktop_combo_msg", &p );
	}
	combo_raz();
}

// Skater and walk cameras: first person.
bool desktop_first_person( Obj::CCompositeObject *sk, Mth::Vector &pos, Mth::Matrix &m )
{
	if( !s_fp || !sk ) return false;
	Mth::Matrix &sm = sk->GetMatrix();
	Mth::Vector v = sk->GetVel();
	v[Y] = 0.0f; v[W] = 0.0f;
	Mth::Vector cible = v;
	if( v.Length() < 60.0f )
	{
		cible = sm[Z];
		cible[Y] = 0.0f; cible[W] = 0.0f;
	}
	if( cible.Length() > 0.01f )
	{
		cible.Normalize();
		const float dt = Tmr::FrameLength();
		const float k = dt * 7.0f > 1.0f ? 1.0f : dt * 7.0f;
		s_fp_dir = s_fp_dir + ( cible - s_fp_dir ) * k;
		s_fp_dir[Y] = 0.0f; s_fp_dir[W] = 0.0f;
		if( s_fp_dir.Length() < 0.01f ) s_fp_dir = cible;
		s_fp_dir.Normalize();
	}
	Mth::Vector haut = sm[Y];
	haut[W] = 0.0f;
	pos = sk->GetPos() + haut * 62.0f + s_fp_dir * 6.0f;
	pos[W] = 1.0f;
	Mth::Vector z = s_fp_dir + Mth::Vector( 0.0f, -0.12f, 0.0f );
	z[W] = 0.0f;
	z.Normalize();
	Mth::Vector yv( 0.0f, 1.0f, 0.0f );
	Mth::Vector xv = Mth::CrossProduct( yv, z );
	xv.Normalize();
	yv = Mth::CrossProduct( z, xv );
	yv.Normalize();
	m[X] = -xv; m[Y] = yv; m[Z] = -z;
	m[X][W] = m[Y][W] = m[Z][W] = 0.0f;
	m[W].Set( 0.0f, 0.0f, 0.0f, 1.0f );
	return true;
}

namespace Mdl
{
void DesktopExtrasUpdate( Skate &mdl )
{
	Obj::CSkater *sk = mdl.GetLocalSkater();
	Game::CGameMode *mode = mdl.GetGameMode();
	const bool jeu = sk && mode && !mode->IsFrontEnd();
	const bool pause = Mdl::FrontEnd::Instance()->GamePaused();
	if( !jeu )
	{
		entree_off();
		chaos_fin();
		if( s_fp_cache ) fp_modele( sk, false );
		s_fp = false;
		return;
	}
	float lx, ly, rx, ry;
	const unsigned b = desktop_pad_raw( &lx, &ly, &rx, &ry );
	const unsigned appui = b & ~s_fp_prec;
	s_fp_prec = b;
	// SELECT: first person on or off.
	if( !pause && ( appui & 0x1 ))
	{
		s_fp = !s_fp;
		Obj::CSkaterPhysicsControlComponent *pc = GetSkaterPhysicsControlComponentFromObject( sk );
		(void)pc;
		Mth::Vector z = sk->GetMatrix()[Z];
		z[Y] = 0.0f; z[W] = 0.0f;
		if( z.Length() > 0.01f ) { z.Normalize(); s_fp_dir = z; }
	}
	if( s_fp || s_fp_cache ) fp_modele( sk, s_fp );

	if( extras_get( EXTRA_INPUT_DISPLAY ) && !pause ) entree_maj(); else entree_off();

	if( !pause ) combo_maj( sk, Tmr::FrameLength());

	const bool libre = mode->GetNameChecksum() == crc( "singlesession" ) || mode->GetNameChecksum() == crc( "freeskate" );
	if( extras_get( EXTRA_CHAOS ) && libre )
	{
		if( !pause )
		{
			s_ch_t -= Tmr::FrameLength();
			if( s_ch_actif < 0 || s_ch_t <= 0.0f )
			{
				s_ch_t = 45.0f;
				chaos_suivant();
			}
		}
	}
	else
	{
		chaos_fin();
		s_ch_t = 0.0f;
	}
}
}

// --- script functions (ftables.cpp) ----------------------------------------------

namespace CFuncs
{

// --- THUG Zero ----------------------------------------------------------------
//
// A story with every stat at 0 and Sick goals. The story menu's "THUG Zero"
// arms it, the new story's set_career_difficulty sets global flag 511 (saved
// with the story; THUG's own flags stop at 461 and reset_career leaves it).

static bool s_zero_arme = false;

// @script | DesktopZeroArm | desktop: on arms THUG Zero for the next new story, off disarms
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

// @script | DesktopExtraToggle | desktop: i = 0 input display, 1 combo stats, 2 chaos mode
bool ScriptDesktopExtraToggle( Script::CStruct *pParams, Script::CScript *pScript )
{
	int i = -1;
	pParams->GetInteger( Script::GenerateCRC( "i" ), &i, Script::NO_ASSERT );
	extras_toggle( i );
	return true;
}

// @script | DesktopExtrasText | desktop: adds text0..text2 ("Input Display: On" ...)
bool ScriptDesktopExtrasText( Script::CStruct *pParams, Script::CScript *pScript )
{
	for( int i = 0; i < NB_EXTRAS; ++i )
	{
		char n[16], t[64];
		snprintf( n, sizeof( n ), "text%d", i );
		snprintf( t, sizeof( t ), "%s: %s", extras_name( i ), extras_get( i ) ? "On" : "Off" );
		pScript->GetParams()->AddString( n, t );
	}
	return true;
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

#endif // THUG_DESKTOP
