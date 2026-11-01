/*****************************************************************************
**  THUG desktop -- Extras game modes, engine side                          **
**  Code/Sk/Modules/Skate/desktop_modes_glue.cpp                            **
**                                                                          **
**  Photo mode, classic runs, the goal attack timer and practice options,   **
**  all reached from Pause > Extras (scripts in desktop/src/custom_music.cpp **
**  scripts_extras()). Run each frame from DesktopLifeUpdate.               **
*****************************************************************************/

#ifdef THUG_DESKTOP

#include <core/defines.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/checksum.h>
#include <gel/scripting/array.h>
#include <gel/scripting/symboltable.h>
#include <gfx/2D/ScreenElemMan.h>
#include <gfx/2D/ScreenElement2.h>
#include <gfx/2D/TextElement.h>
#include <gfx/camera.h>
#include <gfx/nxviewman.h>
#include <gfx/nxmodel.h>
#include <gel/object/compositeobject.h>
#include <gel/object/compositeobjectmanager.h>
#include <gel/components/modelcomponent.h>
#include <sk/modules/skate/skate.h>
#include <sk/modules/skate/gamemode.h>
#include <sk/modules/skate/score.h>
#include <sk/modules/frontend/frontend.h>
#include <sk/objects/skater.h>
#include <sk/scripting/nodearray.h>
#include <sys/timer.h>

#include "skater_life.h"
#include "desktop_extras.h"

extern "C" void desktop_photo_shot( void );
extern "C" const char *desktop_photo_last( void );

