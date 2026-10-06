/*****************************************************************************
**  THUG-Vita â€” backend graphique                                            **
**  Code/Gfx/Vita/p_NxModel.cpp                                             **
**                                                                          **
**  Modeles : le skater, les objets de niveau, les elements animes.         **
**  Voir p_NxModel.h pour la chaine du moteur.                              **
*****************************************************************************/

#include <core/defines.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <psp2/kernel/processmgr.h>

#include <sys/file/filesys.h>
#include <sys/timer.h>
#include "vita_log.h"
#include <gfx/NxLightMan.h>
#include "p_NxLight.h"
#include "p_NxModel.h"
#include "p_shader_decor.h"
namespace NxVita { extern int g_vita_gp_reutil[2]; }
namespace Nx { class CVitaGeom; }
namespace NxVita { extern bool g_vita_sprite_dump; }
namespace NxVita { extern bool g_vita_uvw; }	// "uvw" (#43), p_world_render.cpp
namespace NxVita { void EnregistrerInstance( Nx::CVitaGeom * ); void OublierInstance( Nx::CVitaGeom * ); }
#include "p_world_render.h"		// sommets de rendu des secteurs (#5)
#include "p_NxTexture.h"
#include "p_scene_load.h"
#include "p_world_render.h"
#include "p_ombre.h"

// CHierarchyObject : type complet requis pour lire les matrices de setup
// des modeles a parties rigides (vehicules).
#include <gfx/NxHierarchy.h>
#include <sk/modules/frontend/frontend.h>	// GamePaused (#54)

namespace Nx
{

// Liste les noms des modeles dessines, une seule fois chacun. « tm 1 ».
// Chaque armement repart d'une liste vide (voir plat_render).
bool g_vita_trace_modeles = false;
// Pose par la commande « tm 1 » : vide la liste au prochain dessin.
bool g_vita_trace_reset   = false;
bool g_vita_trace_reset_inactifs = false;

// --- MORCEAUX TRANSLUCIDES REPORTES APRES LE DECOR -------------------------
//
// Nos modeles sont dessines pendant la phase logique, donc AVANT le decor.
// Pour un morceau translucide, les deux comportements possibles sont faux :
//   - s'il ecrit la profondeur, il masque le decor la ou il est transparent
//     (rectangles a la couleur d'effacement autour des halos) ;
//   - s'il ne l'ecrit pas, le decor le recouvre entierement (roues incompletes,
//     objets a ramasser invisibles).
// Les deux ont ete constates a l'ecran. Aucun reglage ne peut etre bon : c'est
// l'ORDRE qui est faux.
//
// XBox dessine les modeles semi-transparents APRES le decor (etapes 10 et 14
// de NOTES/pipeline-rendu-xbox.md). On fait pareil : au lieu de dessiner un
// morceau translucide sur-le-champ, on retient tout ce qu'il faut pour le
// rejouer, et on vide la file une fois le decor pose.
//
// On capture la matrice modelview COMPLETE au moment ou le morceau aurait ete
// dessine : elle contient deja la vue, la pose du modele et la matrice d'os.
// Rien a recalculer a la vidange.
struct SMorceauReporte
{
	const SGpuMesh *p_piece;
	float           mv[16];
	float           rgba[4];
	bool            a_rgba;
};

#define MAX_REPORTES 256
static SMorceauReporte s_reportes[MAX_REPORTES];
static int             s_num_reportes = 0;

// Meme chose pour les morceaux translucides des modeles SKINNES dessines par
// le shader de peau vitaGL (#46). Dessines pendant game_logic, AVANT le decor,
// sans ecrire la profondeur : le decor les recouvrait entierement -- pietons
// sans torse (ped_Bum1 : torse et coiffe en BLEND), visibles seulement devant
// une voiture (dont la profondeur, deja ecrite, empechait le decor derriere).
// On copie tout ce qu'il faut pour les rejouer apres le decor : matrices
// d'os, mvp, eclairage, teinte.
extern int g_vita_mbf;
extern int g_vita_zw_peau;
static void peau_cull( const SGpuMesh *p )
{
	if( g_vita_mbf && !p->no_bfc )
	{
		glEnable( GL_CULL_FACE );
		glCullFace( GL_BACK );
	}
	else
		glDisable( GL_CULL_FACE );
}

#define MAX_PEAU_REPORTEES 48
#define PEAU_REPORT_OS     64
struct SPeauReportee
{
	const SGpuMesh *p_piece;
	float           mvp[16];
	float           os[PEAU_REPORT_OS * 16];
	int             num_os;
	bool            eclaire;
	float           lum[21];
	float           teinte[4];
	bool            a_aom;			// auto-ombrage (#45)
	unsigned int    aom_tex;
	float           aom[16];
};
static SPeauReportee s_peau_reportees[MAX_PEAU_REPORTEES];
static int           s_num_peau_reportees = 0;

// Coupe-circuit : « dm 0 » revient au dessin immediat, pour comparer.
bool g_vita_differer_modeles = true;
// "mbf 0/1" : faces arriere des personnages eliminees selon le materiau (#46).
int g_vita_mbf = 1;
// "pzw 0/1" : morceaux translucides des personnages ecrivent la profondeur,
// avec alpha test 1/255 dans le shader, comme XBox (render.cpp:1518) (#46).
int g_vita_zw_peau = 1;
// "mro 0/1" (issue #69) : vidange des morceaux translucides RIGIDES sans les
// etats refaits a l'identique d'un morceau au suivant (melange active,
// profondeur coupee, meme facteur de melange, meme matrice -- les morceaux
// d'un meme modele et d'un meme os se suivent --, meme couleur). Ordre de
// dessin inchange ; a 0, chaque morceau repose tout, comme avant.
bool g_vita_mro = true;
// "gyr 0/1" (issue #77) : UV WIBBLE des modeles -- defilement de la tache
// claire des gyrophares de police, scintillement des halos de phares. Le
// decor l'avait depuis #43, les modeles jamais. Suit aussi "uvw".
bool g_vita_gyr = true;
// "pad 0/1" (issue #77) : PASSE 1 ADDITIVE des pieces rigides opaques,
// redessinee par-dessus la base -- l'eclat des gyrophares. Sans elle, la
// rampe n'est qu'un aplat rouge et bleu sombre.
bool g_vita_pad = true;

// UV wibble d'une passe de modele (#77) : matrice de texture de l'unite 0,
// comme le chemin fixe du decor (p_world_render.cpp, uvw_fixe) et XBox
// (D3DTS_TEXTURE0, XBox/NX/material.cpp:444) : u' = u + uoff, v' = v + voff.
// Rend vrai si une matrice a ete posee ; uvw_modele_lever la retire.
static bool uvw_modele_poser( unsigned char actif, const float *p_par )
{
	if( !actif || !g_vita_gyr || !NxVita::g_vita_uvw )
		return false;
	float d[2];
	NxVita::UVWibbleDecalage( p_par, (float)Tmr::GetTime() * 0.001f, d );
	glMatrixMode( GL_TEXTURE );
	glLoadIdentity();
	glTranslatef( d[0], d[1], 0.0f );
	glMatrixMode( GL_MODELVIEW );
	return true;
}

static void uvw_modele_lever( void )
{
	glMatrixMode( GL_TEXTURE );
	glLoadIdentity();
	glMatrixMode( GL_MODELVIEW );
}



// Interrupteur d'eclairage, reglable a chaud par la commande ï¿½ lum 0 ï¿½ / ï¿½ lum 1 ï¿½
// (voir p_siodev.cpp). Sert a repondre par la MESURE a ï¿½ est-ce l'eclairage
// qui coute ? ï¿½ : on compare le meme compteur dans les deux etats, sur la
// meme scene, sans recompiler.
// COUPE PAR DEFAUT, et ce n'est pas un abandon : l'integration de l'eclairage
// materiel est incomplete, et une version a moitie juste est pire que pas
// d'eclairage du tout. Trois defauts identifies, tous dans l'ETAT GL :
//
//  1. glLightfv( GL_POSITION ) est transforme par la MODELVIEW COURANTE. Or
//     plat_render charge ï¿½ vue x matrice du modele ï¿½ avant de dessiner : chaque
//     modele pose donc ses lumieres dans SON repere, et la direction bouge avec
//     lui. C'est la cause des variations et du clignotement.
//     -> il faut poser les lumieres quand la modelview ne contient QUE la vue.
//
//  2. GL_COLOR_MATERIAL est active sans glColorMaterial() explicite, alors que
//     glColor4f est pose a trois endroits differents (teinte, blanc, gris).
//     La couleur de matiere devient imprevisible -- d'ou les teintes jaunes.
//     -> soit glColorMaterial( GL_FRONT, GL_AMBIENT_AND_DIFFUSE ) explicite,
//        soit glMaterialfv et pas de color-material du tout.
//
//  3. Aucun glMaterialfv n'est pose : le materiau OpenGL par defaut (diffuse
//     0,8, ambiante 0,2) s'ajoute a tout ce qu'on calcule.
//
// La commande ï¿½ lum 1 ï¿½ reste, pour reprendre le chantier et mesurer.
// Actif depuis le 2026-10-03 : shader de peau eclaire valide contre xemu
// (Manhattan, ombrage du skater comparable, 1,6 ms pour les modeles). #4.
bool g_vita_lighting = true;

// Decomposition du temps de dessin. Mesurer un bloc opaque m'a fait accuser
// deux fois le mauvais coupable ; on separe donc le calcul (skinning +
// eclairage) des appels GL.
SceUInt64 g_vita_t_skin = 0;
SceUInt64 g_vita_t_draw = 0;
// Nombre d'appels de dessin : 18 pieces par skater, c'est la cible du
// point 2 du plan (fusionner celles qui partagent texture et blend).
int       g_vita_n_draws = 0;
// Sommets REELLEMENT transformes par frame. 3,7 ms pour un personnage, c'est
// une quinzaine de fois le temps que demandent ~2000 sommets x 3 os : soit on
// en transforme bien plus qu'on ne croit, soit la boucle est tres mal servie
// par le CPU. Ce compteur tranche.
int       g_vita_n_verts = 0;
// « gsk 0/1 » : skinning des personnages par le GPU (issue #18).
bool      g_vita_skin_gpu = true;
// Matrice racine du geom en cours de rendu (plat_render -> DrawSkinned).
static Mth::Matrix *sp_racine_courante = NULL;
// Modele du geom en cours de rendu (auto-ombrage #45 : est-ce celui qui
// projette l'ombre ?). Seulement compare, jamais dereference.
static const CModel *sp_modele_courant = NULL;

// AUTO-OMBRAGE DU SKATER (issue #45, "aom", p_ombre.h).
// [SOURCE] XBox/NX/instance.cpp:497-555 : distance camera -> modele, ramenee a
// un champ horizontal de 72 degres ; part d'ombre 0,25 pleine de 150 a 500,
// en rampe de 120 a 150 et de 500 a 600, nulle ailleurs. Ici la distance est
// la translation de la modelview (la racine y est multipliee, plat_render),
// et tan( angle / 2 ) = 1 / proj[0] (angle HORIZONTAL, comme
// EngineGlobals.screen_angle, XBox/NX/render.cpp:1792).
// aom[0..11] : lignes vers la carte, [12] part d'ombre, [13..14] biais.
static bool auto_ombre( const float *mv, const float *proj, float aom[16], unsigned int *p_tex )
{
	if( !NxVita::g_vita_auto_ombre || !sp_modele_courant )
		return false;
	if(( proj[15] != 0.0f ) || ( fabsf( proj[0] ) < 1e-6f ))
		return false;		// projection orthogonale : pas de camera a distance
	const float SHADOW_FADE_CLOSE_START    = 150.0f;
	const float SHADOW_FADE_CLOSE_COMPLETE = 120.0f;
	const float SHADOW_FADE_FAR_START      = 500.0f;
	const float SHADOW_FADE_FAR_COMPLETE   = 600.0f;
	const float DEFAULT_SCREEN_ANGLE       = 0.72654f;	// tan( 72 / 2 )
	const float dist = sqrtf( mv[12] * mv[12] + mv[13] * mv[13] + mv[14] * mv[14] )
	                   * ( 1.0f / fabsf( proj[0] )) / DEFAULT_SCREEN_ANGLE;
	float k = NxVita::g_vita_aom_k;
	if( NxVita::g_vita_auto_ombre == 2 )
		k = 1.0f;			// diagnostic : pleine ombre, sans attenuation
	else
	{
		if(( dist <= SHADOW_FADE_CLOSE_COMPLETE ) || ( dist >= SHADOW_FADE_FAR_COMPLETE ))
			return false;
		if( dist < SHADOW_FADE_CLOSE_START )
			k *= ( dist - SHADOW_FADE_CLOSE_COMPLETE ) / ( SHADOW_FADE_CLOSE_START - SHADOW_FADE_CLOSE_COMPLETE );
		else if( dist > SHADOW_FADE_FAR_START )
			k *= ( SHADOW_FADE_FAR_COMPLETE - dist ) / ( SHADOW_FADE_FAR_COMPLETE - SHADOW_FADE_FAR_START );
	}
	if( k <= 0.0f )
		return false;
	if( !NxVita::OmbreAutoOmbrage( sp_modele_courant, aom, p_tex ))
		return false;
	aom[12] = k;
	aom[15] = 0.0f;
	return true;
}

// SONDE « pdt » (issue #46 : tete des pietons generiques decalee vers le
// haut). Pendant UNE image, au plus PDT_MAX_LIGNES lignes [PDT] : nom,
// chemin de dessin, racine, centre des sommets skinnes AU CPU avec la
// formule exacte de la boucle CPU de DrawSkinned. Ne change pas le rendu.
int g_vita_pdt = 0;				// 1 = armee (p_siodev), 2 = image en cours
static int         s_pdt_chemin  = 0;	// 0 Draw, 1 GXM, 2 vitaGL lum, 3 vitaGL, 4 CPU
static const void *s_pdt_premier = NULL;
static int         s_pdt_lignes  = 0;
static int         s_pdt_appels  = 0;
static SceUInt64   s_pdt_t0      = 0;
#define PDT_MAX_LIGNES 80

// --- CVitaMesh -------------------------------------------------------------

// Maillages de modele vivants (diagnostic de fuite, issue #32).
int g_vita_meshes_vivants = 0;
// Registre des maillages vivants (#47) : qui survit aux changements de niveau.
static CVitaMesh *s_meshes[4096];
static int s_nb_meshes = 0;

CVitaMesh::CVitaMesh()
	: mp_pieces( NULL ), m_num( 0 ),
	  mp_cas( NULL ), m_num_cas( 0 ), m_cas_applied( 0xFFFFFFFFu )
{
	// 0 serait une valeur legitime de masque : on part donc d'une valeur
	// impossible, sinon le tout premier HidePolys(0) serait ignore comme
	// ï¿½ deja dans cet etat ï¿½.
	m_CASRemovalMask = 0;
	m_vita_name[0]   = 0;
	m_vita_eclairable = false;
	m_vita_dossier[0] = 0;
	m_bbox.Reset();
	// CMesh::CMesh ne l'initialise pas ; sUnloadMesh (nx.cpp:921) le lit.
	mp_texDict       = NULL;
	m_vita_refs      = 0;
	m_vita_orphelin  = false;
	++g_vita_meshes_vivants;
	if( s_nb_meshes < 4096 )
		s_meshes[s_nb_meshes++] = this;
}

// #48 CLIGNOTEMENT : glFinish() coupe la scene GXM en cours (vitaGL gxm.c :
// dirty_framebuffer + scene_reset) et la profondeur ne survit pas a la coupure.
// Or nos modeles sont dessines PENDANT la logique, avant le decor : un pieton
// ou une voiture liberes en pleine image faisaient perdre la profondeur de
// tous les modeles deja dessines, et le decor les recouvrait -- personnages
// et vehicules absents une image. Pendant une image, on differe donc les
// destructions au debut de l'image suivante (VitaLibererDifferes), ou un
// seul glFinish ne coupe rien.
bool g_vita_image_ouverte = false;
int  g_vita_differer_lib = 1;	// "dlb 0/1"
static const int MAX_DIFFERES = 256;
static CVitaMesh *s_mesh_differes[MAX_DIFFERES];
static int s_nb_mesh_differes = 0;
static int s_nb_finish_evites = 0;
static bool s_finish_en_attente = false;

bool VitaFinishDifferable( void )
{
	if( !g_vita_image_ouverte || !g_vita_differer_lib )
		return false;
	s_finish_en_attente = true;
	++s_nb_finish_evites;
	return true;
}

static bool differer_mesh( CVitaMesh *m )
{
	if( !g_vita_image_ouverte || !g_vita_differer_lib || ( s_nb_mesh_differes >= MAX_DIFFERES ))
		return false;
	s_mesh_differes[s_nb_mesh_differes++] = m;
	++s_nb_finish_evites;
	return true;
}

// Appelee en tete d'image (s_plat_pre_render), avant tout dessin.
void VitaLibererMeshDifferes( void )
{
	if( s_nb_mesh_differes || s_finish_en_attente )
	{
		s_finish_en_attente = false;
		glFinish();
		for( int i = 0; i < s_nb_mesh_differes; ++i )
			delete s_mesh_differes[i];
		s_nb_mesh_differes = 0;
	}
	static SceUInt64 s_t = 0;
	const SceUInt64 t = sceKernelGetProcessTimeWide();
	if(( t - s_t > 5000000 ) && s_nb_finish_evites )
	{
		VLOG( "CHUTE", "glFinish evites en cours d'image (5 s) : %d", s_nb_finish_evites );
		s_nb_finish_evites = 0; s_t = t;
	}
}
void VitaCompterFinishEvite( void ) { ++s_nb_finish_evites; }

// Bilan au vidage du monde : maillages vivants regroupes par nom (#47).
void VitaBilanMeshes( void )
{
	struct { const char *nom; int n, refs, orph; } t[64];
	int nt = 0;
	for( int i = 0; i < s_nb_meshes; ++i )
	{
		CVitaMesh *m = s_meshes[i];
		const char *nm = m->Name()[0] ? m->Name() : "?";
		int k = 0;
		while(( k < nt ) && strcmp( t[k].nom, nm ))
			++k;
		if( k == nt )
		{
			if( nt == 64 ) continue;
			t[nt].nom = nm; t[nt].n = t[nt].refs = t[nt].orph = 0; ++nt;
		}
		++t[k].n; t[k].refs += m->m_vita_refs; t[k].orph += m->m_vita_orphelin;
	}
	VLOG( "FUITE", "%d maillages vivants, %d noms differents", s_nb_meshes, nt );
	for( int k = 0; k < nt; ++k )
		if( t[k].n > 1 )
			VLOG( "FUITE", "  %3d x %s (refs %d, orphelins %d)", t[k].n, t[k].nom, t[k].refs, t[k].orph );
}

void CVitaMesh::Lacher()
{
	if( --m_vita_refs <= 0 && m_vita_orphelin )
	{
		if( differer_mesh( this ))
			return;
		glFinish();		// le GPU peut encore lire ses tampons (s_plat_finish_rendering)
		delete this;
	}
}

void CVitaMesh::Decharger()
{
	if( m_vita_refs > 0 )
	{
		m_vita_orphelin = true;		// le dernier geom le detruira
		return;
	}
	if( differer_mesh( this ))
		return;
	glFinish();
	delete this;
}

CVitaMesh::~CVitaMesh()
{
	--g_vita_meshes_vivants;
	for( int i = 0; i < s_nb_meshes; ++i )
		if( s_meshes[i] == this )
		{
			s_meshes[i] = s_meshes[--s_nb_meshes];
			break;
		}
	for( int i = 0; i < m_num; ++i )
	{
		// Issue #32 : cbo et vbo_normales n'etaient jamais rendus -- une fuite
		// de VRAM par morceau de modele a chaque dechargement. vbo_normales
		// est partage entre morceaux de memes sommets : un seul delete.
		if( mp_pieces[i].cbo )
			glDeleteBuffers( 1, &mp_pieces[i].cbo );
		if( mp_pieces[i].vbo_normales )
		{
			bool deja = false;
			for( int k = 0; k < i; ++k )
				if( mp_pieces[k].vbo_normales == mp_pieces[i].vbo_normales )
					deja = true;
			if( !deja )
				glDeleteBuffers( 1, &mp_pieces[i].vbo_normales );
		}
		if( mp_pieces[i].vbo )		// 0 pour une passe 1 (#55)
			glDeleteBuffers( 1, &mp_pieces[i].vbo );
		if( mp_pieces[i].uvbo )
			glDeleteBuffers( 1, &mp_pieces[i].uvbo );
		if( mp_pieces[i].gloss_uvbo )		// GLOSS_MAP (#45), propre a la piece
			glDeleteBuffers( 1, &mp_pieces[i].gloss_uvbo );
		if( mp_pieces[i].add_uvbo )			// passe 1 ADD (#77), propre a la piece
			glDeleteBuffers( 1, &mp_pieces[i].add_uvbo );
		if( mp_pieces[i].add_cbo )
			glDeleteBuffers( 1, &mp_pieces[i].add_cbo );
		glDeleteBuffers( 1, &mp_pieces[i].ibo );
		if( mp_pieces[i].owns_verts )
		{
			free( mp_pieces[i].p_positions );
			free( mp_pieces[i].p_weights );
			free( mp_pieces[i].p_bones );
			free( mp_pieces[i].p_normals );
		}
		if( mp_pieces[i].owns_skin )
		{
			free( mp_pieces[i].p_skinned );
			free( mp_pieces[i].p_skinned_n );
			free( mp_pieces[i].p_lit_base );
			if( mp_pieces[i].vbo_skin )
				glDeleteBuffers( 1, &mp_pieces[i].vbo_skin );
			if( mp_pieces[i].vbo_repos ) glDeleteBuffers( 1, &mp_pieces[i].vbo_repos );
			if( mp_pieces[i].vbo_poids ) glDeleteBuffers( 1, &mp_pieces[i].vbo_poids );
			if( mp_pieces[i].vbo_os )    glDeleteBuffers( 1, &mp_pieces[i].vbo_os );
		}
		if( mp_pieces[i].cbo_lit )
			glDeleteBuffers( 1, &mp_pieces[i].cbo_lit );
		// p_lit est propre a la piece (sa teinte lui appartient), donc libere
		// sans condition de propriete -- contrairement aux sommets partages.
		free( mp_pieces[i].p_lit );
		free( mp_pieces[i].p_indices_src );
		free( mp_pieces[i].p_n_rig );
		free( mp_pieces[i].p_couleurs_brutes );
		free( mp_pieces[i].p_c_base );
		free( mp_pieces[i].p_c_lit );
		free( mp_pieces[i].p_lum_idx );
		free( mp_pieces[i].p_uv_src );		// #16, propre a la piece
		if( mp_pieces[i].cbo_rig )
			glDeleteBuffers( 1, &mp_pieces[i].cbo_rig );
		if( mp_pieces[i].nbo_rig )		// #67, propre a la piece
			glDeleteBuffers( 1, &mp_pieces[i].nbo_rig );
	}
	free( mp_pieces );
	free( mp_cas );
}


// Passe 1 d'un maillage de PERSONNAGE dessinable en piece translucide (#55) :
// skinne, une deuxieme couche texturee (texture_checksum2 est deja a 0 pour
// une couche en reflet, p_scene_load.cpp), son jeu d'UV, et un melange BLEND
// (5) -- 96 % des passes 1 des pieces de skater_male, mesure sur table. Les
// autres modes (11 GLOSS_MAP, 6 BLEND_FIXED : 2 materiaux) restent ignores.
//
// Issue #16 : meme chose pour la PASSE 2, dessinee apres la passe 1. Le CAS y
// place le tatouage du dos, ceux des avant-bras, le logo du dos du t-shirt et
// deux calques de planche (cas_parts.q:138, master_uv_list) : sans elle, ils
// n'apparaissaient pas du tout. Champs de la passe k (1 ou 2) :
struct SPasseDecal
{
	unsigned int	tex;		// checksum de texture
	const float *	p_uv;		// jeu d'UV consomme
	unsigned int	blend, flags;
	const float *	color;		// 0.5 = neutre
	unsigned char	au, av;
};

static bool passe_decal( const NxVita::SVitaMesh *p_src, int k, SPasseDecal *d )
{
	if( k == 1 )
	{
		d->tex = p_src->texture_checksum2;   d->p_uv  = p_src->p_uvs_couche1;
		d->blend = p_src->blend_mode2;       d->flags = p_src->mat_flags2;
		d->color = p_src->mat_color2;
		d->au = p_src->addr2_u;              d->av = p_src->addr2_v;
	}
	else
	{
		const NxVita::SVitaPasse *x = &p_src->passe_x[k - 2];
		d->tex = x->texture_checksum;        d->p_uv  = x->p_uvs;
		d->blend = x->blend;                 d->flags = x->flags;
		d->color = x->color;
		d->au = x->addr_u;                   d->av = x->addr_v;
	}
	return true;
}

static bool passe_k_dessinable( const NxVita::SVitaMesh *p_src, int k, CTexDict *p_tex_dict )
{
	if( !p_tex_dict || !p_src->p_weights || !p_src->p_bones )
		return false;
	if( p_src->num_passes < (unsigned int)( k + 1 ))
		return false;
	SPasseDecal d;
	passe_decal( p_src, k, &d );
	if( !d.tex || !d.p_uv )
		return false;
	if( d.blend != 5 )
		return false;
	// Passe 2 : seulement par-dessus une passe 1 BLEND dessinee, le cas du
	// CAS (tatouages, logos, calques) -- pas de saut de passe a interpreter.
	if(( k == 2 ) && !passe_k_dessinable( p_src, 1, p_tex_dict ))
		return false;
	if( !p_src->p_indices || ( p_src->num_indices <= 0 ))
		return false;
	CTexture *p_tex = p_tex_dict->GetTexture( d.tex );
	return p_tex && ((CVitaTexture *)p_tex )->GetGLTexture();
}

// Etat de dessin d'une passe 1 (#55), texture DEJA liee sur l'unite 0 :
// adressage de la couche (sinon le logo se repete en damier sur tout le
// torse) et test de profondeur LEQUAL -- la passe 1 est COPLANAIRE de sa
// base, deja dessinee avec les memes sommets, le meme programme et la meme
// matrice : profondeur identique, qu'un test strict rejetterait.
static void poser_passe1( const SGpuMesh *p )
{
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
	                 p->decal_au ? GL_CLAMP_TO_EDGE : GL_REPEAT );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
	                 p->decal_av ? GL_CLAMP_TO_EDGE : GL_REPEAT );
	glDepthFunc( GL_LEQUAL );
}

