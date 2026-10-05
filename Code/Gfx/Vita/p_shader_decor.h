///////////////////////////////////////////////////////////////////////////////
// p_shader_decor.h -- rendu du decor par shader, essai de l'option B
//
// Pourquoi : notre backend traduit les materiaux Xbox vers le pipeline FIXE
// d'OpenGL, que vitaGL retraduit en shaders (ffp.c). Deux traductions, dont
// la seconde perd des choses (2 couches de texture au plus, pas de generation
// de coordonnees, alpha DXT5...). Le moteur d'origine, lui, genere un shader
// par materiau ([SOURCE] XBox/NX/render.cpp:302, get_pixel_shader). L'essai :
// remplacer d'abord le cas de base, a l'identique, et mesurer.
//
// Etape 1 (ce fichier) : texture x couleur de sommet, test alpha optionnel --
// exactement ce que fait aujourd'hui le pipeline fixe pour les lots du decor.
// Aucun changement d'image attendu : c'est la condition pour comparer les
// performances a image egale (drapeau Â« shd Â», voir plus bas).

#ifndef __GFX_VITA_P_SHADER_DECOR_H__
#define __GFX_VITA_P_SHADER_DECOR_H__

#include <psp2/gxm.h>

namespace NxVita
{

// Â« shd N Â» : 0 arret, 1 opaques et translucides, 2 opaques, 3 translucides,
// 5 opaques peints en magenta (voir ce que le shader dessine, et ou).
extern int g_vita_shader_decor;

// Compile le programme au premier appel. Rend false si la compilation a
// echoue (libshacccg absent, erreur CG) : l'appelant garde alors le pipeline
// fixe.
bool ShaderDecorPret();

// mvp : projection x vue, en colonnes comme OpenGL (celle que calcule
// p_world_render.cpp, PAS une relecture par glGetFloatv). Active le programme
// et les trois tableaux de sommets, une fois avant une serie de dessins.
void ShaderDecorDebut( const float *mvp );

// Seuil de test alpha pour les dessins qui suivent, 0 = pas de test.
void ShaderDecorSeuil( float seuil );

// Tampons du dessin suivant : positions (3 flottants), UV (2 flottants),
// couleurs (4 octets normalises).
void ShaderDecorTampons( unsigned int vbo, unsigned int uvbo, unsigned int cbo );

// Rend la main au pipeline fixe.
void ShaderDecorFin();

// --- Etapes 2 et 3 : materiau jusqu'a 4 passes, formule Xbox exacte --------
// (voir p_shader_decor.cpp, [SOURCE] XBox/NX/render.cpp:302)

struct SShaderMateriau
{
	unsigned int vbo, cbo;				// cbo : couleurs de sommets NON teintees
	unsigned int uvbo[4];				// jeu d'UV de chaque passe (0 = absente)
	float        c[4][4];				// couleur de passe (0.5 = neutre), a = alpha fixe / 128
	unsigned int mode[4];				// mode de combinaison des passes 1..3 (0..13)
	bool         ignore_alpha[4];
	int          passes;				// 1..4
	float        seuil;					// test alpha, 0 = aucun
	// Environment mapping (issue #5) : bit p = passe p en reflet, sans jeu
	// d'UV ; ses coordonnees viennent de la normale (nbo) et de la vue.
	unsigned int env;
	unsigned int nbo;
	float        env_tile[4][2];
	// UV wibble (issue #43) : bit p = passe p dont les UV sont decales de
	// wib_uv[p] (offset deja reduit dans [-8, 8], XBox/NX/material.cpp:118).
	// Pose a chaque dessin : jamais dans un lot precalcule.
	unsigned int wib;
	float        wib_uv[4][2];
	// Mode *_FIXED en passe 0 (2, 4, 6, 8, 10) : le facteur de melange est
	// l'alpha FIXE du materiau, pas celui de la texture. XBox/NX/render.cpp
	// (D3DBLEND_CONSTANTALPHA, D3DRS_BLENDCOLOR = min( 2 fixa, 255 )) : le
	// shader rend alors c[0][3] (= fixa / 128) comme alpha de sortie. Le test
	// alpha garde l'alpha texture x sommet.
	bool         fixe0;
	// Brouillard NOIR (issue #45) : modes ADD..SUB_FIXED (1..4). [SOURCE]
	// XBox/NX/render.cpp:1174, set_blend_mode : " For additive and
	// subtractive, we set the fog color to black ". Uniforme de sommets : hors
	// de la cle de variante.
	bool         fog_noir;
};

bool ShaderMateriauPret();
// Compile a l'avance la variante de ce materiau (au chargement du niveau).
void ShaderMateriauPrecompiler( const SShaderMateriau *p );
int  ShaderMateriauNombreVariantes();
// Issue #69 : recherche des variantes par table de hachage (« vhs 0/1 ») au
// lieu du parcours lineaire de s_var. Meme variante rendue dans les deux cas.
extern bool g_vita_var_hash;
// Compteurs cumules depuis le dernier appel, puis remis a zero : variantes en
// table, recherches de variante, sondes (comparaisons de cle), et poses du
// programme de sommets + tampon d'uniformes de sommets du chemin GXM direct.
void ShaderMateriauStats( int *p_nb_var, int *p_recherches, int *p_sondes, int *p_poses_vs );
// Issue #69 (« vpr 0/1 ») : programme de sommets partage entre variantes de
// meme vertex shader, et reposes sautees quand il n'a pas change. Compteur
// des reposes evitees depuis le dernier appel, puis remis a zero.
extern bool g_vita_vpr;
int  ShaderMateriauPosesEvitees( void );
// Cle de variante : a trier pour limiter les changements de programme.
unsigned int ShaderMateriauCle( const SShaderMateriau *p );
void ShaderMateriauDebut( const float *mvp );
// Les textures sont liees par l'appelant : unite p = passe p.
// Faux : pas de variante (table pleine) -- l'appelant ne dessine pas.
bool ShaderMateriauMaillage( const SShaderMateriau *p );
void ShaderMateriauFin();

// --- Chemin GXM direct des lots de decor (issue #18) ------------------------
// Voir p_shader_decor.cpp. « gxd 0/1 ».
extern bool g_vita_gxm_direct;
void GxmMateriauDebut( const float *mvp );
// p_tex : descripteurs GXM des textures des passes (deja adresses). Rend false
// si la variante n'a pas de chemin GXM : l'appelant dessine alors par vitaGL.
// famille : mode de melange, voir GxmFamilleMelange (0 = opaque).
int  GxmFamilleMelange( unsigned int blend, bool translucide );
bool GxmMateriauLot( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                     const int *p_first, const int *p_count, int n_plages,
                     unsigned int ibo, int famille );
// Etats precalcules d'un lot (textures, couleurs, flux, plage d'index
// [first, first+count[) : rend un bloc opaque, NULL si impossible.
void *GxmMateriauPreparer( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                           int first, int count, unsigned int ibo, int famille );
void  GxmMateriauLotPre( void *p_pre );
// Rend la memoire d'un bloc de GxmMateriauPreparer (liberation differee :
// le GPU peut encore lire l'image en cours). NULL accepte.
void  GxmMateriauLibererPre( void *p_pre );
// Matrice de VUE (colonnes, OpenGL) pour les passes en reflet (issue #5).
void GxmMateriauVue( const float *view );
// Un maillage SEUL (bande de triangles, son propre IBO), meme session.
bool GxmMateriauMaillage( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                          unsigned int ibo, int num_indices, int famille );
// A appeler avant tout dessin vitaGL qui suit des dessins GXM directs.
void GxmMateriauFin();
// Listes de commandes GXM (contexte differe), « gcl 0/1 » : entre Debut et
// Fin, les dessins GXM sont enregistres, puis executes a la Fin.
extern bool g_vita_listes_gxm;
bool GxmListeDebut( void );
void GxmListeFin( void );
// Lots opaques sur un autre coeur (« lmd 0/1 », issue #18) : entre
// GxmTravDebut et GxmTravFin (thread principal), les lots precalcules passes a
// GxmTravPousser sont enregistres par un travailleur dans une liste de
// commandes, executee a GxmTravFin. Faux : dessiner soi-meme.
extern bool g_vita_lots_mt;
bool GxmTravDebut( const float *mvp, const float *vue );
bool GxmTravPousser( void *p_pre );
void GxmTravFin( void );

// --- Personnages : skinning par le GPU (issue #18) --------------------------
bool ShaderPeauPret();
int  ShaderPeauMaxOs();
// mvp : projection x modelview (colonnes, OpenGL) ; p_os : num_os matrices Mth.
void ShaderPeauDebut( const float *mvp, const float *p_os, int num_os );
// La texture est liee par l'appelant (unite 0).
void ShaderPeauPiece( unsigned int vbo_repos, unsigned int vbo_poids, unsigned int vbo_os,
                      unsigned int uvbo, bool avec_texture, const float teinte[4] );
void ShaderPeauFin();
// Variante ECLAIREE du chemin vitaGL (issue #4), formule de
// XBox/NX/WeightedMeshVS_VXC_3Weight.vsh + PixelShader0.psh. lum : meme
// tableau de 15 flottants que GxmPeauDebut. vbo_normales / cbo : 0 si absents.
bool ShaderPeauEclaireePret();
void ShaderPeauEclaireeDebut( const float *mvp, const float *p_os, int num_os,
                              const float *lum );
void ShaderPeauEclaireePiece( unsigned int vbo_repos, unsigned int vbo_poids, unsigned int vbo_os,
                              unsigned int uvbo, bool avec_texture, const float teinte[4],
                              unsigned int vbo_normales, unsigned int cbo,
                              const float *spec = NULL,	// couleur + puissance, NULL = sans
                              unsigned int gloss_tex = 0,	// GLOSS_MAP (#45) : texture de la passe 1
                              unsigned int gloss_uvbo = 0,	// et son jeu d'UV ; 0 = sans
                              int gloss_clamp = 0 );		// bit 0 U borne, bit 1 V borne
// Auto-ombrage du skater (issue #45, p_ombre.h), APRES ShaderPeauEclaireeDebut
// (qui le remet a neutre) : O = 3 lignes espace modele -> carte, tex = carte
// (unite 1), k = part d'ombre (XBox 0,25), b0 / b1 = biais constant / de
// pente en unites de profondeur de la carte.
void ShaderPeauEclaireeAutoOmbre( const float *O, unsigned int tex, float k, float b0, float b1 );
void ShaderPeauEclaireeFin();

// --- Modeles RIGIDES eclaires par le GPU (issue #67, "rgp 1") --------------
// Meme formule qu'eclairer_piece (p_NxModel.cpp) : couleur = sat( cbo x k x
// ( ambiante + somme couleur_i x max( 0, N.dir_i ))), k = 0,5 si texture ;
// puis texture x couleur x 2 (GL_COMBINE / GL_RGB_SCALE 2) si texture.
// lum : les 21 flottants de lumieres_rigides (ambiante, puis 3 x direction
// dans l'espace du modele et couleur). La texture est liee par l'appelant
// (unite 0) ; sans texture, une texture blanche 1x1 est liee ici.
// seuil_alpha < 0 : pas de test alpha ; sinon rejet si alpha <= seuil
// (GL_GREATER). fog_noir : brouillard noir (ADD/SUB, BrouillardFixeNoir).
bool ShaderRigideEclairePret();
void ShaderRigideEclaireDebut( const float *lum );
void ShaderRigideEclairePiece( const float *mvp, unsigned int vbo, unsigned int nbo,
                               unsigned int cbo, unsigned int uvbo, bool avec_texture,
                               float seuil_alpha, bool fog_noir );
void ShaderRigideEclaireFin();

// Meme dessin en GXM DIRECT : un seul jeu d'uniformes de sommets par appel de
// GxmPeauDebut (au lieu d'une recopie de 3 Ko par glDrawElements dans
// vitaGL). Entre GxmPeauDebut et GxmPeauFin, aucun appel GL de dessin.
extern bool g_vita_gxm_peau;
bool GxmPeauPret();
// Variante eclairee (issue #4).
bool GxmPeauEclaireePret();
// lum : NULL = sans eclairage ; sinon 15 flottants -- ambiante (3), puis pour
// chacune des deux lumieres sa direction dans l'espace du MODELE (3) et sa
// couleur (3).
void GxmPeauDebut( const float *mvp, const float *p_os, int num_os, bool remplir_os,
                   const float *lum );
void GxmPeauPiece( unsigned int vbo_repos, unsigned int vbo_poids, unsigned int vbo_os,
                   unsigned int uvbo, void *p_tex, bool avec_texture, const float teinte[4],
                   unsigned int ibo, int num_indices, unsigned int vbo_normales );
void GxmPeauFin();

// --- Brouillard (issue #45) --------------------------------------------------
//
// [SOURCE] XBox : brouillard LINEAIRE par table (XBox/NX/nx_init.cpp:181,
// D3DFOG_LINEAR), debut = -distance, fin = debut - 600 pieds
// (XBox/p_nxmiscfx.cpp:1192-1197), densite = min( a / 128, 1 ) dans c4
// (p_nxmiscfx.cpp:1265-1273), puis chaque pixel shader finit par
//     xfc prod, fog.rgb, sum, zero, 1 - fog.a, c4, r0.a
// (XBox/NX/render.cpp:979) : couleur = k x brouillard + (1 - k) x couleur,
// k = densite x ( 1 - f ), f = clamp(( fin - d ) / ( fin - debut ), 0, 1),
// d = profondeur en vue (oFog.x = -w, WeightedMeshVS_VXC_*.vsh).
// Chez nous : k et k x couleur calcules par le VERTEX shader (vFog), d'apres
// deux uniformes de sommets -- les etats fragment des lots precalcules sont
// figes, pas ceux des sommets. Le pixel fait r = r x (1 - k) + k x couleur.

// "fog 0/1" (p_siodev.cpp).
extern int g_vita_fog;
void BrouillardActiver( bool actif );
void BrouillardDistance( float proche );
void BrouillardCouleur( int r, int g, int b, int a );
bool BrouillardActif( void );
// Uniformes : P = ( densite, fin, 1 / ( fin - debut ), 0 ) en distances
// positives, C = couleur (noire pour ADD/SUB). ciel : fin = 21, plage 1
// (XBox/p_nx.cpp:325, le ciel est entierement embrume). Inactif : P = 0.
void BrouillardUniformes( bool noir, bool ciel, float P[4], float C[4] );
// Pipeline fixe (ciel, maillages hors shader, modeles sur CPU) : glFog de
// vitaGL. mode 0 = coupe, 1 = decor, 2 = ciel. Couleur noire a part.
void BrouillardFixe( int mode );
void BrouillardFixeNoir( bool noir );

// Commun a tous les vertex shaders du decor, des personnages et des
// particules. W = w de clip = profondeur en vue (projection OpenGL), recalcule
// plutot que relu dans le parametre de sortie vPosition. uFogP =
// ( densite, fin, 1 / plage, 0 ), uFogC = couleur. Voir BrouillardUniformes.
// k = densite x ( 1 - clamp(( fin - w ) / plage, 0, 1 )) ; le pixel shader
// fait couleur x ( 1 - k ) + k x brouillard, comme le xfc final des pixel
// shaders XBox (XBox/NX/render.cpp:979). Le shader doit declarer
// uniform float4 uFogP, uniform float4 uFogC et float4 out vFog : TEXCOORD4.
#define BROUILLARD_VS( W ) \
	"	{\n" \
	"		float fz = saturate((uFogP.y - " W ") * uFogP.z);\n" \
	"		float fk = uFogP.x * (1.f - fz);\n" \
	"		vFog = float4(uFogC.rgb * fk, 1.f - fk);\n" \
	"	}\n"

}

#endif // __GFX_VITA_P_SHADER_DECOR_H__