namespace
{

static uint32 crc( const char *t ) { return Script::GenerateCRC( t ); }

static Front::CScreenElement *element( uint32 id )
{
	Front::CScreenElementPtr p = Front::CScreenElementManager::Instance()->GetElement( id );
	return p ? p.Convert() : NULL;
}

static void texte( const char *id, const char *t )
{
	Front::CTextElement *p = (Front::CTextElement *)element( crc( id ));
	if( p ) p->SetText( t );
}

static void texte_bloc( const char *id, const char *t )
{
	Front::CTextBlockElement *p = (Front::CTextBlockElement *)element( crc( id ));
	if( p ) p->SetText( t );
}

static void panneau( const char *script, const char *t )
{
	Script::CStruct p;
	p.AddString( "text", t );
	Script::SpawnScript( script, &p );
}

static void son( const char *nom )
{
	Script::CStruct p;
	p.AddChecksum( "s", crc( nom ));
	Script::SpawnScript( "desktop_life_snd", &p );
}

static void temps( char *o, int n, float s )
{
	if( s < 0.0f ) s = 0.0f;
	const int m = (int)( s / 60.0f );
	snprintf( o, n, "%d:%04.1f", m, s - m * 60.0f );
}

static void nombre( char *o, int n, int v )
{
	if( v >= 1000000 ) snprintf( o, n, "%d,%03d,%03d", v / 1000000, ( v / 1000 ) % 1000, v % 1000 );
	else if( v >= 1000 ) snprintf( o, n, "%d,%03d", v / 1000, v % 1000 );
	else snprintf( o, n, "%d", v );
}

static bool mode_libre( Game::CGameMode *m )
{
	return m && ( m->GetNameChecksum() == crc( "singlesession" ) || m->GetNameChecksum() == crc( "freeskate" ));
}

// Records are only kept for runs without practice help or chaos.
static bool aide( void )
{
	return extras_get( EXTRA_SLOWMO ) || extras_get( EXTRA_PERFECT ) || extras_get( EXTRA_CHAOS );
}

static void cache_modele( Obj::CCompositeObject *o, bool cache )
{
	Obj::CModelComponent *mc = o ? GetModelComponentFromObject( o ) : NULL;
	if( mc && mc->GetModel()) mc->GetModel()->Hide( cache );
}

static Mth::Vector vec( float x, float y, float z ) { Mth::Vector v( x, y, z ); v[W] = 0.0f; return v; }

// --- photo mode -----------------------------------------------------------------
//
// The game stays paused; the pause menu and HUD are hidden and the camera is
// free. Left stick moves, right stick looks, L1/R1 down/up, L2/R2 zoom,
// d-pad left/right roll, d-pad up/down speed, Square hides the skater,
// X saves a PNG, SELECT resets, Triangle/Circle back to the pause menu.

struct Photo
{
	bool actif;
	Mth::Vector pos, cam_pos0;
	Mth::Matrix cam_m0;
	float lacet, tangage, roulis, fov, fov0, vitesse;
	bool skater_cache;
	unsigned prec;
	Tmr::Time t;
};
static Photo s_ph;

static void photo_matrice( Mth::Matrix &m )
{
	const float cp = cosf( s_ph.tangage ), sp = sinf( s_ph.tangage );
	Mth::Vector z = vec( sinf( s_ph.lacet ) * cp, sp, cosf( s_ph.lacet ) * cp );	// looking
	Mth::Vector xv = Mth::CrossProduct( vec( 0, 1, 0 ), z );
	xv[W] = 0.0f;
	xv.Normalize();
	Mth::Vector yv = Mth::CrossProduct( z, xv );
	yv[W] = 0.0f;
	yv.Normalize();
	const float cr = cosf( s_ph.roulis ), sr = sinf( s_ph.roulis );
	Mth::Vector x2 = xv * cr + yv * sr, y2 = yv * cr - xv * sr;
	x2[W] = y2[W] = 0.0f;
	m[X] = -x2; m[Y] = y2; m[Z] = -z;
	m[X][W] = m[Y][W] = m[Z][W] = 0.0f;
	m[W].Set( 0.0f, 0.0f, 0.0f, 1.0f );
}

static void photo_reset( Gfx::Camera *cam )
{
	s_ph.pos = s_ph.cam_pos0;
	const Mth::Vector z = -s_ph.cam_m0[Z];	// looking direction
	s_ph.lacet = atan2f( z[X], z[Z] );
	const float h = sqrtf( z[X] * z[X] + z[Z] * z[Z] );
	s_ph.tangage = atan2f( z[Y], h );
	s_ph.roulis = 0.0f;
	s_ph.fov = s_ph.fov0;
	s_ph.vitesse = 1.0f;
	if( cam ) cam->SetHFOV( s_ph.fov );
}

static void photo_debut( void )
{
	Gfx::Camera *cam = Nx::CViewportManager::sGetActiveCamera( 0 );
	if( !cam || s_ph.actif ) return;
	s_ph.actif = true;
	s_ph.cam_pos0 = cam->GetPos();
	s_ph.cam_m0 = cam->GetMatrix();
	s_ph.fov0 = cam->GetHFOV();
	s_ph.skater_cache = false;
	s_ph.prec = life_pad_raw( NULL, NULL, NULL, NULL );
	s_ph.t = Tmr::GetTime();
	photo_reset( cam );
}

static void photo_fin( Obj::CCompositeObject *sk, bool menu )
{
	if( !s_ph.actif ) return;
	s_ph.actif = false;
	Gfx::Camera *cam = Nx::CViewportManager::sGetActiveCamera( 0 );
	if( cam )
	{
		cam->SetPos( s_ph.cam_pos0 );
		cam->SetMatrix( s_ph.cam_m0 );
		cam->SetHFOV( s_ph.fov0 );
	}
	if( s_ph.skater_cache ) cache_modele( sk, false );
	s_ph.skater_cache = false;
	if( menu ) Script::SpawnScript( "desktop_photo_back" );
}

static void photo_maj( Obj::CCompositeObject *sk )
{
	Gfx::Camera *cam = Nx::CViewportManager::sGetActiveCamera( 0 );
	if( !cam ) return;
	const Tmr::Time maintenant = Tmr::GetTime();
	float dt = ( maintenant - s_ph.t ) / 1000.0f;
	s_ph.t = maintenant;
	if( dt > 0.1f ) dt = 0.1f;
	float lx, ly, rx, ry;
	const unsigned b = life_pad_raw( &lx, &ly, &rx, &ry );
	const unsigned appui = b & ~s_ph.prec, lache = s_ph.prec & ~b;
	s_ph.prec = b;

	// Back on release, so the press doesn't reach the pause menu it opens.
	if( lache & ( 0x1000 | 0x2000 ))
	{
		photo_fin( sk, true );
		return;
	}
	if( appui & 0x1 ) photo_reset( cam );
	if( appui & 0x8000 )
	{
		s_ph.skater_cache = !s_ph.skater_cache;
		cache_modele( sk, s_ph.skater_cache );
	}
	if( appui & 0x10 ) s_ph.vitesse = s_ph.vitesse * 2.0f > 4.0f ? 4.0f : s_ph.vitesse * 2.0f;
	if( appui & 0x40 ) s_ph.vitesse = s_ph.vitesse * 0.5f < 0.25f ? 0.25f : s_ph.vitesse * 0.5f;
	if( appui & 0x4000 ) desktop_photo_shot();

	// Look: right stick, slower when zoomed in.
	const float k_vue = 1.8f * ( s_ph.fov / 72.0f );
	s_ph.lacet -= rx * k_vue * dt;
	s_ph.tangage -= ry * k_vue * dt;
	if( s_ph.tangage > 1.5f ) s_ph.tangage = 1.5f;
	if( s_ph.tangage < -1.5f ) s_ph.tangage = -1.5f;
	if( b & 0x80 ) s_ph.roulis -= 0.8f * dt;
	if( b & 0x20 ) s_ph.roulis += 0.8f * dt;
	if( b & 0x100 ) s_ph.fov += 30.0f * dt;
	if( b & 0x200 ) s_ph.fov -= 30.0f * dt;
	if( s_ph.fov < 15.0f ) s_ph.fov = 15.0f;
	if( s_ph.fov > 110.0f ) s_ph.fov = 110.0f;

	// Move: left stick on the ground plane, L1/R1 down/up.
	Mth::Matrix m;
	photo_matrice( m );
	Mth::Vector avant = vec( sinf( s_ph.lacet ), 0, cosf( s_ph.lacet ));
	Mth::Vector droite = -m[X];
	droite[Y] = 0.0f; droite[W] = 0.0f;
	if( droite.Length() > 0.01f ) droite.Normalize();
	const float v = 400.0f * s_ph.vitesse * dt;
	Mth::Vector d = avant * ( -ly * v ) + droite * ( lx * v );
	if( b & 0x400 ) d[Y] -= v;
	if( b & 0x800 ) d[Y] += v;
	s_ph.pos = s_ph.pos + d;
	// No further than 25 m (2500 units) from the skater.
	if( sk )
	{
		Mth::Vector r = s_ph.pos - sk->GetPos();
		r[W] = 0.0f;
		if( r.Length() > 2500.0f )
		{
			r.Normalize();
			s_ph.pos = sk->GetPos() + r * 2500.0f;
		}
	}
	s_ph.pos[W] = 1.0f;
	cam->SetPos( s_ph.pos );
	cam->SetMatrix( m );
	cam->SetHFOV( s_ph.fov );
}

// --- classic run ------------------------------------------------------------------
//
// Two minutes, THPS style: three score goals, a Sick combo, S-K-A-T-E,
// C-O-M-B-O in one combo and a secret tape (a floating goal arrow: THUG has
// no loose tape model). Letters sit on the level's rails, picked the same way
// every time from the first restart point, so a level's layout is fixed.

enum { CL_HIGH, CL_PRO, CL_SICK, CL_COMBO_SCORE, CL_SKATE, CL_COMBO, CL_TAPE, CL_NB };
static const char *s_cl_noms[CL_NB] = { "High Score", "Pro Score", "Sick Score", "Sick Combo", "S-K-A-T-E", "C-O-M-B-O", "Secret Tape" };
static const int s_cl_scores[4] = { 75000, 200000, 500000, 150000 };
static const float CL_DUREE = 120.0f;

enum { NB_OBJ = 11 };	// 0-4 SKATE, 5-9 COMBO, 10 tape
static const char *s_cl_modeles[NB_OBJ] = {
	"gameobjects\\skate\\letter_s\\letter_s.mdl", "gameobjects\\skate\\letter_k\\letter_k.mdl",
	"gameobjects\\skate\\letter_a\\letter_a.mdl", "gameobjects\\skate\\letter_t\\letter_t.mdl",
	"gameobjects\\skate\\letter_e\\letter_e.mdl",
	"gameobjects\\combo\\goal_combo_c\\goal_combo_c.mdl", "gameobjects\\combo\\goal_combo_o\\goal_combo_o.mdl",
	"gameobjects\\combo\\goal_combo_m\\goal_combo_m.mdl", "gameobjects\\combo\\goal_combo_b\\goal_combo_b.mdl",
	"gameobjects\\combo\\goal_combo_o\\goal_combo_o.mdl",
	"goalarrow\\goalarrow.mdl" };
static const char *s_cl_lettres[NB_OBJ] = { "S", "K", "A", "T", "E", "C", "O", "M", "B", "O", "TAPE" };

struct Classic
{
	bool demande, actif, fini_attente;
	float t, fin_t;
	bool fait[CL_NB];
	bool pris[NB_OBJ];
	bool place[NB_OBJ];
	Mth::Vector pos[NB_OBJ];
	int combo_pris;		// COMBO letters taken in the current combo
	uint32 niveau;
	int meilleur_combo;
};
static Classic s_cl;

static void cl_obj_nom( char *o, int n, int i ) { snprintf( o, n, "desktop_cl_obj%d", i ); }

static void cl_obj_cree( int i )
{
	char nom[32];
	cl_obj_nom( nom, sizeof( nom ), i );
	Script::CStruct p;
	p.AddChecksum( "name", crc( nom ));
	p.AddString( "model", s_cl_modeles[i] );
	p.AddVector( "pos", s_cl.pos[i][X], s_cl.pos[i][Y], s_cl.pos[i][Z] );
	Script::RunScript( "desktop_classic_obj", &p );
}

static void cl_obj_tue( int i )
{
	char nom[32];
	cl_obj_nom( nom, sizeof( nom ), i );
	Script::CStruct p;
	p.AddChecksum( "name", crc( nom ));
	Script::RunScript( "desktop_classic_obj_kill", &p );
}

// Rail spots: the middle of each rail segment, 30 units up.
static int cl_rails( Mth::Vector *o, int max, const Mth::Vector &centre )
{
	Script::CArray *na = Script::GetArray( CRCD( 0xc472ecc5, "NodeArray" ), Script::NO_ASSERT );
	if( !na ) return 0;
	const uint32 rail = crc( "RailNode" ), classe = crc( "Class" );
	int n = 0;
	for( uint32 i = 0; i < na->GetSize() && n < max; ++i )
	{
		Script::CStruct *nd = na->GetStructure( i );
		uint32 c = 0;
		if( !nd || !nd->GetChecksum( classe, &c, Script::NO_ASSERT ) || c != rail ) continue;
		if( SkateScript::GetNumLinks( nd ) == 0 ) continue;
		const int l = SkateScript::GetLink( nd, 0 );
		Script::CStruct *nl = SkateScript::GetNode( l );
		uint32 c2 = 0;
		if( !nl || !nl->GetChecksum( classe, &c2, Script::NO_ASSERT ) || c2 != rail ) continue;
		Mth::Vector a, b;
		SkateScript::GetPosition( nd, &a );
		SkateScript::GetPosition( nl, &b );
		Mth::Vector m = ( a + b ) * 0.5f;
		m[Y] += 30.0f;
		m[W] = 1.0f;
		Mth::Vector r = m - centre;
		r[W] = 0.0f;
		const float h = sqrtf( r[X] * r[X] + r[Z] * r[Z] );
		if( h > 4000.0f || r[Y] > 900.0f || r[Y] < -1200.0f ) continue;
		o[n++] = m;
	}
	return n;
}

static float dist( const Mth::Vector &a, const Mth::Vector &b )
{
	Mth::Vector d = a - b;
	d[W] = 0.0f;
	return d.Length();
}

static bool cl_placement( Obj::CSkater *sk )
{
	// Centre: the first restart point (the level's start), else the skater.
	Mth::Vector centre = sk->GetPos();
	Script::CArray *na = Script::GetArray( CRCD( 0xc472ecc5, "NodeArray" ), Script::NO_ASSERT );
	if( na )
		for( uint32 i = 0; i < na->GetSize(); ++i )
		{
			Script::CStruct *nd = na->GetStructure( i );
			uint32 c = 0;
			if( nd && nd->GetChecksum( crc( "Class" ), &c, Script::NO_ASSERT ) && c == crc( "Restart" ))
			{
				SkateScript::GetPosition( nd, &centre );
				break;
			}
		}
	static Mth::Vector cand[2048];
	const int n = cl_rails( cand, 2048, centre );
	if( n < 11 ) return false;
	bool pris[2048];
	memset( pris, 0, sizeof( pris ));

	// Tape: the highest rail.
	int t = 0;
	for( int i = 1; i < n; ++i ) if( cand[i][Y] > cand[t][Y] ) t = i;
	pris[t] = true;
	s_cl.pos[10] = cand[t] + vec( 0, 40, 0 );

	// COMBO: a cluster around the rail nearest 1500 units from the start.
	int g = -1;
	float best = 1e9f;
	for( int i = 0; i < n; ++i )
		if( !pris[i] )
		{
			const float e = fabsf( dist( cand[i], centre ) - 1500.0f );
			if( e < best ) { best = e; g = i; }
		}
	int combo[5] = { g, -1, -1, -1, -1 };
	pris[g] = true;
	for( int k = 1; k < 5; ++k )
	{
		int choix = -1;
		float d0 = 1e9f;
		for( int i = 0; i < n; ++i )
		{
			if( pris[i] ) continue;
			bool loin = true;
			for( int j = 0; j < k; ++j ) if( dist( cand[i], cand[combo[j]] ) < 250.0f ) loin = false;
			const float d = dist( cand[i], cand[g] );
			if( loin && d < d0 ) { d0 = d; choix = i; }
		}
		if( choix < 0 || d0 > 1400.0f ) return false;
		combo[k] = choix;
		pris[choix] = true;
	}
	for( int k = 0; k < 5; ++k ) s_cl.pos[5 + k] = cand[combo[k]];

	// SKATE: spread out (farthest point first, from the start).
	int skate[5];
	for( int k = 0; k < 5; ++k )
	{
		int choix = -1;
		float d0 = -1.0f;
		for( int i = 0; i < n; ++i )
		{
			if( pris[i] ) continue;
			float d = dist( cand[i], centre ) * 0.5f;
			for( int j = 0; j < k; ++j ) { const float e = dist( cand[i], cand[skate[j]] ); if( e < d ) d = e; }
			for( int j = 0; j < 5; ++j ) { const float e = dist( cand[i], cand[combo[j]] ); if( e < d ) d = e; }
			if( d > d0 ) { d0 = d; choix = i; }
		}
		if( choix < 0 ) return false;
		skate[k] = choix;
		pris[choix] = true;
		s_cl.pos[k] = cand[choix];
	}
	return true;
}

static void cl_hud( void )
{
	if( element( crc( "desktop_classic_hud" ))) return;
	Script::RunScript( "desktop_classic_hud_create" );
}

static void cl_hud_maj( Obj::CSkater *sk )
{
	char t[96], s[32];
	temps( t, sizeof( t ), CL_DUREE - s_cl.t );
	texte( "desktop_cl_time", t );
	Mdl::Score *sc = sk->GetScoreObject();
	nombre( s, sizeof( s ), sc ? sc->GetTotalScore() : 0 );
	texte( "desktop_cl_score", s );
	for( int i = 0; i < CL_NB; ++i )
	{
		char id[24];
		snprintf( id, sizeof( id ), "desktop_cl_g%d", i );
		if( i == CL_SKATE || i == CL_COMBO )
		{
			const int o = i == CL_SKATE ? 0 : 5;
			char l[16] = "";
			for( int k = 0; k < 5; ++k )
			{
				const size_t n = strlen( l );
				snprintf( l + n, sizeof( l ) - n, "%s", s_cl.pris[o + k] ? s_cl_lettres[o + k] : "-" );
			}
			snprintf( t, sizeof( t ), "%s %s  %s", s_cl.fait[i] ? "\\c4" : "\\c0", s_cl_noms[i], l );
		}
		else if( i <= CL_COMBO_SCORE )
		{
			nombre( s, sizeof( s ), s_cl_scores[i] );
			snprintf( t, sizeof( t ), "%s %s  %s", s_cl.fait[i] ? "\\c4" : "\\c0", s_cl_noms[i], s );
		}
		else
			snprintf( t, sizeof( t ), "%s %s", s_cl.fait[i] ? "\\c4" : "\\c0", s_cl_noms[i] );
		texte( id, t );
	}
}

static void cl_arret( void )
{
	for( int i = 0; i < NB_OBJ; ++i ) if( s_cl.place[i] ) { cl_obj_tue( i ); s_cl.place[i] = false; }
	s_cl.actif = false;
	s_cl.demande = false;
	s_cl.fini_attente = false;
	Script::RunScript( "desktop_classic_hud_off" );
}

static void cl_gagne( int g, const char *quoi )
{
	if( s_cl.fait[g] ) return;
	s_cl.fait[g] = true;
	son( "goaldone" );
	char t[96];
	snprintf( t, sizeof( t ), "%s!", quoi );
	panneau( "desktop_classic_msg", t );
}

static void cl_debut( Obj::CSkater *sk )
{
	memset( s_cl.fait, 0, sizeof( s_cl.fait ));
	memset( s_cl.pris, 0, sizeof( s_cl.pris ));
	s_cl.t = 0.0f;
	s_cl.combo_pris = 0;
	s_cl.meilleur_combo = 0;
	s_cl.fini_attente = false;
	s_cl.niveau = Mdl::Skate::Instance()->m_cur_level;
	if( !cl_placement( sk ))
	{
		s_cl.demande = false;
		panneau( "desktop_classic_msg", "No classic run here: not enough rails in this level" );
		return;
	}
	Mdl::Score *sc = sk->GetScoreObject();
	if( sc ) sc->Reset();
	for( int i = 0; i < NB_OBJ; ++i ) { cl_obj_cree( i ); s_cl.place[i] = true; }
	s_cl.actif = true;
	s_cl.demande = false;
	cl_hud();
	panneau( "desktop_classic_msg", "CLASSIC RUN: 2:00" );
}

static void cl_fin( Obj::CSkater *sk )
{
	Mdl::Score *sc = sk->GetScoreObject();
	const int score = sc ? sc->GetTotalScore() : 0;
	int n = 0;
	for( int i = 0; i < CL_NB; ++i ) if( s_cl.fait[i] ) ++n;
	char cle[48], t[200], s[32];
	nombre( s, sizeof( s ), score );
	const char *etat = "";
	if( aide())
		etat = "\\n(practice or chaos on: not recorded)";
	else
	{
		snprintf( cle, sizeof( cle ), "classic_%08x_score", s_cl.niveau );
		const float avant = extras_record_get( "thug_records.txt", cle, 0.0f );
		snprintf( cle, sizeof( cle ), "classic_%08x_goals", s_cl.niveau );
		const float g_avant = extras_record_get( "thug_records.txt", cle, 0.0f );
		if( n > g_avant ) extras_record_set( "thug_records.txt", cle, (float)n );
		snprintf( cle, sizeof( cle ), "classic_%08x_score", s_cl.niveau );
		if( score > avant ) { extras_record_set( "thug_records.txt", cle, (float)score ); etat = "\\nNEW BEST SCORE"; }
		else
		{
			static char e[64];
			char b[32];
			nombre( b, sizeof( b ), (int)avant );
			snprintf( e, sizeof( e ), "\\nbest %s", b );
			etat = e;
		}
	}
	snprintf( t, sizeof( t ), "TIME!\\n%s points, %d of %d goals%s", s, n, CL_NB, etat );
	panneau( "desktop_classic_end", t );
	son( "GoalFail" );
	for( int i = 0; i < NB_OBJ; ++i ) if( s_cl.place[i] ) { cl_obj_tue( i ); s_cl.place[i] = false; }
	s_cl.actif = false;
	s_cl.fin_t = 6.0f;
}

static void cl_maj( Obj::CSkater *sk, float dt )
{
	Mdl::Score *sc = sk->GetScoreObject();
	const bool en_combo = sc && sc->GetScorePotValue() > 0;
	s_cl.t += dt;
	if( sc )
	{
		const int tot = sc->GetTotalScore();
		for( int i = 0; i < 3; ++i ) if( tot >= s_cl_scores[i] ) cl_gagne( i, s_cl_noms[i] );
		const int pot = sc->GetScorePotValue();
		if( pot >= s_cl_scores[CL_COMBO_SCORE] ) cl_gagne( CL_COMBO_SCORE, s_cl_noms[CL_COMBO_SCORE] );
	}

	// Pick-ups: within 75 units of the skater's middle.
	Mth::Vector corps = sk->GetPos() + vec( 0, 36, 0 );
	for( int i = 0; i < NB_OBJ; ++i )
	{
		if( !s_cl.place[i] || s_cl.pris[i] ) continue;
		if( dist( corps, s_cl.pos[i] ) > 75.0f ) continue;
		if( i >= 5 && i < 10 && !en_combo ) continue;	// COMBO letters only count in a combo
		s_cl.pris[i] = true;
		cl_obj_tue( i );
		s_cl.place[i] = false;
		son( "goaldone" );
		if( i < 5 )
		{
			bool tout = true;
			for( int k = 0; k < 5; ++k ) if( !s_cl.pris[k] ) tout = false;
			if( tout ) cl_gagne( CL_SKATE, "S-K-A-T-E" );
		}
		else if( i < 10 )
		{
			++s_cl.combo_pris;
			if( s_cl.combo_pris == 5 ) cl_gagne( CL_COMBO, "C-O-M-B-O" );
		}
		else
			cl_gagne( CL_TAPE, "Secret Tape" );
	}
	// The combo ended before all of C-O-M-B-O: they come back.
	if( !en_combo && s_cl.combo_pris > 0 && !s_cl.fait[CL_COMBO] )
	{
		s_cl.combo_pris = 0;
		for( int k = 5; k < 10; ++k )
			if( s_cl.pris[k] ) { s_cl.pris[k] = false; cl_obj_cree( k ); s_cl.place[k] = true; }
		panneau( "desktop_classic_msg", "C-O-M-B-O lost: get them all in one combo" );
	}
	cl_hud_maj( sk );
	// Time: a combo still going when it runs out gets to land.
	if( s_cl.t >= CL_DUREE && !en_combo ) cl_fin( sk );
}

// --- goal attack timer --------------------------------------------------------------
//
// In story mode: a clock from when it's turned on, a split for every goal
// won (time since the previous one) compared with that goal's best.

static float s_ga_t = 0.0f, s_ga_split = 0.0f;
static int   s_ga_n = 0;
static bool  s_ga_actif = false, s_ga_aide = false;

static void ga_arret( void )
{
	if( s_ga_actif ) Script::RunScript( "desktop_ga_hud_off" );
	s_ga_actif = false;
}

static void ga_maj( float dt, bool pause )
{
	if( !s_ga_actif )
	{
		s_ga_actif = true;
		s_ga_t = s_ga_split = 0.0f;
		s_ga_n = 0;
		s_ga_aide = aide();
		Script::RunScript( "desktop_ga_hud_create" );
	}
	if( !pause ) { s_ga_t += dt; s_ga_split += dt; }
	if( aide()) s_ga_aide = true;
	char t[64], c[32];
	temps( c, sizeof( c ), s_ga_t );
	snprintf( t, sizeof( t ), "%s  -  %d goal%s%s", c, s_ga_n, s_ga_n == 1 ? "" : "s", s_ga_aide ? " (practice)" : "" );
	texte( "desktop_ga_time", t );
}

// --- practice -------------------------------------------------------------------------

static bool s_slomo_mis = false;
static char s_lecture[700] = "";

static void pr_maj( bool pause, bool libre )
{
	const bool lent = extras_get( EXTRA_SLOWMO ) && !pause;
	if( lent ) { Tmr::SetSlomo( 0.5f ); s_slomo_mis = true; }
	else if( s_slomo_mis ) { Tmr::SetSlomo( 1.0f ); s_slomo_mis = false; }
	if( extras_get( EXTRA_READOUT ) && !pause )
	{
		if( !element( crc( "desktop_readout" )))
		{
			Script::RunScript( "desktop_readout_create" );
			if( s_lecture[0] ) texte_bloc( "desktop_readout_text", s_lecture );
		}
	}
	else if( element( crc( "desktop_readout" )))
		Script::RunScript( "desktop_readout_off" );
	(void)libre;
}

} // namespace

