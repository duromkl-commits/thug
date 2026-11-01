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
#include <math.h>
#include <gel/object/compositeobject.h>
#include <gel/object/compositeobjectmanager.h>
#include <gel/components/pedlogiccomponent.h>
#include <sk/components/skaterphysicscontrolcomponent.h>
#include <sk/engine/feeler.h>
#include <gfx/camera.h>
#include <gfx/nxviewman.h>
#include <gfx/nxviewport.h>
#include <gfx/nxmodel.h>
#include <gfx/2D/Element3d.h>
#include <gel/components/modelcomponent.h>
#include <gel/scripting/array.h>
#include <gel/scripting/symboltype.h>

#include "skater_life.h"

namespace NxVita { extern float g_vita_world_zoom; }
using NxVita::g_vita_world_zoom;

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


// --- Sims view (live mode) ---------------------------------------------------------
//
// SELECT switches between skating and the Sims view: an overhead camera, a
// cursor (left stick or mouse), click-to-walk and pie menus on yourself and on
// peds. While it is on the game itself only sees Start (life_input_capture).


static const float PIE_RAYON_X = 92.0f, PIE_RAYON_Y = 70.0f;
enum { CIBLE_RIEN = 0, CIBLE_MOI = 1, CIBLE_PED = 2 };

struct PieItem { const char *label; const char *a; };

static const PieItem s_pie_moi[] = {
	{ "Eat Snack", "snack" }, { "Eat Meal", "meal" }, { "Drink Coffee", "coffee" }, { "Shower", "shower" },
	{ "Use Toilet", "toilet" }, { "Sleep", "sleep" }, { "Change Station", "radio" }, { "Skate", "skate" } };
static const PieItem s_pie_ped[] = {
	{ "Talk", "talk" }, { "Tell Joke", "joke" }, { "Compliment", "compliment" }, { "Brag", "brag" },
	{ "Insult", "insult" }, { "Flirt", "flirt" }, { "Ask for Money", "beg" }, { "Slap", "slap" } };

// Skater animation for each social, and the ped's answer (good, bad).
struct AnimSociale { const char *kind, *anim, *bien, *mal; };
static const AnimSociale s_anims[] = {
	{ "talk",       "Ped_M_Talk2_KnowWhatIMean", "Ped_M_Talk2_SureThing", "Ped_M_Talk2_Maybe" },
	{ "joke",       "Ped_M_Talk2_RoundAndRound", "Ped_M_Clap",            "Ped_M_Disgust" },
	{ "compliment", "Ped_M_ThumbUp",             "Ped_M_Cheer1",          "Ped_M_Disgust" },
	{ "brag",       "Ped_M_Cheer1",              "Ped_M_Clap",            "Ped_M_Disgust" },
	{ "insult",     "Ped_M_UpYours",             "Ped_M_ShakeFist",       "Ped_M_UpYours" },
	{ "flirt",      "Ped_M_PlayerWave",          "Ped_M_PlayerWave",      "Ped_M_Disgust" },
	{ "beg",        "Ped_M_Talk2_Maybe",         "Ped_M_ThumbUp",         "Ped_M_ShakeFist" },
	{ "slap",       "WalkingSlap",               "Ped_M_ShakeFist",       "Ped_M_ShakeFist" } };

static const char *anim_moi( const char *a )
{
	if( !strcmp( a, "coffee" )) return "Ped_M_LookAtWatch";
	if( !strcmp( a, "toilet" )) return "Ped_M_LookAround";
	if( !strcmp( a, "sleep" ))  return "Ped_M_Cheer_SitFromStand";
	return NULL;
}

struct Live
{
	bool actif;
	bool cree;
	unsigned prec;			// pad buttons last frame
	int souris_prec;		// mouse buttons last frame
	float souris_x, souris_y;
	float cx, cy;			// cursor, 640x480
	float lacet, dist;		// camera yaw and distance
	Mth::Vector pan;
	float marche_essai;		// seconds until the next try at getting off the board

