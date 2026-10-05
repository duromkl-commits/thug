/*****************************************************************************
**  THUG-Vita — backend VitaGL                                              **
**  Code/Gfx/Vita/p_NxLight.cpp                                             **
**                                                                          **
**  Lumieres propres a un modele. Transposition de DX9/p_NxLight.cpp : la   **
**  reference ecrit ses resultats dans des registres de vertex shader ; ici **
**  ResolveLighting() les rend sous forme de valeurs, et le skinning CPU    **
**  s'en sert pour calculer une couleur par sommet.                         **
*****************************************************************************/

#include <core/defines.h>

#include "p_NxLight.h"
#include "vita_log.h"
#include <math.h>

#include <gfx/NxLightMan.h>

namespace Nx
{

CVitaModelLights::CVitaModelLights()
{
	m_flags         = 0;
	m_ambient_color = Image::RGBA( 0, 0, 0, 0x80 );
	for( int i = 0; i < CLightManager::MAX_LIGHTS; ++i )
	{
		m_diffuse_color[i] = Image::RGBA( 0, 0, 0, 0x80 );
		m_diffuse_direction[i].Set( 0.0f, 1.0f, 0.0f );
	}
}


CVitaModelLights::~CVitaModelLights()
{
}


bool CVitaModelLights::plat_set_light_ambient_color( const Image::RGBA &rgba )
{
	m_ambient_color = rgba;
	return true;
}


Image::RGBA CVitaModelLights::plat_get_light_ambient_color() const
{
	return m_ambient_color;
}


bool CVitaModelLights::plat_set_light_direction( int light_index,
                                                 const Mth::Vector &direction )
{
	m_diffuse_direction[light_index] = direction;
	return true;
}


const Mth::Vector & CVitaModelLights::plat_get_light_direction( int light_index ) const
{
	if( plat_is_diffuse_light_enabled( light_index ))
		return m_diffuse_direction[light_index];
	return CLightManager::sGetLightDirection( light_index );
}


bool CVitaModelLights::plat_set_light_diffuse_color( int light_index,
                                                     const Image::RGBA &rgba )
{
	m_diffuse_color[light_index] = rgba;
	return true;
}


Image::RGBA CVitaModelLights::plat_get_light_diffuse_color( int light_index ) const
{
	return m_diffuse_color[light_index];
}


void CVitaModelLights::plat_enable_ambient_light( bool enable )
{
	if( enable )
		m_flags |= mUSE_MODEL_AMBIENT;
	else
		m_flags &= ~mUSE_MODEL_AMBIENT;
}


void CVitaModelLights::plat_enable_diffuse_light( int light_index, bool enable )
{
	const uint32 bit = ( light_index == 0 ) ? mUSE_MODEL_DIFFUSE_0
	                                        : mUSE_MODEL_DIFFUSE_1;
	if( enable )
		m_flags |= bit;
	else
		m_flags &= ~bit;
}


bool CVitaModelLights::plat_is_ambient_light_enabled() const
{
	return ( m_flags & mUSE_MODEL_AMBIENT ) != 0;
}


bool CVitaModelLights::plat_is_diffuse_light_enabled( int light_index ) const
{
	const uint32 bit = ( light_index == 0 ) ? mUSE_MODEL_DIFFUSE_0
	                                        : mUSE_MODEL_DIFFUSE_1;
	return ( m_flags & bit ) != 0;
}


// DX9/p_NxLight.cpp:64-110, terme pour terme : couleur locale si le drapeau
// correspondant est pose, couleur du monde sinon, divisee par 128 et modulee
// par la luminosite. La direction est INVERSEE, comme dans la reference.
void CVitaModelLights::ResolveLighting( float amb[3], float dif[2][3],
                                        float dir[2][3] ) const
{
	const Image::RGBA a = ( m_flags & mUSE_MODEL_AMBIENT )
	                    ? m_ambient_color
	                    : CLightManager::sGetLightAmbientColor();

	amb[0] = (float)a.r * ( 1.0f / 128.0f ) * m_ambient_brightness;
	amb[1] = (float)a.g * ( 1.0f / 128.0f ) * m_ambient_brightness;
	amb[2] = (float)a.b * ( 1.0f / 128.0f ) * m_ambient_brightness;

	for( int i = 0; i < 2; ++i )
	{
		const bool local = ( m_flags & (( i == 0 ) ? mUSE_MODEL_DIFFUSE_0
		                                           : mUSE_MODEL_DIFFUSE_1 )) != 0;

		const Image::RGBA d = local ? m_diffuse_color[i]
		                            : CLightManager::sGetLightDiffuseColor( i );

		dif[i][0] = (float)d.r * ( 1.0f / 128.0f ) * m_diffuse_brightness[i];
		dif[i][1] = (float)d.g * ( 1.0f / 128.0f ) * m_diffuse_brightness[i];
		dif[i][2] = (float)d.b * ( 1.0f / 128.0f ) * m_diffuse_brightness[i];

		const Mth::Vector v = local ? m_diffuse_direction[i]
		                            : CLightManager::sGetLightDirection( i );
		dir[i][0] = -v[X];
		dir[i][1] = -v[Y];
		dir[i][2] = -v[Z];
	}

	// Qui gagne, et avec quelles valeurs ? Sans cette trace on ne saurait pas
	// distinguer « le modele n'a pas de lumieres propres » de « il en a, mais
	// elles sont claires ».
	{
		static int s_r = 0;
		if(( s_r++ % 600 ) < 2 )
			VLOG( "LUM", "modele : drapeaux 0x%x  ambiante (%.2f %.2f %.2f)  "
			             "diffuse0 (%.2f %.2f %.2f)",
			      (unsigned)m_flags, amb[0], amb[1], amb[2],
			      dif[0][0], dif[0][1], dif[0][2] );
		if(( s_r % 600 ) == 1 )
			VLOG( "LUM", "  brut : ambiante %d %d %d x%.2f, diffuse0 %d %d %d x%.2f, luminosite %.2f -> %.2f",
			      a.r, a.g, a.b, m_ambient_brightness, m_diffuse_color[0].r, m_diffuse_color[0].g,
			      m_diffuse_color[0].b, m_diffuse_brightness[0], m_brightness, m_brightness_target );
	}
}

// [SOURCE] XBox/p_NxLight.cpp:112-139, terme pour terme : direction NON
// inversee (de la lumiere vers le modele), couleur / 255 x intensite x
// sqrt( 1 - distance / rayon ).
void CVitaModelLights::ResolveSceneLight( const Mth::Vector &pos, float dir[3], float col[3] )
{
	dir[0] = dir[1] = dir[2] = 0.0f;
	col[0] = col[1] = col[2] = 0.0f;
	Mth::Vector p = pos;
	CSceneLight *p_l = CLightManager::sGetOptimumSceneLight( p );
	if( !p_l )
		return;
	Mth::Vector lp = p_l->GetLightPosition();
	const float ratio = Mth::Distance( p, lp ) * p_l->GetLightReciprocalRadius();
	Mth::Vector d = p - lp;
	d.Normalize();
	dir[0] = d[X]; dir[1] = d[Y]; dir[2] = d[Z];
	const float k = sqrtf( 1.0f - ratio ) * ( 1.0f / 255.0f ) * p_l->GetLightIntensity();
	col[0] = k * p_l->GetLightColor().r;
	col[1] = k * p_l->GetLightColor().g;
	col[2] = k * p_l->GetLightColor().b;
	static int s_n = 0;
	if(( s_n++ % 1200 ) == 0 )
		VLOG( "LUM", "lumiere de scene : couleur %.2f %.2f %.2f (distance/rayon %.2f)", col[0], col[1], col[2], ratio );
}

} // namespace Nx