// Construction GPU commune : VBO/IBO, partage des sommets par secteur,
// conservation des indices d'origine pour le retrait CAS, textures. La
// liberation de la geometrie source appartient a l'APPELANT.
void CVitaMesh::build_pieces( NxVita::SVitaSceneGeom *p_geom, CTexDict *p_tex_dict )
{
	// [SOURCE] XBox/p_nx.cpp:844, pMesh->SetTexDict( pTexDict ). Jamais fait
	// ici : CModel::AddGeom recopiait donc un dictionnaire NULL, et
	// CModel::ReplaceTexture sautait chaque piece sans rien dire -- plus de
	// visage CAS, de teinte de peau, d'yeux. Et sUnloadMesh ne rendait jamais
	// le dictionnaire au gestionnaire.
	mp_texDict = p_tex_dict;

	// Une piece de plus par maillage skinne dont la passe 1 est dessinable
	// (#55, voir passe_k_dessinable), et une par passe 2 (#16).
	int num_passes1 = 0;
	for( int i = 0; i < p_geom->num_meshes; ++i )
		for( int k = 1; k <= 2; ++k )		// passe 2 : #16
			if( passe_k_dessinable( &p_geom->p_meshes[i], k, p_tex_dict ))
				++num_passes1;
	mp_pieces = (SGpuMesh *)calloc( p_geom->num_meshes + num_passes1, sizeof( SGpuMesh ));
	if( !mp_pieces )
		return;
	m_vita_eclairable = ( p_geom->num_meshes > 0 );

	for( int i = 0; i < p_geom->num_meshes; ++i )
	{
		NxVita::SVitaMesh *p_src = &p_geom->p_meshes[i];
		SGpuMesh          *p_dst = &mp_pieces[m_num];

		glGenBuffers( 1, &p_dst->vbo );
		glBindBuffer( GL_ARRAY_BUFFER, p_dst->vbo );
		glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 3 * p_src->num_vertices,
		              p_src->p_positions, GL_STATIC_DRAW );

		p_dst->uvbo = 0;
		if( p_src->p_uvs )
		{
			glGenBuffers( 1, &p_dst->uvbo );
			glBindBuffer( GL_ARRAY_BUFFER, p_dst->uvbo );
			glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 2 * p_src->num_vertices,
			              p_src->p_uvs, GL_STATIC_DRAW );
		}
		// Matrice d'UV du CAS (#16) : UV d'origine de la passe 0, gardees
		// pour les pieces skinnees (skater, planche, pietons).
		p_dst->uvm_nom   = p_src->mat_nom;
		p_dst->uvm_passe = 0;
		p_dst->p_uv_src  = NULL;
		if( p_src->p_uvs && p_src->p_weights && ( p_src->num_vertices > 0 ))
		{
			p_dst->p_uv_src = (float *)malloc( sizeof( float ) * 2 * p_src->num_vertices );
			if( p_dst->p_uv_src )
				memcpy( p_dst->p_uv_src, p_src->p_uvs, sizeof( float ) * 2 * p_src->num_vertices );
		}

		glGenBuffers( 1, &p_dst->ibo );
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p_dst->ibo );
		glBufferData( GL_ELEMENT_ARRAY_BUFFER,
		              sizeof( unsigned short ) * p_src->num_indices,
		              p_src->p_indices, GL_STATIC_DRAW );

		p_dst->num_indices  = p_src->num_indices;
	p_dst->sector_bone  = p_src->sector_bone;
		p_dst->num_vertices = p_src->num_vertices;
		p_dst->load_order   = p_src->load_order;

		// ECLAIRAGE DES MODELES RIGIDES (XBox instance.cpp:200, 259) : une
		// instance non skinnee dont tous les maillages ont des normales est
		// eclairee. Normales gardees ici pour le calcul CPU (eclairer_piece).
		if( p_src->p_weights && p_src->p_bones )
			m_vita_eclairable = false;		// skinne : son propre eclairage
		else
		{
			if( !p_src->p_normals || p_src->unlit )
				m_vita_eclairable = false;
			else
			{
				p_dst->p_n_rig = (float *)malloc( sizeof( float ) * 3 * p_src->num_vertices );
				if( p_dst->p_n_rig )
					memcpy( p_dst->p_n_rig, p_src->p_normals, sizeof( float ) * 3 * p_src->num_vertices );
			}
		}

		// Copie des indices d'origine : le retrait de polygones doit pouvoir
		// repartir de la bande intacte a chaque changement de tenue.
		p_dst->p_indices_src = (unsigned short *)malloc(
			sizeof( unsigned short ) * p_src->num_indices );
		p_dst->num_indices_src = 0;
		if( p_dst->p_indices_src )
		{
			memcpy( p_dst->p_indices_src, p_src->p_indices,
			        sizeof( unsigned short ) * p_src->num_indices );
			p_dst->num_indices_src = p_src->num_indices;
		}

		// Maillage skinne : on garde de quoi retransformer chaque frame. Les
		// tampons du parseur sont repris tels quels, il ne les liberera pas
		// deux fois -- on les detache en mettant les siens a NULL.
		p_dst->p_positions = NULL;
		p_dst->p_weights   = NULL;
		p_dst->p_bones     = NULL;
		p_dst->p_skinned   = NULL;
		p_dst->p_normals   = NULL;
		p_dst->p_skinned_n = NULL;
		p_dst->p_lit_base  = NULL;
		p_dst->p_lit       = NULL;
		if( p_src->p_weights && p_src->p_bones )
		{
			p_dst->p_positions = p_src->p_positions;
			p_dst->p_weights   = p_src->p_weights;
			p_dst->p_bones     = p_src->p_bones;
			p_dst->p_normals   = p_src->p_normals;

			// Une couleur par sommet, propre a CETTE piece : deux pieces
			// partagent leurs sommets mais pas forcement leur teinte (le
			// pantalon est brun, la chemise blanche).
			if( p_src->p_normals )
				p_dst->p_lit = (unsigned char *)malloc( 4 * p_src->num_vertices );

			// Les maillages d'un meme secteur partagent leurs sommets : un
			// SEUL tampon de travail, un SEUL calcul de skinning par frame.
			// Auparavant chacun avait le sien et refaisait le meme calcul.
			p_dst->p_skinned   = NULL;
			p_dst->owns_skin   = 0;
			p_dst->p_skinned_n = NULL;
			for( int k = 0; k < m_num; ++k )
			{
				if( mp_pieces[k].p_positions == p_src->p_positions )
				{
					p_dst->p_skinned   = mp_pieces[k].p_skinned;
					p_dst->p_skinned_n = mp_pieces[k].p_skinned_n;
					p_dst->p_lit_base  = mp_pieces[k].p_lit_base;
					p_dst->vbo_skin    = mp_pieces[k].vbo_skin;
					p_dst->vbo_repos   = mp_pieces[k].vbo_repos;
					p_dst->vbo_poids   = mp_pieces[k].vbo_poids;
					p_dst->vbo_os      = mp_pieces[k].vbo_os;
					break;
				}
			}
			if( !p_dst->p_skinned )
			{
				p_dst->p_skinned = (float *)malloc( sizeof( float ) * 3 * p_src->num_vertices );
				p_dst->owns_skin = 1;
				if( p_src->p_normals )
				{
					p_dst->p_skinned_n = (float *)malloc( sizeof( float ) * 3 * p_src->num_vertices );
					p_dst->p_lit_base  = (unsigned char *)malloc( 4 * p_src->num_vertices );
				}

				// Skinning GPU : tampons statiques du groupe (issue #18).
				{
					const int nv = p_src->num_vertices;
					float *p_w = (float *)malloc( sizeof( float ) * 3 * nv );
					float *p_b = (float *)malloc( sizeof( float ) * 3 * nv );
					if( p_w && p_b )
					{
						for( int v = 0; v < nv; ++v )
						{
							const unsigned int pk = p_src->p_weights[v];
							p_w[v * 3 + 0] = (float)(( pk       ) & 0x7FF ) / 1023.0f;
							p_w[v * 3 + 1] = (float)(( pk >> 11 ) & 0x7FF ) / 1023.0f;
							p_w[v * 3 + 2] = (float)(( pk >> 22 ) & 0x3FF ) / 511.0f;
							for( int k = 0; k < 3; ++k )
								p_b[v * 3 + k] = (float)p_src->p_bones[v * 4 + k];
						}
						glGenBuffers( 1, &p_dst->vbo_repos );
						glBindBuffer( GL_ARRAY_BUFFER, p_dst->vbo_repos );
						glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 3 * nv,
						              p_src->p_positions, GL_STATIC_DRAW );
						glGenBuffers( 1, &p_dst->vbo_poids );
						glBindBuffer( GL_ARRAY_BUFFER, p_dst->vbo_poids );
						glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 3 * nv,
						              p_w, GL_STATIC_DRAW );
						glGenBuffers( 1, &p_dst->vbo_os );
						glBindBuffer( GL_ARRAY_BUFFER, p_dst->vbo_os );
						glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 3 * nv,
						              p_b, GL_STATIC_DRAW );
					}
					free( p_w );
					free( p_b );
				}

				// Tampon GPU des positions skinnees, partage comme elles.
				glGenBuffers( 1, &p_dst->vbo_skin );
				glBindBuffer( GL_ARRAY_BUFFER, p_dst->vbo_skin );
				glBufferData( GL_ARRAY_BUFFER,
				              sizeof( float ) * 3 * p_src->num_vertices,
				              NULL, GL_DYNAMIC_DRAW );
			}

			// Les couleurs, elles, sont propres a la piece : un tampon chacune.
			if( p_dst->p_lit )
			{
				glGenBuffers( 1, &p_dst->cbo_lit );
				glBindBuffer( GL_ARRAY_BUFFER, p_dst->cbo_lit );
				glBufferData( GL_ARRAY_BUFFER, 4 * p_src->num_vertices,
				              NULL, GL_DYNAMIC_DRAW );
			}

			// Le proprietaire des sommets bruts est celui que le parseur a
			// designe ; on le lui reprend pour que la scene ne les libere pas.
			p_dst->owns_verts  = p_src->owns_vertices;
			if( p_src->owns_vertices )
			{
				p_src->p_positions = NULL;
				p_src->p_weights   = NULL;
				p_src->p_bones     = NULL;
				p_src->p_normals   = NULL;
				p_src->owns_vertices = 0;
			}
		}

		p_dst->texture = 0;
		if( p_tex_dict && p_src->texture_checksum )
		{
			CTexture *p_tex = p_tex_dict->GetTexture( p_src->texture_checksum );
			if( p_tex )
				p_dst->texture = ((CVitaTexture *)p_tex )->GetGLTexture();

	// Identifiants de texture des PERSONNAGES. Le decor a deja ete innocente
	// (intersection vide avec le ciel) ; reste a savoir si les personnages,
	// eux, tombent sur les memes numeros.
	{
		static int s_dits = 0;
		if( s_dits < 60 )
		{
			VLOG( "TEX", "modele : texture GL %u",
			      (unsigned)p_dst->texture );
			++s_dits;
		}
	}
		}

		// Couleurs de sommets et blend du materiau : les modeles d'interface
		// (UI_bg.mdl, mainmenu_bg.mdl...) sont des panneaux dont l'alpha vit
		// dans ces donnees. Sans elles, le voile du menu sortait en rectangle
		// blanc opaque -- le meme defaut deja corrige pour le decor-monde.
		p_dst->cbo   = 0;
		p_dst->blend = p_src->blend_mode;
		p_dst->color_locked = ( p_src->mat_flags0 & 0x80 ) ? 1 : 0;
		p_dst->no_bfc       = p_src->no_bfc;
		// UV wibble de la passe 0 (#77), bit 0 du masque du chargeur.
		p_dst->uvw0 = ( p_src->uvw & 1u ) ? 1 : 0;
		memcpy( p_dst->uvw_par0, p_src->uvw_par[0], sizeof( p_dst->uvw_par0 ));
		// Speculaire (issue #45), XBox/NX/WeightedMeshVS_VXC_Specular_*.vsh.
		// Sous une passe GLOSS_MAP (11), XBox module la speculaire par l'alpha
		// de la texture de cette passe (render.cpp:483, "mul v1.rgb,v1.rgb,
		// t1.a") : la peau eclairee la lit sur l'unite 2 avec le jeu d'UV 1
		// (shirt_windbreaker, seul cas du jeu). Sans cette texture (piece non
		// skinnee, texture ou UV absents), pas de speculaire du tout -- sinon
		// tout le vetement brillerait.
		p_dst->gloss_tex   = 0;
		p_dst->gloss_uvbo  = 0;
		p_dst->gloss_clamp = 0;
		{
			const bool gloss = ( p_src->num_passes >= 2 ) && ( p_src->blend_mode2 == 11 );
			GLuint tex_gloss = 0;
			if( gloss && ( p_src->spec[3] > 0.0f ) && p_tex_dict && p_src->texture_checksum2
			    && p_src->p_uvs_couche1 && p_src->p_weights && p_src->p_bones )
			{
				CTexture *p_tg = p_tex_dict->GetTexture( p_src->texture_checksum2 );
				if( p_tg )
					tex_gloss = ((CVitaTexture *)p_tg )->GetGLTexture();
			}
			if(( p_src->spec[3] > 0.0f ) && ( !gloss || tex_gloss ))
			{
				memcpy( p_dst->spec, p_src->spec, sizeof( p_dst->spec ));
				if( tex_gloss )
				{
					glGenBuffers( 1, &p_dst->gloss_uvbo );
					glBindBuffer( GL_ARRAY_BUFFER, p_dst->gloss_uvbo );
					glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 2 * p_src->num_vertices,
					              p_src->p_uvs_couche1, GL_STATIC_DRAW );
					p_dst->gloss_tex   = tex_gloss;
					p_dst->gloss_clamp = ( p_src->addr2_u ? 1 : 0 ) | ( p_src->addr2_v ? 2 : 0 );
					VLOG( "SHD", "GLOSS_MAP : texture GL %u, %d sommets", (unsigned)tex_gloss,
					      p_src->num_vertices );
				}
			}
			else
				p_dst->spec[0] = p_dst->spec[1] = p_dst->spec[2] = p_dst->spec[3] = 0.0f;
		}
		p_dst->mat_checksum = p_src->mat_nom;	// NOM, compare par SetMaterialColor (#58)
		if( p_src->p_colors )
		{
			// Couleur du materiau, cuite ici comme pour le decor : le tampon
			// GL est propre au morceau, le tampon CPU est partage.
			const float mat_f[3] = { p_src->mat_color[0] * 2.0f,
			                         p_src->mat_color[1] * 2.0f,
			                         p_src->mat_color[2] * 2.0f };
			const unsigned char *p_up = p_src->p_colors;
			if(( mat_f[0] != 1.0f ) || ( mat_f[1] != 1.0f ) || ( mat_f[2] != 1.0f ))
				p_up = NxVita::TeindreCouleursMateriau( p_src->p_colors,
				                                        p_src->num_vertices,
				                                        mat_f );
			glGenBuffers( 1, &p_dst->cbo );
			glBindBuffer( GL_ARRAY_BUFFER, p_dst->cbo );
			glBufferData( GL_ARRAY_BUFFER, 4 * p_src->num_vertices,
			              p_up, GL_STATIC_DRAW );
			p_dst->p_couleurs_brutes = (unsigned char *)malloc( 4 * p_src->num_vertices );
			if( p_dst->p_couleurs_brutes )
				memcpy( p_dst->p_couleurs_brutes, p_src->p_colors, 4 * p_src->num_vertices );
			if( p_dst->p_n_rig )
			{
				p_dst->p_c_base = (unsigned char *)malloc( 4 * p_src->num_vertices );
				p_dst->p_c_lit  = (unsigned char *)malloc( 4 * p_src->num_vertices );
				if( p_dst->p_c_base && p_dst->p_c_lit )
				{
					memcpy( p_dst->p_c_base, p_up, 4 * p_src->num_vertices );
					glGenBuffers( 1, &p_dst->cbo_rig );
					glBindBuffer( GL_ARRAY_BUFFER, p_dst->cbo_rig );
					glBufferData( GL_ARRAY_BUFFER, 4 * p_src->num_vertices,
					              p_up, GL_DYNAMIC_DRAW );
					// Sommets utilises par cette piece (#18) : une voiture de
					// 49 pieces reeclairait 49 fois tout le secteur (18 ms).
					if( p_src->p_indices && ( p_src->num_indices > 0 ))
					{
						unsigned char *vu = (unsigned char *)calloc( p_src->num_vertices, 1 );
						int n = 0;
						if( vu )
						{
							for( int k = 0; k < p_src->num_indices; ++k )
								if(( p_src->p_indices[k] < p_src->num_vertices ) && !vu[p_src->p_indices[k]] )
								{
									vu[p_src->p_indices[k]] = 1;
									++n;
								}
							p_dst->p_lum_idx = ( n > 0 ) ? (unsigned short *)malloc( 2 * n ) : NULL;
							if( p_dst->p_lum_idx )
							{
								int j = 0;
								for( int v = 0; v < p_src->num_vertices; ++v )
									if( vu[v] )
										p_dst->p_lum_idx[j++] = (unsigned short)v;
								p_dst->num_lum_idx = n;
							}
							free( vu );
						}
					}
				}
			}
		}

		// PASSE 1 ADD D'UNE PIECE RIGIDE OPAQUE (#77). veh_policecar_nj :
		// materiaux ec4b5c1e (rouge), fc7791d5 (bleu), 2d30a377 (blanc) --
		// passe 0 opaque, passe 1 ADD de la meme texture, toutes deux a UV
		// wibble. [SOURCE] XBox/NX/render.cpp:424 : r1 = 4 t1 c1 v0 (couleur
		// de passe, x4 avec v0 et c1 a 0,5 neutres), r1.a = 2 t1.a v0.a, puis
		// r0 += r1.rgb * r1.a. Ici : cbo = 2 v0 x 2 c1 (meme cuisson que la
		// passe 0), alpha 2 v0.a, et melange GL_SRC_ALPHA, GL_ONE au dessin.
		// Ecart : v0 est la couleur ECLAIREE chez XBox ; on prend la brute.
		// ADD_FIXED (2) et les autres modes de passe 1 restent ignores.
		if( !p_src->p_weights && ( p_src->blend_mode == 0 ) && ( p_src->num_passes >= 2 )
		    && ( p_src->blend_mode2 == 1 ) && p_src->texture_checksum2
		    && p_src->p_uvs_couche1 && p_tex_dict )
		{
			CTexture *p_t1 = p_tex_dict->GetTexture( p_src->texture_checksum2 );
			const GLuint t1 = p_t1 ? ((CVitaTexture *)p_t1 )->GetGLTexture() : 0;
			if( t1 )
			{
				p_dst->add_tex    = t1;
				p_dst->add_uvw    = ( p_src->uvw & 2u ) ? 1 : 0;
				memcpy( p_dst->add_uvw_par, p_src->uvw_par[1], sizeof( p_dst->add_uvw_par ));
				p_dst->add_locked = ( p_src->mat_flags2 & 0x80 ) ? 1 : 0;
				glGenBuffers( 1, &p_dst->add_uvbo );
				glBindBuffer( GL_ARRAY_BUFFER, p_dst->add_uvbo );
				glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 2 * p_src->num_vertices,
				              p_src->p_uvs_couche1, GL_STATIC_DRAW );
				if( p_src->p_colors )
				{
					const float mat_f[3] = { p_src->mat_color2[0] * 2.0f,
					                         p_src->mat_color2[1] * 2.0f,
					                         p_src->mat_color2[2] * 2.0f };
					const unsigned char *p_up = p_src->p_colors;
					if(( mat_f[0] != 1.0f ) || ( mat_f[1] != 1.0f ) || ( mat_f[2] != 1.0f ))
						p_up = NxVita::TeindreCouleursMateriau( p_src->p_colors,
						                                        p_src->num_vertices, mat_f );
					glGenBuffers( 1, &p_dst->add_cbo );
					glBindBuffer( GL_ARRAY_BUFFER, p_dst->add_cbo );
					glBufferData( GL_ARRAY_BUFFER, 4 * p_src->num_vertices, p_up, GL_STATIC_DRAW );
				}
				static int s_add_traces = 0;
				if( s_add_traces < 24 )
				{
					++s_add_traces;
					VLOG( "GYR", "passe 1 ADD : materiau %08x, texture GL %u, uvw %u/%u, %d sommets",
					      (unsigned)p_src->mat_checksum, (unsigned)t1, (unsigned)p_dst->uvw0,
					      (unsigned)p_dst->add_uvw, p_src->num_vertices );
				}
			}
		}

		// Boite englobante du modele : union des sommets de chaque morceau.
		// Les positions du parseur sont en espace modele, celles-la memes que
		// AutoComputeScale attend.
		if( p_src->p_positions && ( p_src->num_vertices > 0 ))
		{
			for( int v = 0; v < p_src->num_vertices; ++v )
			{
				const float *q = &p_src->p_positions[v * 3];
				m_bbox.AddPoint( Mth::Vector( q[0], q[1], q[2], 1.0f ));
			}
		}

		++m_num;
	}

	// PASSE 1 DES PERSONNAGES (#55). La boucle ci-dessus ne fait jamais de
	// "continue" : la piece de base du maillage i est donc mp_pieces[i].
	if( num_passes1 > 0 )
	{
		const int num_base = m_num;
		int faites = 0, faites2 = 0;
		// Toutes les passes 1, puis toutes les passes 2 : les pieces sont
		// dessinees dans l'ordre du tableau, la passe 2 recouvre la passe 1.
		for( int kp = 1; kp <= 2; ++kp )
		for( int i = 0; ( i < p_geom->num_meshes ) && ( i < num_base ); ++i )
		{
			NxVita::SVitaMesh *p_src = &p_geom->p_meshes[i];
			if( !passe_k_dessinable( p_src, kp, p_tex_dict ))
				continue;
			SPasseDecal dk;
			passe_decal( p_src, kp, &dk );
			const SGpuMesh *p_base = &mp_pieces[i];
			SGpuMesh       *p_dst  = &mp_pieces[m_num];
			const float    *p_uv1  = dk.p_uv;
			const int       nv     = p_src->num_vertices;

			// Sommets, poids, os, normales, tampons skinnes : ceux de la base,
			// SANS propriete -- le destructeur ne les rendra que par elle.
			*p_dst = *p_base;
			p_dst->owns_verts   = 0;
			p_dst->owns_skin    = 0;
			p_dst->vbo          = 0;
			p_dst->cbo          = 0;
			p_dst->cbo_lit      = 0;
			p_dst->p_lit        = NULL;
			p_dst->p_n_rig      = NULL;
			p_dst->p_c_base     = NULL;
			p_dst->p_c_lit      = NULL;
			p_dst->p_lum_idx    = NULL;
			p_dst->num_lum_idx  = 0;
			p_dst->cbo_rig      = 0;
			p_dst->nbo_rig      = 0;
			p_dst->p_couleurs_brutes = NULL;
			p_dst->gloss_tex    = 0;		// GLOSS_MAP (#45) : a la base seule
			p_dst->gloss_uvbo   = 0;
			p_dst->gxm_tex      = NULL;
			p_dst->gxm_tex_nom  = 0;
			p_dst->lum_ok       = 0;
			// SetMaterialColor ne vise que la passe 0 (seule cuite) : une
			// passe 1 n'a pas a suivre la couleur CAS du vetement.
			p_dst->mat_checksum = 0;
			p_dst->decal        = 1;
			p_dst->decal_au     = dk.au;
			p_dst->decal_av     = dk.av;
			// #77 : ni UV wibble ni passe ADD heritees de la base (tampons
			// propres a celle-ci, liberes par elle seule).
			p_dst->uvw0         = 0;
			p_dst->add_tex      = 0;
			p_dst->add_uvbo     = 0;
			p_dst->add_cbo      = 0;
			p_dst->add_uvw      = 0;
			p_dst->blend        = dk.blend;
			// [SOURCE] XBox/NX/mesh.cpp:603 : le verrou de couleur est PAR
			// PASSE (logos du t-shirt : 0xa4, verrouilles).
			p_dst->color_locked = ( dk.flags & 0x80 ) ? 1 : 0;
			p_dst->texture      = ((CVitaTexture *)p_tex_dict->GetTexture( dk.tex ))->GetGLTexture();

			glGenBuffers( 1, &p_dst->uvbo );
			glBindBuffer( GL_ARRAY_BUFFER, p_dst->uvbo );
			glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 2 * nv, p_uv1, GL_STATIC_DRAW );
			// Matrice d'UV du CAS (#16) : tatouages et logos sont des passes 1
			// (cas_parts.q:138, master_uv_list). UV d'origine de la couche 1.
			p_dst->uvm_nom   = p_src->mat_nom;
			p_dst->uvm_passe = (unsigned char)kp;
			p_dst->uvm_actif = 0;
			p_dst->p_uv_src  = (float *)malloc( sizeof( float ) * 2 * nv );
			if( p_dst->p_uv_src )
				memcpy( p_dst->p_uv_src, p_uv1, sizeof( float ) * 2 * nv );

			// Couleurs de sommets teintes par la couleur de la PASSE 1.
			if( p_src->p_colors )
			{
				const float mat_f[3] = { dk.color[0] * 2.0f,
				                         dk.color[1] * 2.0f,
				                         dk.color[2] * 2.0f };
				const unsigned char *p_up = p_src->p_colors;
				if(( mat_f[0] != 1.0f ) || ( mat_f[1] != 1.0f ) || ( mat_f[2] != 1.0f ))
					p_up = NxVita::TeindreCouleursMateriau( p_src->p_colors, nv, mat_f );
				glGenBuffers( 1, &p_dst->cbo );
				glBindBuffer( GL_ARRAY_BUFFER, p_dst->cbo );
				glBufferData( GL_ARRAY_BUFFER, 4 * nv, p_up, GL_STATIC_DRAW );
			}

			// Indices : copie propre, pour que HidePolys (meme load_order que
			// la base) retire les memes triangles dans les deux pieces.
			p_dst->p_indices_src   = NULL;
			p_dst->num_indices_src = 0;
			if( p_base->p_indices_src )
			{
				p_dst->p_indices_src = (unsigned short *)malloc(
					sizeof( unsigned short ) * p_base->num_indices_src );
				if( p_dst->p_indices_src )
				{
					memcpy( p_dst->p_indices_src, p_base->p_indices_src,
					        sizeof( unsigned short ) * p_base->num_indices_src );
					p_dst->num_indices_src = p_base->num_indices_src;
				}
			}
			glGenBuffers( 1, &p_dst->ibo );
			glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p_dst->ibo );
			glBufferData( GL_ELEMENT_ARRAY_BUFFER,
			              sizeof( unsigned short ) * p_src->num_indices,
			              p_src->p_indices, GL_STATIC_DRAW );
			p_dst->num_indices = p_src->num_indices;

			++m_num;
			if( kp == 1 ) ++faites; else ++faites2;
		}
		VLOG( "CAS", "passe 1 (logos/decalques, #55) : %d pieces ajoutees, passe 2 (#16) : %d", faites, faites2 );
	}

	{
		int nn = 0, nu = 0, ns = 0;
		for( int i = 0; i < p_geom->num_meshes; ++i )
		{
			if( p_geom->p_meshes[i].p_weights ) ++ns;
			else if( !p_geom->p_meshes[i].p_normals ) ++nn;
			if( p_geom->p_meshes[i].unlit ) ++nu;
		}
		VLOG( "LUM", "maillage %d pieces : eclairable=%d (sans normales %d, unlit %d, skinnes %d)",
		      p_geom->num_meshes, (int)m_vita_eclairable, nn, nu, ns );
	}
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );

}