	// walking somewhere
	bool va;
	Mth::Vector but;
	uint32 ped_but;			// walking to a ped (0: to a spot)
	int action_but;			// s_pie_ped index to do on arrival, -1 none
	float bloque_t;
	Mth::Vector bloque_pos;

	// pie menu
	bool pie;
	int pie_genre;			// CIBLE_MOI / CIBLE_PED
	uint32 pie_ped;
	float px, py;
	int sel;
	bool attente;			// a self action is running
};
static Live s_l = { false, false, 0, 0, -1.0f, -1.0f, 320.0f, 240.0f, 0.0f, 1100.0f, Mth::Vector( 0, 0, 0 ), 0.0f,
					false, Mth::Vector( 0, 0, 0 ), 0, -1, 0.0f, Mth::Vector( 0, 0, 0 ), false, 0, 0, 0, 0, -1, false };

static uint32 crc( const char *t ) { return Script::GenerateCRC( t ); }

static void lance( const char *script, Script::CStruct *p = NULL )
{
	Script::SpawnScript( script, p );
}

static void son( const char *nom )
{
	Script::CStruct p;
	p.AddChecksum( "s", crc( nom ));
	lance( "desktop_life_snd", &p );
}

static void montre( uint32 id, float a )
{
	Front::CScreenElement *p = element( id );
	if( p ) p->SetAlpha( a, Front::CScreenElement::FORCE_INSTANT );
}

static void place( uint32 id, float x, float y )
{
	Front::CScreenElement *p = element( id );
	if( p ) p->SetPos( x, y, Front::CScreenElement::FORCE_INSTANT );
}

static bool marche( Obj::CCompositeObject *sk )
{
	Obj::CSkaterPhysicsControlComponent *c = GetSkaterPhysicsControlComponentFromObject( sk );
	return c && c->IsWalking();
}

// Screen position (640x480) of a world point under the active camera.
static bool projette( const Mth::Vector &w, float *x, float *y )
{
	Gfx::Camera *cam = Nx::CViewportManager::sGetActiveCamera( 0 );
	Nx::CViewport *vp = Nx::CViewportManager::sGetActiveViewport( 0 );
	if( !cam ) return false;
	Mth::Matrix &m = cam->GetMatrix();
	Mth::Vector d = w - cam->GetPos();
	const float prof = -Mth::DotProduct( d, m[Z] );
	if( prof < 10.0f ) return false;
	float hfov = cam->GetAdjustedHFOV();
	if( hfov < 1.0f || hfov > 179.0f ) hfov = 72.0f;
	float aspect = vp ? vp->GetAspectRatio() : 4.0f / 3.0f;
	if( aspect < 0.01f ) aspect = 4.0f / 3.0f;
	const float zoom = g_vita_world_zoom > 0.01f ? g_vita_world_zoom : 1.0f;
	const float th = tanf( hfov * 0.5f * 3.14159265f / 180.0f ) / zoom, tv = th / aspect;
	*x = 320.0f + ( Mth::DotProduct( d, m[X] ) / prof ) / th * 320.0f;
	*y = 240.0f - ( Mth::DotProduct( d, m[Y] ) / prof ) / tv * 240.0f;
	return true;
}

