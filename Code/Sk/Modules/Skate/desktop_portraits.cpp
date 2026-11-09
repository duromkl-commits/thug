/*****************************************************************************
**  THUG desktop -- skater portraits                                        **
**  Code/Sk/Modules/Skate/desktop_portraits.cpp                             **
**                                                                          **
**  "Build Portraits" (main menu Options) puts every skater on the menu     **
**  stage in turn, aims a narrow camera at the head, grabs the world        **
**  before bloom and before the UI (shim_gl.cpp desktop_portrait_grab),     **
**  and turns it into a select-screen icon: grayscale, contrast, a slight   **
**  lens bulge and a blurred brick backdrop where the depth says the        **
**  skater isn't. Saved to portraits\<name checksum>.raw (+ .png to look    **
**  at); the select screen swaps its SS_<first_name> headshot for the       **
**  portrait when one exists (DesktopPortrait).                             **
*****************************************************************************/

#ifdef THUG_DESKTOP

#include <core/defines.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <map>

#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/checksum.h>
#include <gel/components/skeletoncomponent.h>
#include <gel/components/animationcomponent.h>
#include <gfx/nxviewman.h>
#include <gfx/camera.h>
#include <sk/modules/skate/skate.h>
#include <sk/objects/skater.h>
#include <sk/objects/skaterprofile.h>
#include <sk/objects/playerprofilemanager.h>
#include "vita_log.h"

namespace NxVita
{
extern bool  g_desktop_portrait_cam;
extern float g_desktop_portrait_eye[3], g_desktop_portrait_at[3], g_desktop_portrait_vfov;
extern float g_vita_plan_loin;
}
using NxVita::g_desktop_portrait_cam;
using NxVita::g_desktop_portrait_eye;
using NxVita::g_desktop_portrait_at;
using NxVita::g_desktop_portrait_vfov;
using NxVita::g_vita_plan_loin;
extern "C" void desktop_portrait_request( int w, int h );
extern "C" int  desktop_portrait_result( unsigned char *rgb, float *dep );
extern "C" int  desktop_portrait_save( unsigned int key, const unsigned char *rgba, int w, int h );
extern "C" int  desktop_portrait_load( unsigned int key, unsigned char *rgba, int w, int h );
bool DesktopSpriteFromRGBA( uint32 checksum, const unsigned char *rgba, int pw, int ph, int w, int h );