void CVitaMesh::Build( const char *p_filename, CTexDict *p_tex_dict )
{
	NxVita::SVitaSceneGeom geom;
	NxVita::g_vita_garder_normales = true;
	const bool lu = NxVita::LoadSceneGeometry( p_filename, &geom );
	NxVita::g_vita_garder_normales = false;
	if( !lu )
	{
		NxVita::FreeSceneGeometry( &geom );
		return;
	}

	build_pieces( &geom, p_tex_dict );
	if( m_vita_eclairable )
		VLOG( "LUM", "eclairable : %s", p_filename );

	// HIERARCHIE : on la reprend AVANT de liberer la scene. Le moteur y accede
	// par CMesh::GetHierarchy(), qui rend simplement mp_hierarchyObjects --
	// champ de la classe de base, qu'il suffit donc de renseigner.
	//
	// C'est ce chainon qui manquait : sans lui GetHierarchy() rendait NULL,
	// CalculateCarHierarchyMatrices sortait sans poser la moindre matrice
	// d'os, et les six parties d'un vehicule se dessinaient empilees a
	// l'origine du modele -- voiture « verticale » et sans roues.
	// Repartition des morceaux par os, une fois par modele hierarchique.
	// « les roues de droite ne sont pas rendues, les gauches a moitie » : si
	// plusieurs roues partagent le meme index d'os, elles se superposent.
	if( geom.num_hierarchy > 0 )
	{
		int par_os[16];
		for( int k = 0; k < 16; ++k ) par_os[k] = 0;
		for( int k = 0; k < m_num; ++k )
		{
			const int b = mp_pieces[k].sector_bone;
			if(( b >= 0 ) && ( b < 16 )) ++par_os[b];
		}
		VLOG( "BONE", "'%s' : %d morceaux, %d objets  os0=%d os1=%d os2=%d "
		              "os3=%d os4=%d os5=%d",
		      p_filename, m_num, geom.num_hierarchy,
		      par_os[0], par_os[1], par_os[2], par_os[3], par_os[4], par_os[5] );

		// Detail des morceaux de ROUE (os >= 2) : ce qui distingue celui qui
		// s'affiche de celui qui manque -- la jante, d'apres la description.
		for( int k = 0; k < m_num; ++k )
		{
			const SGpuMesh *q = &mp_pieces[k];
			if( q->sector_bone < 2 ) continue;
			VLOG( "BONE", "   roue os=%d : %d indices, tex=%u, blend=%u, "
			              "%d sommets",
			      q->sector_bone, q->num_indices, (unsigned)q->texture,
			      (unsigned)q->blend, q->num_vertices );
		}
	}

	if( geom.p_hierarchy && ( geom.num_hierarchy > 0 ))
	{
		mp_hierarchyObjects   = (CHierarchyObject *)geom.p_hierarchy;
		m_numHierarchyObjects = geom.num_hierarchy;
		geom.p_hierarchy      = NULL;   /* transfert de propriete */
		geom.num_hierarchy    = 0;
	}

	NxVita::FreeSceneGeometry( &geom );

	// Nom court, pour les traces de rendu.
	{
		const char *p_base = p_filename;
		for( const char *q = p_filename; *q; ++q )
			if(( *q == '/' ) || ( *q == '\\' ))
				p_base = q + 1;
		int k = 0;
		while( p_base[k] && ( k < (int)sizeof( m_vita_name ) - 1 ))
		{
			m_vita_name[k] = p_base[k];
			++k;
		}
		m_vita_name[k] = 0;

		// Dossier parent (sonde « pdt », issue #46).
		const char *p_dir = p_filename;
		for( const char *q = p_filename; ( q + 1 ) < p_base; ++q )
			if(( *q == '/' ) || ( *q == '\\' ))
				p_dir = q + 1;
		k = 0;
		while(( p_dir + k + 1 < p_base ) && ( k < (int)sizeof( m_vita_dossier ) - 1 ))
		{
			m_vita_dossier[k] = p_dir[k];
			++k;
		}
		m_vita_dossier[k] = 0;
	}

	load_cas_table( p_filename );

	static int s_traced = 0;
	if( s_traced < 20 )
	{
		++s_traced;
		VLOG( "MDL", "'%s' : %d morceaux", p_filename, m_num );
	}
}



// Chemin Create-A-Skater : la piece arrive en RAM, pas sur le disque.
//
// C'etait LE stub restant : la surcharge memoire de s_plat_load_mesh rendait
// un CVitaMesh vide -- pose pour arreter un plantage, jamais implemente. Le
// skater custom etait donc invisible piece par piece : jambes, pantalon,
// mains... tout ce qui venait de skaterparts.pre. Le format en RAM est le
// meme .skin.xbx que sur disque (la reference DX9 lit les deux avec le meme
// parseur et un drapeau is_file) ; les donnees CAS arrivent a part, deja en
// memoire.
void CVitaMesh::BuildFromMemory( const void *p_data, int size,
                                 const unsigned char *p_cas_data,
                                 CTexDict *p_tex_dict, const char *p_label )
{
	NxVita::SVitaSceneGeom geom;
	NxVita::g_vita_garder_normales = true;
	const bool lu = NxVita::LoadSceneGeometryFromMemory( p_data, size, p_label, &geom );
	NxVita::g_vita_garder_normales = false;
	if( !lu )
	{
		NxVita::FreeSceneGeometry( &geom );
		return;
	}

	build_pieces( &geom, p_tex_dict );

	NxVita::FreeSceneGeometry( &geom );

	{
		int k = 0;
		while( p_label && p_label[k] && ( k < (int)sizeof( m_vita_name ) - 1 ))
		{
			m_vita_name[k] = p_label[k];
			++k;
		}
		m_vita_name[k] = 0;
	}

	load_cas_from_memory( p_cas_data );

	VLOG( "MDL", "'%s' (memoire) : %d morceaux, %d entrees CAS, masque=0x%08x",
	      m_vita_name, m_num, m_num_cas, (unsigned)m_CASRemovalMask );
}


// Table CAS depuis la RAM : memes champs que le fichier (version, masque si
// version >= 2, compte, entrees) -- reference DX9
// build_casdata_table_from_memory, p_NxMesh.cpp:113.
void CVitaMesh::load_cas_from_memory( const unsigned char *p_cas_data )
{
	if( !p_cas_data )
		return;

	const unsigned char *p = p_cas_data;
	unsigned int version;
	memcpy( &version, p, 4 );  p += 4;
	if( version >= 2 )
	{
		memcpy( &m_CASRemovalMask, p, 4 );  p += 4;
	}
	int count;
	memcpy( &count, p, 4 );  p += 4;
	if(( count <= 0 ) || ( count >= ( 1 << 20 )))
		return;

	mp_cas = (SCasEntry *)malloc( sizeof( SCasEntry ) * count );
	if( !mp_cas )
		return;
	memcpy( mp_cas, p, sizeof( SCasEntry ) * count );
	m_num_cas = count;
}


// Charge la table CAS associee a un maillage skinne.
//
// Format releve sur la reference DX9 (p_NxMesh.cpp:69-97) : pour tout
// ï¿½ xxx.skin.xbx ï¿½ il existe un ï¿½ xxx.cas.xbx ï¿½ contenant
//
//     uint32 version
//     uint32 masque global        (seulement si version >= 2)
//     int    nombre d'entrees
//     N x    { uint32 mask, data0, data1 }
//
// Chaque entree designe UN triangle du corps a retirer quand le vetement
// correspondant est porte. Sans cette table, rien n'est retirable et le torse
// traverse le t-shirt.
void CVitaMesh::load_cas_table( const char *p_skin_filename )
{
	if( !p_skin_filename )
		return;

	// Seuls les maillages skinnes en ont une.
	const char *p_ext = strstr( p_skin_filename, "skin.xbx" );
	if( !p_ext )
		return;

	char name[256];
	const size_t prefix = (size_t)( p_ext - p_skin_filename );
	if( prefix + 8 >= sizeof( name ))
		return;
	memcpy( name, p_skin_filename, prefix );
	strcpy( name + prefix, "cas.xbx" );

	void *p_file = File::Open( name, "rb" );
	if( !p_file )
		return;		// pas de table : normal pour beaucoup de maillages

	unsigned int version = 0;
	File::Read( &version, sizeof( unsigned int ), 1, p_file );
	if( version >= 2 )
	{
		// LE masque de retrait de ce maillage -- a stocker, pas a jeter.
		//
		// C'est lui qui dit ï¿½ ce vetement couvre telle zone du corps ï¿½.
		// CModel::GetPolyRemovalMask agrege les masques de toutes les pieces
		// portees (NxModel.cpp:695) et passe le total a HidePolys. Premiere
		// version : je le lisais pour avancer le curseur, et je le jetais --
		// le masque restait donc nul et aucun triangle n'etait jamais retire,
		// alors que les 2362 entrees du corps etaient bien chargees.
		//
		// Cela explique aussi la repartition vue au log : les vetements ont
		// zero entree mais un masque, le corps a les entrees mais pas de
		// masque.
		File::Read( &m_CASRemovalMask, sizeof( unsigned int ), 1, p_file );
	}

	int count = 0;
	File::Read( &count, sizeof( int ), 1, p_file );
	if(( count > 0 ) && ( count < ( 1 << 20 )))
	{
		mp_cas = (SCasEntry *)malloc( sizeof( SCasEntry ) * count );
		if( mp_cas )
		{
			File::Read( mp_cas, sizeof( SCasEntry ), count, p_file );
			m_num_cas = count;
		}
	}
	File::Close( p_file );

	VLOG( "CAS", "'%s' : %d entrees, masque=0x%08x",
	      name, m_num_cas, (unsigned)m_CASRemovalMask );
}


// Retire les triangles du corps couverts par un vetement.
//
// Methode DX9 (NX/scene.cpp:347) : reperer le triangle dans la BANDE, marquer
// l'indice qui le precede, puis ecrire l'indice marque TROIS fois a la
// reconstruction -- les degeneres d'aire nulle coupent la bande proprement.
//
// LE point qui a coute huit essais console : les tables CAS sont exprimees
// dans l'espace NORMALISE de chaque maillage (le plus petit index utilise
// devient 0, les trous du pool de sommets sont compactes), reproduisant
// sMesh::Initialize (DX9/NX/mesh.cpp:509-537). Nos indices restent les bruts
// du pool de secteur : il faut donc normaliser AVANT de chercher.
// Verifie hors console par vita/tools/cas_check.py sur les vrais fichiers :
//   bruts 4/2362, moins-min 831/2362, normalises 2362/2362.
void CVitaMesh::HidePolys( unsigned int mask )
{
	if(( m_num_cas == 0 ) || !mp_cas )
		return;
	if( mask == m_cas_applied )
		return;					// deja dans cet etat
	m_cas_applied = mask;

	for( int p = 0; p < m_num; ++p )
	{
		SGpuMesh *p_piece = &mp_pieces[p];
		const int n_src   = p_piece->num_indices_src;
		if( !p_piece->p_indices_src || ( n_src < 3 ))
			continue;

		// On repart TOUJOURS des indices d'origine, avec LEUR taille --
		// num_indices est ecrase par chaque reconstruction.
		unsigned short *p_work = (unsigned short *)malloc(
			sizeof( unsigned short ) * n_src * 3 );
		if( !p_work )
			continue;
		memcpy( p_work, p_piece->p_indices_src,
		        sizeof( unsigned short ) * n_src );

		int marked = 0;
		if( mask )
		{
			// --- normalisation facon DX9 -------------------------------------
			unsigned int max_i = 0;
			for( int i = 0; i < n_src; ++i )
				if( p_work[i] > max_i )
					max_i = p_work[i];

			int *p_ws = (int *)malloc( sizeof( int ) * ( max_i + 1 ));
			unsigned short *p_norm = (unsigned short *)malloc(
				sizeof( unsigned short ) * n_src );
			if( p_ws && p_norm )
			{
				for( unsigned int v = 0; v <= max_i; ++v )
					p_ws[v] = 1;			// 1 = non utilise
				for( int i = 0; i < n_src; ++i )
					p_ws[p_work[i]] = 0;	// 0 = utilise
				int offset = 0;
				for( unsigned int v = 0; v <= max_i; ++v )
				{
					if( p_ws[v] == 0 )
						p_ws[v] = offset;
					else
						--offset;
				}
				for( int i = 0; i < n_src; ++i )
					p_norm[i] = (unsigned short)( (int)p_work[i] + p_ws[p_work[i]] );

				// --- recherche des triangles vises ---------------------------
				for( int e = 0; e < m_num_cas; ++e )
				{
					if( !( mp_cas[e].mask & mask ))
						continue;
					if(( int )( mp_cas[e].data0 >> 16 ) != p_piece->load_order )
						continue;

					const unsigned int i0 = mp_cas[e].data0 & 0xFFFF;
					const unsigned int i1 = mp_cas[e].data1 >> 16;
					const unsigned int i2 = mp_cas[e].data1 & 0xFFFF;

					for( int i = 2; i < n_src; ++i )
					{
						if(( p_norm[i - 2] == i0 ) && ( p_norm[i - 1] == i1 )
						   && ( p_norm[i] == i2 ))
						{
							// Marque sur le 2e indice du triangle, comme DX9.
							p_work[i - 1] |= 0x8000;
							++marked;
							break;
						}
					}
				}
			}
			free( p_norm );
			free( p_ws );
		}

		// --- reconstruction : l'indice marque est ecrit trois fois -----------
		//
		// Dans un tampon de sortie SEPARE, comme le index_workbuffer de DX9.
		// La premiere version relisait et ecrivait p_work en place : des la
		// premiere marque, l'ecriture (n = i + 2) depassait la lecture et
		// ecrasait les indices pas encore lus -- tout le strip apres la
		// premiere marque devenait de la bouillie auto-referente qui ne
		// rasterise rien. C'est pour cela que chaque morceau MARQUE
		// disparaissait (corps, pantalon, deux morceaux du visage) pendant
		// que les morceaux sans marque restaient intacts. La simulation
		// Python, elle, ecrivait dans une liste separee : parfaite sur table,
		// fausse sur console -- les deux divergaient sur CETTE ligne.
		unsigned short *p_out = (unsigned short *)malloc(
			sizeof( unsigned short ) * ( n_src + 2 * marked ));
		if( !p_out )
		{
			free( p_work );
			continue;
		}
		int n = 0;
		for( int i = 0; i < n_src; ++i )
		{
			const unsigned short v = p_work[i];
			p_out[n++] = v & 0x7FFF;
			if( v & 0x8000 )
			{
				p_out[n++] = v & 0x7FFF;
				p_out[n++] = v & 0x7FFF;
			}
		}

		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p_piece->ibo );
		glBufferData( GL_ELEMENT_ARRAY_BUFFER,
		              sizeof( unsigned short ) * n, p_out, GL_STATIC_DRAW );
		free( p_out );
		p_piece->num_indices = n;
		free( p_work );

		if( marked )
			VLOG( "CAS", "  morceau %d (lo=%d) : %d triangles retires",
			      p, p_piece->load_order, marked );
	}
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
}


bool CVitaMesh::IsSkinned() const
{
	return ( m_num > 0 ) && ( mp_pieces[0].p_weights != NULL );
}