// The ground point under the cursor.
static bool sol_sous( float sx, float sy, Mth::Vector *out )
{
	Gfx::Camera *cam = Nx::CViewportManager::sGetActiveCamera( 0 );
	Nx::CViewport *vp = Nx::CViewportManager::sGetActiveViewport( 0 );
	if( !cam ) return false;
	Mth::Matrix &m = cam->GetMatrix();
	float hfov = cam->GetAdjustedHFOV();
	if( hfov < 1.0f || hfov > 179.0f ) hfov = 72.0f;
	float aspect = vp ? vp->GetAspectRatio() : 4.0f / 3.0f;
	if( aspect < 0.01f ) aspect = 4.0f / 3.0f;
	const float zoom = g_vita_world_zoom > 0.01f ? g_vita_world_zoom : 1.0f;
	const float th = tanf( hfov * 0.5f * 3.14159265f / 180.0f ) / zoom, tv = th / aspect;
	Mth::Vector dir = -m[Z] + m[X] * (( sx - 320.0f ) / 320.0f * th ) + m[Y] * (( 240.0f - sy ) / 240.0f * tv );
	dir[W] = 0.0f;
	dir.Normalize();
	Mth::Vector a = cam->GetPos(), b = a + dir * 10000.0f;
	a[W] = b[W] = 1.0f;
	CFeeler f( a, b );
	if( !f.GetCollision()) return false;
	*out = f.GetPoint();
	return true;
}

// What the cursor is over: you or the nearest ped (within 30 px).
static int cible_sous( Obj::CCompositeObject *sk, uint32 *ped )
{
	float best = 30.0f * 30.0f;
	int r = CIBLE_RIEN;
	float x, y;
	if( projette( sk->GetPos() + Mth::Vector( 0, 40, 0 ), &x, &y ))
	{
		const float d = ( x - s_l.cx ) * ( x - s_l.cx ) + ( y - s_l.cy ) * ( y - s_l.cy );
		if( d < best ) { best = d; r = CIBLE_MOI; }
	}
	Obj::CPedLogicComponent *c = static_cast<Obj::CPedLogicComponent *>( Obj::CCompositeObjectManager::Instance()->GetFirstComponentByType( CRC_PEDLOGIC ));
	for( ; c; c = static_cast<Obj::CPedLogicComponent *>( c->GetNextSameType()))
	{
		Obj::CCompositeObject *o = c->GetObject();
		if( !o || o->IsHidden() || o->IsSuspended()) continue;
		if( !projette( o->GetPos() + Mth::Vector( 0, 36, 0 ), &x, &y )) continue;
		const float d = ( x - s_l.cx ) * ( x - s_l.cx ) + ( y - s_l.cy ) * ( y - s_l.cy );
		if( d < best ) { best = d; r = CIBLE_PED; *ped = o->GetID(); }
	}
	return r;
}

static Obj::CCompositeObject *ped_par_id( uint32 id )
{
	Obj::CPedLogicComponent *c = static_cast<Obj::CPedLogicComponent *>( Obj::CCompositeObjectManager::Instance()->GetFirstComponentByType( CRC_PEDLOGIC ));
	for( ; c; c = static_cast<Obj::CPedLogicComponent *>( c->GetNextSameType()))
		if( c->GetObject() && c->GetObject()->GetID() == id ) return c->GetObject();
	return NULL;
}

static int nb_pie( void ) { return 8; }
static const PieItem &item_pie( int i ) { return s_l.pie_genre == CIBLE_MOI ? s_pie_moi[i] : s_pie_ped[i]; }

// The Sims pie menu shows your Sim's head in the middle, looking at the
// option under the cursor. Here it is your skater's own head: the head,
// hair, hat and glasses geoms cloned off the skater's model.
static const char *s_geoms_tete[] = {
	"skater_m_head", "skater_f_head", "ped_m_head", "ped_f_head", "skater_m_jaw",
	"skater_m_hair", "skater_f_hair", "hat", "helmet", "glasses" };

static bool s_tete = false;
static float s_tete_ax = 0.0f, s_tete_ay = 0.0f;