// CSkaterCareer::GetCheat (through desktop_chaos_cheat): practice's perfect balance.
bool desktop_practice_cheat( uint32 cheat )
{
	return extras_get( EXTRA_PERFECT ) && ( cheat == crc( "CHEAT_PERFECT_RAIL" ) || cheat == crc( "CHEAT_PERFECT_MANUAL" ));
}

// Score::Land / Bail: the combo's tricks for the practice readout.
void desktop_combo_detail( int score, int base, int mult, int n, const char *tricks, bool bail )
{
	if( n <= 0 ) return;
	char s[32], b[32];
	nombre( s, sizeof( s ), score );
	nombre( b, sizeof( b ), base );
	if( bail )
		snprintf( s_lecture, sizeof( s_lecture ), "\\c2BAILED\\c0  %s x %d, %d trick%s\\n%s", b, mult, n, n == 1 ? "" : "s", tricks );
	else
		snprintf( s_lecture, sizeof( s_lecture ), "\\c4%s\\c0  = %s x %d, %d trick%s\\n%s", s, b, mult, n, n == 1 ? "" : "s", tricks );
	if( extras_get( EXTRA_READOUT )) texte_bloc( "desktop_readout_text", s_lecture );
}

// CGoal::Win: a goal won (goal attack splits).
void desktop_goal_won( uint32 goal, const char *nom )
{
	if( !s_ga_actif ) return;
	++s_ga_n;
	char cle[48], t[200], c[32], d[48] = "";
	snprintf( cle, sizeof( cle ), "ga_%08x", goal );
	const float best = extras_record_get( "thug_records.txt", cle, 0.0f );
	temps( c, sizeof( c ), s_ga_split );
	if( best > 0.0f )
	{
		const float e = s_ga_split - best;
		snprintf( d, sizeof( d ), "  %s%c%.1fs\\c0", e < 0 ? "\\c4" : "\\c2", e < 0 ? '-' : '+', fabsf( e ));
	}
	if( !s_ga_aide && ( best <= 0.0f || s_ga_split < best ))
		extras_record_set( "thug_records.txt", cle, s_ga_split );
	snprintf( t, sizeof( t ), "%s\\n%s%s%s", nom && nom[0] ? nom : "Goal", c, d, best <= 0.0f ? "  (first)" : "" );
	panneau( "desktop_ga_msg", t );
	s_ga_split = 0.0f;
}