// Poids empaquetes dans un uint32, format releve sur DX9/NX/instance.cpp:757 :
//   w0 = bits 0..10  / 1023      w1 = bits 11..21 / 1023
//   w2 = bits 22..31 / 511
// Le troisieme poids a un bit de moins et un diviseur different : recopier
// betement la formule des deux premiers donnerait un membre deforme.
void CVitaMesh::DrawSkinned( Mth::Matrix *p_bone_mats, int num_bones,
                             const float *p_rgba, CVitaModelLights *p_lights )
{
	if( !p_bone_mats || ( num_bones <= 0 ) || !IsSkinned())
	{
		s_pdt_chemin = 0;
		Draw( p_rgba );
		return;
	}

	// SKINNING PAR LE GPU (issue #18, « gsk 0/1 »). Memes conditions que le
	// dessin CPU ci-dessous quand il est le plus simple : eclairage coupe,
	// pas de piece rigide (vehicules : sector_bone), squelette de 64 os au plus.
	static int s_chemin[4] = { 0, 0, 0, 0 };		// direct, vitaGL, cpu, os/eclairage
	{
		static SceUInt64 s_d = 0;
		const SceUInt64 tn = sceKernelGetProcessTimeWide();
		if( !s_d ) s_d = tn;
		if( tn - s_d > 2000000 )
		{
			VLOG( "PROF", "peau : direct %d (tampon os reutilise %d, ecrit %d), vitaGL %d, cpu %d (dont %d par os/eclairage) par 2 s",
			      s_chemin[0], NxVita::g_vita_gp_reutil[0], NxVita::g_vita_gp_reutil[1], s_chemin[1], s_chemin[2], s_chemin[3] );
			NxVita::g_vita_gp_reutil[0] = NxVita::g_vita_gp_reutil[1] = 0;
			s_chemin[0] = s_chemin[1] = s_chemin[2] = s_chemin[3] = 0;
			s_d = tn;
		}
	}
	// Eclairage (issue #4) : le shader vitaGL a sa variante eclairee, le
	// chemin CPU n'est plus qu'un repli (compilation refusee, matrice racine
	// absente, piece rigide, squelette trop grand).
	const bool peau_lum_ok = !g_vita_lighting || NxVita::ShaderPeauEclaireePret()
	                         || ( NxVita::g_vita_gxm_peau && NxVita::GxmPeauEclaireePret());
	if( !( g_vita_skin_gpu && peau_lum_ok && ( num_bones <= NxVita::ShaderPeauMaxOs())))
		++s_chemin[3];
	if( g_vita_skin_gpu && ( num_bones <= NxVita::ShaderPeauMaxOs())
	    && NxVita::ShaderPeauPret() && peau_lum_ok )
	{
		bool gpu = true;
		for( int i = 0; ( i < m_num ) && gpu; ++i )
			if( mp_pieces[i].sector_bone >= 0 )
				gpu = false;
		if( gpu )
		{
			const SceUInt64 t0 = sceKernelGetProcessTimeWide();
			float proj[16], mv[16], mvp[16];
			glGetFloatv( GL_PROJECTION_MATRIX, proj );
			glGetFloatv( GL_MODELVIEW_MATRIX, mv );
			for( int c = 0; c < 4; ++c )
				for( int r = 0; r < 4; ++r )
				{
					float s = 0.0f;
					for( int k = 0; k < 4; ++k )
						s += proj[k * 4 + r] * mv[c * 4 + k];
					mvp[c * 4 + r] = s;
				}
			// GXM direct (« gpk 0/1 ») : il faut un descripteur de texture
			// pour CHAQUE piece, meme non texturee (le shader echantillonne
			// toujours, puis ignore) -- on prend celui d'une piece texturee.
			bool direct = NxVita::g_vita_gxm_peau && NxVita::GxmPeauPret();
			bool remplir_os = false;
			void *tex_secours = NULL;
			if( direct )
			{
				for( int i = 0; i < m_num; ++i )
				{
					SGpuMesh *p = &mp_pieces[i];
					if( !p->vbo_repos || !p->num_indices )
						continue;
					if( p->texture && ( p->gxm_tex_nom != p->texture ))
					{
						glBindTexture( GL_TEXTURE_2D, p->texture );
						p->gxm_tex     = vglGetGxmTexture( GL_TEXTURE_2D );
						p->gxm_tex_nom = p->texture;
					}
					if( p->texture && p->gxm_tex && !tex_secours )
						tex_secours = p->gxm_tex;
					if( !p->max_os_ok )
					{
						int mx = 0;
						if( p->p_bones )
						{
							for( int v = 0; v < p->num_vertices; ++v )
								for( int k = 0; k < 3; ++k )
									if( (int)p->p_bones[v * 4 + k] > mx )
										mx = (int)p->p_bones[v * 4 + k];
						}
						else
						{
							mx = 32767;		// inconnu : remplir par prudence
						}
						p->max_os    = (short)(( mx > 32767 ) ? 32767 : mx );
						p->max_os_ok = 1;
					}
					if( p->max_os >= num_bones )
						remplir_os = true;
				}
				if( !tex_secours )
					direct = false;
			}
			// Eclairage (issue #4) : ambiante et deux lumieres, directions
			// ramenees dans l'espace du MODELE par la rotation de la matrice
			// racine, comme le moteur Xbox (anim.cpp:353).
			float lum[21];
			bool eclaire = false;
			if( g_vita_lighting && sp_racine_courante )
			{
				float amb_l[3], dif_l[2][3], dir_l[2][3];
				if( p_lights )
					p_lights->ResolveLighting( amb_l, dif_l, dir_l );
				else
				{
					Image::RGBA a = Nx::CLightManager::sGetLightAmbientColor();
					amb_l[0] = a.r / 128.0f; amb_l[1] = a.g / 128.0f; amb_l[2] = a.b / 128.0f;
					for( int L = 0; L < 2; ++L )
					{
						Image::RGBA d = Nx::CLightManager::sGetLightDiffuseColor( L );
						dif_l[L][0] = d.r / 128.0f; dif_l[L][1] = d.g / 128.0f; dif_l[L][2] = d.b / 128.0f;
						Mth::Vector v0 = Nx::CLightManager::sGetLightDirection( L );
						dir_l[L][0] = -v0[0]; dir_l[L][1] = -v0[1]; dir_l[L][2] = -v0[2];
					}
				}
				Mth::Matrix &R = *sp_racine_courante;
				lum[0] = amb_l[0]; lum[1] = amb_l[1]; lum[2] = amb_l[2];
				for( int L = 0; L < 2; ++L )
				{
					// Mth::Matrix : lignes = axes du modele en coordonnees monde.
					float d[3];
					for( int a = 0; a < 3; ++a )
						d[a] = dir_l[L][0] * R[a][0] + dir_l[L][1] * R[a][1] + dir_l[L][2] * R[a][2];
					const float n = sqrtf( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
					const float inv = ( n > 1e-6f ) ? 1.0f / n : 0.0f;
					lum[3 + L * 6] = d[0] * inv; lum[4 + L * 6] = d[1] * inv; lum[5 + L * 6] = d[2] * inv;
					lum[6 + L * 6] = dif_l[L][0]; lum[7 + L * 6] = dif_l[L][1]; lum[8 + L * 6] = dif_l[L][2];
				}
				// 3e lumiere : lumiere de scene, seulement pour un modele qui a
				// ses propres lumieres (XBox instance.cpp:360).
				{
					float d3[3] = { 0.0f, 0.0f, 0.0f }, c3[3] = { 0.0f, 0.0f, 0.0f };
					if( p_lights )
						CVitaModelLights::ResolveSceneLight( Mth::Vector( R[3][0], R[3][1], R[3][2], 1.0f ), d3, c3 );
					for( int a = 0; a < 3; ++a )
						lum[15 + a] = d3[0] * R[a][0] + d3[1] * R[a][1] + d3[2] * R[a][2];
					lum[18] = c3[0]; lum[19] = c3[1]; lum[20] = c3[2];
				}
				eclaire = true;
				// Normales sur le GPU, une fois par groupe de sommets.
				for( int i = 0; i < m_num; ++i )
				{
					SGpuMesh *p = &mp_pieces[i];
					if( p->vbo_normales || !p->p_normals )
						continue;
					for( int k = 0; k < m_num; ++k )
						if(( k != i ) && mp_pieces[k].vbo_normales && ( mp_pieces[k].p_normals == p->p_normals ))
						{
							p->vbo_normales = mp_pieces[k].vbo_normales;
							break;
						}
					if( !p->vbo_normales )
					{
						glGenBuffers( 1, &p->vbo_normales );
						glBindBuffer( GL_ARRAY_BUFFER, p->vbo_normales );
						glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 3 * p->num_vertices,
						              p->p_normals, GL_STATIC_DRAW );
						glBindBuffer( GL_ARRAY_BUFFER, 0 );
					}
				}
			}
			if( g_vita_lighting && !eclaire )
				direct = false;		// le CPU s'en chargera, plus bas
			if( direct && eclaire && !NxVita::GxmPeauEclaireePret())
				direct = false;		// variante GXM absente : celle de vitaGL
			if( direct )
			{
				s_pdt_chemin = 1;
				NxVita::GxmPeauDebut( mvp, (const float *)p_bone_mats, num_bones, remplir_os,
				                      eclaire ? lum : NULL );
				static const float blanc_g[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
				static const float gris_g[4]  = { 0.8f, 0.8f, 0.85f, 1.0f };
				for( int i = 0; i < m_num; ++i )
				{
					const SGpuMesh *p = &mp_pieces[i];
					if( !p->vbo_repos || !p->num_indices )
						continue;
					// Ce chemin ne melange pas : une passe 1 (#55) sortirait
					// opaque. Desactive par defaut (gpk 0).
					if( p->decal )
						continue;
					const bool tex = ( p->texture && p->uvbo && p->gxm_tex );
					const float *teinte = ( p_rgba && !p->color_locked ) ? p_rgba
					                      : ( tex ? blanc_g : gris_g );
					++g_vita_n_draws;
					NxVita::GxmPeauPiece( p->vbo_repos, p->vbo_poids, p->vbo_os, p->uvbo,
					                      tex ? p->gxm_tex : tex_secours, tex, teinte,
					                      p->ibo, p->num_indices,
					                      p->vbo_normales ? p->vbo_normales : p->vbo_repos );
				}
				NxVita::GxmPeauFin();
				++s_chemin[0];
				g_vita_t_draw += sceKernelGetProcessTimeWide() - t0;
				return;
			}
			// Variante eclairee du shader vitaGL (issue #4) : le CPU ne sert
			// plus que si elle manque.
			if( g_vita_lighting && !( eclaire && NxVita::ShaderPeauEclaireePret()))
				goto cpu;
			++s_chemin[1];
			s_pdt_chemin = eclaire ? 2 : 3;
			float aom[16];
			unsigned int aom_tex = 0;
			const bool a_aom = eclaire && auto_ombre( mv, proj, aom, &aom_tex );
			if( eclaire )
			{
				NxVita::ShaderPeauEclaireeDebut( mvp, (const float *)p_bone_mats, num_bones, lum );
				if( a_aom )
					NxVita::ShaderPeauEclaireeAutoOmbre( aom, aom_tex, aom[12], aom[13], aom[14] );
			}
			else
				NxVita::ShaderPeauDebut( mvp, (const float *)p_bone_mats, num_bones );
			glActiveTexture( GL_TEXTURE0 );
			static const float blanc[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
			static const float gris[4]   = { 0.8f, 0.8f, 0.85f, 1.0f };
			// Issue #35 : deux tours, comme le chemin CPU plus bas -- d'abord
			// les pieces opaques, puis celles a melange (blend != 0 : calques
			// de la planche, DXT2 dont le transparent est BLANC en RVB) avec
			// SRCALPHA/INVSRCALPHA et sans ecrire la profondeur. Dessinees
			// opaques, elles couvraient le dessous de la planche de blanc.
			for( int passe = 0; passe < 2; ++passe )
			{
			if( passe == 1 )
			{
				glEnable( GL_BLEND );
				glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
				glDepthMask( g_vita_zw_peau ? GL_TRUE : GL_FALSE );	// XBox : profondeur ecrite en semi-transparent (#46)
			}
			for( int i = 0; i < m_num; ++i )
			{
				const SGpuMesh *p = &mp_pieces[i];
				if( !p->vbo_repos || !p->num_indices )
					continue;
				if(( p->blend != 0 ) != ( passe == 1 ))
					continue;
				const bool tex = ( p->texture && p->uvbo );
				if( tex )
					glBindTexture( GL_TEXTURE_2D, p->texture );
				const float *teinte = ( p_rgba && !p->color_locked ) ? p_rgba
				                      : ( tex ? blanc : gris );
				if(( passe == 1 ) && g_vita_differer_modeles
				    && ( s_num_peau_reportees < MAX_PEAU_REPORTEES ) && ( num_bones <= PEAU_REPORT_OS ))
				{
					SPeauReportee *r = &s_peau_reportees[s_num_peau_reportees++];
					r->p_piece = p;
					memcpy( r->mvp, mvp, sizeof( r->mvp ));
					memcpy( r->os, p_bone_mats, sizeof( float ) * 16 * num_bones );
					r->num_os  = num_bones;
					r->eclaire = eclaire;
					if( eclaire )
						memcpy( r->lum, lum, sizeof( r->lum ));
					memcpy( r->teinte, teinte, sizeof( r->teinte ));
					r->a_aom   = a_aom;
					r->aom_tex = aom_tex;
					if( a_aom )
						memcpy( r->aom, aom, sizeof( r->aom ));
					continue;
				}
				peau_cull( p );
				if( p->decal && tex )
					poser_passe1( p );
				if( eclaire )
					NxVita::ShaderPeauEclaireePiece( p->vbo_repos, p->vbo_poids, p->vbo_os,
					                                 p->uvbo, tex, teinte,
					                                 p->vbo_normales, p->cbo, p->spec,
					                                 p->gloss_tex, p->gloss_uvbo, p->gloss_clamp );
				else
					NxVita::ShaderPeauPiece( p->vbo_repos, p->vbo_poids, p->vbo_os,
					                         p->uvbo, tex, teinte );
				glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p->ibo );
				++g_vita_n_draws;
				glDrawElements( GL_TRIANGLE_STRIP, p->num_indices,
				                GL_UNSIGNED_SHORT, NULL );
			}
			}
			glDisable( GL_BLEND );
			glDisable( GL_CULL_FACE );
			glDepthMask( GL_TRUE );
			if( eclaire )
				NxVita::ShaderPeauEclaireeFin();
			else
				NxVita::ShaderPeauFin();
			glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
			g_vita_t_draw += sceKernelGetProcessTimeWide() - t0;
			return;
		}
	}

cpu:
	// Eclairage : les memes valeurs pour tout le modele. Echelle 0..128
	// heritee de la PS2 (DX9/p_NxLight.cpp:69 divise par 128).
	float amb[3] = { 0.0f, 0.0f, 0.0f };
	float dif[2][3] = {{ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }};
	float dir[2][3] = {{ 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }};
	bool  lighting_ready = false;
	if( g_vita_lighting )
	{
		if( p_lights )
		{
			p_lights->ResolveLighting( amb, dif, dir );
		}
		else
		{
			Image::RGBA a = Nx::CLightManager::sGetLightAmbientColor();
			amb[0] = (float)a.r / 128.0f;
			amb[1] = (float)a.g / 128.0f;
			amb[2] = (float)a.b / 128.0f;
			for( int L = 0; L < 2; ++L )
			{
				Image::RGBA d = Nx::CLightManager::sGetLightDiffuseColor( L );
				dif[L][0] = (float)d.r / 128.0f;
				dif[L][1] = (float)d.g / 128.0f;
				dif[L][2] = (float)d.b / 128.0f;
				Mth::Vector v0 = Nx::CLightManager::sGetLightDirection( L );
				dir[L][0] = -v0[0];
				dir[L][1] = -v0[1];
				dir[L][2] = -v0[2];
			}
		}
		lighting_ready = true;
	}

	// Teinte en entiers 0..256, une fois pour tout l'appel. ï¿½ Blanche ï¿½ veut
	// dire : aucune modulation, donc rien a recalculer par piece.
	int m_tint[4] = { 256, 256, 256, 255 };
	bool tint_white = true;
	if( p_rgba )
	{
		for( int c = 0; c < 3; ++c )
		{
			m_tint[c] = (int)( p_rgba[c] * 256.0f );
			if( m_tint[c] < 250 )
				tint_white = false;
		}
		m_tint[3] = (int)( p_rgba[3] * 255.0f );
		if( m_tint[3] < 250 )
			tint_white = false;
	}

	++s_chemin[2];
	s_pdt_chemin = 4;
	SceUInt64 t_phase = sceKernelGetProcessTimeWide();

	// DEUX PASSES : les morceaux OPAQUES d'abord, les TRANSLUCIDES ensuite.
	//
	// [VERIFIE sur table] dans chaque roue de vehicule, le fichier range le
	// morceau translucide (blend=5, materiau fb39174e) AVANT le morceau
	// opaque (blend=0, materiau d63fd906). Dessines dans cet ordre, le
	// translucide -- qui n'ecrit pas la profondeur (glDepthMask FALSE) --
	// est recouvert par l'opaque peint juste apres, et disparait.
	//
	// C'est la regle elementaire du rendu translucide, que le decor applique
	// deja (p_world_render.cpp : ciel, puis opaque, puis blend) mais que les
	// modeles ignoraient. Le mode 5 lui-meme etait correct : la reference
	// (DX9/NX/render.cpp, vBLEND_MODE_BLEND) demande SRCALPHA/INVSRCALPHA,
	// ce que nous posions deja.
	for( int passe = 0; passe < 2; ++passe )
	for( int i = 0; i < m_num; ++i )
	{
		SGpuMesh *p = &mp_pieces[i];

		if(( p->blend != 0 ) != ( passe == 1 ))
			continue;
		if( !p->p_weights || !p->p_bones || !p->p_skinned || !p->p_positions )
			continue;

		// Teinte blanche : le tableau de base suffit tel quel.
		if( tint_white && p->p_lit )
		{
			free( p->p_lit );
			p->p_lit = NULL;
		}

		// La POSITION ne se calcule qu'une fois par groupe de sommets
		// partages. La COULEUR, elle, appartient a chaque piece : sa teinte
		// lui est propre (le pantalon est brun, la chemise blanche). Sauter
		// toute la piece, comme je le faisais, laissait son p_lit tel que
		// malloc l'avait rendu -- de la memoire non initialisee, lue telle
		// quelle au rendu. C'est ce qui donnait au torse une teinte arbitraire
		// au lieu du noir demande.
		// Positions ET normales sont PARTAGEES par les pieces d'un meme
		// groupe : seul leur proprietaire a quelque chose a calculer. Les
		// autres entraient quand meme dans la boucle des sommets et y
		// depaquetaient leurs poids -- trois divisions par sommet, jetees
		// aussitot, dix-sept fois sur dix-huit.
		const bool do_skin = ( p->owns_skin != 0 );
		if( !do_skin )
			continue;

		// Les normales ne servent qu'a l'eclairage materiel : inutile de les
		// transformer quand il est coupe.
		const bool do_normals = g_vita_lighting && ( p->p_skinned_n != NULL )
		                        && ( p->p_normals != NULL );

		g_vita_n_verts += p->num_vertices;
		for( int v = 0; v < p->num_vertices; ++v )
		{
			const float *p_src = &p->p_positions[v * 3];
			float       *p_dst = &p->p_skinned[v * 3];

			unsigned int packed = p->p_weights[v];
			float w[3];
			// Multiplications par l'inverse plutot que divisions : une division
			// flottante coute une quinzaine de cycles sur Cortex-A9 et ne se
			// pipeline pas, et il y en avait TROIS par sommet. Le compilateur ne
			// pouvait pas le faire lui-meme -- ce fichier est compile en
			// -fno-fast-math, et a raison. Les diviseurs sont des constantes,
			// donc le faire a la main est sans danger.
			const float INV_1023 = 1.0f / 1023.0f;
			const float INV_511  = 1.0f / 511.0f;
			w[0] = (float)(( packed       ) & 0x7FF ) * INV_1023;
			w[1] = (float)(( packed >> 11 ) & 0x7FF ) * INV_1023;
			w[2] = (float)(( packed >> 22 ) & 0x3FF ) * INV_511;

			{
			// Accumulateurs en registre : la version precedente lisait et
			// reecrivait p_dst[c] a chaque terme, soit neuf allers-retours en
			// memoire par sommet la ou trois ecritures suffisent.
			float ax = 0.0f, ay = 0.0f, az = 0.0f;
			const float sx = p_src[0], sy = p_src[1], sz = p_src[2];

			for( int k = 0; k < 3; ++k )
			{
				if( w[k] <= 0.0f )
					continue;
				int b = (int)p->p_bones[v * 4 + k];
				if(( b < 0 ) || ( b >= num_bones ))
					continue;

				Mth::Matrix &m = p_bone_mats[b];
				const float wk = w[k];
				// Mth::Matrix est range par lignes, translation en ligne 3. Chaque
				// terme est lu une fois : la boucle sur c parcourait la matrice par
				// COLONNE, donc par pas de quatre flottants -- le pire cas pour une
				// ligne de cache.
				const float m00 = m[0][0], m10 = m[1][0], m20 = m[2][0], m30 = m[3][0];
				const float m01 = m[0][1], m11 = m[1][1], m21 = m[2][1], m31 = m[3][1];
				const float m02 = m[0][2], m12 = m[1][2], m22 = m[2][2], m32 = m[3][2];

				ax += wk * (( sx * m00 ) + ( sy * m10 ) + ( sz * m20 ) + m30 );
				ay += wk * (( sx * m01 ) + ( sy * m11 ) + ( sz * m21 ) + m31 );
				az += wk * (( sx * m02 ) + ( sy * m12 ) + ( sz * m22 ) + m32 );
			}

			p_dst[0] = ax;
			p_dst[1] = ay;
			p_dst[2] = az;
			}	// fin de do_skin

			if( !do_normals )
				continue;

			// La normale subit la ROTATION seule -- pas la translation, sinon
			// elle pointerait vers la position de l'os au lieu d'indiquer une
			// direction. Comme la position, elle ne se calcule QU'UNE FOIS par
			// groupe de sommets partages : c'est le proprietaire du skinning
			// qui s'en charge, les autres pieces la relisent.
			float *p_sn = &p->p_skinned_n[v * 3];

			{
				const float *p_n = &p->p_normals[v * 3];
				float n[3] = { 0.0f, 0.0f, 0.0f };
				for( int k = 0; k < 3; ++k )
				{
					if( w[k] <= 0.0f )
						continue;
					int b = (int)p->p_bones[v * 4 + k];
					if(( b < 0 ) || ( b >= num_bones ))
						continue;
					Mth::Matrix &m = p_bone_mats[b];
					for( int c = 0; c < 3; ++c )
						n[c] += w[k] * ( p_n[0] * m[0][c]
						               + p_n[1] * m[1][c]
						               + p_n[2] * m[2][c] );
				}

				float len = sqrtf( n[0]*n[0] + n[1]*n[1] + n[2]*n[2] );
				if( len > 0.0001f )
				{
					n[0] /= len; n[1] /= len; n[2] /= len;
				}
				p_sn[0] = n[0]; p_sn[1] = n[1]; p_sn[2] = n[2];
			}

			// PLUS AUCUN calcul d'eclairage ici : le GPU le fait.
			//
			// Le moteur d'origine ne l'a jamais calcule au CPU non plus -- il
			// pose les lumieres dans des registres et laisse le vertex shader
			// travailler (DX9/p_NxLight.cpp:69 ecrit dans EngineGlobals, le
			// VU1 fait pareil sur PS2). Ma version CPU refaisait a la main ce
			// que le materiel donne gratuitement : mesure, 153 ms par frame.
			//
			// Il ne reste donc que le skinning de la normale, ci-dessus, qui
			// lui est indispensable et ne se fait qu'une fois par groupe.
		}


		// Un sommet temoin : si la transformation produit des zeros ou des
		// valeurs aberrantes, le maillage est invisible ou hors champ, et la
		// difference se voit ici bien avant de se voir a l'ecran.
		{
			static int s_t = 0;
			if(( i == 0 ) && ( p->num_vertices > 0 ) && ( s_t < 6 ))
			{
				++s_t;
				unsigned int pk = p->p_weights[0];
				VLOG( "SKIN", "repos(%.1f %.1f %.1f) -> peau(%.1f %.1f %.1f) "
				              "poids %.2f/%.2f/%.2f os %d/%d/%d",
				      p->p_positions[0], p->p_positions[1], p->p_positions[2],
				      p->p_skinned[0], p->p_skinned[1], p->p_skinned[2],
				      (double)(( pk & 0x7FF ) / 1023.0f),
				      (double)((( pk >> 11 ) & 0x7FF ) / 1023.0f),
				      (double)((( pk >> 22 ) & 0x3FF ) / 511.0f),
				      (int)p->p_bones[0], (int)p->p_bones[1], (int)p->p_bones[2] );
			}
		}

	}

	// PAS de glBufferData ici.
	//
	// Premiere version : un glBufferData par maillage et par frame. C'est une
	// REALLOCATION complete du tampon a chaque image, et le pilote doit
	// attendre que le GPU en ait fini avec l'ancien contenu. Mesure : la
	// cadence est tombee de 60 a 4 fps.
	//
	// Les sommets skinnes changent a chaque frame : ils n'ont rien a faire
	// dans un tampon GPU. On les passe en tableau CLIENT, que VitaGL recopie
	// dans son flux de commandes -- pas de reallocation, pas d'attente.
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	g_vita_t_skin += sceKernelGetProcessTimeWide() - t_phase;
	t_phase = sceKernelGetProcessTimeWide();

	glEnableClientState( GL_VERTEX_ARRAY );

	// Eclairage confie au GPU, comme dans le moteur d'origine.
	//
	// Les positions et normales skinnees sont en espace MONDE, et la matrice
	// courante ne contient que la vue : poser les directions ici les fait donc
	// transformer correctement (OpenGL applique la modelview courante a
	// GL_POSITION). Le quatrieme terme a zero = lumiere DIRECTIONNELLE.
	if( g_vita_lighting && lighting_ready )
	{
		// LA MATRICE COURANTE DECIDE DE LA DIRECTION DES LUMIERES.
		//
		// glLightfv( GL_POSITION ) applique la MODELVIEW du moment. La version
		// precedente les posait alors que la matrice valait Â« vue x matrice du
		// modele Â» : chaque personnage eclairait donc dans SON repere, et la
		// direction tournait avec lui. C'est ce qui faisait clignoter et virer
		// les couleurs d'un modele a l'autre.
		//
		// Nos normales skinnees sont en espace MONDE : il faut donc poser les
		// lumieres avec la VUE SEULE. On l'installe le temps de le faire.
		float view[16];
		const bool have_view = NxVita::GetViewMatrix( view );

		glMatrixMode( GL_MODELVIEW );
		glPushMatrix();
		if( have_view )
			glLoadMatrixf( view );

		const float amb4[4] = { amb[0], amb[1], amb[2], 1.0f };
		glLightModelfv( GL_LIGHT_MODEL_AMBIENT, amb4 );

		for( int L = 0; L < 2; ++L )
		{
			const GLenum id = ( L == 0 ) ? GL_LIGHT0 : GL_LIGHT1;
			const float pos4[4] = { dir[L][0], dir[L][1], dir[L][2], 0.0f };
			const float col4[4] = { dif[L][0], dif[L][1], dif[L][2], 1.0f };
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			glLightfv( id, GL_POSITION, pos4 );
			glLightfv( id, GL_DIFFUSE,  col4 );
			glLightfv( id, GL_AMBIENT,  zero );
			glLightfv( id, GL_SPECULAR, zero );
			glEnable( id );
		}

		glPopMatrix();

		// La teinte de la piece devient la couleur de MATIERE. Il FAUT dire
		// laquelle : sans glColorMaterial explicite, le mode par defaut varie
		// et glColor4f -- pose a trois endroits differents dans cette fonction
		// -- donnait des teintes imprevisibles, d'ou les personnages jaunes.
		glEnable( GL_LIGHTING );
		glColorMaterial( GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE );
		glEnable( GL_COLOR_MATERIAL );

		// Et il faut aussi neutraliser ce que le materiau apporte de son
		// cote : par defaut OpenGL ajoute une speculaire et une emission qui
		// n'ont rien a voir avec ce que le moteur demande.
		const float noir[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		glMaterialfv( GL_FRONT_AND_BACK, GL_SPECULAR, noir );
		glMaterialfv( GL_FRONT_AND_BACK, GL_EMISSION, noir );
	}
	else
	{
		glDisable( GL_LIGHTING );
	}

	// Une piece entiere (le pantalon) manque a l'ecran alors que ses tables
	// CAS s'appliquent : ce compteur nomme dit, pour chaque mesh, s'il est
	// APPELE au rendu et combien de morceaux sortent reellement.
	int drawn = 0, skipped = 0;
	NxVita::BrouillardFixe( 1 );		// issue #45, pipeline fixe

	// TEST DISCRIMINANT -- corps et pantalon dessines, places, opaques, et
	// pourtant invisibles. On les force en MAGENTA, sans texture, sans depth
	// test : s'ils apparaissent, leurs triangles rasterisent et le probleme
	// est un recouvrement ; sinon, leurs draw calls ne produisent AUCUN pixel
	// et le probleme est dans les indices ou vitaGL. Reponse binaire.
	const bool debug_flash = false;	// verdict rendu : reconstruction in-place
	if( debug_flash )
		glDisable( GL_DEPTH_TEST );

	for( int i = 0; i < m_num; ++i )
	{
		SGpuMesh *p = &mp_pieces[i];
		if( !p->p_skinned || p->decal )		// passe 1 (#55) : ce chemin ne melange pas
		{
			++skipped;
			continue;
		}
		++drawn;

		// Tableau CLIENT, conformement a la mesure consignee plus haut : le
		// VBO dynamique a deja ete essaye ici et coutait plus cher.
		glVertexPointer( 3, GL_FLOAT, 0, p->p_skinned );

		// Eclairage par sommet quand il a pu etre calcule. Le tableau de
		// couleurs REMPLACE glColor4f -- la teinte y est deja multipliee.
		// Une piece sans normales ne peut pas etre eclairee : elle ressortira
		// claire au milieu d'une silhouette noire. On compte les cas.
		if( !p->p_skinned_n )
		{
			static int s_nn = 0;
			if(( s_nn++ % 900 ) < 2 )
				VLOG( "LUM", "'%s' piece %d : AUCUNE normale (%d sommets)",
				      m_vita_name, i, p->num_vertices );
		}

		// Les normales partent au GPU, qui applique l'eclairage pose plus bas.
		if( p->p_skinned_n && g_vita_lighting && !debug_flash )
		{
			glEnableClientState( GL_NORMAL_ARRAY );
			glNormalPointer( GL_FLOAT, 0, p->p_skinned_n );
		}
		else
		{
			glDisableClientState( GL_NORMAL_ARRAY );
		}
		glDisableClientState( GL_COLOR_ARRAY );

		if( debug_flash )
		{
			glDisableClientState( GL_TEXTURE_COORD_ARRAY );
			glDisable( GL_TEXTURE_2D );
			glColor4f( 1.0f, 0.0f, 1.0f, 1.0f );	// magenta
		}
		else if( p->texture && p->uvbo )
		{
			glEnable( GL_TEXTURE_2D );
			++NxVita::g_vita_ordre_modeles;
		glBindTexture( GL_TEXTURE_2D, p->texture );
			glEnableClientState( GL_TEXTURE_COORD_ARRAY );
			glBindBuffer( GL_ARRAY_BUFFER, p->uvbo );
			glTexCoordPointer( 2, GL_FLOAT, 0, NULL );
			glBindBuffer( GL_ARRAY_BUFFER, 0 );
			// Teinte du geom, pas blanc fixe : le patch precedent n'avait
			// corrige que Draw(), et le skater du menu -- SKINNE -- restait
			// blanc au lieu de devenir la silhouette noire.
			if( p_rgba && !p->color_locked )
				glColor4f( p_rgba[0], p_rgba[1], p_rgba[2], p_rgba[3] );
			else
				glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
		}
		else
		{
			glDisableClientState( GL_TEXTURE_COORD_ARRAY );
			glDisable( GL_TEXTURE_2D );
			if( p_rgba && !p->color_locked )
				glColor4f( p_rgba[0], p_rgba[1], p_rgba[2], p_rgba[3] );
			else
				glColor4f( 0.8f, 0.8f, 0.85f, 1.0f );
		}


		// MATRICE DE L'OS DU MORCEAU, pour les modeles a parties RIGIDES.
		//
		// [VERIFIE sur table] les vehicules n'ont AUCUN drapeau POIDS (0x10) :
		// leurs six secteurs -- carrosserie, roues -- sont chacun attaches a
		// UN os, et c'est la matrice de cet os qui les place. Le parseur
		// jetait cet index (« skip( p_file, 4 ); // index d'os ») et le rendu
		// n'appliquait rien : toutes les parties se dessinaient empilees a
		// l'origine du modele. D'ou une voiture « verticale » et amputee de
		// ses roues, ce qu'aucune rotation seule n'expliquait.
		//
		// Le skinning par sommet est un mecanisme DIFFERENT (DrawSkinned) :
		// les deux ne coexistent pas sur un meme morceau.
		// D'OU VIENT LA MATRICE DE CE MORCEAU ?
		//
		// Deux sources, dans cet ordre :
		//
		//  1. les matrices d'os fournies par le moteur, quand le modele est
		//     ANIME (roues qui tournent) ;
		//  2. a defaut, la matrice de SETUP de la hierarchie du fichier.
		//
		// Le cas 2 n'est pas un repli de fortune, c'est le cas NORMAL des
		// vehicules : CModel::Render passe deliberement ( NULL, 0 ) quand
		// l'objet n'est pas anime (« update root position without updating
		// bones »). Mesure a l'appui : les traces montraient
		// « os=0 mats=0x0 n=0 » alors que la hierarchie etait bien lue et les
		// matrices bien calculees en amont.
		//
		// La matrice de setup EST la transformation locale de la piece ; sans
		// animation, c'est exactement ce qu'il faut appliquer.
		const Mth::Matrix *p_mat_os = NULL;
		Mth::Matrix        compose;
		if( p->sector_bone >= 0 )
		{
			if( p_bone_mats && ( p->sector_bone < num_bones ))
				p_mat_os = &p_bone_mats[p->sector_bone];
			else if( mp_hierarchyObjects && m_numHierarchyObjects )
			{
				// COMPOSER AVEC LA CHAINE DE PARENTS.
				//
				// [SOURCE] car.cpp:187 : « frontTire = obj[2].GetSetupMatrix();
				// frontTire *= obj[0].GetSetupMatrix(); » -- la matrice d'un
				// objet est LOCALE a son parent, et doit etre multipliee par
				// celle du parent pour arriver dans le repere du modele.
				//
				// C'est ce qui manquait : appliquer la seule matrice locale
				// laissait les roues sans la rotation portee par l'objet
				// racine, dont elles descendent toutes (parent = 0).
				int idx = -1;
				for( int k = 0; k < m_numHierarchyObjects; ++k )
				{
					if( mp_hierarchyObjects[k].GetBoneIndex() == p->sector_bone )
					{
						idx = k;
						break;
					}
				}
				if( idx >= 0 )
				{
					compose = mp_hierarchyObjects[idx].GetSetupMatrix();

					// Remontee bornee : une hierarchie corrompue ne doit pas
					// boucler indefiniment dans la boucle de rendu.
					int parent = mp_hierarchyObjects[idx].GetParentIndex();
					for( int garde = 0;
					     ( parent >= 0 ) && ( parent < m_numHierarchyObjects )
					     && ( garde < 8 ); ++garde )
					{
						compose *= mp_hierarchyObjects[parent].GetSetupMatrix();
						parent = mp_hierarchyObjects[parent].GetParentIndex();
					}
					p_mat_os = &compose;

				}
			}
		}
		const bool os_pose = ( p_mat_os != NULL );

		if( os_pose )
		{
			glPushMatrix();
			glMultMatrixf( (const float *)p_mat_os );
		}

		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p->ibo );
		++g_vita_n_draws;
		NxVita::BrouillardFixeNoir(( p->blend >= 1 ) && ( p->blend <= 4 ));
		glDrawElements( GL_TRIANGLE_STRIP, p->num_indices,
		                GL_UNSIGNED_SHORT, NULL );

		if( os_pose )
			glPopMatrix();
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	}

	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );
	glDisableClientState( GL_NORMAL_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );

	// REMETTRE TOUT l'etat d'eclairage, pas seulement GL_LIGHTING.
	//
	// Les lumieres et GL_COLOR_MATERIAL restaient armes en sortant d'ici. Le
	// decor du monde, lui, porte son eclairage dans ses COULEURS DE SOMMETS :
	// repasser dessus avec des lumieres noires l'eteint. C'est ce qui a rendu
	// une grande partie du niveau noire.
	glDisable( GL_LIGHT0 );
	glDisable( GL_LIGHT1 );
	glDisable( GL_COLOR_MATERIAL );
	glDisable( GL_LIGHTING );

	NxVita::BrouillardFixe( 0 );
	g_vita_t_draw += sceKernelGetProcessTimeWide() - t_phase;
	glDisable( GL_TEXTURE_2D );
	if( debug_flash )
		glEnable( GL_DEPTH_TEST );

	{
		// PERIODIQUE, pas un budget epuise au boot : les traces mortes des la
		// premiere minute m'ont laisse aveugle sur EDIT SKATER une manche
		// entiere. Une salve reguliere coute quelques lignes toutes les ~10 s
		// et reste vivante toute la session.
		static int s_n = 0;
		if(( s_n++ % 1800 ) < 10 )
		{
			// Centre et etendue des positions SKINNEES du premier morceau :
			// une piece dessinee mais invisible a soit un centre aberrant
			// (partie ailleurs), soit une etendue quasi nulle (effondree).
			// C'est la mesure qui separe les deux familles restantes.
			float cx = 0, cy = 0, cz = 0, r = 0;
			if( m_num > 0 && mp_pieces[0].p_skinned && mp_pieces[0].num_vertices > 0 )
			{
				const float *ps = mp_pieces[0].p_skinned;
				const int    nv = mp_pieces[0].num_vertices;
				for( int v = 0; v < nv; ++v )
				{
					cx += ps[v * 3];  cy += ps[v * 3 + 1];  cz += ps[v * 3 + 2];
				}
				cx /= nv;  cy /= nv;  cz /= nv;
				for( int v = 0; v < nv; ++v )
				{
					const float dx = ps[v * 3] - cx, dy = ps[v * 3 + 1] - cy,
					            dz = ps[v * 3 + 2] - cz;
					const float d2 = dx * dx + dy * dy + dz * dz;
					if( d2 > r )
						r = d2;
				}
				r = sqrtf( r );
			}
			VLOG( "DRAW", "'%s' skinne : %d dessines, %d sautes | "
			              "centre (%.1f %.1f %.1f) rayon %.1f | teinte %.2f %.2f %.2f %.2f",
			      m_vita_name, drawn, skipped, cx, cy, cz, r,
			      p_rgba ? p_rgba[0] : -1.0f, p_rgba ? p_rgba[1] : -1.0f,
			      p_rgba ? p_rgba[2] : -1.0f, p_rgba ? p_rgba[3] : -1.0f );
		}
	}
}


// Vide la file des morceaux translucides, une fois le decor pose.
//
// Appelee depuis s_plat_render_world juste apres RenderWorld. Chaque entree
// porte sa matrice modelview complete : on la repose telle quelle, on remet
// l'etat de melange du morceau, et on dessine.
// Morceaux translucides SKINNES reportes (#46) : rejoues apres le decor avec
// leurs propres matrices d'os, comme ils l'auraient ete pendant game_logic.
static void dessiner_peau_reportee( void )
{
	if( s_num_peau_reportees <= 0 )
		return;
	static const float blanc[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	(void)blanc;
	glEnable( GL_DEPTH_TEST );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glDepthMask( g_vita_zw_peau ? GL_TRUE : GL_FALSE );	// XBox : profondeur ecrite en semi-transparent (#46)
	glActiveTexture( GL_TEXTURE0 );
	for( int i = 0; i < s_num_peau_reportees; ++i )
	{
		const SPeauReportee *r = &s_peau_reportees[i];
		const SGpuMesh *p = r->p_piece;
		if( !p || !p->vbo_repos || !p->num_indices )
			continue;
		const bool tex = ( p->texture && p->uvbo );
		// Meme ordre que le dessin immediat : programme, PUIS texture.
		if( r->eclaire )
		{
			NxVita::ShaderPeauEclaireeDebut( r->mvp, r->os, r->num_os, r->lum );
			if( r->a_aom )
				NxVita::ShaderPeauEclaireeAutoOmbre( r->aom, r->aom_tex, r->aom[12], r->aom[13], r->aom[14] );
			glActiveTexture( GL_TEXTURE0 );
			if( tex )
				glBindTexture( GL_TEXTURE_2D, p->texture );
			NxVita::ShaderPeauEclaireePiece( p->vbo_repos, p->vbo_poids, p->vbo_os,
			                                 p->uvbo, tex, r->teinte,
			                                 p->vbo_normales, p->cbo, p->spec,
			                                 p->gloss_tex, p->gloss_uvbo, p->gloss_clamp );
		}
		else
		{
			NxVita::ShaderPeauDebut( r->mvp, r->os, r->num_os );
			glActiveTexture( GL_TEXTURE0 );
			if( tex )
				glBindTexture( GL_TEXTURE_2D, p->texture );
			NxVita::ShaderPeauPiece( p->vbo_repos, p->vbo_poids, p->vbo_os,
			                         p->uvbo, tex, r->teinte );
		}
		peau_cull( p );
		if( p->decal && tex )
			poser_passe1( p );
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p->ibo );
		++g_vita_n_draws;
		glDrawElements( GL_TRIANGLE_STRIP, p->num_indices, GL_UNSIGNED_SHORT, NULL );
		// Apres RenderWorld le test est strict (p_world_render.cpp, fin du
		// ciel) : on l'y remet.
		if( p->decal && tex )
			glDepthFunc( GL_LESS );
		if( r->eclaire )
			NxVita::ShaderPeauEclaireeFin();
		else
			NxVita::ShaderPeauFin();
	}
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	{
		static int s_max = 0, s_img = 0;
		if( s_num_peau_reportees > s_max ) s_max = s_num_peau_reportees;
		if(( ++s_img % 300 ) == 0 )
		{
			VLOG( "PEAU", "translucides skinnes reportes : max %d par image (file %d)", s_max, MAX_PEAU_REPORTEES );
			s_max = 0;
		}
	}
	glDepthMask( GL_TRUE );
	s_num_peau_reportees = 0;
}

