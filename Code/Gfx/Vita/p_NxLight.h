///////////////////////////////////////////////////////////////////////////////
// p_NxLight.h — lumieres propres a un modele
//
// Transposition de DX9/p_NxLight.h. CModelLights ne stocke PAS les couleurs :
// ses champs sont commentes (NxLight.h:64-66), c'est la classe de plateforme
// qui les porte. Sans elle, un modele ne peut pas avoir d'eclairage propre et
// retombe toujours sur celui du monde -- c'est pourquoi le skater du menu
// restait clair quoi qu'on fasse.

#ifndef __GFX_VITA_P_NXLIGHT_H__
#define __GFX_VITA_P_NXLIGHT_H__

#include <core/allmath.h>
#include <gfx/image/imagebasic.h>
#include <gfx/NxLight.h>

namespace Nx
{

class CVitaModelLights : public CModelLights
{
public:
	CVitaModelLights();
	virtual ~CVitaModelLights();

	// Rend l'eclairage EFFECTIF de ce modele : couleurs locales s'il en a,
	// sinon celles du monde, le tout module par ses luminosites. C'est la
	// selection de DX9/p_NxLight.cpp:64-110, sortie sous forme de valeurs
	// plutot qu'ecrite dans des registres de shader.
	void	ResolveLighting( float amb[3], float dif[2][3], float dir[2][3] ) const;
	// 3e lumiere : la lumiere de SCENE la plus proche (XBox p_NxLight.cpp:112).
	// dir = normalise( pos - lumiere ), couleur nulle s'il n'y en a pas.
	static void ResolveSceneLight( const Mth::Vector &pos, float dir[3], float col[3] );

private:
	virtual bool				plat_set_light_ambient_color( const Image::RGBA &rgba );
	virtual bool				plat_set_light_direction( int light_index, const Mth::Vector &direction );
	virtual bool				plat_set_light_diffuse_color( int light_index, const Image::RGBA &rgba );
	virtual Image::RGBA			plat_get_light_ambient_color() const;
	virtual const Mth::Vector &	plat_get_light_direction( int light_index ) const;
	virtual Image::RGBA			plat_get_light_diffuse_color( int light_index ) const;

	virtual void				plat_enable_ambient_light( bool enable );
	virtual void				plat_enable_diffuse_light( int light_index, bool enable );
	virtual bool				plat_is_ambient_light_enabled() const;
	virtual bool				plat_is_diffuse_light_enabled( int light_index ) const;

	uint32			m_flags;
	Image::RGBA		m_ambient_color;
	Image::RGBA		m_diffuse_color[CLightManager::MAX_LIGHTS];
	Mth::Vector		m_diffuse_direction[CLightManager::MAX_LIGHTS];
};

} // namespace Nx

#endif // __GFX_VITA_P_NXLIGHT_H__