namespace
{

const int CW = 192, CH = 256;	// grabbed 3:4 crop (the headshot frame is 48 x 64)
const int OW = 96, OH = 128;	// stored pixels, shown as 48 x 64
const int LW = 48, LH = 64;
const float DIST = 70.0f;	// camera to face, inches
const float VFOV = 13.0f;	// ~16 inches of head and neck in frame
const float VFOV_LARGE = 44.0f;	// first look, to find the top of the head
const float SOUS_CRANE = 7.0f;	// aim this far below the top of the head
const float LACET = 12.0f;	// degrees off straight on, like a three-quarter shot
const int ATTENTE = 14;		// frames for the model, its pose and the lights to settle
const int ATTENTE_2 = 3;	// frames after re-aiming

uint32 crc( const char *t ) { return Script::GenerateCRC( t ); }

struct Etat
{
	bool actif;
	std::vector<uint32> liste;
	size_t i;
	int frames;
	int passe;		// 0 wide look, 1 portrait
	bool demande;
	uint32 avant;
	int faits, rates;
	float cible[3], dir[3];
	float os_tete;		// Bone_Head height, -1e9 when the model has none
} s;

std::map<uint32, int> s_charge;		// 1 in the sprite dictionary, -1 no file

uint32 tex_de( uint32 nom )
{
	// The created skater's portrait goes over the game's own ss_cas picture.
	if( nom == crc( "custom" )) return crc( "ss_cas" );
	char t[40];
	snprintf( t, sizeof( t ), "SS_portrait_%08x", (unsigned)nom );
	return crc( t );
}

void lancer( const char *script, uint32 nom = 0, const char *texte = NULL )
{
	Script::CStruct p;
	if( nom ) p.AddChecksum( crc( "name" ), nom );
	if( texte ) p.AddString( crc( "text" ), texte );
	Script::RunScript( crc( script ), &p );
}

// --- the picture -------------------------------------------------------------

float lisse( float a, float b, float x )
{
	float t = ( x - a ) / ( b - a );
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * ( 3.0f - 2.0f * t );
}

void flou( std::vector<float> &img, int n, int r )
{
	std::vector<float> tmp( img.size());
	for( int y = 0; y < n; ++y )
		for( int x = 0; x < n; ++x )
		{
			float s = 0.0f; int k = 0;
			for( int d = -r; d <= r; ++d ) { int xx = x + d; if( xx < 0 || xx >= n ) continue; s += img[y * n + xx]; ++k; }
			tmp[y * n + x] = s / k;
		}
	for( int y = 0; y < n; ++y )
		for( int x = 0; x < n; ++x )
		{
			float s = 0.0f; int k = 0;
			for( int d = -r; d <= r; ++d ) { int yy = y + d; if( yy < 0 || yy >= n ) continue; s += tmp[yy * n + x]; ++k; }
			img[y * n + x] = s / k;
		}
}

uint32 bruit( uint32 a )
{
	a ^= a >> 16; a *= 0x7feb352dU; a ^= a >> 15; a *= 0x846ca68bU; a ^= a >> 16;
	return a;
}

// Blurred brick wall, lit behind the head.
void briques( std::vector<float> &bg, int n )
{
	bg.assign( n * n, 0.0f );
	const int bh = n / 9, bw = n / 4, joint = n / 85 + 1;
	for( int y = 0; y < n; ++y )
	{
		const int rang = y / bh;
		const int dec = ( rang & 1 ) ? bw / 2 : 0;
		for( int x = 0; x < n; ++x )
		{
			const int col = ( x + dec ) / bw;
			const bool mortier = ( y % bh ) < joint || (( x + dec ) % bw ) < joint;
			const float teinte = 0.30f + 0.12f * ( bruit( rang * 131 + col * 7919 ) % 1000 ) / 1000.0f;
			const float grain = 0.04f * (( bruit( y * n + x + 17 ) % 1000 ) / 1000.0f - 0.5f );
			bg[y * n + x] = ( mortier ? 0.17f : teinte ) + grain;
		}
	}
	flou( bg, n, 3 );
	flou( bg, n, 3 );
	for( int y = 0; y < n; ++y )
		for( int x = 0; x < n; ++x )
		{
			const float u = ( x + 0.5f ) / n - 0.5f, v = ( y + 0.5f ) / n - 0.42f;
			bg[y * n + x] *= 0.65f + 0.7f * expf( -( u * u + v * v ) / 0.08f );
		}
}


float lin( float d )
{
	const float n = 4.0f, f = g_vita_plan_loin;
	const float z = d * 2.0f - 1.0f;
	return 2.0f * n * f / (( f + n ) - z * ( f - n ));
}

void masque_de( const float *dep, std::vector<float> &masque )
{
	masque.resize( CW * CH );
	const float seuil = DIST + 16.0f;
	for( int i = 0; i < CW * CH; ++i )
		masque[i] = ( dep[i] < 1.0f && lin( dep[i] ) < seuil ) ? 1.0f : 0.0f;
}

// Brick wall built square, then the middle 3:4 kept.
void briques_34( std::vector<float> &bg )
{
	std::vector<float> carre;
	briques( carre, CH );
	bg.resize( CW * CH );
	const int x0 = ( CH - CW ) / 2;
	for( int y = 0; y < CH; ++y )
		for( int x = 0; x < CW; ++x )
			bg[y * CW + x] = carre[y * CH + x + x0];
}

void flou_rect( std::vector<float> &img, int w, int h, int r )
{
	std::vector<float> tmp( img.size());
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			float t = 0.0f; int k = 0;
			for( int d = -r; d <= r; ++d ) { int xx = x + d; if( xx < 0 || xx >= w ) continue; t += img[y * w + xx]; ++k; }
			tmp[y * w + x] = t / k;
		}
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			float t = 0.0f; int k = 0;
			for( int d = -r; d <= r; ++d ) { int yy = y + d; if( yy < 0 || yy >= h ) continue; t += tmp[yy * w + x]; ++k; }
			img[y * w + x] = t / k;
		}
}