// INSTANCES PERSISTANTES (#54).
//
// Sur Vita un modele est dessine DEPUIS plat_render, appele par
// CModelComponent::Update dans la phase logique. Quand le jeu est en pause
// (menus en jeu) ou qu'un objet est suspendu (SuspendComponent, distance),
// Update n'est plus appele : skater, voitures et pietons disparaissaient.
// XBox, lui, garde une instance par geom, dessinee a CHAQUE image avec la
// derniere matrice recue (p_NxGeom.cpp:493, render_instances) tant qu'elle
// est active. On fait pareil : chaque geom retient sa derniere pose, et le
// rendu rejoue celles qui n'ont pas ete posees dans l'image. Cacher un
// modele passe par plat_set_active (m_vita_affiche), donc reste respecte.
// "rej 0/1" pour comparer.
int      g_vita_rejeu = 1;
// #48 : par image, geoms dessines / inactifs / sans camera / culles / rejoues.
int      g_cpt_dessin[5];
int      g_vita_zw_modeles = 1;	// "zwm 0/1" (#48)
int VitaDessinsEnAttente( void ) { return g_cpt_dessin[0]; }
// "osp 0/1" : derniere pose d'os des vehicules non animes cette image (#48).
int      g_vita_os_persist = 1;
static unsigned   s_image_modeles = 1;
static unsigned   s_image_avant_pause = 0;	// derniere image jouee hors pause
static bool       s_en_rejeu = false;
static const int  MAX_POSES = 2048;
static CVitaGeom *s_poses[MAX_POSES];
static int        s_num_poses = 0;

void CVitaGeom::VitaRetenirPose( const Mth::Matrix *pRoot, Mth::Matrix *pOs, int nOs )
{
	if( s_en_rejeu )
		return;
	if( g_vita_rejeu == 2 && mp_mesh )
		VLOG( "MDL", "vivant %s pos (%.0f %.0f %.0f)", mp_mesh->Name() ? mp_mesh->Name() : "?",
		      (*pRoot)[3][0], (*pRoot)[3][1], (*pRoot)[3][2] );
	m_vita_pose_racine = *pRoot;
	mp_vita_pose_os    = pOs;
	m_vita_pose_nos    = nOs;
	m_vita_pose_image  = s_image_modeles;
	m_vita_pose_ok     = true;
	if( !m_vita_pose_inscrit )
	{
		if( s_num_poses < MAX_POSES )
		{
			s_poses[s_num_poses++] = this;
			m_vita_pose_inscrit = true;
		}
		else
		{
			static bool s_dit = false;
			if( !s_dit ) { s_dit = true; VLOG( "MDL", "!! plus de place pour les poses (%d)", s_num_poses ); }
		}
	}
}

void CVitaGeom::VitaOublierPose()
{
	if( !m_vita_pose_inscrit )
		return;
	for( int i = 0; i < s_num_poses; ++i )
		if( s_poses[i] == this )
		{
			s_poses[i] = s_poses[--s_num_poses];
			break;
		}
	m_vita_pose_inscrit = false;
	m_vita_pose_ok      = false;
}

const char *CVitaGeom::VitaNomTrou( unsigned image ) const
{
	if( !m_vita_pose_ok || !m_vita_affiche || !mp_mesh || ( image - m_vita_pose_image ) != 1 )
		return NULL;
	return mp_mesh->Name();
}

bool CVitaGeom::VitaRejouerPose( unsigned image, unsigned fenetre )
{
	// image 0 : derniere pose quelle qu'elle soit (objet en pause, CO_PAUSED).
	if( !m_vita_pose_ok || !m_vita_affiche || !mp_mesh )
		return false;
	// fenetre > 0 : pose vieille de 1 a fenetre images (objet anime une image
	// sur 2 ou 4 par SuspendComponent, #48).
	if( fenetre && (( s_image_modeles - m_vita_pose_image ) > fenetre ))
		return false;
	if(( image && !fenetre && ( m_vita_pose_image != image ))
	    || ( m_vita_pose_image == s_image_modeles )		// deja pose en direct
	    || ( m_vita_rejeu_image == s_image_modeles ))	// deja rejoue
		return false;
	m_vita_rejeu_image = s_image_modeles;
	Mth::Matrix racine = m_vita_pose_racine;	// plat_render peut la modifier
	if( g_vita_rejeu == 2 )		// "rej 2" : nomme les geoms rejoues, une image
		VLOG( "MDL", "rejeu %s pos (%.0f %.0f %.0f) os %d", mp_mesh->Name() ? mp_mesh->Name() : "?",
		      m_vita_pose_racine[3][0], m_vita_pose_racine[3][1], m_vita_pose_racine[3][2], m_vita_pose_nos );
	plat_render( &racine, mp_vita_pose_os, m_vita_pose_nos );
	return true;
}

// Objet en pause (CO_PAUSED) : CCompositeObject::Update sort sans mettre ses
// composants a jour, donc sans plat_render. Appelee de la, sa derniere pose
// est redessinee a sa place dans la phase logique (#54 : skater cache par la
// boite « Stat increased », qui ne passe pas par FrontEnd::PauseGame).
void VitaRejouerModele( CModel *p_model )
{
	if( !g_vita_rejeu || !p_model )
		return;
	s_en_rejeu = true;
	const int n = p_model->GetNumGeoms();
	for( int i = 0; i < n; ++i )
	{
		CVitaGeom *g = static_cast< CVitaGeom * >( p_model->GetGeomByIndex( i ));
		if( g )
			g->VitaRejouerPose( 0 );
	}
	s_en_rejeu = false;
}

static void rejouer_poses( void )
{
	// Seulement EN PAUSE (FrontEnd::PauseGame : objets du jeu suspendus) : on
	// refige les geoms poses dans la derniere image jouee. Ceux que le menu
	// pose en direct (planche 3D du menu pause) ont une image plus recente et
	// ne sont pas doubles. Les objets suspendus un par un (distance) ne sont
	// pas rejoues : parmi eux des restes sans place (mainmenu_bg dans NJ,
	// ombres de pietons a l'origine) que XBox ne montre pas non plus.
	Mdl::FrontEnd *p_fe = Mdl::FrontEnd::Instance();
	const bool pause = p_fe && p_fe->GamePaused();
	const unsigned image = s_image_avant_pause;
	int n = 0;
	if( !pause )
		s_image_avant_pause = s_image_modeles;
	// HORS PAUSE (#48, clignotement des pietons, voitures, pigeons) : a distance
	// SuspendComponent n'anime un objet qu'une image sur 2 ou 4
	// (SuspendComponent.cpp:325), et notre dessin part de cette mise a jour :
	// l'objet disparaissait les images sautees. XBox redessine chaque instance
	// a chaque image. On rejoue donc les poses RECENTES (1 a 8 images) ; les
	// restes jamais reposes (ombres a l'origine, mainmenu_bg) restent exclus.
	if( g_vita_rejeu && ( !pause || s_image_avant_pause ))
	{
		glMatrixMode( GL_MODELVIEW );
		glPushMatrix();
		// Copie : plat_render peut inscrire un geom (registre modifie).
		const int total = s_num_poses;
		s_en_rejeu = true;
		for( int i = 0; i < total && i < s_num_poses; ++i )
			if( s_poses[i]->VitaRejouerPose( pause ? image : 0, pause ? 0 : 8 ))
				++n;
		s_en_rejeu = false;
		glMatrixMode( GL_MODELVIEW );
		glPopMatrix();
	}
	// TROUS (#48) : geoms poses a l'image precedente mais pas a celle-ci, par
	// nom, cumules sur 5 s. Dit QUI clignote, rejeu actif ou non.
	if( !pause )
	{
		static const char *s_nom[32];
		static int s_trous[32];
		static int s_nn = 0;
		static SceUInt64 s_tt = 0;
		for( int i = 0; i < s_num_poses; ++i )
		{
			const char *nm = s_poses[i]->VitaNomTrou( s_image_modeles );
			if( !nm )
				continue;
			int k = 0;
			while(( k < s_nn ) && ( s_nom[k] != nm ))
				++k;
			if( k == s_nn )
			{
				if( s_nn == 32 )
					continue;
				s_nom[s_nn] = nm; s_trous[s_nn] = 0; ++s_nn;
			}
			++s_trous[k];
		}
		const SceUInt64 tn = sceKernelGetProcessTimeWide();
		if( tn - s_tt > 5000000 )
		{
			char b[300]; int o = 0;
			for( int k = 0; k < s_nn && o < 260; ++k )
				o += snprintf( b + o, sizeof( b ) - o, " %s:%d", s_nom[k], s_trous[k] );
			if( s_nn )
				VLOG( "MDL", "trous (5 s) :%s", b );
			s_nn = 0; s_tt = tn;
		}
	}
	// CHUTE (#48) : une image qui dessine nettement moins de modeles que la
	// precedente. Dit pourquoi (inactifs, sans camera, culles) ou s'il n'y
	// a simplement eu AUCUN appel du moteur (logique sautee).
	{
		static int s_prec = 0, s_nchutes = 0;
		const int d = g_cpt_dessin[0] + g_cpt_dessin[4];
		if(( s_prec >= 6 ) && ( d * 10 < s_prec * 7 ) && ( s_nchutes < 400 ))
		{
			++s_nchutes;
			VLOG( "CHUTE", "image %u : %d dessines (avant %d) | direct %d rejeu %d inactifs %d sans_cam %d culles %d | pause %d",
			      s_image_modeles, d, s_prec, g_cpt_dessin[0], g_cpt_dessin[4], g_cpt_dessin[1],
			      g_cpt_dessin[2], g_cpt_dessin[3], (int)pause );
		}
		s_prec = d;
		for( int k = 0; k < 5; ++k ) g_cpt_dessin[k] = 0;
	}
	if( g_vita_rejeu == 2 )
		g_vita_rejeu = 1;
	++s_image_modeles;
	static SceUInt64 s_t = 0;
	static int s_cumul = 0, s_images = 0, s_max = 0;
	s_cumul += n; ++s_images; if( n > s_max ) s_max = n;
	const SceUInt64 t = sceKernelGetProcessTimeWide();
	if( t - s_t > 5000000 )
	{
		if( s_cumul )
			VLOG( "MDL", "rejeu des poses : %.1f geoms/image (max %d) sur %d inscrits",
			      (float)s_cumul / (float)s_images, s_max, s_num_poses );
		s_t = t; s_cumul = 0; s_images = 0; s_max = 0;
	}
}

void VitaDessinerModelesReportes( void )
{
	// Decoupe du poste (issue #69, « pmd ») : rejeu, peau, puis rigides
	// (MW_REPORTES). Appelee entre l'etape MW_REPORTES et VitaMondeFin.
	NxVita::VitaMondeEtape( NxVita::MW_REP_REJEU );
	rejouer_poses();		// #54 : avant de vider les files reportees
	NxVita::VitaMondeEtape( NxVita::MW_REP_PEAU );
	dessiner_peau_reportee();
	NxVita::VitaMondeEtape( NxVita::MW_REPORTES );
	if( s_num_reportes <= 0 )
		return;

	glMatrixMode( GL_MODELVIEW );
	glPushMatrix();

	glEnable( GL_DEPTH_TEST );
	glDisable( GL_LIGHTING );
	glDisable( GL_COLOR_MATERIAL );
	glEnableClientState( GL_VERTEX_ARRAY );
	NxVita::BrouillardFixe( 1 );		// issue #45

	// « mro » (issue #69) : rien dans la boucle ne touche au melange ni a la
	// profondeur (BrouillardFixeNoir ne pose que la couleur du brouillard) :
	// poses une fois. Matrice, facteur de melange et couleur : seulement
	// quand ils changent. Dans vitaGL, glLoadMatrixf marque la matrice sale
	// (produit MVP et televersement des uniformes au dessin suivant) et
	// glColor4f les uniformes de fragment ; glDepthMask emet deux commandes
	// GXM. Sautes, l'etat reste celui du morceau precedent, egal.
	const bool mro = g_vita_mro;
	float  mv_prec[16];
	bool   mv_ok = false;
	GLenum dst_prec = GL_ZERO;
	bool   dst_ok = false;
	float  coul_prec[4];
	bool   coul_ok = false;
	static int s_mro_n = 0, s_mro_mat = 0, s_mro_img = 0;
	if( mro )
	{
		glEnable( GL_BLEND );
		glDepthMask( GL_FALSE );
	}

	for( int i = 0; i < s_num_reportes; ++i )
	{
		const SMorceauReporte *r = &s_reportes[i];
		const SGpuMesh        *p = r->p_piece;
		if( !p || !p->num_indices )
			continue;
		++s_mro_n;

		if( !mro || !mv_ok || ( memcmp( r->mv, mv_prec, sizeof( mv_prec )) != 0 ))
		{
			glLoadMatrixf( r->mv );
			memcpy( mv_prec, r->mv, sizeof( mv_prec ));
			mv_ok = true;
		}
		else
			++s_mro_mat;

		const GLenum dst = ( p->blend == 1 || p->blend == 2 )
		                   ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA;
		if( !mro )
			glEnable( GL_BLEND );
		if( !mro || !dst_ok || ( dst != dst_prec ))
		{
			glBlendFunc( GL_SRC_ALPHA, dst );
			dst_prec = dst;
			dst_ok = true;
		}

		// Le decor est deja pose : un morceau translucide n'a plus rien a
		// masquer, et ne doit pas ecrire la profondeur -- c'est tout l'interet
		// de l'avoir reporte. Il se compose simplement par-dessus.
		if( !mro )
			glDepthMask( GL_FALSE );

		if( p->cbo )
		{
			glEnableClientState( GL_COLOR_ARRAY );
			glBindBuffer( GL_ARRAY_BUFFER, p->cbo );
			glColorPointer( 4, GL_UNSIGNED_BYTE, 0, NULL );
		}
		else
			glDisableClientState( GL_COLOR_ARRAY );

		glBindBuffer( GL_ARRAY_BUFFER, p->vbo );
		glVertexPointer( 3, GL_FLOAT, 0, NULL );

		float coul[4];
		if( p->texture && p->uvbo )
		{
			glEnable( GL_TEXTURE_2D );
			glBindTexture( GL_TEXTURE_2D, p->texture );
			glEnableClientState( GL_TEXTURE_COORD_ARRAY );
			glBindBuffer( GL_ARRAY_BUFFER, p->uvbo );
			glTexCoordPointer( 2, GL_FLOAT, 0, NULL );
			coul[0] = coul[1] = coul[2] = coul[3] = 1.0f;
		}
		else
		{
			glDisableClientState( GL_TEXTURE_COORD_ARRAY );
			glDisable( GL_TEXTURE_2D );
			coul[0] = 0.8f; coul[1] = 0.8f; coul[2] = 0.85f; coul[3] = 1.0f;
		}
		if( r->a_rgba )
			for( int k = 0; k < 4; ++k )
				coul[k] = r->rgba[k];
		if( !mro || !coul_ok || ( memcmp( coul, coul_prec, sizeof( coul )) != 0 ))
		{
			glColor4f( coul[0], coul[1], coul[2], coul[3] );
			memcpy( coul_prec, coul, sizeof( coul ));
			coul_ok = true;
		}

		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p->ibo );
		++g_vita_n_draws;
		NxVita::BrouillardFixeNoir(( p->blend >= 1 ) && ( p->blend <= 4 ));
		// Halos de la rampe et des phares (#77) : UV wibble.
		const bool uvw_pose = p->texture && p->uvbo
		                      && uvw_modele_poser( p->uvw0, p->uvw_par0 );
		glDrawElements( GL_TRIANGLE_STRIP, p->num_indices,
		                GL_UNSIGNED_SHORT, NULL );
		if( uvw_pose )
			uvw_modele_lever();
	}

	// Etat rendu tel qu'on l'a trouve : un melange ou une profondeur laisses
	// derriere soi teintent tout ce qui suit. Trois defauts de ce projet
	// venaient exactement de la.
	glDisableClientState( GL_COLOR_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisable( GL_TEXTURE_2D );
	glDisable( GL_BLEND );
	glDepthMask( GL_TRUE );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	glPopMatrix();

	NxVita::BrouillardFixe( 0 );
	s_num_reportes = 0;
	if(( ++s_mro_img % 300 ) == 0 )
	{
		VLOG( "MDL", "translucides rigides reportes : %.1f morceaux/image, matrice reprise "
		             "pour %.1f (mro %d)",
		      s_mro_n / 300.0f, s_mro_mat / 300.0f, g_vita_mro ? 1 : 0 );
		s_mro_n = s_mro_mat = 0;
	}
}


// "rlu 0/1" : eclairage des modeles rigides (XBox instance.cpp:200).
bool g_vita_rigide_lum = true;
// "rgp 0/1" (#67) : cet eclairage calcule par le GPU (shader rigide eclaire,
// p_shader_decor.cpp) au lieu d'eclairer_piece. 0 = chemin CPU. Programme
// refuse a la compilation : chemin CPU.
bool g_vita_rigide_gpu = false;
// "lpi 0/1" (#78) : couleurs eclairees des pieces rigides propres a CHAQUE
// instance (SLumInstance, tenue par le geom) au lieu d'un cache unique dans
// la piece du maillage partage. 0 = ancien cache partage : deux camions du
// meme modele s'y ecrasent l'un l'autre, et selon les seuils (50 ms, 1
// degre, budget de 60) chacun s'affiche tantot avec SON eclairage, tantot
// avec celui de l'autre -- clignotement clair / moins clair.
bool g_vita_lum_instance = true;