static void cree_tete( Obj::CCompositeObject *sk, float x, float y )
{
	s_tete = false;
	s_tete_ax = s_tete_ay = 0.0f;
	Obj::CModelComponent *mc = GetModelComponentFromObject( sk );
	if( !mc || !mc->GetModel()) return;
	Nx::CModel *m = mc->GetModel();
	uint32 trouve[16];
	int n = 0;
	for( unsigned k = 0; k < sizeof( s_geoms_tete ) / sizeof( s_geoms_tete[0] ); ++k )
	{
		const uint32 g = crc( s_geoms_tete[k] );
		for( int i = 0; i < m->GetNumGeoms() && n < 16; ++i )
			if( m->GetGeomNameByIndex( i ) == g && m->GetGeom( g )) { trouve[n++] = g; break; }
	}
	// No head part (a single-mesh skater): no head, just the name.
	if( !n || ( trouve[0] != crc( "skater_m_head" ) && trouve[0] != crc( "skater_f_head" ) &&
				trouve[0] != crc( "ped_m_head" ) && trouve[0] != crc( "ped_f_head" )))
		return;
	Script::CArray *a = new Script::CArray;
	a->SetSizeAndType( n, ESYMBOLTYPE_NAME );
	for( int i = 0; i < n; ++i ) a->SetChecksum( i, trouve[i] );
	Script::CStruct p;
	p.AddChecksum( "obj", sk->GetID());
	p.AddArrayPointer( "geoms", a );	// the struct owns it now
	p.AddPair( "pos", x, y );
	Script::RunScript( "desktop_life_head_create", &p );
	s_tete = element( crc( "desktop_life_head" )) != NULL;
}

static void detruit_tete( void )
{
	if( element( crc( "desktop_life_head" ))) Script::RunScript( "desktop_life_head_off" );
	s_tete = false;
}

// Turns the head toward (dx, dy), -1..1 from the pie's centre, eased.
static void tourne_tete( float dx, float dy, float dt )
{
	Front::CScreenElement *e = element( crc( "desktop_life_head" ));
	if( !e ) return;
	const float vy = dx * 0.7f, vx = dy * 0.45f;
	const float k = dt * 8.0f > 1.0f ? 1.0f : dt * 8.0f;
	s_tete_ay += ( vy - s_tete_ay ) * k;
	s_tete_ax += ( vx - s_tete_ax ) * k;
	((Front::CElement3d *)e )->DesktopSetAngles( s_tete_ax, s_tete_ay, 0.0f );
}

static void ferme_pie( void )
{
	s_l.pie = false;
	s_l.sel = -1;
	char t[40];
	for( int i = 0; i < 8; ++i )
	{
		snprintf( t, sizeof( t ), "desktop_life_pie%d", i );    montre( crc( t ), 0.0f );
		snprintf( t, sizeof( t ), "desktop_life_pie_bg%d", i ); montre( crc( t ), 0.0f );
	}
	montre( crc( "desktop_life_pie_title" ), 0.0f );
	detruit_tete();
}

static void ouvre_pie( int genre, uint32 ped, Obj::CCompositeObject *sk )
{
	s_l.pie = true;
	s_l.pie_genre = genre;
	s_l.pie_ped = ped;
	s_l.sel = -1;
	s_l.px = s_l.cx < 110.0f ? 110.0f : ( s_l.cx > 530.0f ? 530.0f : s_l.cx );
	s_l.py = s_l.cy < 95.0f ? 95.0f : ( s_l.cy > 400.0f ? 400.0f : s_l.cy );
	char t[40];
	for( int i = 0; i < nb_pie(); ++i )
	{
		const float a = -1.5707963f + i * 6.2831853f / nb_pie();
		const float x = s_l.px + cosf( a ) * PIE_RAYON_X, y = s_l.py + sinf( a ) * PIE_RAYON_Y;
		snprintf( t, sizeof( t ), "desktop_life_pie%d", i );
		Front::CTextElement *e = (Front::CTextElement *)element( crc( t ));
		if( e ) { e->SetText( item_pie( i ).label ); e->SetPos( x, y, Front::CScreenElement::FORCE_INSTANT ); e->SetAlpha( 1.0f, Front::CScreenElement::FORCE_INSTANT ); }
		snprintf( t, sizeof( t ), "desktop_life_pie_bg%d", i );
		place( crc( t ), x, y );
		montre( crc( t ), 0.8f );
	}
	cree_tete( sk, s_l.px, s_l.py );
	// With the head in the middle, the name goes under it.
	Front::CTextElement *ti = (Front::CTextElement *)element( crc( "desktop_life_pie_title" ));
	if( ti ) { ti->SetText( genre == CIBLE_MOI ? "You" : "Local" ); ti->SetPos( s_l.px, s_l.py + ( s_tete ? 34.0f : 0.0f ), Front::CScreenElement::FORCE_INSTANT ); ti->SetAlpha( 1.0f, Front::CScreenElement::FORCE_INSTANT ); }
	son( "UI_PIEMENU_APPEAR" );
}