void fabriquer( const unsigned char *rgb, const float *dep, std::vector<unsigned char> &out )
{
	const int np = CW * CH;
	std::vector<float> masque, lum( np );
	masque_de( dep, masque );
	for( int i = 0; i < np; ++i )
		lum[i] = ( 0.299f * rgb[i * 3] + 0.587f * rgb[i * 3 + 1] + 0.114f * rgb[i * 3 + 2] ) / 255.0f;
	flou_rect( masque, CW, CH, 1 );

	// Levels stretched over the skater's own pixels (2nd..98th percentile),
	// then an S curve: every skin and light comes out with the same punch.
	int hist[256] = { 0 }, total = 0;
	for( int i = 0; i < np; ++i )
		if( masque[i] > 0.5f ) { ++hist[(int)( lum[i] * 255.0f )]; ++total; }
	float bas = 0.0f, haut = 1.0f;
	if( total > 64 )
	{
		int acc = 0, b = 0;
		while( b < 255 && acc + hist[b] < total / 50 ) acc += hist[b++];
		bas = b / 255.0f;
		acc = 0; b = 255;
		while( b > 0 && acc + hist[b] < total / 50 ) acc += hist[b--];
		haut = b / 255.0f;
		if( haut - bas < 0.1f ) haut = bas + 0.1f;
	}
	std::vector<float> bg;
	briques_34( bg );
	std::vector<float> img( np );
	for( int i = 0; i < np; ++i )
	{
		float l = ( lum[i] - bas ) / ( haut - bas );
		l = l < 0.0f ? 0.0f : l > 1.0f ? 1.0f : l;
		l = 0.06f + 0.9f * ( l * 0.4f + lisse( 0.0f, 1.0f, l ) * 0.6f );
		img[i] = masque[i] * l + ( 1.0f - masque[i] ) * bg[i];
	}

	// Lens bulge (centre magnified, edges where they were), vignette,
	// then down to OW x OH with a 2x2 box.
	const int mw = OW * 2, mh = OH * 2;
	std::vector<float> fin( mw * mh );
	const float k = 0.16f, asp = (float)CW / CH;
	for( int y = 0; y < mh; ++y )
		for( int x = 0; x < mw; ++x )
		{
			const float u = ( x + 0.5f ) / mw * 2.0f - 1.0f, v = ( y + 0.5f ) / mh * 2.0f - 1.0f;
			const float r2 = ( u * u * asp * asp + v * v ) * 0.5f;
			const float e = ( 1.0f - k ) + k * r2;
			float sx = ( u * e * 0.5f + 0.5f ) * CW - 0.5f, sy = ( v * e * 0.5f + 0.5f ) * CH - 0.5f;
			sx = sx < 0.0f ? 0.0f : sx > CW - 1.001f ? CW - 1.001f : sx;
			sy = sy < 0.0f ? 0.0f : sy > CH - 1.001f ? CH - 1.001f : sy;
			const int ix = (int)sx, iy = (int)sy;
			const float fx = sx - ix, fy = sy - iy;
			const float a = img[iy * CW + ix] * ( 1 - fx ) + img[iy * CW + ix + 1] * fx;
			const float b = img[( iy + 1 ) * CW + ix] * ( 1 - fx ) + img[( iy + 1 ) * CW + ix + 1] * fx;
			float c = a * ( 1 - fy ) + b * fy;
			const float ru = u * u, rv = v * v;
			c *= 1.0f - 0.45f * lisse( 0.35f, 1.0f, ( ru > rv ? ru : rv ) * 0.6f + ( ru + rv ) * 0.4f );
			fin[y * mw + x] = c;
		}
	out.resize( OW * OH * 4 );
	for( int y = 0; y < OH; ++y )
		for( int x = 0; x < OW; ++x )
		{
			const float c = 0.25f * ( fin[( 2 * y ) * mw + 2 * x] + fin[( 2 * y ) * mw + 2 * x + 1]
			                        + fin[( 2 * y + 1 ) * mw + 2 * x] + fin[( 2 * y + 1 ) * mw + 2 * x + 1] );
			int g = (int)( c * 255.0f + 0.5f );
			g = g < 0 ? 0 : g > 255 ? 255 : g;
			unsigned char *o = &out[( y * OW + x ) * 4];
			o[0] = o[1] = o[2] = (unsigned char)g;
			o[3] = 255;
		}
}

