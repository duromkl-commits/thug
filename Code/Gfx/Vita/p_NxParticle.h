///////////////////////////////////////////////////////////////////////////////
// p_NxParticle.h -- ANCIENNES particules (CParticle, nxparticle.cpp) sur Vita.
//
// Systemes crees par la cfunc CreateParticleSystem (cfuncs.cpp:12477), par
// exemple InitSkaterParticles (scripts\game\skater\skater_particlesys.qb) :
//   - skater_blood_system  : type par defaut Flat, texture particle_test02,
//                            Blend, max 25 (sang, os Bone_Head) ;
//   - skater_sparks_system : GlowRibbonTrail, history 2, Blend, max 40
//                            (etincelles des grinds, os Bone_Board_Tail) ;
//   - skatersplash         : Glow, 5 segments, Blend, max 30/80 (eclaboussures).
// Inventaire : vita/tools/particules_qb.py (seuls ces trois types sont
// demandes par les scripts lus : qb.prx, NJ, FL, SD).
//
// Contrat plateforme ([SOURCE] Gfx/nxparticle.h) : le portable fait la
// physique (CParticle::process : force, vitesse, historique des positions,
// morts) et l'emission (CParticle::emit) ; la plateforme stocke les positions
// (plat_get/set/add_position, une liste par cran d'historique), les couleurs
// (plat_set_s/m/e r/g/b/a, par "coin") et dessine (plat_render). Reprise de
// XBox/p_nxparticle.cpp (fabrique), p_nxparticleflat.cpp,
// p_nxparticleglow.cpp, p_nxparticleglowribbontrail.cpp : voir
// p_NxParticle.cpp.
//
// Les autres types (Line, Shaded, Smooth, Star, SmoothStar, Ribbon,
// SmoothRibbon, RibbonTrail) sont stockes et simules avec le nombre de listes
// et de couleurs du XBox, mais pas dessines (journal [PRS] a la creation).

#ifndef __GFX_VITA_P_NXPARTICLE_H__
#define __GFX_VITA_P_NXPARTICLE_H__

#include <core/defines.h>
#include <gfx/nxparticle.h>

namespace Nx
{

class CVitaParticle : public CParticle
{
public:
	enum
	{
		VPRS_FLAT,
		VPRS_GLOW,
		VPRS_GLOWRIBBONTRAIL,
		VPRS_AUTRE			// stocke et simule, non dessine
	};

	CVitaParticle( uint32 checksum, uint32 type_checksum, int max_particles,
	               uint32 texture_checksum, uint32 blendmode_checksum, int fix,
	               int num_segments, float split, int history );
	virtual ~CVitaParticle( void );

	// Dessin d'un systeme (etat GL commun pose par RenderParticulesAnciennes).
	void	dessiner( void );

private:
	virtual void	plat_render( void );
	virtual void	plat_get_position( int entry, int list, float *x, float *y, float *z );
	virtual void	plat_set_position( int entry, int list, float x, float y, float z );
	virtual void	plat_add_position( int entry, int list, float x, float y, float z );
	virtual int		plat_get_num_vertex_lists( void );
	virtual int		plat_get_num_particle_colors( void );
	virtual void	plat_set_sr( int entry, uint8 value );
	virtual void	plat_set_sg( int entry, uint8 value );
	virtual void	plat_set_sb( int entry, uint8 value );
	virtual void	plat_set_sa( int entry, uint8 value );
	virtual void	plat_set_mr( int entry, uint8 value );
	virtual void	plat_set_mg( int entry, uint8 value );
	virtual void	plat_set_mb( int entry, uint8 value );
	virtual void	plat_set_ma( int entry, uint8 value );
	virtual void	plat_set_er( int entry, uint8 value );
	virtual void	plat_set_eg( int entry, uint8 value );
	virtual void	plat_set_eb( int entry, uint8 value );
	virtual void	plat_set_ea( int entry, uint8 value );
	virtual void	plat_set_active( bool active );
	virtual void	plat_build_path( void );

	void	dessiner_flat( void );
	void	dessiner_glow( void );
	void	dessiner_glow_ribbon_trail( void );

	inline float *	pos_liste( int list, int entry )	{ return mp_vertices + ( list * m_max_particles + entry ) * 3; }
	inline uint8	alpha_stocke( uint8 v )				{ return m_alpha_demi ? (uint8)( v >> 1 ) : v; }

	int				m_type;
	uint32			m_type_checksum;
	uint32			m_texture_checksum;
	uint32			m_blend;				// famille (NxVita::PrtFamilleMelange)
	int				m_num_lists;			// listes de positions (historique + 1)
	int				m_num_colors;			// "coins" de couleur
	int				m_num_segments;
	float			m_split;
	bool			m_alpha_demi;			// GlowRibbonTrail : alpha stocke >> 1
	float			m_emit_rate_fractional;	// extension PS2 de Flat (EmitRate)

	float *			mp_vertices;			// m_num_lists x m_max_particles x 3
	Image::RGBA *	mp_start;				// m_num_colors chacune
	Image::RGBA *	mp_mid;
	Image::RGBA *	mp_end;
};

} // namespace Nx

namespace NxVita
{
// "prs 0/1" (p_siodev.cpp) : 0 = anciennes particules simulees (comme le jeu)
// mais pas dessinees ; 1 = dessinees. Defaut 0 tant que non valide a l'ecran.
extern int g_vita_prs;

// Appele par CEngine::s_plat_render_world juste AVANT les particules
// parametriques, comme XBox/p_nx.cpp:392-397 (render_particles puis
// RenderParticles, profondeur non ecrite). Fait toujours render_particles()
// (l'emission a debit de Flat vit dans plat_render, XBox), dessine si prs 1,
// tient le journal [PRS].
void RenderParticulesAnciennes( void );
}

#endif // __GFX_VITA_P_NXPARTICLE_H__