static void sel_pie( void )
{
	const float dx = ( s_l.cx - s_l.px ) / PIE_RAYON_X, dy = ( s_l.cy - s_l.py ) / PIE_RAYON_Y;
	int sel = -1;
	if( dx * dx + dy * dy > 0.25f * 0.25f )
	{
		float a = atan2f( dy, dx ) + 1.5707963f;	// 0 at the top, clockwise
		if( a < 0.0f ) a += 6.2831853f;
		sel = (int)floorf( a / ( 6.2831853f / nb_pie()) + 0.5f ) % nb_pie();
	}
	if( sel == s_l.sel ) return;
	char t[40];
	for( int i = 0; i < nb_pie(); ++i )
	{
		snprintf( t, sizeof( t ), "desktop_life_pie_bg%d", i );
		Front::CScreenElement *b = element( crc( t ));
		if( b ) b->SetRGBA( i == sel ? Image::RGBA( 40, 90, 128, 120 ) : Image::RGBA( 10, 30, 60, 100 ));
		snprintf( t, sizeof( t ), "desktop_life_pie%d", i );
		Front::CScreenElement *e = element( crc( t ));
		if( e ) e->SetRGBA( i == sel ? Image::RGBA( 128, 128, 40, 128 ) : Image::RGBA( 128, 128, 128, 128 ));
	}
	if( sel >= 0 ) son( "UI_PIEMENU_HIGHLIGHT" );
	s_l.sel = sel;
}

static void arrete_marche( void )
{
	s_l.va = false;
	s_l.ped_but = 0;
	s_l.action_but = -1;
	montre( crc( "desktop_life_marker" ), 0.0f );
}

static void va_vers( const Mth::Vector &p, uint32 ped, int action, Obj::CCompositeObject *sk )
{
	s_l.va = true;
	s_l.but = p;
	s_l.ped_but = ped;
	s_l.action_but = action;
	s_l.bloque_t = 0.0f;
	s_l.bloque_pos = sk->GetPos();
}

static void sociale( int i, Obj::CCompositeObject *ped )
{
	const char *kind = s_pie_ped[i].a;
	char texte[96], dossier[32], voix[128];
	const int ok = life_social( kind, texte, sizeof( texte ), dossier, sizeof( dossier ));
	const AnimSociale *an = &s_anims[0];
	for( unsigned k = 0; k < sizeof( s_anims ) / sizeof( s_anims[0] ); ++k )
		if( !strcmp( s_anims[k].kind, kind )) an = &s_anims[k];
	Script::CStruct p;
	p.AddChecksum( "anim", crc( an->anim ));
	p.AddChecksum( "ped_anim", crc( ok ? an->bien : an->mal ));
	p.AddString( "text", texte );
	if( ped ) p.AddChecksum( "ped", ped->GetID());
	if( dossier[0] && life_voice( dossier, voix, sizeof( voix )))
	{
		p.AddString( "voice", voix );
		const char *base = strrchr( voix, '\\' );
		p.AddChecksum( "voice_sound", crc( base ? base + 1 : voix ));
	}
	lance( "desktop_life_social", &p );
}

static void action_moi( int i, Obj::CCompositeObject *sk );