// --- the run -----------------------------------------------------------------

Obj::CSkater *skater( void ) { return Mdl::Skate::Instance()->GetLocalSkater(); }

void poser_camera( float vfov )
{
	for( int c = 0; c < 3; ++c )
	{
		g_desktop_portrait_at[c] = s.cible[c];
		g_desktop_portrait_eye[c] = s.cible[c] + s.dir[c] * DIST;
	}
	g_desktop_portrait_vfov = vfov;
	g_desktop_portrait_cam = true;
}

// First guess: the head bone (any height is fine, the wide look corrects it).
bool viser( void )
{
	Obj::CSkater *p = skater();
	if( !p ) return false;
	Obj::CSkeletonComponent *sq = GetSkeletonComponentFromObject( p );
	Mth::Vector pied = p->GetPos(), tete = pied;
	s.os_tete = -1e9f;
	if( !sq || !sq->GetBoneWorldPosition( crc( "Bone_Head" ), &tete ) || tete[Y] - pied[Y] < 20.0f )
		tete = pied + Mth::Vector( 0.0f, 62.0f, 0.0f, 0.0f );
	else
		s.os_tete = tete[Y];
	Mth::Vector avant = p->GetMatrix()[Z];
	avant[Y] = 0.0f; avant[W] = 0.0f;
	if( avant.Length() < 0.01f ) avant.Set( 0.0f, 0.0f, 1.0f, 0.0f );
	avant.Normalize();
	const float a = LACET * 3.14159265f / 180.0f;
	s.dir[0] = avant[X] * cosf( a ) + avant[Z] * sinf( a );
	s.dir[1] = 0.0f;
	s.dir[2] = -avant[X] * sinf( a ) + avant[Z] * cosf( a );
	for( int c = 0; c < 3; ++c ) s.cible[c] = tete[c];
	s.cible[1] += 3.0f;
	poser_camera( VFOV_LARGE );
	return true;
}

// From the wide look: the first row (from the top) with a few skater
// pixels in the middle half is the top of the head. Re-aim below it.
void recadrer( const float *dep )
{
	std::vector<float> m;
	masque_de( dep, m );
	int haut = -1;
	for( int y = 0; y < CH && haut < 0; ++y )
	{
		int n = 0;
		for( int x = CW / 4; x < CW * 3 / 4; ++x ) n += m[y * CW + x] > 0.5f;
		if( n >= 3 ) haut = y;
	}
	if( haut < 0 )
	{
		VLOG( "DSK", "portraits: %08x, no head found in the wide look", (unsigned)s.liste[s.i] );
		poser_camera( VFOV );
		return;
	}
	const float demi = DIST * tanf( VFOV_LARGE * 0.5f * 3.14159265f / 180.0f );
	const float dy = ( 1.0f - 2.0f * ( haut + 0.5f ) / CH ) * demi;
	s.cible[1] += dy - SOUS_CRANE;
	// A tall hat (the chef's) is not the top of the head: never aim more
	// than a few inches over the head bone.
	if( s.os_tete > -1e8f && s.cible[1] > s.os_tete + 5.0f )
	{
		VLOG( "DSK", "portraits: %08x, hat or hair %.0f in over the head, aimed at the head bone",
		      (unsigned)s.liste[s.i], s.cible[1] - s.os_tete );
		s.cible[1] = s.os_tete + 3.0f;
	}
	poser_camera( VFOV );
}