// Lumieres d'un modele RIGIDE dans l'espace du MODELE : ambiante, puis pour
// chaque directionnelle sa direction (normalisee) et sa couleur. Memes
// conventions que l'eclairage de la peau (verifie contre xemu, #4).
static void lumieres_rigides( CVitaModelLights *p_lights, float lum[21] )
{
	float amb[3], dif[2][3], dir[2][3];
	if( p_lights )
		p_lights->ResolveLighting( amb, dif, dir );
	else
	{
		Image::RGBA a = Nx::CLightManager::sGetLightAmbientColor();
		amb[0] = (float)a.r / 128.0f; amb[1] = (float)a.g / 128.0f; amb[2] = (float)a.b / 128.0f;
		for( int L = 0; L < 2; ++L )
		{
			Image::RGBA d = Nx::CLightManager::sGetLightDiffuseColor( L );
			dif[L][0] = (float)d.r / 128.0f; dif[L][1] = (float)d.g / 128.0f; dif[L][2] = (float)d.b / 128.0f;
			Mth::Vector v0 = Nx::CLightManager::sGetLightDirection( L );
			dir[L][0] = -v0[0]; dir[L][1] = -v0[1]; dir[L][2] = -v0[2];
		}
	}
	lum[0] = amb[0]; lum[1] = amb[1]; lum[2] = amb[2];
	for( int L = 0; L < 2; ++L )
	{
		float d[3] = { dir[L][0], dir[L][1], dir[L][2] };
		if( sp_racine_courante )
		{
			Mth::Matrix &R = *sp_racine_courante;
			for( int a = 0; a < 3; ++a )
				d[a] = dir[L][0] * R[a][0] + dir[L][1] * R[a][1] + dir[L][2] * R[a][2];
		}
		const float n = sqrtf( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
		const float inv = ( n > 1e-6f ) ? 1.0f / n : 0.0f;
		lum[3 + L * 6] = d[0] * inv; lum[4 + L * 6] = d[1] * inv; lum[5 + L * 6] = d[2] * inv;
		lum[6 + L * 6] = dif[L][0]; lum[7 + L * 6] = dif[L][1]; lum[8 + L * 6] = dif[L][2];
	}
	// 3e lumiere : lumiere de scene (XBox instance.cpp:360, modele a lumieres propres).
	lum[15] = lum[16] = lum[17] = lum[18] = lum[19] = lum[20] = 0.0f;
	if( p_lights && sp_racine_courante )
	{
		Mth::Matrix &R = *sp_racine_courante;
		float d3[3], c3[3];
		CVitaModelLights::ResolveSceneLight( Mth::Vector( R[3][0], R[3][1], R[3][2], 1.0f ), d3, c3 );
		for( int a = 0; a < 3; ++a )
			lum[15 + a] = d3[0] * R[a][0] + d3[1] * R[a][1] + d3[2] * R[a][2];
		lum[18] = c3[0]; lum[19] = c3[1]; lum[20] = c3[2];
	}
	static int s_n = 0;
	if(( s_n++ % 3600 ) == 0 )
		VLOG( "LUM", "rigide : ambiante %.2f %.2f %.2f, diffuse0 %.2f %.2f %.2f, diffuse1 %.2f %.2f %.2f%s",
		      lum[0], lum[1], lum[2], lum[6], lum[7], lum[8], lum[12], lum[13], lum[14],
		      p_lights ? " (modele)" : "" );
}

// Couleurs eclairees d'une piece : brut x ( ambiante + somme diffuse x N.L ),
// sature -- le D3DRS_LIGHTING de XBox avec la couleur de sommet en ambiante et
// diffuse (nx_init.cpp:102-104). Le cbo vaut 2 x brut : on divise par 2 ici
// et le dessin texture double (GL_RGB_SCALE), comme le pixel shader XBox.
// Televerse seulement si les lumieres ont bouge depuis le dernier calcul.
static SceUInt64 g_pic_max = 0; static int g_pic_idx = -1;
static SceUInt64 g_pic_calc = 0, g_pic_tele = 0; static int g_pic_n = 0, g_pic_v = 0;	// pics de dessin (#69)
static int s_budget_image = 0;	// pieces reeclairees dans l'image (#67)
void VitaBudgetEclairageImage( void ) { s_budget_image = 0; }

// Etat d'eclairage de l'instance en cours de dessin (#78), pose par
// CVitaGeom::plat_render autour de Draw comme sp_racine_courante.
static SLumInstance *sp_lum_inst = NULL;
static int           s_lum_inst_n = 0;
// "lpa 0/1" (#78, suite) : decision de reeclairage prise UNE fois pour tout le
// vehicule. Avant, chaque piece avait son propre minuteur de 50 ms et sa part
// du budget de 60 pieces/image : sur un camion qui tourne, les pieces etaient
// reeclairees a des images differentes -- morceaux du meme vehicule a des
// luminosites decalees, scintillement leger (revu par l'humain apres lpi 1).
// L'entree [s_lum_inst_n] du tableau de l'instance porte cle et date du
// vehicule. s_lum_decision : -1 pas de decision (piece par piece), 0 garder,
// 1 tout reeclairer.
bool g_vita_lum_atomique = true;
static int s_lum_decision = -1;
static const char *sp_lum_nom = "?";	// diagnostic #78 : modele en cours

// inst : etat propre a l'instance (#78) ; NULL = cache de la piece, partage
// par toutes les instances du maillage (ancien comportement, "lpi 0").
static void eclairer_piece( SGpuMesh *p, const float lum[21], bool texture,
                            SLumInstance *inst = NULL )
{
	if( inst && ( s_lum_decision >= 0 ))
	{
		// Decision du vehicule (lpa) : une piece jamais eclairee l'est
		// toujours ; sinon tout ou rien, pour toutes les pieces ensemble.
		if( inst->ok && inst->cbo && ( s_lum_decision == 0 ))
			return;
		inst->ok = 0;		// force le calcul ci-dessous
	}
	float              *lum_cle = inst ? inst->cle : p->lum_cle;
	unsigned long long *lum_t   = inst ? &inst->t  : &p->lum_t;
	unsigned char      *lum_ok  = inst ? &inst->ok : &p->lum_ok;
	GLuint             *p_cbo   = inst ? &inst->cbo : &p->cbo_rig;
	float cle[21];
	for( int k = 0; k < 21; ++k ) cle[k] = lum[k];
	cle[0] += texture ? 0.0f : 1000.0f;	// echelle differente sans texture
	if( *lum_ok && *p_cbo )
	{
		// ~1 degre : 0,002 reeclairait les vehicules a chaque image (#18).
		// Et au plus une fois toutes les 50 ms, sauf grand changement : une
		// tourelle de tank qui tourne reeclairait ses pieces chaque image.
		const bool recent = ( sceKernelGetProcessTimeWide() - *lum_t ) < 50000;
		float ecart = 0.0f;
		for( int k = 0; k < 21; ++k )
		{
			const float e = fabsf( cle[k] - lum_cle[k] );
			if( e > ecart )
				ecart = e;
		}
		if(( ecart <= 0.02f ) || ( recent && ( ecart <= 0.15f )))
			return;
		// Budget par image (#67) : au-dela de 60 pieces deja reeclairees dans
		// l'image, une piece DEJA eclairee garde ses couleurs (une image de
		// retard au plus) -- Manhattan, 30 voitures qui tournent : 240 ms/s.
		if( s_budget_image >= 60 )
			return;
	}
	++s_budget_image;
	{
		// Diagnostic #78 : par quel chemin passent les reeclairages ?
		static int s_par_inst = 0, s_partage = 0;
		static const char *s_ex_partage = NULL;
		static SceUInt64 s_td = 0;
		if( inst ) ++s_par_inst; else { ++s_partage; s_ex_partage = sp_lum_nom; }
		const SceUInt64 tn = sceKernelGetProcessTimeWide();
		if( tn - s_td > 2000000 )
		{
			s_td = tn;
			if( s_partage )
			VLOG( "LUM", "diag chemins : %d pieces par instance, %d par cache PARTAGE (ex. '%s')",
			      s_par_inst, s_partage, s_ex_partage ? s_ex_partage : "-" );
			s_par_inst = s_partage = 0; s_ex_partage = NULL;
		}
	}
	*lum_t = sceKernelGetProcessTimeWide();
	const float k_ech = texture ? 0.5f : 1.0f;
	const int nv = p->num_vertices;
	static SceUInt64 s_tc = 0, s_tu = 0, s_t0 = 0;
	static int s_np = 0, s_nv = 0;
	const SceUInt64 t0 = sceKernelGetProcessTimeWide();
	const int nl = p->p_lum_idx ? p->num_lum_idx : nv;
	for( int iv = 0; iv < nl; ++iv )
	{
		const int v = p->p_lum_idx ? p->p_lum_idx[iv] : iv;
		const float *n = &p->p_n_rig[v * 3];
		float l[3] = { lum[0], lum[1], lum[2] };
		for( int L = 0; L < 3; ++L )
		{
			const float *q = &lum[3 + L * 6];
			const float d = n[0] * q[0] + n[1] * q[1] + n[2] * q[2];
			if( d > 0.0f )
			{
				l[0] += d * q[3]; l[1] += d * q[4]; l[2] += d * q[5];
			}
		}
		const unsigned char *b = &p->p_c_base[v * 4];
		unsigned char       *o = &p->p_c_lit[v * 4];
		for( int c = 0; c < 3; ++c )
		{
			const float x = (float)b[c] * k_ech * l[c];
			o[c] = ( x >= 255.0f ) ? 255 : (unsigned char)x;
		}
		o[3] = b[3];
	}
	const SceUInt64 t1 = sceKernelGetProcessTimeWide();
	// Les sommets hors p_lum_idx gardent dans p_c_lit (tampon CPU commun) les
	// valeurs d'une autre instance : la piece ne les reference pas.
	if( *p_cbo == 0 )
	{
		glGenBuffers( 1, p_cbo );
		glBindBuffer( GL_ARRAY_BUFFER, *p_cbo );
		glBufferData( GL_ARRAY_BUFFER, 4 * nv, p->p_c_lit, GL_DYNAMIC_DRAW );
	}
	else
	{
		glBindBuffer( GL_ARRAY_BUFFER, *p_cbo );
		glBufferSubData( GL_ARRAY_BUFFER, 0, 4 * nv, p->p_c_lit );
	}
	const SceUInt64 t2 = sceKernelGetProcessTimeWide();
	s_tc += t1 - t0; s_tu += t2 - t1; ++s_np; s_nv += nl;
	g_pic_calc += t1 - t0; g_pic_tele += t2 - t1; ++g_pic_n; g_pic_v += nl;
	if( !s_t0 ) s_t0 = t2;
	if( t2 - s_t0 > 2000000 )
	{
		VLOG( "LUM", "rigides reeclaires : %d pieces/2 s, %d sommets, calcul %.1f ms, televersement %.1f ms",
		      s_np, s_nv, s_tc / 1000.0, s_tu / 1000.0 );
		s_tc = s_tu = 0; s_np = s_nv = 0; s_t0 = t2;
	}
	for( int k = 0; k < 21; ++k ) lum_cle[k] = cle[k];
	*lum_ok = 1;
}

void CVitaMesh::Draw( const float *p_rgba,
                      Mth::Matrix *p_bone_mats, int num_bones,
                      CVitaModelLights *p_lights ) const
{
	glDisable( GL_LIGHTING );
	glDisable( GL_COLOR_MATERIAL );

	// ECLAIRAGE FIXE DES MODELES RIGIDES (XBox instance.cpp:200-232, 349-381) :
	// D3DRS_LIGHTING avec la couleur de sommet comme ambiante ET diffuse
	// (nx_init.cpp:102-104), ambiante du monde ou du modele, deux
	// directionnelles. Le fond 3D du menu principal (normales, ambiante
	// sombre) sortait 2,5 fois trop clair sans cela.
	// Calcul CPU (eclairer_piece) : le pipeline fixe de vitaGL avec lumieres
	// bloquait le jeu (essaye le 2026-10-04). Recalcul seulement quand les
	// lumieres changent dans l'espace du modele : un plan d'interface ou une
	// voiture garee n'est calcule qu'une fois.
	const bool eclaire = g_vita_lighting && g_vita_rigide_lum && m_vita_eclairable;
	if( NxVita::g_vita_sprite_dump && sp_racine_courante )
		VLOG( "SPR", "modele %s : %d pieces, eclairable %d, pos (%.0f %.0f %.0f) ech %.2f",
		      m_vita_name, m_num, (int)m_vita_eclairable, (*sp_racine_courante)[3][0],
		      (*sp_racine_courante)[3][1], (*sp_racine_courante)[3][2], (*sp_racine_courante)[0][0] );
	float lum[21];
	sp_lum_nom = m_vita_name;
	if( eclaire )
		lumieres_rigides( p_lights, lum );
	// #78 (lpa) : decision de reeclairage pour le vehicule entier.
	s_lum_decision = -1;
	if( eclaire && g_vita_lum_atomique && sp_lum_inst && ( s_lum_inst_n > 0 ))
	{
		SLumInstance *v = &sp_lum_inst[s_lum_inst_n];
		const SceUInt64 maintenant = sceKernelGetProcessTimeWide();
		s_lum_decision = 1;
		if( v->ok )
		{
			const bool recent = ( maintenant - v->t ) < 50000;
			float ecart = 0.0f;
			for( int k = 0; k < 21; ++k )
			{
				const float e = fabsf( lum[k] - v->cle[k] );
				if( e > ecart )
					ecart = e;
			}
			if(( ecart <= 0.02f ) || ( recent && ( ecart <= 0.15f )) || ( s_budget_image >= 60 ))
				s_lum_decision = 0;
			// Diagnostic #78 : quel terme de l'eclairage fait reeclairer ?
			// 0-2 ambiante, 3-8 lumiere 0 (dir, couleur), 9-14 lumiere 1,
			// 15-20 3e lumiere (lumiere de scene la plus proche).
			if( ecart > 0.02f )
			{
				static SceUInt64 s_t_diag = 0;
				static int s_n_terme[4] = { 0, 0, 0, 0 };
				static float s_max_terme[4] = { 0, 0, 0, 0 };
				for( int k = 0; k < 21; ++k )
				{
					const float e = fabsf( lum[k] - v->cle[k] );
					const int g = ( k < 3 ) ? 0 : ( k < 9 ) ? 1 : ( k < 15 ) ? 2 : 3;
					if( e > 0.02f ) ++s_n_terme[g];
					if( e > s_max_terme[g] ) s_max_terme[g] = e;
				}
				if( maintenant - s_t_diag > 2000000 )
				{
					s_t_diag = maintenant;
					VLOG( "LUM", "diag reeclairage : termes changes ambiante %d (max %.2f), lum0 %d (%.2f), lum1 %d (%.2f), scene %d (%.2f) | ex. '%s' ecart %.2f, budget %d",
					      s_n_terme[0], s_max_terme[0], s_n_terme[1], s_max_terme[1],
					      s_n_terme[2], s_max_terme[2], s_n_terme[3], s_max_terme[3],
					      m_vita_name, ecart, s_budget_image );
					for( int g = 0; g < 4; ++g ) { s_n_terme[g] = 0; s_max_terme[g] = 0.0f; }
				}
			}
		}
		if( s_lum_decision == 1 )
		{
			for( int k = 0; k < 21; ++k ) v->cle[k] = lum[k];
			v->t  = maintenant;
			v->ok = 1;
		}
	}

	// mp_pieces AUSSI, pas seulement m_num : le geom nous obtient par un cast
	// non verifie (plat_load_geom_data), donc rien ne garantit qu'on soit un
	// CVitaMesh reellement construit. Un stub a deja livre un CMesh de base
	// ici, et les deux champs valaient de la memoire etrangere.
	if(( m_num == 0 ) || ( mp_pieces == NULL ))
		return;

	glEnableClientState( GL_VERTEX_ARRAY );
	NxVita::BrouillardFixe( 1 );		// issue #45, pipeline fixe

	// "rgp 1" (#67) : pieces eclairees par le shader rigide eclaire. Le
	// programme n'est actif qu'entre deux pieces qui l'emploient.
	const bool rig_gpu = eclaire && g_vita_rigide_gpu && NxVita::ShaderRigideEclairePret();
	bool rig_actif = false;
	static int s_rig_np = 0;
	static SceUInt64 s_rig_t0 = 0;


	// Le rectangle blanc du menu resiste : ni sprite 2D (traces),
	// ni texture manquante (toutes resolues), ni UI_bg (texture bleue).
	// On journalise donc CE QUE CHAQUE MORCEAU DESSINE : nom, texture GL,
	// blend, et l'etendue projetee. Un quad qui couvre un quart de l'ecran
	// sortira du lot.
	{
		static int s_n = 0;
		if(( s_n++ % 1800 ) < 3 )
			for( int i = 0; i < m_num; ++i )
				VLOG( "DRAW", "  '%s' morceau %d : tex=%u blend=%u cbo=%u %d indices",
				      m_vita_name, i, (unsigned)mp_pieces[i].texture,
				      (unsigned)mp_pieces[i].blend, (unsigned)mp_pieces[i].cbo,
				      mp_pieces[i].num_indices );
	}

	SceUInt64 pic_tp = sceKernelGetProcessTimeWide();
	g_pic_max = 0; g_pic_idx = -1;
	for( int i = 0; i < m_num; ++i )
	{
		{
			// #69 : piece la plus lente du dessin (mesuree au debut de la suivante)
			const SceUInt64 n = sceKernelGetProcessTimeWide();
			if(( i > 0 ) && (( n - pic_tp ) > g_pic_max )) { g_pic_max = n - pic_tp; g_pic_idx = i - 1; }
			pic_tp = n;
		}
		const SGpuMesh *p = &mp_pieces[i];
		if( p->decal )		// passe 1 (#55) : pas de vbo, chemin skinne seulement
			continue;

		// Blend du materiau : les panneaux d'interface (voiles, degrades)
		// en dependent -- opaques, ils sortent en rectangles blancs.
		if( p->blend )
		{
			glEnable( GL_BLEND );
			// ADD/ADD_FIXED : additif ; sinon alpha classique.
			glBlendFunc( GL_SRC_ALPHA,
			             ( p->blend == 1 || p->blend == 2 )
			             ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA );

			// PROFONDEUR TOUJOURS ECRITE.
			//
			// La couper rendait invisible tout morceau de modele ayant un
			// melange : les modeles sont dessines AVANT le decor (176 maillages
			// deja passes quand RenderWorld demarre), donc un morceau qui
			// n'ecrit pas la profondeur est ensuite RECOUVERT par le sol et les
			// murs. Les objets a ramasser d'une mission -- corps en alpha, halo
			// en additif -- disparaissaient ainsi completement, tout en restant
			// ramassables.
			//
			// XBox n'eteint jamais l'ecriture de profondeur hors volumes
			// d'ombre (render.cpp) ; c'est le tri qui gere l'ordre chez lui.
			//
			// SAUF POUR L'ADDITIF. Un morceau additif n'a jamais vocation a
			// masquer : il AJOUTE de la lumiere a ce qui est deja peint.
			// Lui faire ecrire la profondeur bloque le decor derriere lui, qui
			// est alors rejete au test -- et le tampon efface reste apparent.
			//
			// [MESURE] le halo d'un objet a ramasser (blend 1, un quad de 4
			// indices) laissait un rectangle exactement a la couleur
			// d'effacement, (115,13,166) au pixel pres. Meme cause pour les
			// ombres et les phares de vehicules.
			// PROFONDEUR ECRITE, ET TEST ALPHA POUR NE PAS MASQUER A VIDE.
			//
			// [MESURE] les deux extremes sont faux, et pour la meme raison :
			// nos modeles sont dessines AVANT le decor.
			//   - sans ecriture : le decor les recouvre. Roues incompletes,
			//     objets a ramasser invisibles.
			//   - avec ecriture : ils masquent le decor la ou ils sont
			//     transparents. Rectangles a la couleur d'effacement.
			// Aucun reglage ne peut etre bon tant que l'ordre est faux ; XBox
			// dessine les modeles semi-transparents APRES le decor (etapes 10
			// et 14, NOTES/pipeline-rendu-xbox.md).
			//
			// En attendant cette correction de fond : on ecrit la profondeur,
			// et on rejette les fragments transparents pour qu'ils ne masquent
			// rien. Le seuil est haut -- un halo n'a pas de demi-teintes utiles
			// en dessous, et c'est ce qui laissait un lisere violet a 0,02.
			glDepthMask( GL_TRUE );
			glEnable( GL_ALPHA_TEST );
			glAlphaFunc( GL_GREATER, 0.35f );


		}

		// Eclairage des pieces rigides (XBox instance.cpp:200) : couleurs
		// eclairees a la place du cbo, doublees au dessin si texture.
		const bool tex_p = ( p->texture && p->uvbo );
		const bool lit_p = eclaire && p->cbo_rig && p->p_c_lit && p->p_n_rig;
		// rgp 1 (#67) : couleurs eclairees par le shader, depuis le cbo et les
		// normales sur le GPU -- plus de calcul ni de televersement CPU. Les
		// pieces reportees (translucides) n'en ont pas besoin : la vidange
		// les dessine depuis p->cbo, non eclaire, comme avant.
		// #77 : le shader rigide ne sait pas decaler les UV ; une piece a UV
		// wibble (quelques sommets : rampe de gyrophare) passe par le chemin
		// fixe, eclairee par le CPU.
		const bool uvw_p = tex_p && p->uvw0 && g_vita_gyr && NxVita::g_vita_uvw;
		bool rig_p = lit_p && rig_gpu && p->cbo && p->vbo && !uvw_p;
		if( rig_p && !p->nbo_rig )
		{
			SGpuMesh *pm = &mp_pieces[i];
			glGenBuffers( 1, &pm->nbo_rig );
			glBindBuffer( GL_ARRAY_BUFFER, pm->nbo_rig );
			glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 3 * pm->num_vertices,
			              pm->p_n_rig, GL_STATIC_DRAW );
			glBindBuffer( GL_ARRAY_BUFFER, 0 );
		}
		rig_p = rig_p && ( p->nbo_rig != 0 );
		// #78 : couleurs eclairees de CETTE instance, pas du maillage partage.
		SLumInstance *li = ( lit_p && !rig_p && sp_lum_inst && ( i < s_lum_inst_n ))
		                 ? &sp_lum_inst[i] : NULL;
		if( lit_p && !rig_p )
			eclairer_piece( &mp_pieces[i], lum, tex_p, li );
		if( p->cbo )
		{
			glEnableClientState( GL_COLOR_ARRAY );
			glBindBuffer( GL_ARRAY_BUFFER, ( lit_p && !rig_p )
			              ? (( li && li->cbo ) ? li->cbo : p->cbo_rig ) : p->cbo );
			glColorPointer( 4, GL_UNSIGNED_BYTE, 0, NULL );
		}
		else
			glDisableClientState( GL_COLOR_ARRAY );

		glBindBuffer( GL_ARRAY_BUFFER, p->vbo );
		glVertexPointer( 3, GL_FLOAT, 0, NULL );

		if( p->texture && p->uvbo )
		{
			glEnable( GL_TEXTURE_2D );
			glBindTexture( GL_TEXTURE_2D, p->texture );
			glEnableClientState( GL_TEXTURE_COORD_ARRAY );
			glBindBuffer( GL_ARRAY_BUFFER, p->uvbo );
			glTexCoordPointer( 2, GL_FLOAT, 0, NULL );
			// Teinte du geom plutot que blanc : c'est elle qui fait la
			// silhouette noire du menu principal.
			if( p_rgba && !p->color_locked )
				glColor4f( p_rgba[0], p_rgba[1], p_rgba[2], p_rgba[3] );
			else
				glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
		}
		else
		{
			glDisableClientState( GL_TEXTURE_COORD_ARRAY );
			glDisable( GL_TEXTURE_2D );
			if( p_rgba && !p->color_locked )
				glColor4f( p_rgba[0], p_rgba[1], p_rgba[2], p_rgba[3] );
			else
				glColor4f( 0.8f, 0.8f, 0.85f, 1.0f );
		}

		// MATRICE DE L'OS DU MORCEAU, pour les modeles a parties RIGIDES.
		//
		// [VERIFIE sur table] les vehicules n'ont AUCUN drapeau POIDS (0x10) :
		// leurs six secteurs -- carrosserie, roues -- sont chacun attaches a
		// UN os, et c'est la matrice de cet os qui les place. Le parseur
		// jetait cet index (« skip( p_file, 4 ); // index d'os ») et le rendu
		// n'appliquait rien : toutes les parties se dessinaient empilees a
		// l'origine du modele. D'ou une voiture « verticale » et amputee de
		// ses roues, ce qu'aucune rotation seule n'expliquait.
		//
		// Le skinning par sommet est un mecanisme DIFFERENT (DrawSkinned) :
		// les deux ne coexistent pas sur un meme morceau.
		// D'OU VIENT LA MATRICE DE CE MORCEAU ?
		//
		// Deux sources, dans cet ordre :
		//
		//  1. les matrices d'os fournies par le moteur, quand le modele est
		//     ANIME (roues qui tournent) ;
		//  2. a defaut, la matrice de SETUP de la hierarchie du fichier.
		//
		// Le cas 2 n'est pas un repli de fortune, c'est le cas NORMAL des
		// vehicules : CModel::Render passe deliberement ( NULL, 0 ) quand
		// l'objet n'est pas anime (« update root position without updating
		// bones »). Mesure a l'appui : les traces montraient
		// « os=0 mats=0x0 n=0 » alors que la hierarchie etait bien lue et les
		// matrices bien calculees en amont.
		//
		// La matrice de setup EST la transformation locale de la piece ; sans
		// animation, c'est exactement ce qu'il faut appliquer.
		const Mth::Matrix *p_mat_os = NULL;
		Mth::Matrix        compose;
		if( p->sector_bone >= 0 )
		{
			if( p_bone_mats && ( p->sector_bone < num_bones ))
				p_mat_os = &p_bone_mats[p->sector_bone];
			else if( mp_hierarchyObjects && m_numHierarchyObjects )
			{
				// COMPOSER AVEC LA CHAINE DE PARENTS.
				//
				// [SOURCE] car.cpp:187 : « frontTire = obj[2].GetSetupMatrix();
				// frontTire *= obj[0].GetSetupMatrix(); » -- la matrice d'un
				// objet est LOCALE a son parent, et doit etre multipliee par
				// celle du parent pour arriver dans le repere du modele.
				//
				// C'est ce qui manquait : appliquer la seule matrice locale
				// laissait les roues sans la rotation portee par l'objet
				// racine, dont elles descendent toutes (parent = 0).
				int idx = -1;
				for( int k = 0; k < m_numHierarchyObjects; ++k )
				{
					if( mp_hierarchyObjects[k].GetBoneIndex() == p->sector_bone )
					{
						idx = k;
						break;
					}
				}
				if( idx >= 0 )
				{
					compose = mp_hierarchyObjects[idx].GetSetupMatrix();

					// Remontee bornee : une hierarchie corrompue ne doit pas
					// boucler indefiniment dans la boucle de rendu.
					int parent = mp_hierarchyObjects[idx].GetParentIndex();
					for( int garde = 0;
					     ( parent >= 0 ) && ( parent < m_numHierarchyObjects )
					     && ( garde < 8 ); ++garde )
					{
						compose *= mp_hierarchyObjects[parent].GetSetupMatrix();
						parent = mp_hierarchyObjects[parent].GetParentIndex();
					}
					p_mat_os = &compose;

				}
			}
		}
		const bool os_pose = ( p_mat_os != NULL );

		if( os_pose )
		{
			glPushMatrix();
			glMultMatrixf( (const float *)p_mat_os );
		}

		// Morceau translucide : REPORTE apres le decor au lieu d'etre dessine
		// ici. La matrice modelview courante porte deja la vue, la pose du
		// modele et l'os -- on la capture telle quelle, rien a recalculer.
		const bool reporter = ( g_vita_differer_modeles && ( p->blend != 0 )
		                        && ( s_num_reportes < MAX_REPORTES ));
		if( reporter )
		{
			SMorceauReporte *r = &s_reportes[s_num_reportes++];
			r->p_piece = p;
			glGetFloatv( GL_MODELVIEW_MATRIX, r->mv );
			r->a_rgba = ( p_rgba != NULL );
			if( p_rgba )
				for( int k = 0; k < 4; ++k ) r->rgba[k] = p_rgba[k];
		}
		else if( rig_p )
		{
			// Meme dessin que plus bas, couleurs eclairees par le shader
			// (#67). Melange, profondeur, faces : etats GXM, communs aux deux
			// chemins ; test alpha et brouillard repris par le shader.
			float proj[16], mv[16], mvp[16];
			glGetFloatv( GL_PROJECTION_MATRIX, proj );
			glGetFloatv( GL_MODELVIEW_MATRIX, mv );
			for( int c = 0; c < 4; ++c )
				for( int r = 0; r < 4; ++r )
				{
					float s = 0.0f;
					for( int k = 0; k < 4; ++k )
						s += proj[k * 4 + r] * mv[c * 4 + k];
					mvp[c * 4 + r] = s;
				}
			if( !rig_actif )
			{
				NxVita::ShaderRigideEclaireDebut( lum );
				rig_actif = true;
			}
			NxVita::ShaderRigideEclairePiece( mvp, p->vbo, p->nbo_rig, p->cbo, p->uvbo, tex_p,
			                                  p->blend ? 0.35f : -1.0f,
			                                  ( p->blend >= 1 ) && ( p->blend <= 4 ));
			glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p->ibo );
			++g_vita_n_draws;
			glDrawElements( GL_TRIANGLE_STRIP, p->num_indices,
			                GL_UNSIGNED_SHORT, NULL );
			++s_rig_np;
		}
		else
		{
			if( rig_actif )
			{
				NxVita::ShaderRigideEclaireFin();
				rig_actif = false;
			}
			glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p->ibo );
			++g_vita_n_draws;
			NxVita::BrouillardFixeNoir(( p->blend >= 1 ) && ( p->blend <= 4 ));
			if( lit_p && tex_p )
			{
				glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
				glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
				glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE );
				glTexEnvi( GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE );
				glTexEnvi( GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR );
				glTexEnvi( GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_TEXTURE );
				glTexEnvi( GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_PRIMARY_COLOR );
				const GLfloat deux = 2.0f;
				glTexEnvfv( GL_TEXTURE_ENV, GL_RGB_SCALE, &deux );
			}
			const bool uvw_pose = uvw_p && uvw_modele_poser( p->uvw0, p->uvw_par0 );
			glDrawElements( GL_TRIANGLE_STRIP, p->num_indices,
			                GL_UNSIGNED_SHORT, NULL );
			if( uvw_pose )
				uvw_modele_lever();
			if( lit_p && tex_p )
			{
				const GLfloat un = 1.0f;
				glTexEnvfv( GL_TEXTURE_ENV, GL_RGB_SCALE, &un );
				glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
			}
		}

		// PASSE 1 ADD (#77) : l'eclat des gyrophares, par-dessus la base qui
		// vient d'ecrire sa profondeur, avec ses sommets, son ibo et la meme
		// matrice d'os. Le test de profondeur est deja LEQUAL (plat_render) :
		// la passe coplanaire passe. Pas d'ecriture de profondeur, brouillard
		// noir comme tout additif (XBox/NX/render.cpp:1174).
		if( p->add_tex && g_vita_pad && !reporter && p->vbo && p->add_uvbo )
		{
			if( rig_actif )
			{
				NxVita::ShaderRigideEclaireFin();
				rig_actif = false;
			}
			glEnable( GL_BLEND );
			glBlendFunc( GL_SRC_ALPHA, GL_ONE );
			glDepthMask( GL_FALSE );
			if( p->add_cbo )
			{
				glEnableClientState( GL_COLOR_ARRAY );
				glBindBuffer( GL_ARRAY_BUFFER, p->add_cbo );
				glColorPointer( 4, GL_UNSIGNED_BYTE, 0, NULL );
			}
			else
				glDisableClientState( GL_COLOR_ARRAY );
			glBindBuffer( GL_ARRAY_BUFFER, p->vbo );
			glVertexPointer( 3, GL_FLOAT, 0, NULL );
			glEnable( GL_TEXTURE_2D );
			glBindTexture( GL_TEXTURE_2D, p->add_tex );
			glEnableClientState( GL_TEXTURE_COORD_ARRAY );
			glBindBuffer( GL_ARRAY_BUFFER, p->add_uvbo );
			glTexCoordPointer( 2, GL_FLOAT, 0, NULL );
			if( p_rgba && !p->add_locked )
				glColor4f( p_rgba[0], p_rgba[1], p_rgba[2], p_rgba[3] );
			else
				glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
			NxVita::BrouillardFixeNoir( true );
			const bool uvw_add = uvw_modele_poser( p->add_uvw, p->add_uvw_par );
			glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, p->ibo );
			++g_vita_n_draws;
			glDrawElements( GL_TRIANGLE_STRIP, p->num_indices,
			                GL_UNSIGNED_SHORT, NULL );
			if( uvw_add )
				uvw_modele_lever();
			glDepthMask( GL_TRUE );
			glDisable( GL_BLEND );
		}

		if( os_pose )
			glPopMatrix();

		if( p->blend )
		{
			glDisable( GL_ALPHA_TEST );
			glDisable( GL_BLEND );
			// ET ON REMET LA PROFONDEUR. C'est l'oubli d'origine : le melange
			// etait bien remis a l'arret apres chaque morceau, jamais
			// l'ecriture de profondeur. Un seul morceau translucide la coupait
			// donc pour TOUS LES MORCEAUX OPAQUES SUIVANTS du meme modele --
			// qui cessaient d'ecrire la profondeur et se faisaient recouvrir
			// par le decor. C'est l'origine des roues manquantes.
			//
			// La forcer a GL_TRUE pour tout le monde reparait les roues mais
			// faisait masquer le decor par les halos, d'ou des rectangles a la
			// couleur d'effacement. Restaurer l'etat apres chaque morceau
			// traite la cause au lieu du symptome.
			glDepthMask( GL_TRUE );
			glDepthMask( GL_TRUE );
		}
	}

	if( rig_actif )
		NxVita::ShaderRigideEclaireFin();
	if( rig_gpu )
	{
		const SceUInt64 t = sceKernelGetProcessTimeWide();
		if( !s_rig_t0 ) s_rig_t0 = t;
		if( t - s_rig_t0 > 2000000 )
		{
			VLOG( "LUM", "rigides eclaires par le GPU (rgp 1) : %d pieces/2 s", s_rig_np );
			s_rig_np = 0; s_rig_t0 = t;
		}
	}
	glDisableClientState( GL_COLOR_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	NxVita::BrouillardFixe( 0 );

	{
		static int s_n = 0;
		if(( s_n++ % 1800 ) < 6 )
			VLOG( "DRAW", "'%s' statique : %d morceaux", m_vita_name, m_num );
	}
}


// --- CVitaGeom -------------------------------------------------------------

// Clone d'un geom. La version de base du moteur rend NULL et laisse
// CGeom::Clone appeler SetActive() dessus -- Data abort, this = 0.
//
// On rend un CVitaGeom valide qui PARTAGE le maillage de l'original : la
// geometrie est deja cote GPU, la dupliquer ne servirait a rien. Le clone
// s'affiche donc comme sa source.
//
// Dette : mp_mesh est partage sans compteur de references. Si l'un des deux
// est detruit, l'autre garde un pointeur mort. Acceptable tant que les geoms
// vivent aussi longtemps que leur modele ; a revoir si des clones sont
// liberes independamment.
// DESACTIVE tant que l'espace d'indices n'est pas compris.
//
// Tout le mecanisme est en place et correct : lecture du ï¿½ .cas.xbx ï¿½, masque
// de retrait par vetement, marquage puis insertion de degeneres dans la bande.
// Il ne trouve pourtant que ~19 triangles sur 1500 entrees.
//
// Cause, mesuree : les triplets du fichier CAS -- (31 32 20), (32 20 18)... --
// n'existent nulle part dans nos indices. Les notres contiennent deja des
// doublons consecutifs (157 157, 139 139 139) : le format joint plusieurs
// bandes en une seule par des degeneres, et il monte a 1783 la ou le morceau
// ne couvre que quelques centaines de sommets. Les deux cotes ne parlent pas
// du meme espace d'indices.
//
// Les rares correspondances trouvees sont donc des COINCIDENCES, et elles
// retiraient des triangles au hasard : le pantalon se trouait. Un torse qui
// depasse vaut mieux qu'un vetement perce.
//
// A reprendre en comprenant d'abord comment le parseur reindexe les bandes.
// Le reste du code est bon et n'aura pas a etre reecrit.
#define VITA_CAS_POLY_REMOVAL 1

const Mth::CBBox & CVitaGeom::plat_get_bounding_box() const
{
	// Sans maillage, une boite vide -- mais SILENCIEUSE : la version de base
	// imprimait un stub a chaque appel.
	static Mth::CBBox s_empty;
	if( mp_mesh )
		return mp_mesh->BBox();
	// Geom d'un secteur du decor, ou son clone : la boite du fichier (#65).
	// Vide, ParkGen.cpp:917 donnait a chaque piece de l'editeur une cellule
	// et une hauteur negative, et Element3d ne pouvait pas cadrer ses
	// vignettes (drapeaux de depart invisibles dans le selecteur).
	return m_vita_bbox_ok ? m_vita_bbox : s_empty;
}