namespace Mdl
{
void DesktopModesUpdate( Skate &mdl )
{
	Obj::CSkater *sk = mdl.GetLocalSkater();
	Game::CGameMode *mode = mdl.GetGameMode();
	const bool jeu = sk && mode && !mode->IsFrontEnd();
	const bool pause = Mdl::FrontEnd::Instance()->GamePaused();
	if( !jeu )
	{
		photo_fin( sk, false );
		if( s_cl.actif || s_cl.demande ) cl_arret();
		ga_arret();
		if( s_slomo_mis ) { Tmr::SetSlomo( 1.0f ); s_slomo_mis = false; }
		return;
	}
	const float dt = Tmr::FrameLength();

	// Photo mode: only while paused.
	if( s_ph.actif )
	{
		if( pause ) photo_maj( sk );
		else photo_fin( sk, false );
	}

	// Classic: free skate only, not Skater Life; a level change ends it.
	const bool libre = mode_libre( mode ) && !life_on();
	if(( s_cl.actif || s_cl.demande ) && ( !libre || mdl.m_cur_level != s_cl.niveau ))
	{
		if( s_cl.actif && mdl.m_cur_level != s_cl.niveau ) { for( int i = 0; i < NB_OBJ; ++i ) s_cl.place[i] = false; }
		cl_arret();
	}
	if( s_cl.demande && !pause && libre ) { s_cl.niveau = mdl.m_cur_level; cl_debut( sk ); }
	if( s_cl.actif && !pause ) cl_maj( sk, dt );
	if( !s_cl.actif && s_cl.fin_t > 0.0f && !pause )
	{
		s_cl.fin_t -= dt;
		if( s_cl.fin_t <= 0.0f ) Script::RunScript( "desktop_classic_hud_off" );
	}

	// Goal attack: story mode.
	if( extras_get( EXTRA_GOAL_ATTACK ) && mode->IsTrue( CRCD( 0x1ded1ea4, "is_career" ))) ga_maj( dt, pause );
	else ga_arret();

	pr_maj( pause, libre );
}
}