void progres( void )
{
	char t[96];
	snprintf( t, sizeof( t ), "Building portraits %d / %d", (int)s.i + 1, (int)s.liste.size());
	lancer( "desktop_portrait_progress", 0, t );
}

void suivant( void )
{
	s.frames = 0;
	s.passe = 0;
	s.demande = false;
	if( s.i >= s.liste.size())
	{
		g_desktop_portrait_cam = false;
		s.actif = false;
		lancer( "desktop_portrait_pose", s.avant );
		char t[96];
		snprintf( t, sizeof( t ), s.faits == 1 ? "%d portrait built" : "%d portraits built", s.faits );
		lancer( "desktop_portrait_done", 0, t );
		VLOG( "DSK", "portraits: %d built, %d failed", s.faits, s.rates );
		return;
	}
	progres();
	lancer( "desktop_portrait_pose", s.liste[s.i] );
}

} // namespace

namespace Mdl
{
void DesktopPortraitsUpdate( Skate &mdl )
{
	(void)mdl;
	if( !s.actif )
		return;
	if( !skater())
	{
		VLOG( "DSK", "portraits: no skater, stopped" );
		s.i = s.liste.size();
		suivant();
		return;
	}
	++s.frames;
	// Everyone in the plain standing idle (Gene, Iron Man and THUD have
	// their own entrance in SkateshopAI, the others a random pose)...
	if( s.passe == 0 && s.frames == 3 )
		lancer( "desktop_portrait_idle" );
	// ...frozen in its first frames: no sway, no turning between the wide
	// look and the portrait.
	if( s.passe == 1 || s.frames >= 6 )
	{
		Obj::CAnimationComponent *an = GetAnimationComponentFromObject( skater());
		if( an ) an->SetAnimSpeed( 0.0f, false, true );
	}
	if( s.passe == 0 && s.frames <= ATTENTE )
	{
		viser();
		return;
	}
	if( s.passe == 1 && s.frames <= ATTENTE_2 )
		return;
	if( !s.demande )
	{
		Gfx::Camera *cam = Nx::CViewportManager::sGetActiveCamera( 0 );
		if( s.i == 0 && s.passe == 0 && cam )
		{
			Mth::Vector vers = cam->GetPos() - skater()->GetPos(), avant = skater()->GetMatrix()[Z];
			vers[Y] = avant[Y] = 0.0f; vers[W] = avant[W] = 0.0f;
			if( vers.Length() > 0.01f && avant.Length() > 0.01f )
				VLOG( "DSK", "portraits: skater facing vs menu camera %.2f", Mth::DotProduct( vers.Normalize(), avant.Normalize()));
		}
		desktop_portrait_request( CW, CH );
		s.demande = true;
		return;
	}
	static std::vector<unsigned char> rgb;
	static std::vector<float> dep;
	rgb.resize( CW * CH * 3 );
	dep.resize( CW * CH );
	if( !desktop_portrait_result( &rgb[0], &dep[0] ))
	{
		if( s.frames > ATTENTE + 40 )
		{
			VLOG( "DSK", "portraits: no capture for %08x", (unsigned)s.liste[s.i] );
			++s.rates; ++s.i; suivant();
		}
		return;
	}
	if( s.passe == 0 )
	{
		recadrer( &dep[0] );
		s.passe = 1;
		s.frames = 0;
		s.demande = false;
		return;
	}
	std::vector<unsigned char> img;
	fabriquer( &rgb[0], &dep[0], img );
	const uint32 nom = s.liste[s.i];
	if( desktop_portrait_save( nom, &img[0], OW, OH ))
	{
		++s.faits;
		if( DesktopSpriteFromRGBA( tex_de( nom ), &img[0], OW, OH, LW, LH ))
			s_charge[nom] = 1;
	}
	else
		++s.rates;
	++s.i;
	suivant();
}
} // namespace Mdl