void CVitaGeom::plat_clear_color()
{
	// [SOURCE] XBox/p_NxGeom.cpp:621 : retire la surcharge, le materiau
	// reprend sa couleur. Chez nous : teinte neutre.
	m_vita_rgba[0] = m_vita_rgba[1] = m_vita_rgba[2] = m_vita_rgba[3] = 1.0f;
	m_vita_color = Image::RGBA( 0x80, 0x80, 0x80, 0x80 );
	// Secteur du decor (#63) : ses maillages reprennent leurs couleurs.
	if( m_vita_secteur && !m_vita_clone )
	{
		const unsigned char n[3] = { 0x80, 0x80, 0x80 };
		NxVita::TeinterSecteur( m_vita_secteur, n, true );
	}
}


void CVitaGeom::plat_set_color( Image::RGBA rgba )
{
	// Secteur du DECOR (#63) : SetSceneColor / SetObjectColor (changement
	// d'ambiance). [SOURCE] XBox/p_NxGeom.cpp:575 : 0x80 ou 0x81 en rouge,
	// 0x80 en vert et bleu = retrait de la teinte ; sinon la couleur remplace
	// celle du materiau. Les couleurs de sommets du secteur sont recalculees
	// (p_world_render.cpp, TeinterSecteur), seulement si la valeur change.
	if( m_vita_secteur && !m_vita_clone
	    && (( rgba.r != m_vita_color.r ) || ( rgba.g != m_vita_color.g ) || ( rgba.b != m_vita_color.b )))
	{
		const bool neutre = (( rgba.r == 0x80 ) || ( rgba.r == 0x81 ))
		                    && ( rgba.g == 0x80 ) && ( rgba.b == 0x80 );
		const unsigned char c[3] = { rgba.r, rgba.g, rgba.b };
		NxVita::TeinterSecteur( m_vita_secteur, c, neutre );
	}
	m_vita_color = rgba;

	// Convention du moteur : composantes sur 0..128 (heritage PS2), pas 0..255
	// -- la meme que pour les sprites 2D, ou prendre 255 pour plein
	// assombrissait tout de moitie.
	m_vita_rgba[0] = (float)rgba.r / 128.0f;
	m_vita_rgba[1] = (float)rgba.g / 128.0f;
	m_vita_rgba[2] = (float)rgba.b / 128.0f;
	m_vita_rgba[3] = (float)rgba.a / 128.0f;
	for( int i = 0; i < 4; ++i )
		if( m_vita_rgba[i] > 1.0f )
			m_vita_rgba[i] = 1.0f;
}


bool CVitaGeom::plat_hide_polys( uint32 mask )
{
#if VITA_CAS_POLY_REMOVAL
	if( mp_mesh )
		mp_mesh->HidePolys( (unsigned int)mask );
#else
	(void)mask;
#endif
	return true;
}