namespace CFuncs
{

// @script | DesktopPhoto | desktop: on starts photo mode (game paused, menu hidden), off ends it
bool ScriptDesktopPhoto( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( pParams->ContainsFlag( Script::GenerateCRC( "on" ))) photo_debut();
	else photo_fin( Mdl::Skate::Instance()->GetLocalSkater(), false );
	return true;
}

// @script | DesktopClassic | desktop: start asks for a classic run (begins on unpause),
// stop ends it; false when it can't run here (adds reason)
bool ScriptDesktopClassic( Script::CStruct *pParams, Script::CScript *pScript )
{
	Mdl::Skate *mdl = Mdl::Skate::Instance();
	if( pParams->ContainsFlag( Script::GenerateCRC( "stop" )))
	{
		cl_arret();
		return true;
	}
	if( !mode_libre( mdl->GetGameMode()) || life_on())
	{
		pScript->GetParams()->AddString( "reason", "Classic runs are for Free Skate" );
		return false;
	}
	if( s_cl.actif ) cl_arret();
	s_cl.demande = true;
	return true;
}

// @script | DesktopClassicOn | desktop: true while a classic run is going
bool ScriptDesktopClassicOn( Script::CStruct *pParams, Script::CScript *pScript )
{
	return s_cl.actif || s_cl.demande;
}

} // namespace CFuncs

#endif // THUG_DESKTOP