namespace CFuncs
{

// @script | DesktopBuildPortraits | desktop: render skater portraits; all = every skater, else only the ones without one (and the created skater); restore = the skater to put back after
bool ScriptDesktopBuildPortraits( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( s.actif ) return false;
	const bool tous = pParams->ContainsFlag( crc( "all" ));
	Obj::CPlayerProfileManager *ppm = Mdl::Skate::Instance()->GetPlayerProfileManager();
	s.liste.clear();
	const int n = (int)ppm->GetNumProfileTemplates();
	static std::vector<unsigned char> tmp( OW * OH * 4 );
	for( int i = 0; i < n; ++i )
	{
		Script::CStruct *p_info = ppm->GetProfileTemplateByIndex( i )->GetInfo();
		uint32 nom = 0;
		int pro = 0;
		p_info->GetChecksum( crc( "name" ), &nom, Script::NO_ASSERT );
		p_info->GetInteger( crc( "is_pro" ), &pro, Script::NO_ASSERT );
		const bool cree = nom == crc( "custom" );
		if( !nom || ( !pro && !cree )) continue;
		// "New" still redoes the created skater: its look changes in CAS.
		if( !tous && !cree && desktop_portrait_load( nom, &tmp[0], OW, OH )) continue;
		s.liste.push_back( nom );
	}
	s.avant = Mdl::Skate::Instance()->GetCurrentProfile()->GetSkaterNameChecksum();
	pParams->GetChecksum( crc( "restore" ), &s.avant, Script::NO_ASSERT );
	s.i = 0; s.faits = s.rates = 0;
	s.actif = !s.liste.empty();
	for( std::map<uint32, int>::iterator it = s_charge.begin(); it != s_charge.end(); )
	{
		if( it->second < 0 ) s_charge.erase( it++ ); else ++it;
	}
	VLOG( "DSK", "portraits: building %d (%s)", (int)s.liste.size(), tous ? "all" : "new" );
	if( s.actif ) suivant();
	return s.actif;
}

// @script | DesktopPortraitsBuilding | desktop: true while Build Portraits runs
bool ScriptDesktopPortraitsBuilding( Script::CStruct *pParams, Script::CScript *pScript )
{
	return s.actif;
}

// @script | DesktopPortrait | desktop: name = skater; if a built portrait exists, sets headshot to it (custom: puts it over ss_cas)
bool ScriptDesktopPortrait( Script::CStruct *pParams, Script::CScript *pScript )
{
	uint32 nom = 0;
	pParams->GetChecksum( crc( "name" ), &nom, Script::NO_ASSERT );
	if( !nom ) return false;
	const bool cree = nom == crc( "custom" );
	std::map<uint32, int>::iterator it = s_charge.find( nom );
	int etat = it == s_charge.end() ? 0 : it->second;
	// ss_cas is reloaded with its sprite dictionary: always put ours back.
	if( !etat || ( cree && etat > 0 ))
	{
		static std::vector<unsigned char> img;
		img.resize( OW * OH * 4 );
		etat = ( desktop_portrait_load( nom, &img[0], OW, OH )
		         && DesktopSpriteFromRGBA( tex_de( nom ), &img[0], OW, OH, LW, LH )) ? 1 : -1;
		s_charge[nom] = etat;
	}
	if( etat < 0 ) return false;
	pScript->GetParams()->AddChecksum( crc( "headshot" ), tex_de( nom ));
	return true;
}

} // namespace CFuncs

#endif
