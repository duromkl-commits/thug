///////////////////////////////////////////////////////////////////////////////
// p_NxNewParticle.h -- particules parametriques (CNewParticle) sur Vita.
//
// Contrat plateforme ([SOURCE] Gfx/NxNewParticle.h) : plat_build,
// plat_destroy, plat_hide, plat_render, plat_update. Le portable ne fait que
// ranger les parametres (CParticleParams) et calculer le volume englobant
// (m_bbox / m_bsphere) ; TOUTE la simulation est cote plateforme.
//
// Reprise de XBox/p_nxnewparticle.cpp :
//   - plat_build : passage du format "3 points" (boites de depart, milieu,
//     fin + rayons) au format position/vitesse/acceleration (m_p0..m_p2 pour
//     les centres, m_s0..m_s2 pour l'etendue des boites et l'ecart de rayon),
//     debit d'emission divise par 2 ("to improve performance", XBox).
//   - plat_render : flux d'emission (5 au plus, tampon circulaire), age des
//     flux, naissances et morts, puis dessin. Une particule n'est PAS stockee :
//     sa position se recalcule a chaque image depuis son age et 4 nombres
//     pseudo-aleatoires rejoues a partir de la graine du flux.
//   - plat_update : ne recalcule m_p* que pour les systemes en coordonnees
//     locales (LocalSpace).
//
// Le XBox faisait le calcul par particule dans un vertex shader
// (NX/ParticleNewFlatVS.vsh) ; ici il est fait sur CPU et dessine en quads
// face camera par vitaGL, tampons clients (aucune memoire GPU a nous).

#ifndef __GFX_VITA_P_NXNEWPARTICLE_H__
#define __GFX_VITA_P_NXNEWPARTICLE_H__

#include <core/defines.h>
#include <gfx/NxNewParticle.h>
#include <gfx/NxNewParticleMgr.h>

namespace Nx
{

// [SOURCE] XBox/p_nxnewparticle.h, CParticleStream. Renomme pour ne pas
// heurter un homonyme si un autre backend entrait un jour dans le build.
class CVitaParticleStream
{
public:
	int		m_num_particles;
	float	m_rate;
	float	m_interval;
	float	m_oldest_age;
	uint32	m_rand_seed;
	uint32	m_rand_a;
	uint32	m_rand_b;

	void	AdvanceSeed( int num_places );
};


class CVitaNewParticle : public CNewParticle
{
public:
	CVitaNewParticle( void );
	virtual ~CVitaNewParticle( void );

protected:
	virtual void	plat_build( void );
	virtual void	plat_destroy( void );
	virtual void	plat_hide( bool should_hide );
	virtual void	plat_render( void );
	virtual void	plat_update( void );

private:
	void	update_position( void );
	bool	avancer_flux( float dt );
	void	dessiner( void );
	void	dessiner_shader( void );

	bool					m_emitting;
	int						m_max_streams;
	int						m_num_streams;
	CVitaParticleStream *	mp_stream;
	CVitaParticleStream *	mp_newest_stream;
	CVitaParticleStream *	mp_oldest_stream;
	uint32					m_blend;		// checksum du mode, deja resolu
	Mth::Vector				m_s0, m_s1, m_s2;
	Mth::Vector				m_p0, m_p1, m_p2;
};


class CVitaNewParticleManager : public CNewParticleManager
{
public:
	// Pose l'etat GL commun, fait RenderParticles (portable : parcourt la
	// table et appelle plat_render de chaque systeme visible), restaure
	// l'etat et tient le journal [PRT]. A appeler apres les translucides du
	// monde, comme XBox/p_nx.cpp:399.
	void	RenderVita( void );

private:
	virtual CNewParticle *	plat_create_particle( void );
};

} // namespace Nx

namespace NxVita
{
// "prt 0..4" (p_siodev.cpp). 0 = rien n'est simule ni dessine ;
// 1 = comme XBox, y compris la limite de 64 pixels des point sprites au-dela
// de 240 unites (XBox/p_nxnewparticle.cpp:252) ; 2 = quads sans cette limite,
// calcules sur CPU ; 3 = calcul CPU seul, sans dessin (mesure) ; 4 = quads
// calcules par un vertex shader, comme ParticleNewFlatVS.vsh (repli
// automatique sur 2 si le shader manque).
extern int g_vita_particules;

// Partages avec les anciennes particules (p_NxParticle.cpp), pour ne pas
// dupliquer : familles de melange de GetBlendMode (XBox render.cpp:119) et
// leur etat GL, brouillard noir des modes additifs/soustractifs, x2 de
// PixelShader0 (128 = 1.0) sature a 255, repere d'ecran XBox (droite =
// at x Y, haut = droite x at) aligne sur l'ecran, et position camera.
uint32			PrtFamilleMelange( uint32 checksum );
bool			PrtPoserMelange( uint32 famille );
bool			PrtMelangeNoir( uint32 famille );
unsigned char	PrtCouleurX2( float c );
void			PrtRepereEcran( const float v[16], float droite[3], float haut[3], float at[3], float cam[3] );
}

#endif // __GFX_VITA_P_NXNEWPARTICLE_H__
