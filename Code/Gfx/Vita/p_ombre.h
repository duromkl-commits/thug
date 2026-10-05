///////////////////////////////////////////////////////////////////////////////
// p_ombre.h -- ombre portee "detailed" du skater (issues #45 V2 et #4)
//
// [SOURCE] Gfx/shadow.cpp:39-125 (CDetailedShadow) : une texture cible
// 256x256, un modele projete, une camera de lumiere placee a 72 unites de la
// cible dans la direction de l'ombre. Le backend XBox en fait :
//   - render_shadow_targets (XBox/NX/render.cpp:2119), au DEBUT de l'image :
//     profondeur du modele vue de la lumiere, ortho 96 x 96, plans 1 et 128 ;
//   - render_shadow_meshes (render.cpp:2239), apres les opaques : les
//     maillages visibles du decor redessines en MODULATE_COLOR (dst x src),
//     avec ShadowBufferStaticGeomPS : dst x min( 1, eclaire + 0.6 ).
//
// Vita : vitaGL n'a pas de comparaison de profondeur materielle et ne garde
// pas la profondeur d'une scene GXM a l'autre. La carte est donc une texture
// RGBA8 (r = profondeur, g = couverture), rendue dans s_plat_pre_render AVANT
// l'ouverture de la scene principale, avec la pose du skater memorisee a
// l'image precedente (les modeles sont dessines pendant la phase logique).
// La comparaison se fait dans le shader de reception.
//
// Tout est coupe par "omb 0" (defaut tant que ce n'est pas valide a l'ecran).

#ifndef __GFX_VITA_P_OMBRE_H__
#define __GFX_VITA_P_OMBRE_H__

namespace Nx  { class CTexture; class CModel; class CVitaMesh; }
namespace Gfx { class Camera; }

namespace NxVita
{

// "omb 0/1/2/3" : 0 = coupe, 1 = ombre, 2 = ombre + vue de la carte en coin
// d'ecran, 3 = comme 2 avec la carte lue a l'envers en V (diagnostic du
// retournement des FBO).
extern int   g_vita_ombre;
// Decalage de profondeur de la reception (glPolygonOffset facteur, unites).
// XBox : SLOPESCALE -2, ZOFFSET -4 (render.cpp:2327). "omb N f u".
extern float g_vita_ombre_pf;
extern float g_vita_ombre_pu;

// --- contrat CEngine (s_plat_*, XBox/p_nx.cpp:950-1010) ---------------------
Nx::CTexture *	OmbreCreerCible( int largeur, int hauteur );
void			OmbreProjeter( Nx::CTexture *p_tex, Nx::CModel *p_modele );
void			OmbreCamera( Nx::CTexture *p_tex, Gfx::Camera *p_cam );
void			OmbreArreter( Nx::CTexture *p_tex );

// --- capture de la pose (CVitaGeom::plat_render, phase logique) ------------
// Vrai si ce modele porte une ombre active : seuls ceux-la sont captures.
bool			OmbreModeleSuivi( const Nx::CModel *p_modele );
// p_racine : Mth::Matrix (16 flottants, translation en ligne 3).
// p_os : num_os matrices Mth consecutives.
void			OmbreCapturer( const void *p_geom, const Nx::CModel *p_modele,
				               const Nx::CVitaMesh *p_mesh, const float *p_racine,
				               const float *p_os, int num_os );
// Un geom detruit ne doit plus etre dessine dans la carte.
void			OmbreOublierGeom( const void *p_geom );

// --- passes ------------------------------------------------------------------
// Debut d'image (s_plat_pre_render), AVANT GammaImageDebut : rend la carte.
void			OmbreCarteRendu( void );
// Fin d'image (s_plat_post_render), AVANT GammaImageFin : vue de la carte
// en coin d'ecran si "omb 2/3".
void			OmbreVueCarte( void );

// Reception (p_world_render.cpp, fin de la passe opaque ; et, "omt 1", fin de
// la derniere passe translucide).
extern int		g_vita_ombre_transp;
// "omc 0/1" (#70) : decoupe materielle de la reception aux bords de la carte.
extern int		g_vita_ombre_decoupe;
// "omr 0/1" (#73) : reception limitee au rectangle ecran de la boite (tuiles GXM).
extern int		g_vita_ombre_region;
struct SOmbreReception
{
	float P[3], r[3], u[3], f[3];	// camera de lumiere de CETTE image
	float bmin[3], bmax[3];			// boite englobante de la boite de lumiere
};
// Ombres dont la carte a ete rendue a cette image. Rend leur nombre.
int				OmbreReceptions( SOmbreReception *p_out, int max );
// Active le programme de reception pour l'ombre i (meme ordre que
// OmbreReceptions). pv : projection x vue du decor (colonnes, s_pv_shader).
bool			OmbreReceptionDebut( int i, const float *pv );
// Un maillage du decor. masque : la texture du materiau decoupe l'ombre
// (alpha nul = pas d'ombre, test alpha XBox a 1).
void			OmbreReceptionMaillage( unsigned int vbo, unsigned int uvbo,
				                        unsigned int ibo, int num_indices,
				                        unsigned int texture, bool masque );
void			OmbreReceptionFin( void );
// Cout processeur de la reception, cumule pour le journal periodique
// (translucides : compte a part).
void			OmbreCompterReception( unsigned long long us, int maillages,
				                       bool translucides = false );

// --- auto-ombrage du skater (issue #45) --------------------------------------
// [SOURCE] XBox/NX/instance.cpp:494-668 : un modele dont la carte vient d'etre
// rendue (SCENE_FLAG_SELF_SHADOWS, render.cpp:2202) est redessine, TOUS ses
// maillages, en MODULATE_COLOR (dst x src), brouillard coupe, avec
// PixelShader_ShadowBuffer.psh : src = ( 1 - k ) + k x eclaire, eclaire = test
// de la carte (D3DRS_SHADOWFUNC GREATER, filtrage LINEAR = PCF 2x2). k = 0,25,
// attenue par la distance a la camera ramenee a un champ de 72 degres :
// nul sous 120, plein de 150 a 500, nul au-dela de 600 (instance.cpp:500-555).
// Vita : le meme facteur, multiplie a la sortie du shader de peau eclairee
// (p_shader_decor.cpp), sans second dessin.
// "aom 0/1/2 [b0 b1 [k]]" : 0 = coupe (defaut tant que non valide a l'ecran),
// 1 = comme XBox, 2 = diagnostic (k = 1, sans attenuation : les zones
// auto-ombrees sortent noires). b0, b1 : biais constant et de pente, en
// unites monde (la carte RGBA8 n'a que 8 bits sur 127 unites).
extern int   g_vita_auto_ombre;
extern float g_vita_aom_b0;
extern float g_vita_aom_b1;
extern float g_vita_aom_k;
// Vrai si p_modele projette une ombre dont la carte a ete rendue a cette
// image. O[0..11] : 3 lignes float4 qui menent un point en espace MODELE (pose
// skinnee) a ( u, v, profondeur 0..1 ) de la carte -- avec la racine et la
// camera de lumiere de la CARTE : le retard d'une image de la carte ne porte
// alors que sur l'animation, pas sur le deplacement du skater. O[13], O[14] :
// biais b0, b1 en unites de profondeur de la carte. O[12], O[15] non touches.
// *p_tex : texture GL de la carte.
bool			OmbreAutoOmbrage( const Nx::CModel *p_modele, float O[16], unsigned int *p_tex );

} // namespace NxVita

#endif // __GFX_VITA_P_OMBRE_H__
