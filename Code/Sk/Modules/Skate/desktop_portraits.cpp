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
extern "C" void desktop_portrait_request( int n );
extern "C" int  desktop_portrait_result( unsigned char *rgb, float *dep );
extern "C" int  desktop_portrait_save( unsigned int key, const unsigned char *rgba, int w, int h );
extern "C" int  desktop_portrait_load( unsigned int key, unsigned char *rgba, int w, int h );
bool DesktopSpriteFromRGBA( uint32 checksum, const unsigned char *rgba, int pw, int ph, int w, int h );

namespace
{

const int CAPT = 256;		// grabbed square, before the bulge
const int OUT  = 128;		// stored pixels (the menu shows them 64 x 64)
const float DIST = 70.0f;	// camera to face, inches
const float VFOV = 13.0f;	// ~16 inches of head and neck in frame
const float HAUT = 3.5f;	// aim above the head bone (it sits at the skull base)
const float LACET = 12.0f;	// degrees off straight on, like a three-quarter shot
const int ATTENTE = 14;		// frames for the model, its pose and the lights to settle

uint32 crc( const char *t ) { return Script::GenerateCRC( t ); }

struct Etat
{
	bool actif;
	std::vector<uint32> liste;
	size_t i;
	int frames;
	bool demande;
	uint32 avant;
	int faits, rates;
} s = { false, std::vector<uint32>(), 0, 0, false, 0, 0, 0 };

std::map<uint32, int> s_charge;		// 1 in the sprite dictionary, -1 no file

uint32 tex_de( uint32 nom )
{
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

void fabriquer( const unsigned char *rgb, const float *dep, std::vector<unsigned char> &out )
{
	const int n = CAPT;
	// Skater = closer than a little behind the face.
	std::vector<float> masque( n * n ), lum( n * n );
	const float seuil = DIST + 16.0f;
	for( int i = 0; i < n * n; ++i )
	{
		masque[i] = ( dep[i] < 1.0f && lin( dep[i] ) < seuil ) ? 1.0f : 0.0f;
		lum[i] = ( 0.299f * rgb[i * 3] + 0.587f * rgb[i * 3 + 1] + 0.114f * rgb[i * 3 + 2] ) / 255.0f;
	}
	flou( masque, n, 1 );

	// Levels stretched over the skater's own pixels (2nd..98th percentile),
	// then an S curve: every skin and light comes out with the same punch.
	int hist[256] = { 0 }, total = 0;
	for( int i = 0; i < n * n; ++i )
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
	briques( bg, n );
	std::vector<float> img( n * n );
	for( int i = 0; i < n * n; ++i )
	{
		float l = ( lum[i] - bas ) / ( haut - bas );
		l = l < 0.0f ? 0.0f : l > 1.0f ? 1.0f : l;
		l = 0.06f + 0.9f * ( l * 0.4f + lisse( 0.0f, 1.0f, l ) * 0.6f );
		img[i] = masque[i] * l + ( 1.0f - masque[i] ) * bg[i];
	}

	// Lens bulge (centre magnified, edges where they were), vignette,
	// then down to OUT with a 2x2 box.
	const int m = OUT * 2;
	std::vector<float> fin( m * m );
	const float k = 0.16f;
	for( int y = 0; y < m; ++y )
		for( int x = 0; x < m; ++x )
		{
			const float u = ( x + 0.5f ) / m * 2.0f - 1.0f, v = ( y + 0.5f ) / m * 2.0f - 1.0f;
			const float r2 = ( u * u + v * v ) * 0.5f;
			const float e = ( 1.0f - k ) + k * r2;
			float sx = ( u * e * 0.5f + 0.5f ) * n - 0.5f, sy = ( v * e * 0.5f + 0.5f ) * n - 0.5f;
			sx = sx < 0.0f ? 0.0f : sx > n - 1.001f ? n - 1.001f : sx;
			sy = sy < 0.0f ? 0.0f : sy > n - 1.001f ? n - 1.001f : sy;
			const int ix = (int)sx, iy = (int)sy;
			const float fx = sx - ix, fy = sy - iy;
			const float a = img[iy * n + ix] * ( 1 - fx ) + img[iy * n + ix + 1] * fx;
			const float b = img[( iy + 1 ) * n + ix] * ( 1 - fx ) + img[( iy + 1 ) * n + ix + 1] * fx;
			float c = a * ( 1 - fy ) + b * fy;
			c *= 1.0f - 0.45f * lisse( 0.35f, 1.0f, r2 * 2.0f );
			fin[y * m + x] = c;
		}
	out.resize( OUT * OUT * 4 );
	for( int y = 0; y < OUT; ++y )
		for( int x = 0; x < OUT; ++x )
		{
			const float c = 0.25f * ( fin[( 2 * y ) * m + 2 * x] + fin[( 2 * y ) * m + 2 * x + 1]
			                        + fin[( 2 * y + 1 ) * m + 2 * x] + fin[( 2 * y + 1 ) * m + 2 * x + 1] );
			int g = (int)( c * 255.0f + 0.5f );
			g = g < 0 ? 0 : g > 255 ? 255 : g;
			unsigned char *o = &out[( y * OUT + x ) * 4];
			o[0] = o[1] = o[2] = (unsigned char)g;
			o[3] = 255;
		}
}

// --- the run -----------------------------------------------------------------

Obj::CSkater *skater( void ) { return Mdl::Skate::Instance()->GetLocalSkater(); }

bool viser( void )
{
	Obj::CSkater *p = skater();
	if( !p ) return false;
	Obj::CSkeletonComponent *sq = GetSkeletonComponentFromObject( p );
	Mth::Vector pied = p->GetPos(), tete = pied;
	if( !sq || !sq->GetBoneWorldPosition( crc( "Bone_Head" ), &tete ) || tete[Y] - pied[Y] < 20.0f )
		tete = pied + Mth::Vector( 0.0f, 62.0f, 0.0f, 0.0f );
	Mth::Vector avant = p->GetMatrix()[Z];
	avant[Y] = 0.0f; avant[W] = 0.0f;
	if( avant.Length() < 0.01f ) avant.Set( 0.0f, 0.0f, 1.0f, 0.0f );
	avant.Normalize();
	const float a = LACET * 3.14159265f / 180.0f;
	Mth::Vector dir( avant[X] * cosf( a ) + avant[Z] * sinf( a ), 0.0f, -avant[X] * sinf( a ) + avant[Z] * cosf( a ), 0.0f );
	Mth::Vector cible = tete + Mth::Vector( 0.0f, HAUT, 0.0f, 0.0f );
	Mth::Vector oeil = cible + dir * DIST;
	for( int c = 0; c < 3; ++c ) { g_desktop_portrait_at[c] = cible[c]; g_desktop_portrait_eye[c] = oeil[c]; }
	g_desktop_portrait_vfov = VFOV;
	g_desktop_portrait_cam = true;
	return true;
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
	s.demande = false;
	if( s.i >= s.liste.size())
	{
		g_desktop_portrait_cam = false;
		s.actif = false;
		lancer( "desktop_portrait_pose", s.avant );
		char t[96];
		snprintf( t, sizeof( t ), "%d portraits built", s.faits );
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
	viser();
	++s.frames;
	if( s.frames < ATTENTE )
		return;
	if( !s.demande )
	{
		// Which way the portrait looks, against where the menu camera
		// stood: a negative number on a skater facing the menu camera
		// would mean the back of the head.
		Gfx::Camera *cam = Nx::CViewportManager::sGetActiveCamera( 0 );
		if( s.i == 0 && cam && skater())
		{
			Mth::Vector vers = cam->GetPos() - skater()->GetPos(), avant = skater()->GetMatrix()[Z];
			vers[Y] = avant[Y] = 0.0f; vers[W] = avant[W] = 0.0f;
			if( vers.Length() > 0.01f && avant.Length() > 0.01f )
				VLOG( "DSK", "portraits: skater facing vs menu camera %.2f", Mth::DotProduct( vers.Normalize(), avant.Normalize()));
		}
		desktop_portrait_request( CAPT );
		s.demande = true;
		return;
	}
	static std::vector<unsigned char> rgb;
	static std::vector<float> dep;
	rgb.resize( CAPT * CAPT * 3 );
	dep.resize( CAPT * CAPT );
	if( !desktop_portrait_result( &rgb[0], &dep[0] ))
	{
		if( s.frames > ATTENTE + 30 )
		{
			VLOG( "DSK", "portraits: no capture for %08x", (unsigned)s.liste[s.i] );
			++s.rates; ++s.i; suivant();
		}
		return;
	}
	std::vector<unsigned char> img;
	fabriquer( &rgb[0], &dep[0], img );
	const uint32 nom = s.liste[s.i];
	if( desktop_portrait_save( nom, &img[0], OUT, OUT ))
	{
		++s.faits;
		if( DesktopSpriteFromRGBA( tex_de( nom ), &img[0], OUT, OUT, 64, 64 ))
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

// @script | DesktopBuildPortraits | desktop: render a portrait of every skater; restore = the skater to put back after
bool ScriptDesktopBuildPortraits( Script::CStruct *pParams, Script::CScript *pScript )
{
	if( s.actif ) return false;
	Obj::CPlayerProfileManager *ppm = Mdl::Skate::Instance()->GetPlayerProfileManager();
	s.liste.clear();
	const int n = (int)ppm->GetNumProfileTemplates();
	for( int i = 0; i < n; ++i )
	{
		Script::CStruct *p_info = ppm->GetProfileTemplateByIndex( i )->GetInfo();
		uint32 nom = 0;
		int pro = 0;
		p_info->GetChecksum( crc( "name" ), &nom, Script::NO_ASSERT );
		p_info->GetInteger( crc( "is_pro" ), &pro, Script::NO_ASSERT );
		if( nom && pro && nom != crc( "custom" ))
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
	VLOG( "DSK", "portraits: building %d", (int)s.liste.size());
	if( s.actif ) suivant();
	return s.actif;
}

// @script | DesktopPortraitsBuilding | desktop: true while Build Portraits runs
bool ScriptDesktopPortraitsBuilding( Script::CStruct *pParams, Script::CScript *pScript )
{
	return s.actif;
}

// @script | DesktopPortrait | desktop: name = skater; if a built portrait exists, sets headshot to it
bool ScriptDesktopPortrait( Script::CStruct *pParams, Script::CScript *pScript )
{
	uint32 nom = 0;
	pParams->GetChecksum( crc( "name" ), &nom, Script::NO_ASSERT );
	if( !nom || nom == crc( "custom" )) return false;
	std::map<uint32, int>::iterator it = s_charge.find( nom );
	int etat = it == s_charge.end() ? 0 : it->second;
	if( !etat )
	{
		static std::vector<unsigned char> img;
		img.resize( OUT * OUT * 4 );
		etat = ( desktop_portrait_load( nom, &img[0], OUT, OUT )
		         && DesktopSpriteFromRGBA( tex_de( nom ), &img[0], OUT, OUT, 64, 64 )) ? 1 : -1;
		s_charge[nom] = etat;
	}
	if( etat < 0 ) return false;
	pScript->GetParams()->AddChecksum( crc( "headshot" ), tex_de( nom ));
	return true;
}

} // namespace CFuncs

#endif