static void sort_live( void )
{
	if( !s_l.actif ) return;
	s_l.actif = false;
	life_input_capture( 0 );
	ferme_pie();
	arrete_marche();
	if( element( crc( "desktop_life_live" ))) Script::RunScript( "desktop_life_live_off" );
}

static void entre_live( Obj::CCompositeObject *sk )
{
	s_l.actif = true;
	s_l.pan.Set( 0, 0, 0 );
	s_l.cx = 320.0f; s_l.cy = 240.0f;
	s_l.marche_essai = 0.0f;
	s_l.pie = false;
	s_l.sel = -1;
	arrete_marche();
	// Start with the camera behind the skater, like the game's.
	Gfx::Camera *cam = Nx::CViewportManager::sGetActiveCamera( 0 );
	if( cam ) s_l.lacet = atan2f( cam->GetMatrix()[Z][X], cam->GetMatrix()[Z][Z] );
	life_input_capture( 1 );
	if( !element( crc( "desktop_life_live" ))) Script::RunScript( "desktop_life_live_create" );
	ferme_pie();
	montre( crc( "desktop_life_marker" ), 0.0f );
	son( "UI_WHOOSH" );
}

static void action_moi( int i, Obj::CCompositeObject *sk )
{
	const char *a = s_pie_moi[i].a;
	if( !strcmp( a, "skate" ))
	{
		sort_live();
		return;
	}
	Script::CStruct p;
	p.AddChecksum( "a", crc( a ));
	const char *an = anim_moi( a );
	if( an ) p.AddChecksum( "anim", crc( an ));
	lance( "desktop_life_self", &p );
}