// [SOURCE] XBox/p_NxGeom.cpp:650. Couleur d'une passe de materiau, par nom :
// le CAS l'utilise pour les roues, la peau, les vetements colores (#58). Ici la
// couleur de materiau est cuite dans les couleurs de sommets (cbo, x2 comme
// XBox) : on recalcule cbo depuis les couleurs brutes. Passe 0 seulement (seule
// cuite) ; materiaux scindes a la conversion : memes regles de checksum que XBox.
bool CVitaGeom::plat_set_material_color( uint32 mat_name_checksum, int pass, Image::RGBA rgba )
{
	if( !mp_mesh )
		return false;
	bool change = false;
	for( int i = 0; i < mp_mesh->NumPieces(); ++i )
	{
		SGpuMesh *p = const_cast< SGpuMesh * >( &mp_mesh->Pieces()[i] );
		bool voulu = false;
		int  passe = pass;
		if( p->mat_checksum == mat_name_checksum )
			voulu = true;
		else if(( p->mat_checksum > mat_name_checksum ) && (( p->mat_checksum - mat_name_checksum ) <= 0x7F ))
		{
			const uint32 d = p->mat_checksum - mat_name_checksum;
			const uint32 sep = d & 0x07, drapeaux = d >> 3;
			if( sep )
			{
				if( sep == (uint32)( pass + 1 )) { voulu = true; passe = 0; }
			}
			else if( drapeaux & ( 1 << pass ))
			{
				voulu = true;
				for( int q = 0; q < pass; ++q )
					if(( drapeaux & ( 1 << q )) == 0 )
						--passe;
			}
		}
		if( !voulu || ( passe != 0 ) || p->color_locked || !p->cbo || !p->p_couleurs_brutes )
			continue;
		const float f[3] = { rgba.r / 255.0f * 2.0f, rgba.g / 255.0f * 2.0f, rgba.b / 255.0f * 2.0f };
		const unsigned char *p_up = NxVita::TeindreCouleursMateriau( p->p_couleurs_brutes, p->num_vertices, f );
		glBindBuffer( GL_ARRAY_BUFFER, p->cbo );
		glBufferData( GL_ARRAY_BUFFER, 4 * p->num_vertices, p_up, GL_STATIC_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		change = true;
	}
	{
		static int s_n = 0;
		if( s_n++ < 40 )
			VLOG( "CAS", "couleur de materiau %08x passe %d -> %d %d %d : %s (%s)",
			      (unsigned)mat_name_checksum, pass, rgba.r, rgba.g, rgba.b, change ? "appliquee" : "aucune piece", mp_mesh->Name());
	}
	return change;
}

// Issue #16. [SOURCE] XBox/p_NxGeom.cpp:1131 : meme recherche du materiau par
// nom (materiaux scindes a la conversion : bits 0-2 = passe isolee, bits 3-6 =
// passes retenues), puis la matrice ne garde que m00, m01 (rotation x echelle)
// et m30, m31 (translation) ; le shader *_UVTransform reconstruit
// u' = m00 u - m01 v + m30, v' = m01 u + m00 v + m31 (material.cpp:431).
// Le uvbo de la piece est recalcule depuis ses UV d'origine : le resultat ne
// depend que de la DERNIERE matrice, comme sur XBox.
bool CVitaGeom::plat_set_uv_matrix( uint32 mat_name_checksum, int pass, const Mth::Matrix &mat )
{
	if( !mp_mesh )
		return false;
	const float m00 = mat[0][0], m01 = mat[0][1], m30 = mat[3][0], m31 = mat[3][1];
	int n = 0;
	for( int i = 0; i < mp_mesh->NumPieces(); ++i )
	{
		SGpuMesh *p = const_cast< SGpuMesh * >( &mp_mesh->Pieces()[i] );
		if( !p->p_uv_src || !p->uvbo || ( p->num_vertices <= 0 ))
			continue;
		bool voulu = false;
		int  passe = pass;
		if( p->uvm_nom == mat_name_checksum )
			voulu = true;
		else if(( p->uvm_nom > mat_name_checksum ) && (( p->uvm_nom - mat_name_checksum ) <= 0x7F ))
		{
			const uint32 d = p->uvm_nom - mat_name_checksum;
			const uint32 sep = d & 0x07, drapeaux = d >> 3;
			if( sep )
			{
				if( sep == (uint32)( pass + 1 )) { voulu = true; passe = 0; }
			}
			else if( drapeaux & ( 1 << pass ))
			{
				voulu = true;
				for( int q = 0; q < pass; ++q )
					if(( drapeaux & ( 1 << q )) == 0 )
						--passe;
			}
		}
		if( !voulu || ( passe != (int)p->uvm_passe ))
			continue;
		++n;
		const float m[4] = { m00, m01, m30, m31 };
		const bool ident = ( m00 == 1.0f ) && ( m01 == 0.0f ) && ( m30 == 0.0f ) && ( m31 == 0.0f );
		// Meme matrice que celle deja posee : rien a refaire ni a tracer.
		if( p->uvm_actif ? ( memcmp( p->uvm_m, m, sizeof( m )) == 0 ) : ident )
			continue;
		const int nv = p->num_vertices;
		float *p_uv = (float *)malloc( sizeof( float ) * 2 * nv );
		if( !p_uv )
			continue;
		// TOUJOURS depuis les UV d'origine (p_uv_src), jamais depuis le uvbo
		// deja transforme : pas d'accumulation.
		float b[4] = { 1e30f, 1e30f, -1e30f, -1e30f };
		for( int v = 0; v < nv; ++v )
		{
			const float u = p->p_uv_src[v * 2 + 0], w = p->p_uv_src[v * 2 + 1];
			const float u2 = m00 * u - m01 * w + m30, w2 = m01 * u + m00 * w + m31;
			p_uv[v * 2 + 0] = u2;
			p_uv[v * 2 + 1] = w2;
			if( u2 < b[0] ) b[0] = u2;
			if( w2 < b[1] ) b[1] = w2;
			if( u2 > b[2] ) b[2] = u2;
			if( w2 > b[3] ) b[3] = w2;
		}
		glBindBuffer( GL_ARRAY_BUFFER, p->uvbo );
		glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 2 * nv, p_uv, GL_STATIC_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		free( p_uv );
		memcpy( p->uvm_m, m, sizeof( m ));
		p->uvm_actif = ident ? 0 : 1;
		// Une ligne par CHANGEMENT de matrice d'une piece, sans plafond. Les
		// UV transformees (u0..u1, v0..v1) disent ou tombe le decalque : la
		// texture n'est lue que dans [0,1] (bloquee au bord si adr=1).
		VLOG( "CAS", "matrice d'UV materiau %08x passe %d -> piece %d (%s passe %d, adr %d/%d, %d sommets) "
		      "m00 %.3f m01 %.3f m30 %.3f m31 %.3f : UV u %.2f..%.2f v %.2f..%.2f (%s)",
		      (unsigned)mat_name_checksum, pass, i, p->decal ? "decalque" : "base", (int)p->uvm_passe,
		      (int)p->decal_au, (int)p->decal_av, nv, m00, m01, m30, m31, b[0], b[2], b[1], b[3],
		      mp_mesh->Name());
	}
	return n > 0;
}

// Retour aux UV d'origine des pieces transformees du maillage. Appele quand un
// geom est (re)lie a son maillage (reconstruction du modele) : le script de
// construction rappelle ensuite set_uv_from_appearance, qui ne repose que les
// matrices des parties hors use_default_uv.
void CVitaGeom::restaurer_uv_cas()
{
	if( !mp_mesh )
		return;
	int n = 0;
	for( int i = 0; i < mp_mesh->NumPieces(); ++i )
	{
		SGpuMesh *p = const_cast< SGpuMesh * >( &mp_mesh->Pieces()[i] );
		if( !p->uvm_actif || !p->p_uv_src || !p->uvbo || ( p->num_vertices <= 0 ))
			continue;
		glBindBuffer( GL_ARRAY_BUFFER, p->uvbo );
		glBufferData( GL_ARRAY_BUFFER, sizeof( float ) * 2 * p->num_vertices, p->p_uv_src, GL_STATIC_DRAW );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		p->uvm_actif = 0;
		++n;
	}
	if( n )
		VLOG( "CAS", "UV d'origine restaurees : %d piece(s) (%s, modele reconstruit)", n, mp_mesh->Name());
}

CGeom *	CVitaGeom::plat_clone( bool instance, CScene *pDestScene )
{
	(void)pDestScene;
	CVitaGeom *p_new = new CVitaGeom;
	p_new->mp_mesh = mp_mesh;
	if( mp_mesh )
		mp_mesh->Retenir();
	// Piece de decor clonee (editeur de parc, issue #29) : une INSTANCE du
	// secteur source, active comme sur Xbox (p_NxGeom.cpp:1355).
	if( m_vita_secteur )
	{
		p_new->m_vita_secteur = m_vita_secteur;
		p_new->m_vita_clone   = true;
		// [SOURCE] XBox/p_NxGeom.cpp:1405 : p_clone->m_bbox = m_bbox, dans la
		// branche instance seulement (vignettes Element3d, pieces souples) ;
		// une piece dure (instance == false) garde une boite vide (#65).
		p_new->m_vita_scene   = m_vita_scene;
		if( instance )
		{
			p_new->m_vita_bbox    = m_vita_bbox;
			p_new->m_vita_bbox_ok = m_vita_bbox_ok;
		}
		p_new->m_vita_active  = true;
		p_new->maj_affiche();
		// Sans scene de destination, ce n'est pas l'editeur de parc
		// (ParkGen.cpp:721 passe mp_cloned_scene) mais un objet de niveau
		// (modelcomponent.cpp:67) : il sera place par son modele (#40).
		p_new->m_vita_suit_modele = ( pDestScene == NULL );
		NxVita::EnregistrerInstance( p_new );
	}
	return p_new;
}

CVitaGeom::~CVitaGeom()
{
	if( m_vita_clone )
		NxVita::OublierInstance( this );
	NxVita::OmbreOublierGeom( this );	// sa pose ne doit plus aller a la carte d'ombre
	VitaOublierPose();
	liberer_lum();
	NxVita::VitaSommetsDetruire( mp_vita_sommets );
	mp_vita_sommets = NULL;
	if( mp_mesh )
		mp_mesh->Lacher();
}

// --- Sommets de rendu (#5) ---------------------------------------------------
// Point d'entree pour le code de jeu (RailEditorComponent, sous __PLAT_VITA__).
void VitaExposerSommets( CGeom *p_geom )
{
	if( p_geom )
		static_cast< CVitaGeom * >( p_geom )->VitaExposerSommets();
}

// [SOURCE] XBox/p_NxGeom.cpp:770-900. XBox lit et ecrit le tampon de sommets
// du maillage, positions deja placees (sMesh::SetPosition les translate) ;
// un clone XBox a sa propre copie (sMesh::Clone). Ici le clone partage le
// maillage source et est place par une matrice : sans ecriture on rend donc
// R x source + position ; une ecriture cree des tampons PRIVES de positions
// finales, dessines sans matrice (p_world_render.cpp, dessiner_instances).
NxVita::SVitaSommets *CVitaGeom::vita_sommets()
{
	if( !m_vita_expose || !m_vita_secteur )
		return NULL;
	if( !mp_vita_sommets )
		mp_vita_sommets = NxVita::VitaSommetsCreer( m_vita_secteur, m_vita_scene );
	return mp_vita_sommets;
}

int CVitaGeom::plat_get_num_render_verts()
{
	return NxVita::VitaSommetsNombre( vita_sommets());
}

void CVitaGeom::plat_get_render_verts( Mth::Vector *p_verts )
{
	NxVita::SVitaSommets *s = vita_sommets();
	const int n = NxVita::VitaSommetsNombre( s );
	if( !n || !p_verts )
		return;
	float *tmp = (float *)malloc( sizeof( float ) * 3 * n );
	if( !tmp )
		return;
	const float pos[3] = { m_vita_pos[X], m_vita_pos[Y], m_vita_pos[Z] };
	NxVita::VitaSommetsLire( s, m_vita_rot, pos, tmp );
	for( int i = 0; i < n; ++i )
		p_verts[i].Set( tmp[3 * i], tmp[3 * i + 1], tmp[3 * i + 2] );
	free( tmp );
}

void CVitaGeom::plat_get_render_colors( Image::RGBA *p_colors )
{
	// Couleurs cuites non relues : neutre (FakeLights les rend telles quelles,
	// plat_set_render_colors ne fait rien).
	const int n = NxVita::VitaSommetsNombre( vita_sommets());
	for( int i = 0; i < n; ++i )
		p_colors[i] = Image::RGBA( 0x80, 0x80, 0x80, 0x80 );
}

void CVitaGeom::plat_set_render_verts( Mth::Vector *p_verts )
{
	NxVita::SVitaSommets *s = vita_sommets();
	const int n = NxVita::VitaSommetsNombre( s );
	if( !n || !p_verts )
		return;
	float *tmp = (float *)malloc( sizeof( float ) * 3 * n );
	if( !tmp )
		return;
	Mth::CBBox bbox;
	for( int i = 0; i < n; ++i )
	{
		tmp[3 * i]     = p_verts[i][X];
		tmp[3 * i + 1] = p_verts[i][Y];
		tmp[3 * i + 2] = p_verts[i][Z];
		bbox.AddPoint( p_verts[i] );
	}
	NxVita::VitaSommetsEcrire( s, tmp );
	free( tmp );
	// Boite du geom : XBox recalcule la sphere du maillage (p_NxGeom.cpp:888).
	m_vita_bbox    = bbox;
	m_vita_bbox_ok = true;
}

// #78 : couleurs eclairees propres a l'instance.
void CVitaGeom::liberer_lum()
{
	if( mp_vita_lum )
	{
		for( int i = 0; i < m_vita_lum_n; ++i )
			if( mp_vita_lum[i].cbo )
				glDeleteBuffers( 1, &mp_vita_lum[i].cbo );
		free( mp_vita_lum );
	}
	mp_vita_lum      = NULL;
	m_vita_lum_n     = 0;
	mp_vita_lum_mesh = NULL;
}

void CVitaGeom::plat_set_world_position( const Mth::Vector &pos )
{
	// #5 : positions privees = positions finales ; XBox translate ses sommets
	// (sMesh::SetPosition), on fait de meme.
	if( mp_vita_sommets && NxVita::VitaSommetsPrives( mp_vita_sommets ))
	{
		const int n = NxVita::VitaSommetsNombre( mp_vita_sommets );
		float *tmp = (float *)malloc( sizeof( float ) * 3 * n );
		if( tmp )
		{
			const float zero[3] = { 0.0f, 0.0f, 0.0f };
			NxVita::VitaSommetsLire( mp_vita_sommets, 0, zero, tmp );
			for( int i = 0; i < n; ++i )
			{
				tmp[3 * i]     += pos[X] - m_vita_pos[X];
				tmp[3 * i + 1] += pos[Y] - m_vita_pos[Y];
				tmp[3 * i + 2] += pos[Z] - m_vita_pos[Z];
			}
			NxVita::VitaSommetsEcrire( mp_vita_sommets, tmp );
			free( tmp );
		}
	}
	m_vita_pos.Set( pos[X], pos[Y], pos[Z], 1.0f );
}

void CVitaGeom::plat_rotate_y( Mth::ERot90 rot )
{
	// Rotation RELATIVE (CSector::SetYRotation passe le delta).
	m_vita_rot = ( m_vita_rot + (int)rot ) & 3;
}

CGeom *	CVitaGeom::plat_clone( bool instance, CModel *pDestModel )
{
	(void)instance;
	CVitaGeom *p_new = new CVitaGeom;
	p_new->mp_mesh = mp_mesh;
	p_new->mp_vita_modele = pDestModel;
	if( mp_mesh )
		mp_mesh->Retenir();
	return p_new;
}


bool CVitaGeom::plat_load_geom_data( CMesh *pMesh, CModel *pModel,
                                     bool color_per_material )
{
	liberer_lum();		// #78 : etat d'eclairage de l'ancien maillage
	if( mp_mesh )
		mp_mesh->Lacher();
	mp_mesh = (CVitaMesh *)pMesh;
	if( mp_mesh )
		mp_mesh->Retenir();
	mp_vita_modele = pModel;
	restaurer_uv_cas();		// #16 : reset CAS
	return ( mp_mesh != NULL );
}


// --- rendu d'un geom -------------------------------------------------------

// Trace bornee : les changements d'etat sont rares, mais le chargement d'un
// niveau en produit une rafale qu'il ne sert a rien de repeter.
void CVitaGeom::VitaTraceActif( bool active ) const
{
	static int s_n = 0;
	if( s_n++ < 40 )
		VLOG( "SCN", "secteur/geom %s ('%s')",
		      active ? "RALLUME" : "eteint",
		      ( mp_mesh && mp_mesh->Name()) ? mp_mesh->Name() : "sans nom" );
}

// « mdl 0/1 » : saute le dessin des modeles (mesure, issue #18).
bool g_vita_dessin_modeles = true;

// --- SONDE « pdt » (issue #46) ----------------------------------------------
//
// Les pietons generiques (ped_male/Head_*, Torso_*, Legs_*) montrent une tete
// decalee de ~20 unites vers le haut alors que l'etude sur table (squelette,
// animations, skinning simule) la donne attachee. La sonde recalcule, AU CPU
// et avec la formule exacte de la boucle CPU de DrawSkinned (poids 11/11/10
// bits, os[v*4+k], Mth::Matrix rangee par lignes), le centre des sommets de
// chaque piece a partir des MEMES matrices d'os que le chemin GPU recoit,
// puis le transforme par la racine. Si ce centre est au cou alors que l'ecran
// montre la tete flottante, le defaut est dans le chemin GPU ; s'il flotte
// deja, ce sont les matrices d'os.
static void pdt_minuscule( char *d, int taille, const char *a, const char *b )
{
	int k = 0;
	const char *src[2] = { a, b };
	for( int s = 0; s < 2; ++s )
	{
		for( const char *q = src[s]; q && *q && ( k < taille - 2 ); ++q )
			d[k++] = (( *q >= 'A' ) && ( *q <= 'Z' )) ? (char)( *q + 32 ) : *q;
		if(( s == 0 ) && ( k < taille - 2 ))
			d[k++] = '/';
	}
	d[k] = 0;
}

static bool pdt_cible( const CVitaMesh *m )
{
	char b[96];
	pdt_minuscule( b, sizeof( b ), m->Dossier(), m->Name());
	// Les pieces des pietons assembles sont construites en memoire (CAS) :
	// sans nom de fichier. Elles sont justement celles qu'on cherche (#46).
	return ( b[0] == 0 ) || ( strcmp( b, "/" ) == 0 )
	       || strstr( b, "head_" ) || strstr( b, "torso_" ) || strstr( b, "legs_" )
	       || strstr( b, "ped_" );
}

static void pdt_journal( const CVitaMesh *m, const Mth::Matrix &R,
                         Mth::Matrix *os, int n_os, bool skin )
{
	static const char *noms[5] = { "Draw", "GXM direct", "vitaGL lum", "vitaGL", "CPU" };
	const char *chemin = skin ? noms[( s_pdt_chemin >= 0 && s_pdt_chemin < 5 ) ? s_pdt_chemin : 0]
	                          : "Draw (sans os)";
	double rx = 0.0, ry = 0.0, rz = 0.0;		// repos, espace modele
	double lx = 0.0, ly = 0.0, lz = 0.0;		// skinne, espace modele
	float  ymin = 1e30f, ymax = -1e30f;		// Y monde des sommets skinnes
	int    n = 0;
	int    dom[128];
	memset( dom, 0, sizeof( dom ));

	const SGpuMesh *P = m->Pieces();
	const float INV_1023 = 1.0f / 1023.0f;
	const float INV_511  = 1.0f / 511.0f;
	for( int i = 0; P && ( i < m->NumPieces()); ++i )
	{
		const SGpuMesh *p = &P[i];
		// Un groupe de sommets partages n'est compte qu'une fois : par son
		// proprietaire, comme la boucle CPU.
		if( !p->owns_skin || !p->p_positions )
			continue;
		for( int v = 0; v < p->num_vertices; ++v )
		{
			const float sx = p->p_positions[v * 3 + 0];
			const float sy = p->p_positions[v * 3 + 1];
			const float sz = p->p_positions[v * 3 + 2];
			float ax = sx, ay = sy, az = sz;
			if( skin && os && p->p_weights && p->p_bones )
			{
				const unsigned int packed = p->p_weights[v];
				float w[3];
				w[0] = (float)(( packed       ) & 0x7FF ) * INV_1023;
				w[1] = (float)(( packed >> 11 ) & 0x7FF ) * INV_1023;
				w[2] = (float)(( packed >> 22 ) & 0x3FF ) * INV_511;
				ax = ay = az = 0.0f;
				int kmax = -1;
				for( int k = 0; k < 3; ++k )
				{
					if( w[k] <= 0.0f )
						continue;
					const int b = (int)p->p_bones[v * 4 + k];
					if(( b < 0 ) || ( b >= n_os ))
						continue;
					const Mth::Matrix &mo = os[b];
					ax += w[k] * (( sx * mo[0][0] ) + ( sy * mo[1][0] ) + ( sz * mo[2][0] ) + mo[3][0] );
					ay += w[k] * (( sx * mo[0][1] ) + ( sy * mo[1][1] ) + ( sz * mo[2][1] ) + mo[3][1] );
					az += w[k] * (( sx * mo[0][2] ) + ( sy * mo[1][2] ) + ( sz * mo[2][2] ) + mo[3][2] );
					if(( kmax < 0 ) || ( w[k] > w[kmax] ))
						kmax = k;
				}
				if( kmax >= 0 )
				{
					const int b = (int)p->p_bones[v * 4 + kmax];
					if( b < 128 )
						++dom[b];
				}
			}
			const float wy = ( ax * R[0][1] ) + ( ay * R[1][1] ) + ( az * R[2][1] ) + R[3][1];
			if( wy < ymin ) ymin = wy;
			if( wy > ymax ) ymax = wy;
			rx += sx; ry += sy; rz += sz;
			lx += ax; ly += ay; lz += az;
			++n;
		}
	}
	++s_pdt_lignes;
	if( !n )
	{
		VLOG( "PDT", "%s/%s : %s, os=%d, aucun sommet CPU (piece non skinnee)",
		      m->Dossier(), m->Name(), chemin, n_os );
		return;
	}
	rx /= n; ry /= n; rz /= n;
	lx /= n; ly /= n; lz /= n;
	// Transformation affine : le centre transforme = transforme du centre.
	const double wx = lx * R[0][0] + ly * R[1][0] + lz * R[2][0] + R[3][0];
	const double wy = lx * R[0][1] + ly * R[1][1] + lz * R[2][1] + R[3][1];
	const double wz = lx * R[0][2] + ly * R[1][2] + lz * R[2][2] + R[3][2];
	// Trois os dominants (par nombre de sommets dont ils portent le plus fort poids).
	int top[3] = { -1, -1, -1 };
	for( int t = 0; t < 3; ++t )
		for( int b = 0; b < 128; ++b )
			if( dom[b] && (( top[t] < 0 ) || ( dom[b] > dom[top[t]] ))
			    && ( b != top[0] ) && ( b != top[1] ))
				top[t] = b;
	VLOG( "PDT", "%s/%s : %s os=%d racine(%.1f %.1f %.1f) repos(%.1f %.1f %.1f) "
	             "local(%.1f %.1f %.1f) MONDE(%.1f %.1f %.1f) Y[%.1f..%.1f] n=%d "
	             "dom %d:%d %d:%d %d:%d",
	      m->Dossier(), m->Name(), chemin, n_os,
	      R[3][0], R[3][1], R[3][2], rx, ry, rz, lx, ly, lz, wx, wy, wz,
	      ymin, ymax, n,
	      top[0], ( top[0] >= 0 ) ? dom[top[0]] : 0,
	      top[1], ( top[1] >= 0 ) ? dom[top[1]] : 0,
	      top[2], ( top[2] >= 0 ) ? dom[top[2]] : 0 );

	// Piece de tete : translation (espace modele) des os qui la portent.
	char b[96];
	pdt_minuscule( b, sizeof( b ), m->Dossier(), m->Name());
	if(( strstr( b, "head_" ) || strstr( b, "ped_" ) || ( b[0] == 0 ) || ( strcmp( b, "/" ) == 0 )) && skin && os && ( s_pdt_lignes < PDT_MAX_LIGNES ))
	{
		static const int liste[5] = { 4, 29, 30, 31, 33 };
		float t[5][3];
		for( int k = 0; k < 5; ++k )
			for( int c = 0; c < 3; ++c )
				t[k][c] = ( liste[k] < n_os ) ? os[liste[k]][3][c] : 0.0f;
		++s_pdt_lignes;
		VLOG( "PDT", "   os 4(%.1f %.1f %.1f) 29(%.1f %.1f %.1f) 30(%.1f %.1f %.1f) "
		             "31(%.1f %.1f %.1f) 33(%.1f %.1f %.1f)",
		      t[0][0], t[0][1], t[0][2], t[1][0], t[1][1], t[1][2],
		      t[2][0], t[2][1], t[2][2], t[3][0], t[3][1], t[3][2],
		      t[4][0], t[4][1], t[4][2] );
	}
}

bool CVitaGeom::plat_render( Mth::Matrix *pRootMatrix,
                             Mth::Matrix *ppBoneMatrices, int numBones )
{
	if( !pRootMatrix )
		return false;

	if( mp_mesh )
		VitaRetenirPose( pRootMatrix, ppBoneMatrices, numBones );	// #54

	// Sonde « pdt » : une image complete = jusqu'au retour du premier geom vu.
	bool pdt_ici = false;
	if( g_vita_pdt )
	{
		if( g_vita_pdt == 1 )
		{
			g_vita_pdt    = 2;
			s_pdt_premier = this;
			s_pdt_t0      = sceKernelGetProcessTimeWide();
			s_pdt_lignes  = 0;
			s_pdt_appels  = 0;
			VLOG( "PDT", "--- sonde : une image (lignes max %d) ---", PDT_MAX_LIGNES );
		}
		// Fin par le TEMPS (une image, 16 ms) et non au retour du premier geom :
		// un modele dessine deux fois dans l'image arretait la sonde avant
		// les pietons assembles (#46).
		else if(( sceKernelGetProcessTimeWide() - s_pdt_t0 > 16000 )
		        || ( s_pdt_lignes >= PDT_MAX_LIGNES ) || ( ++s_pdt_appels > 4000 ))
		{
			g_vita_pdt = 0;
			VLOG( "PDT", "--- fin de sonde : %d lignes ---", s_pdt_lignes );
		}
		pdt_ici = ( g_vita_pdt == 2 ) && mp_mesh && pdt_cible( mp_mesh );
	}

	// OBJET DE NIVEAU (Class = LevelObject) : tourelles et canons des tanks de
	// Moscou, issue #40. Le geom est un clone de SECTEUR, sans mp_mesh : il
	// n'y a rien a dessiner ici, le rendu du decor le dessine comme instance
	// (dessiner_instances). On ne fait que retenir la matrice racine, comme
	// XBox/p_NxGeom.cpp:493 (mp_instance->SetTransform( *pRootMatrix )).
	//
	// Avant : on sortait plus bas faute de mp_mesh, et l'instance restait
	// dessinee a m_vita_pos = (0,0,0) -- la geometrie d'un LevelObject est
	// exportee centree sur l'origine (cfuncs.cpp:6224), donc la tourelle
	// partait a l'origine du monde, loin de sa caisse.
	if( m_vita_clone && !mp_mesh )
	{
		m_vita_racine    = *pRootMatrix;
		m_vita_racine_ok = true;
		return true;
	}

	// Geom desactive par le moteur : on ne le dessine pas.
	//
	// C'est ainsi que THUG cache les parties du corps couvertes par un
	// vetement (CModel::Render pose l'etat depuis m_geomActiveMask,
	// NxModel.cpp:292). L'etat etait jete par les stubs de CGeom, donc tout
	// etait dessine -- d'ou le torse qui ressortait du t-shirt.
	if( !IsActive())
	{
		// Nomme, sinon impossible de distinguer ï¿½ piece jamais appelee au
		// rendu ï¿½ de ï¿½ piece appelee mais desactivee ï¿½ -- le pantalon absent
		// releve de l'un des deux.
		// DEDUPLIQUEE PAR NOM, comme la liste des dessines.
		//
		// La version precedente n'affichait que six lignes toutes les 1800
		// images : elle pouvait rater completement un objet, et son silence se
		// lisait a tort comme « cet objet n'est pas desactive ». Un echantillon
		// trop maigre ne prouve pas une absence.
		if( g_vita_trace_modeles && mp_mesh )
		{
			static const char *s_vus[128];
			static int         s_ni = 0;
			if( g_vita_trace_reset_inactifs )
			{
				s_ni = 0;
				g_vita_trace_reset_inactifs = false;
			}
			const char *nom = mp_mesh->Name();
			if( nom )
			{
				bool deja = false;
				for( int k = 0; k < s_ni; ++k )
					if( s_vus[k] == nom ) { deja = true; break; }
				if( !deja && ( s_ni < 128 ))
				{
					s_vus[s_ni++] = nom;
					VLOG( "DRAW", "geom INACTIF #%d : '%s'", s_ni, nom );
				}
			}
		}
		++g_cpt_dessin[1];	// #48
		return true;
	}

	// NOM des modeles reellement dessines.
	//
	// Sans le nom, un objet fantome n'est qu'un polygone de plus : impossible
	// de savoir si c'est un decor legitime ou une rampe de mission qui aurait
	// du rester cachee. Avec le nom, on interroge les scripts du jeu et on sait
	// QUI aurait du la desactiver -- au lieu d'essayer des correctifs au juge.
	//
	// Liste periodique et dedupliquee : muette une fois le tour fait.
	if( g_vita_trace_modeles && mp_mesh )
	{
		static const char *s_vus[256];
		static int         s_n = 0;

		// La liste se VIDE a chaque armement de la trace. Sans cela elle reste
		// muette apres le premier tour, et « aucun nom » se lit a tort comme
		// « rien n'est dessine » -- ce qui m'a deja fait conclure de travers.
		if( g_vita_trace_reset )
		{
			s_n = 0;
			g_vita_trace_reset = false;
		}

		const char *nom = mp_mesh->Name();
		if( nom )
		{
			bool deja = false;
			for( int k = 0; k < s_n; ++k )
				if( s_vus[k] == nom ) { deja = true; break; }
			if( !deja && ( s_n < 256 ))
			{
				s_vus[s_n++] = nom;
				// La POSITION avec le nom : « dessine » ne veut pas dire
				// « visible ». Un objet soumis au rendu mais pose a l'origine
				// du monde, ou a mille metres, ne se voit pas davantage qu'un
				// objet absent -- et les deux se ressemblent a l'ecran.
				VLOG( "DRAW", "modele dessine #%d : '%s'  en (%.0f %.0f %.0f)",
				      s_n, nom,
				      (*pRootMatrix)[3][0], (*pRootMatrix)[3][1],
				      (*pRootMatrix)[3][2] );
			}
		}
	}

	// La vue de MAINTENANT, pas celle de la frame precedente.
	//
	// Ce chemin tourne dans la phase logique, avant RenderWorld. Reprendre la
	// vue memorisee projetait les modeles depuis le point de vue d'il y a une
	// frame pendant que le decor l'etait depuis le point de vue courant : les
	// personnages et les voitures glissaient sur le sol des que la camera
	// tournait. Observe sur console, explique, corrige ici.
	// OMBRE PORTEE (issue #45 V2 / #4) : la pose de ce geom, si son modele
	// projette une ombre, AVANT le tri de visibilite -- un skater hors champ
	// projette quand meme son ombre dans le champ. La carte est rendue au
	// debut de l'image suivante (p_ombre.cpp), comme XBox la rend au debut
	// du rendu (render_shadow_targets) a partir de toutes les instances
	// actives du modele.
	if( mp_mesh && ppBoneMatrices && ( numBones > 0 )
	    && NxVita::OmbreModeleSuivi( mp_vita_modele ))
		NxVita::OmbreCapturer( this, mp_vita_modele, mp_mesh,
		                       (const float *)pRootMatrix,
		                       (const float *)ppBoneMatrices, numBones );

	NxVita::RefreshViewFromCamera();

	float view[16];
	if( !NxVita::GetViewMatrix( view ))
	{
		++g_cpt_dessin[2];	// #48
		return false;		// pas encore de camera : rien a dessiner de sense
	}

	// Tri de visibilite AVANT le skinning.
	//
	// Ce chemin est appele depuis CModelComponent::Update, donc dans la phase
	// LOGIQUE -- mesure a 0,91 ms par modele, 83 % du temps des composants et
	// ~80 % de la frame. Un passant a l'autre bout du niveau etait skinne au
	// CPU sommet par sommet, puis dessine, pour finir hors de l'ecran.
	//
	// Le rayon est genereux (400 unites, un skater en fait ~65) : mieux vaut
	// dessiner quelques modeles inutiles que de faire disparaitre une jambe au
	// bord du champ. On se sert de la translation de la matrice racine, rangee
	// en ligne 3 (convention Direct3D de Mth::Matrix).
	{
		Mth::Matrix &rm = *pRootMatrix;

		// Les personnages et les voitures ï¿½ bougent quand on bouge le stick ï¿½.
		// Deux causes possibles et opposees : soit leur position MONDE change
		// vraiment, soit seule leur projection bouge. On journalise donc la
		// translation de la matrice racine -- exactement la position utilisee
		// pour les dessiner.
		//
		// On suit UN objet, identifie par son adresse.
		//
		// Premiere version : ï¿½ les trois premiers modeles de la frame ï¿½. Elle
		// ne valait rien -- l'ordre de parcours change, donc chaque battement
		// montrait des personnages DIFFERENTS, et leurs positions sautaient
		// sans rien prouver. Une mesure qui ne suit pas le meme sujet ne
		// mesure rien.
		{
			// Le sujet doit RESTER VIVANT pendant la mesure.
			//
			// Version precedente : on figeait le premier geom rencontre. C'etait
			// un objet du menu, qui disparait des l'entree en jeu -- la trace
			// s'arretait donc avant la seule phase qui nous interesse. Deuxieme
			// mesure de suite invalidee par sa conception, apres ï¿½ les trois
			// premiers modeles de la frame ï¿½ qui ne suivait pas le meme objet.
			//
			// On re-arme donc le suivi des que le sujet n'a plus ete vu depuis
			// 600 rendus : le nouveau sujet est forcement un objet present
			// maintenant.
			// Plusieurs geoms DISTINCTS a la meme position ?
			//
			// Le skater rendu par GetLocalSkater() est fige a (626 -44 2992)
			// pendant que le jeu bouge, et une mesure precedente montrait trois
			// modeles differents exactement au meme endroit. Si des adresses
			// distinctes partagent une position dans la MEME frame, tous les
			// modeles sont dessines avec la matrice du joueur -- ce qui explique
			// mot pour mot ï¿½ quand je bouge le stick, les personnages et les
			// voitures bougent aussi ï¿½.
			//
			// On journalise donc adresse + position pour les six premiers geoms
			// d'une frame sur soixante : la comparaison se fait a l'interieur
			// d'une meme frame, seule facon de conclure.
			{
				static int s_frame_seq = 0;
				static int s_in_frame  = 0;
				static int s_last_log  = -1;

				if( s_in_frame >= 60 )
				{
					s_in_frame = 0;
					++s_frame_seq;
				}
				if(( s_frame_seq % 60 ) == 1 )
				{
					if( s_last_log != s_frame_seq )
					{
						s_last_log = s_frame_seq;
						VLOG( "POS", "--- frame %d ---", s_frame_seq );
					}
					if( s_in_frame < 6 )
						VLOG( "POS", "  geom %p : (%.1f %.1f %.1f)",
						      (const void *)this, rm[3][0], rm[3][1], rm[3][2] );
				}
				++s_in_frame;
			}
		}

		// Rayon resserre : 400 etait pose ï¿½ genereux ï¿½ avant toute mesure.
		// Depuis, le rayon SKINNE reel d'un personnage est mesure a ~44
		// (centre/rayon des pieces) ; 150 couvre pose etendue et marge, et
		// cesse de skinner les PNJ franchement hors champ.
		const bool visible = NxVita::SphereVisible( rm[3][0], rm[3][1], rm[3][2], 150.0f );

		// Combien d'instances par frame, combien cullees : c'est le compteur
		// qui decide de la prochaine optimisation (frequence de skinning,
		// suspension...) au lieu d'un pari.
		{
			static int s_calls = 0, s_culled = 0, s_frames = 0;
			++s_calls;
			if( !visible )
				++s_culled;
			if( ++s_frames >= 4000 )
			{
				VLOG( "PROF", "modeles : %d appels, %d culles (%d%%)",
				      s_calls, s_culled,
				      s_calls ? ( s_culled * 100 / s_calls ) : 0 );
				s_calls = s_culled = s_frames = 0;
			}
		}

		if( !g_vita_dessin_modeles )
			return true;	// A/B « mdl 0 » : tout sauf le dessin
		if( !visible )
		{
			++g_cpt_dessin[3];	// #48 culle
			return true;	// ï¿½ rien a signaler ï¿½, pas ï¿½ echec ï¿½
		}
	}

	// L'ETAT GL COMPLET, pas seulement les matrices.
	//
	// Ce chemin tourne dans la phase logique : l'etat herite vient de la FIN
	// de la frame precedente, donc d'APRES les sprites 2D -- depth test
	// DESACTIVE, blend au gre du dernier quad. Les pieces du personnage se
	// dessinaient en ordre du peintre pur : le torse ecrasait un bras selon
	// la pose, le col du t-shirt (dessine apres le visage) ecrasait le cou.
	// L'asymetrie du bras -- impossible pour un retrait CAS symetrique, les
	// comptes le prouvent -- est la signature d'un recouvrement dependant de
	// la pose.
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );

	// LA PROJECTION, avant tout le reste.
	//
	// Sans cette ligne on herite de ce que la derniere passe a laisse -- et la
	// derniere passe est le 2D, en ORTHOGRAPHIQUE sur 0..960 x 0..544. Un
	// modele a quelques centaines d'unites de la camera se retrouve alors
	// projete tres loin hors de l'ecran. Tout le reste etait juste :
	// geometrie, skinning, matrice racine, position devant la camera et dans
	// le tronc de vision -- verifies un par un. Il ne manquait que ceci.
	NxVita::SetWorldProjection();

	glMatrixMode( GL_MODELVIEW );
	glPushMatrix();
	glLoadMatrixf( view );

	// Mth::Matrix est un float[4][4] range par LIGNES, translation en ligne 3
	// -- convention Direct3D. Sa disposition en memoire est exactement celle
	// qu'attend OpenGL, qui lit ses matrices par COLONNES : la transposition
	// implicite tombe juste. On peut donc passer les flottants tels quels.
	glMultMatrixf( (const float *)pRootMatrix );

	// Qui place ce modele ? Le skater du menu ne se deplace pas quand on
	// change le champ de vision : sa position d'ecran ne peut donc pas venir
	// de la conversion de CElement3d (qui l'excentre de 59 unites en espace
	// camera). La translation de sa matrice tranche : (0 ~0 -350) = objet
	// pose dans le monde ; toute autre valeur = element d'interface.
	// QUEL AXE POINTE VERS LE HAUT ?
	//
	// Le commentaire ci-dessus affirme que la transposition D3D -> OpenGL
	// « tombe juste ». Ce n'est reste qu'une affirmation : on la mesure.
	// Un objet pose a plat dans le monde doit avoir son vecteur Y proche de
	// (0,1,0). S'il vaut (0,0,1) ou (0,0,-1), l'objet est bascule de 90 --
	// exactement « les voitures sont verticales et rentrent dans le sol ».
	{
		// Periodique et non « les dix premiers » : les premiers modeles rendus
		// sont ceux du menu, tous a l'identite. Ce qui nous interesse est en
		// JEU. Trois modeles toutes les 600 images.
		static int s_m = 0;
		if(( s_m++ % 600 ) < 3 )
		{
			const float *m = (const float *)pRootMatrix;
			VLOG( "MAT", "modele #%d : X(%.2f %.2f %.2f) Y(%.2f %.2f %.2f) "
			             "Z(%.2f %.2f %.2f) T(%.0f %.0f %.0f)",
			      s_m,
			      m[0], m[1], m[2],
			      m[4], m[5], m[6],
			      m[8], m[9], m[10],
			      m[12], m[13], m[14] );
		}
	}

	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	// #48 : ECRITURE de profondeur imposee. Les modeles sont dessines AVANT le
	// decor ; si la passe precedente (translucides, particules, 2D) a laisse
	// glDepthMask a FALSE, ils n'ecrivaient pas leur profondeur et le decor
	// les recouvrait entierement : personnages et voitures absents une image.
	if( g_vita_zw_modeles )
	{
		glDepthMask( GL_TRUE );
		glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	}
	glDisable( GL_CULL_FACE );
	glDisable( GL_BLEND );

	// Combien coute REELLEMENT le dessin des modeles ? Le compteur Â« logique Â»
	// de la boucle principale (Mainloop.cpp:556) englobe la mise a jour des
	// objets ET leur rendu, donc il ne dit pas lequel des deux pese. Sans
	// cette mesure-ci, optimiser le skinning serait un pari -- et j'en ai deja
	// perdu un sur ce meme sujet.
	const SceUInt64 t_geom = sceKernelGetProcessTimeWide();

	// Les luminosites ne sont plus extraites ici : ResolveLighting() les
	// applique deja, au meme endroit que la selection entre couleurs locales
	// et couleurs du monde. Les separer, c'etait risquer de moduler les unes
	// avec les facteurs des autres.

	if( mp_mesh )
	{
		// PAS de compteur ici : DrawSkinned tient deja ses propres bornes,
		// une pour le calcul et une pour le dessin. Cumuler les deux ici
		// comptait tout DEUX FOIS -- au point que le ï¿½ calcul ï¿½ annonce
		// depassait le total de la fonction qui le contient.
		const SceUInt64 t_d0 = sceKernelGetProcessTimeWide();
		g_pic_calc = g_pic_tele = 0; g_pic_n = g_pic_v = 0;
		// Image sans animation (should_animate faux : une image sur 2-4 de
		// loin, toujours au-dela de lod_dist1) : XBox garde la DERNIERE pose,
		// son instance pointe sur GetBoneTransforms() (XBox/p_NxGeom.cpp:223).
		// Vita dessinait la pose de repos : PNJ en T ou qui clignotent (#37,
		// #46). On reprend les matrices gardees par le modele.
		if(( !ppBoneMatrices || ( numBones <= 0 )) && mp_vita_modele && mp_mesh->IsSkinned()
		    && mp_vita_modele->GetBoneTransforms() && ( mp_vita_modele->GetNumBones() > 0 ))
		{
			ppBoneMatrices = mp_vita_modele->GetBoneTransforms();
			numBones       = mp_vita_modele->GetNumBones();
		}
		// VEHICULES (#48) : a distance SuspendComponent ne les anime qu'une
		// image sur 2 ou 4, et CModel::Render passe alors ( NULL, 0 ) -- nos
		// pieces retombaient sur la matrice de SETUP du fichier : la cabine
		// d'un camion apparaissait une image sur deux. XBox garde les dernieres
		// matrices de l'instance. Seulement si ce geom en a deja recu (une
		// voiture jamais animee garde la matrice de setup).
		if( ppBoneMatrices && ( numBones > 0 ) && !mp_mesh->IsSkinned())
			m_vita_os_vus = true;
		else if(( !ppBoneMatrices || ( numBones <= 0 )) && m_vita_os_vus && mp_vita_modele && g_vita_os_persist
		         && mp_vita_modele->GetBoneTransforms() && ( mp_vita_modele->GetNumBones() > 0 ))
		{
			ppBoneMatrices = mp_vita_modele->GetBoneTransforms();
			numBones       = mp_vita_modele->GetNumBones();
		}
		++g_cpt_dessin[ s_en_rejeu ? 4 : 0 ];	// #48 : dessines (rejeu a part)
		const bool skin = ( ppBoneMatrices && ( numBones > 0 ));
		sp_racine_courante = pRootMatrix;	// eclairage dans l'espace du modele (#4)
		sp_modele_courant  = mp_vita_modele;	// auto-ombrage (#45)
		// #78 : etat d'eclairage de l'instance pose pour LES DEUX chemins. Les
		// vehicules ont des os (roues, portes) : ils passent par DrawSkinned,
		// qui renvoie leur maillage rigide vers Draw -- sans cet etat, ils
		// retombaient sur le cache partage (diag : 0-6 pieces par instance,
		// jusqu'a 6400 par le cache partage / 2 s, veh_cab_ny, policecar).
		// #78 : etat d'eclairage propre a cette instance, une entree par
		// piece, refait si le maillage a change sous le geom.
		if( mp_vita_lum && (( mp_vita_lum_mesh != mp_mesh ) || ( m_vita_lum_n != mp_mesh->NumPieces())))
		{
			static int s_n_refait = 0;
			static SceUInt64 s_t_refait = 0;
			++s_n_refait;
			const SceUInt64 tn = sceKernelGetProcessTimeWide();
			if( tn - s_t_refait > 2000000 )
			{
				VLOG( "LUM", "diag : etat d'eclairage d'instance refait %d fois / 2 s (ex. '%s')",
				      s_n_refait, sp_lum_nom );
				s_t_refait = tn; s_n_refait = 0;
			}
			liberer_lum();
		}
		if( g_vita_lum_instance && !mp_vita_lum && ( mp_mesh->NumPieces() > 0 ))
		{
			// +1 : entree du vehicule entier (lpa, #78).
			mp_vita_lum = (SLumInstance *)calloc( mp_mesh->NumPieces() + 1, sizeof( SLumInstance ));
			if( mp_vita_lum )
			{
				m_vita_lum_n     = mp_mesh->NumPieces();
				mp_vita_lum_mesh = mp_mesh;
			}
		}
		sp_lum_inst  = g_vita_lum_instance ? mp_vita_lum : NULL;
		s_lum_inst_n = m_vita_lum_n;
		if( skin )
			mp_mesh->DrawSkinned( ppBoneMatrices, numBones, m_vita_rgba,
			                      (CVitaModelLights *)mp_vita_lights );
		else
			mp_mesh->Draw( m_vita_rgba, ppBoneMatrices, numBones,
			               (CVitaModelLights *)mp_vita_lights );
		sp_lum_inst  = NULL;
		s_lum_inst_n = 0;
		s_lum_decision = -1;
		if( pdt_ici )
			pdt_journal( mp_mesh, *pRootMatrix, ppBoneMatrices, numBones, skin );
		// Issue #18 : repartition skinnes / statiques, par seconde.
		{
			static SceUInt64 s_t[2] = { 0, 0 }, s_debut = 0;
			static int s_n[2] = { 0, 0 }, s_p[2] = { 0, 0 };
			static SceUInt64 s_pire = 0;
			static int s_lents = 0;
			static char s_pire_nom[48] = "";
			const SceUInt64 t_d1 = sceKernelGetProcessTimeWide();
			s_t[skin ? 1 : 0] += t_d1 - t_d0;
			// Pointes (#18) : le dessin le plus lent de la fenetre, et combien
			// depassent 2 ms -- un dessin skinne ordinaire coute ~0,2 ms.
			if(( t_d1 - t_d0 ) > 2000 )
				++s_lents;
			if(( t_d1 - t_d0 ) > 3000 )
			{
				static int s_pics = 0;
				if( s_pics++ < 60 )
					VLOG( "PIC", "%s/%d : %.2f ms dont eclairage CPU %d pieces %d sommets calcul %.2f televersement %.2f ms",
					      mp_mesh->Name(), mp_mesh->NumPieces(), ( t_d1 - t_d0 ) / 1000.0, g_pic_n, g_pic_v,
					      g_pic_calc / 1000.0, g_pic_tele / 1000.0 );
				if(( g_pic_idx >= 0 ) && ( s_pics < 60 ))
				{
					const SGpuMesh *q = &mp_mesh->Pieces()[g_pic_idx];
					VLOG( "PIC", "  piece %d la plus lente : %.2f ms (tex %u blend %u %d indices cbo_rig %u nbo %u)",
					      g_pic_idx, g_pic_max / 1000.0, (unsigned)q->texture, (unsigned)q->blend, q->num_indices,
					      (unsigned)q->cbo_rig, (unsigned)q->vbo );
				}
			}
			g_pic_calc = g_pic_tele = 0; g_pic_n = g_pic_v = 0;
			if(( t_d1 - t_d0 ) > s_pire )
			{
				s_pire = t_d1 - t_d0;
				snprintf( s_pire_nom, sizeof( s_pire_nom ), "%s/%d", mp_mesh->Name(), mp_mesh->NumPieces());
			}
			++s_n[skin ? 1 : 0];
			s_p[skin ? 1 : 0] += mp_mesh->NumPieces();
			if( !s_debut )
				s_debut = t_d1;
			if( t_d1 - s_debut > 2000000 )
			{
				VLOG( "PROF", "geoms/s : statiques %d (%d pieces) %.1f ms/s | skinnes %d (%d pieces) %.1f ms/s",
				      s_n[0] / 2, s_p[0] / 2, s_t[0] / 2000.0, s_n[1] / 2, s_p[1] / 2, s_t[1] / 2000.0 );
				VLOG( "PROF", "  dessin le plus lent %.2f ms (%s), %d dessins > 2 ms", s_pire / 1000.0, s_pire_nom, s_lents );
				s_pire = 0; s_lents = 0;
				s_t[0] = s_t[1] = 0; s_n[0] = s_n[1] = 0; s_p[0] = s_p[1] = 0; s_debut = t_d1;
			}
		}
	}

	glPopMatrix();

	// Cumul sur 60 frames : un total par geom serait illisible, une moyenne
	// par frame se compare directement au Â« logique Â» de la boucle.
	{
		static SceUInt64 s_acc = 0;
		static int       s_n   = 0;
		static int       s_calls = 0;
		s_acc += sceKernelGetProcessTimeWide() - t_geom;
		++s_calls;
		if( ++s_n >= 600 )
		{
			VLOG( "PROF", "modeles %.1f ms/frame = calcul %.1f + dessin %.1f "
			              "| %d modeles, %d draws (eclairage %s)",
			      (double)s_acc / 1000.0 / 60.0,
			      (double)g_vita_t_skin / 1000.0 / 60.0,
			      (double)g_vita_t_draw / 1000.0 / 60.0,
			      s_calls / 60, g_vita_n_draws / 60,
			      g_vita_lighting ? "on" : "off" );
			VLOG( "PROF", "  sommets skinnes : %d par frame",
			      g_vita_n_verts / 60 );
			g_vita_n_verts = 0;
			s_acc = 0; s_n = 0; s_calls = 0;
			g_vita_t_skin = 0; g_vita_t_draw = 0; g_vita_n_draws = 0;
		}
	}

	// Ou le modele est-il place, et ou regarde la camera ? Si les deux sont
	// loin l'un de l'autre, l'invisibilite s'explique sans chercher plus loin.
	{
		static int s_p = 0;
		if( ++s_p <= 3 )
		{
			// La 4e ligne de la matrice racine est la translation.
			const float *m = (const float *)pRootMatrix;
			// La position de la camera se relit dans la matrice de vue :
			// c'est -(R^T * t), donc on inverse.
			float cx = -( view[0] * view[12] + view[1] * view[13] + view[2]  * view[14] );
			float cy = -( view[4] * view[12] + view[5] * view[13] + view[6]  * view[14] );
			float cz = -( view[8] * view[12] + view[9] * view[13] + view[10] * view[14] );
			// Position du modele DANS LE REPERE DE LA CAMERA. En OpenGL, ce
			// qui est visible a un Z NEGATIF ; un Z positif est derriere
			// l'observateur. C'est le seul test qui tranche Â« hors champ Â».
			float ex = view[0] * m[12] + view[4] * m[13] + view[8]  * m[14] + view[12];
			float ey = view[1] * m[12] + view[5] * m[13] + view[9]  * m[14] + view[13];
			float ez = view[2] * m[12] + view[6] * m[13] + view[10] * m[14] + view[14];
			VLOG( "POS", "modele monde(%.0f %.0f %.0f) camera(%.0f %.0f %.0f) "
			             "-> oeil(%.0f %.0f %.0f) %s",
			      m[12], m[13], m[14], cx, cy, cz, ex, ey, ez,
			      ( ez < 0.0f ) ? "DEVANT" : "DERRIERE" );
		}
	}

	static int s_f = 0;
	if( ++s_f <= 3 )
		VLOG( "MDL", "rendu de geom : %d morceaux, %s, %d os",
		      mp_mesh ? mp_mesh->NumPieces() : 0,
		      ( mp_mesh && mp_mesh->IsSkinned()) ? "skinne" : "statique",
		      numBones );

	return true;
}

} // namespace Nx