static void live_update( Obj::CCompositeObject *sk, bool pause )
{
	float lx, ly, rx, ry;
	const unsigned b = life_pad_raw( &lx, &ly, &rx, &ry );
	const unsigned appui = b & ~s_l.prec;
	s_l.prec = b;
	float mx, my;
	int molette = 0;
	const int sb = life_mouse( &mx, &my, &molette );
	const int clic = sb & ~s_l.souris_prec;
	s_l.souris_prec = sb;

	if( pause )
	{
		// The pause menu needs the pad.
		if( s_l.actif ) life_input_capture( 0 );
		return;
	}

	if( appui & 0x1 )	// SELECT
	{
		if( s_l.actif ) sort_live();
		else entre_live( sk );
		return;
	}
	if( !s_l.actif ) return;
	life_input_capture( 1 );
	if( !element( crc( "desktop_life_live" )))
	{
		Script::RunScript( "desktop_life_live_create" );
		ferme_pie();
	}

	const float dt = Tmr::FrameLength();

	// Off the board: the Sims view walks.
	if( !marche( sk ))
	{
		s_l.marche_essai -= dt;
		if( s_l.marche_essai <= 0.0f )
		{
			s_l.marche_essai = 1.0f;
			Script::RunScript( "desktop_life_to_walk" );
		}
	}

	// Cursor: stick, or the mouse when it moves.
	if( mx != s_l.souris_x || my != s_l.souris_y )
	{
		if( s_l.souris_x >= 0.0f ) { s_l.cx = mx; s_l.cy = my; }
		s_l.souris_x = mx; s_l.souris_y = my;
	}
	s_l.cx += lx * 420.0f * dt;
	s_l.cy += ly * 420.0f * dt;
	if( s_l.cx < 0 ) s_l.cx = 0; if( s_l.cx > 640 ) s_l.cx = 640;
	if( s_l.cy < 0 ) s_l.cy = 0; if( s_l.cy > 480 ) s_l.cy = 480;

	// Camera: right stick X turns, Y and the wheel zoom; D-pad and the screen edge pan.
	s_l.lacet += rx * 2.2f * dt;
	s_l.dist *= 1.0f + ry * 1.2f * dt;
	s_l.dist *= powf( 0.88f, (float)molette );
	if( s_l.dist < 500.0f ) s_l.dist = 500.0f;
	if( s_l.dist > 2600.0f ) s_l.dist = 2600.0f;
	float px = 0.0f, pz = 0.0f;
	if( b & 0x80 ) px -= 1.0f;
	if( b & 0x20 ) px += 1.0f;
	if( b & 0x10 ) pz += 1.0f;
	if( b & 0x40 ) pz -= 1.0f;
	if( !s_l.pie )
	{
		if( s_l.cx < 8.0f ) px -= 1.0f;
		if( s_l.cx > 632.0f ) px += 1.0f;
		if( s_l.cy < 8.0f ) pz += 1.0f;
		if( s_l.cy > 472.0f ) pz -= 1.0f;
	}
	if( px != 0.0f || pz != 0.0f )
	{
		// right = (cos, 0, -sin), forward = (-sin, 0, -cos) for a camera behind at yaw
		const float v = s_l.dist * 0.9f * dt;
		const float c = cosf( s_l.lacet ), s = sinf( s_l.lacet );
		s_l.pan[X] += ( px * c - pz * s ) * v;
		s_l.pan[Z] += ( -px * s - pz * c ) * v;
	}
	if( appui & 0x8000 ) s_l.pan.Set( 0, 0, 0 );	// SQUARE

	// Markers.
	float x, y;
	if( projette( sk->GetPos() + Mth::Vector( 0, 92, 0 ), &x, &y ))
	{
		place( crc( "desktop_life_plumbob" ), x, y );
		montre( crc( "desktop_life_plumbob" ), 1.0f );
		Front::CScreenElement *pb = element( crc( "desktop_life_plumbob" ));
		if( pb ) pb->SetRGBA( couleur( life_mood()));
	}
	else montre( crc( "desktop_life_plumbob" ), 0.0f );
	if( s_l.va && !s_l.ped_but && projette( s_l.but, &x, &y ))
	{
		place( crc( "desktop_life_marker" ), x, y );
		montre( crc( "desktop_life_marker" ), 1.0f );
	}
	else montre( crc( "desktop_life_marker" ), 0.0f );
	place( crc( "desktop_life_cursor" ), s_l.cx, s_l.cy );
	place( crc( "desktop_life_cursor_bg" ), s_l.cx, s_l.cy );

	uint32 ped = 0;
	const int sous = s_l.pie ? CIBLE_RIEN : cible_sous( sk, &ped );
	Front::CScreenElement *cu = element( crc( "desktop_life_cursor" ));
	if( cu ) cu->SetRGBA( sous != CIBLE_RIEN ? Image::RGBA( 40, 128, 40, 128 ) : Image::RGBA( 128, 128, 128, 128 ));

	const bool ok = ( appui & 0x4000 ) || ( clic & 1 );					// X, left click
	const bool annule = ( appui & ( 0x2000 | 0x1000 )) || ( clic & 2 );	// O, triangle, right click

	if( s_l.pie )
	{
		sel_pie();
		{
			float hx = ( s_l.cx - s_l.px ) / PIE_RAYON_X, hy = ( s_l.cy - s_l.py ) / PIE_RAYON_Y;
			if( hx > 1.0f ) hx = 1.0f; if( hx < -1.0f ) hx = -1.0f;
			if( hy > 1.0f ) hy = 1.0f; if( hy < -1.0f ) hy = -1.0f;
			tourne_tete( hx, hy, dt );
		}
		if( annule || ( ok && s_l.sel < 0 ))
		{
			ferme_pie();
		}
		else if( ok )
		{
			const int i = s_l.sel;
			const int genre = s_l.pie_genre;
			const uint32 pid = s_l.pie_ped;
			ferme_pie();
			son( "UI_PIEMENU_SELECT" );
			if( genre == CIBLE_MOI ) action_moi( i, sk );
			else
			{
				Obj::CCompositeObject *o = ped_par_id( pid );
				if( o ) va_vers( o->GetPos(), pid, i, sk );
			}
		}
	}
	else if( annule && s_l.va )
	{
		arrete_marche();
		son( "UI_QUEUE_DELETE" );
	}
	else if( ok )
	{
		if( sous != CIBLE_RIEN ) ouvre_pie( sous, ped, sk );
		else
		{
			Mth::Vector p;
			if( sol_sous( s_l.cx, s_l.cy, &p ))
			{
				va_vers( p, 0, -1, sk );
				son( "UI_QUEUED" );
			}
			else son( "UI_ERROR" );
		}
	}

	// Walking somewhere.
	if( s_l.va )
	{
		Obj::CCompositeObject *o = s_l.ped_but ? ped_par_id( s_l.ped_but ) : NULL;
		if( s_l.ped_but && !o ) arrete_marche();
		else
		{
			if( o ) s_l.but = o->GetPos();
			Mth::Vector d = s_l.but - sk->GetPos();
			d[Y] = 0.0f;
			if( d.Length() < ( o ? 70.0f : 30.0f ))
			{
				const int act = s_l.action_but;
				arrete_marche();
				if( o && act >= 0 ) sociale( act, o );
			}
			else
			{
				s_l.bloque_t += dt;
				if( s_l.bloque_t > 1.5f )
				{
					Mth::Vector m = sk->GetPos() - s_l.bloque_pos;
					m[Y] = 0.0f;
					if( m.Length() < 15.0f && marche( sk ))
					{
						arrete_marche();
						lance( "desktop_life_route_fail" );
					}
					s_l.bloque_t = 0.0f;
					s_l.bloque_pos = sk->GetPos();
				}
			}
		}
	}
}

} // namespace

// --- engine hooks (THUG_DESKTOP) -----------------------------------------------

// WalkCameraComponent: the overhead camera while the Sims view is on.
bool desktop_life_camera( const Mth::Vector &target, Mth::Vector &pos, Mth::Matrix &m )
{
	if( !s_l.actif || !life_on()) return false;
	const float tangage = 0.85f;
	Mth::Vector focus = target + s_l.pan + Mth::Vector( 0.0f, 30.0f, 0.0f );
	focus[W] = 1.0f;
	pos = focus + Mth::Vector( sinf( s_l.lacet ) * cosf( tangage ) * s_l.dist, sinf( tangage ) * s_l.dist,
							   cosf( s_l.lacet ) * cosf( tangage ) * s_l.dist );
	pos[W] = 1.0f;
	Mth::Vector z = focus - pos;
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

// WalkComponent::get_controller_input: click-to-walk drives the skater.
bool desktop_life_autopilot( const Mth::Vector &skater_pos, Mth::Vector &dir, float &magnitude )
{
	if( !s_l.actif || !life_on()) return false;
	magnitude = 0.0f;
	if( !s_l.va ) return true;
	Mth::Vector d = s_l.but - skater_pos;
	d[Y] = 0.0f;
	d[W] = 0.0f;
	const float l = d.Length();
	if( l < 1.0f ) return true;
	dir = d / l;
	magnitude = l > 400.0f ? 1.0f : 0.7f;
	return true;
}

namespace
{
} // namespace

namespace Mdl
{

// Each frame, from Skate::s_logic_code.
void DesktopLifeUpdate( Skate &mdl )
{
	if( !life_on())
	{
		detruit_hud();
		sort_live();
		s_radio_lancee = false;
		return;
	}
	Game::CGameMode *p_mode = mdl.GetGameMode();
	if( !p_mode || p_mode->IsFrontEnd())
	{
		detruit_hud();
		sort_live();
		return;
	}
	// Skater Life is a free skate session; any other game mode ends it.
	if( p_mode->GetNameChecksum() != Script::GenerateCRC( "singlesession" ) && p_mode->GetNameChecksum() != Script::GenerateCRC( "freeskate" ))
	{
		detruit_hud();
		sort_live();
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
	live_update( mdl.GetLocalSkater(), Mdl::FrontEnd::Instance()->GamePaused());
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
