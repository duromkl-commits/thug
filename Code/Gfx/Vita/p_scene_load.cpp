/*****************************************************************************
**  THUG-Vita — backend graphique                                            **
**  Code/Gfx/Vita/p_scene_load.cpp                                          **
**                                                                          **
**  Lecture du format de scene Xbox (.scn.xbx / .mdl.xbx).                  **
**                                                                          **
**  C'est le coeur du palier 3 : sans lui, s_plat_load_scene rend une scene **
**  vide, aucun objet de niveau n'existe et le front-end tourne en rond a   **
**  chercher des objets jamais crees.                                       **
**                                                                          **
**  SOURCE DU FORMAT — pas de retro-ingenierie ici, tout est dans l'arbre : **
**    Code/Gfx/DX9/p_nx.cpp:768        s_plat_load_scene                    **
**    Code/Gfx/DX9/p_NxSector.cpp:49   CXboxSector::Load_Internal           **
**    Code/Gfx/DX9/NX/material.cpp:883 LoadMaterials                        **
**  Le backend DX9 est celui que kisak maintient ; les fichiers XBox/ ont   **
**  ete vides (LoadScene y rend NULL).                                       **
**                                                                          **
**  Ce qu'on garde a ce stade : positions et indices, rien d'autre. Pas de  **
**  normales, pas d'UV, pas de couleurs, pas de materiaux -- le critere du  **
**  palier 3 est « de la geometrie non texturee a l'ecran ». Les autres     **
**  flux sont lus quand meme, car il faut avancer dans le fichier au bon    **
**  endroit : une seule lecture omise decale tout le reste.                 **
*****************************************************************************/

#include <core/defines.h>
#include <sys/file/filesys.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "vita_log.h"
#include "p_scene_load.h"

namespace NxVita
{

// Nom du fichier en cours de lecture, pour que la trace des jeux d'UV dise DE
// QUEL modele elle parle. Sans lui, � 3 jeux de coordonnees � ne designe
// personne.
static const char *sp_current_file = "?";

// Drapeaux de secteur (Load_Internal). Seuls ceux qui changent la disposition
// du fichier nous interessent.
#define SECTOR_FLAG_HAS_TEXCOORDS	0x01
#define SECTOR_FLAG_HAS_COLORS		0x02
#define SECTOR_FLAG_HAS_NORMALS		0x04
#define SECTOR_FLAG_HAS_WEIGHTS		0x10
#define SECTOR_FLAG_HAS_VC_WIBBLE	0x800
#define SECTOR_FLAG_BILLBOARD		0x00800000UL

// Drapeaux de materiau (DX9/NX/material.h)
#define MATFLAG_UV_WIBBLE				(1<<0)
#define MATFLAG_VC_WIBBLE				(1<<1)
#define MATFLAG_ENVIRONMENT				(1<<3)
#define MATFLAG_TRANSPARENT				(1<<6)
#define MATFLAG_PASS_TEXTURE_ANIMATES	(1<<11)


// --- petites aides de lecture ---------------------------------------------
//
// On ne teste pas le retour de chaque lecture : le fichier vient d'une ISO
// dont on suppose l'integrite. En revanche on verifie les COMPTES lus, qui
// sont le vrai signal qu'on s'est desynchronise (voir check_count).

static bool s_read_error = false;
bool g_vita_garder_normales = false;

// Flux MEMOIRE optionnel. Le skater Create-A-Skater arrive en RAM (ses pieces
// vivent dans skaterparts.pre, deja charge) : c'est le meme format .skin.xbx,
// lu au meme endroit, mais depuis un tampon. Quand s_mem est pose, rd() et
// skip() lisent dedans ; sinon ils passent par File::*. Un seul parseur pour
// les deux chemins -- c'est exactement ce que fait la reference DX9 avec son
// drapeau is_file (p_nxsector.cpp).
static const unsigned char *s_mem      = NULL;
static size_t               s_mem_off  = 0;
static size_t               s_mem_size = 0;

static void rd( void *p_dst, int size, int count, void *p_file )
{
	if( s_read_error )
		return;
	const size_t want = (size_t)size * (size_t)count;
	if( s_mem )
	{
		if( s_mem_off + want > s_mem_size )
		{
			s_read_error = true;
			return;
		}
		memcpy( p_dst, s_mem + s_mem_off, want );
		s_mem_off += want;
		return;
	}
	// File::Read rend des OCTETS, pas des elements -- voir p_filesys.cpp.
	const size_t got = File::Read( p_dst, size, count, p_file );
	if( got != want )
	{
		// Cet echec etait MUET. Un niveau entier est arrive vide (« 0
		// maillages [LECTURE INTERROMPUE] ») sans qu'une seule ligne du log
		// dise pourquoi -- alors que les deux autres causes d'arret, elles,
		// s'annoncent. On perd moins de temps a le dire qu'a le deviner.
		VLOG( "SCN", "!! lecture courte : %u octets demandes, %u obtenus",
		      (unsigned)want, (unsigned)got );
		s_read_error = true;
	}
}

static uint32 rd_u32( void *p_file )
{
	uint32 v = 0;
	rd( &v, sizeof( uint32 ), 1, p_file );
	return v;
}

static int rd_int( void *p_file )
{
	int v = 0;
	rd( &v, sizeof( int ), 1, p_file );
	return v;
}

static void skip( void *p_file, int bytes )
{
	if( s_read_error || bytes <= 0 )
		return;
	if( s_mem )
	{
		s_mem_off += (size_t)bytes;
		if( s_mem_off > s_mem_size )
			s_read_error = true;
		return;
	}
	File::Seek( p_file, bytes, SEEK_CUR );
}

// Un compte aberrant veut dire qu'on ne lit plus au bon endroit. Mieux vaut
// s'arreter net que d'allouer 2 Go et planter trois fonctions plus loin.
static bool check_count( const char *what, int n, int max )
{
	if(( n < 0 ) || ( n > max ))
	{
		VLOG( "SCN", "!! %s = %d : hors bornes (max %d), lecture desynchronisee",
		      what, n, max );
		s_read_error = true;
		return false;
	}
	return true;
}


// --- table des materiaux ---------------------------------------------------
//
// On ne construit pas de vrais materiaux : on retient seulement, pour chaque
// materiau, la texture de sa PREMIERE passe. C'est ce qui permettra ensuite de
// relier un maillage a une texture. Tout le reste est traverse -- chaque champ
// compte, y compris les booleens d'un octet et les blocs optionnels.
#define MAX_MATERIALS 4096
static uint32 s_mat_checksum[MAX_MATERIALS];
static uint32 s_mat_nom[MAX_MATERIALS];		// checksum du NOM (#58, SetMaterialColor)
static uint32 s_mat_texture[MAX_MATERIALS];
static uint32 s_mat_blend[MAX_MATERIALS];
static uint32 s_mat_passes[MAX_MATERIALS];
// Deuxieme couche : texture, mode de combinaison, drapeaux.
static uint32 s_mat_texture2[MAX_MATERIALS];
static float  s_mat_envtile2[MAX_MATERIALS][2];	// tuilage envmap de la couche 1 (#5)
static float  s_mat_envtile0[MAX_MATERIALS][2];	// et de la passe 0
static uint32 s_mat_blend2[MAX_MATERIALS];
static uint32 s_mat_flags2[MAX_MATERIALS];
// Option B, etape 2 : ce que la formule Xbox demande PAR PASSE. [SOURCE]
// XBox/NX/material.cpp:651 (couleur) et :671 (m_color[p][3] = alpha fixe / 128,
// lu par les modes _FIXED de render.cpp:420).
static float  s_mat_color2[MAX_MATERIALS][3];
static unsigned char s_mat_fixa[MAX_MATERIALS];
static unsigned char s_mat_fixa2[MAX_MATERIALS];
// Option B, etape 3 (issue #6) : passes 2 et 3. Les pelouses de New Jersey en
// ont 3 ou 4 (decode sur table : herbe, terre en BLEND selon l'alpha des
// sommets, puis BLEND_PREVIOUS_MASK) ; sans elles, le melange intermediaire
// restait a l'ecran -- les taches noires signalees par l'humain.
static uint32 s_mat_xtex[MAX_MATERIALS][2];
static uint32 s_mat_xblend[MAX_MATERIALS][2];
static uint32 s_mat_xflags[MAX_MATERIALS][2];
static float  s_mat_xcolor[MAX_MATERIALS][2][3];
static unsigned char s_mat_xfixa[MAX_MATERIALS][2];
static unsigned char s_mat_xau[MAX_MATERIALS][2], s_mat_xav[MAX_MATERIALS][2];
static float  s_mat_xenvtile[MAX_MATERIALS][2][2];	// tuilage envmap des passes 2-3
// Passe 0 : les champs qui decident du classement et de l'etat de rendu.
static uint32 s_mat_flags0[MAX_MATERIALS];
static unsigned char s_mat_au[MAX_MATERIALS],  s_mat_av[MAX_MATERIALS];
static unsigned char s_mat_au2[MAX_MATERIALS], s_mat_av2[MAX_MATERIALS];
static uint32 s_mat_cutoff[MAX_MATERIALS];
static unsigned char s_mat_zbias[MAX_MATERIALS];
// UV WIBBLE (issue #43) : 8 flottants par passe, dans l'ordre de
// sUVWibbleParams ([SOURCE] XBox/NX/material.h:40) -- vitesses U/V,
// frequences U/V, amplitudes U/V, phases U/V. Bit p de s_mat_uvw = la passe
// p porte MATFLAG_UV_WIBBLE. On le sautait (skip de 32 octets).
static unsigned char s_mat_uvw[MAX_MATERIALS];
static float  s_mat_uvw_par[MAX_MATERIALS][4][8];
static unsigned char s_mat_nobfc[MAX_MATERIALS];	// XBox/NX/material.cpp:306	// XBox/NX/material.cpp:615
static uint32 s_mat_sorted[MAX_MATERIALS];
static float  s_mat_draworder[MAX_MATERIALS];
// COULEUR DU MATERIAU, passe 0. [SOURCE] XBox/NX/material.cpp:651 --
// trois flottants, neutre = 0.5, consommes par PixelShader0.psh :
//     saturate( 4 * v0.rgb * t0.rgb * c0.rgb )
// Le facteur 4 et le neutre 0.5 se compensent, ce qui rend le doublage
// des couleurs de sommets suffisant TANT QUE la couleur est neutre.
// Mesure sur table : 408 des 1424 materiaux de New Jersey ne le sont pas
// (ecart median 18 %, jusqu'a x2). Le champ etait lu et jete.
static float  s_mat_color[MAX_MATERIALS][3];
// SPECULAIRE (issue #45) : couleur + puissance, XBox/NX/material.cpp:629.
// Seuls les personnages s'en servent (WeightedMeshVS_VXC_Specular_*.vsh).
static float  s_mat_spec[MAX_MATERIALS][4];
static int    s_num_materials = 0;

// VERTEX COLOR WIBBLE (issue #45 V2). [SOURCE] XBox/NX/material.cpp:704 :
// passe 0 seulement, si MATFLAG_VC_WIBBLE -- num_seqs, puis par sequence
// num_keys, phase (int), num_keys sVCWibbleKeyframe (int temps + RGBA, 8
// octets). On le sautait. Rangement : un tableau d'entiers par scene, ou le
// materiau i commence en s_mat_vcw_debut[i] (-1 = pas d'animation) :
//     num_seqs, puis par sequence : phase, num_keys, num_keys x ( t, rgba ).
static int    s_mat_vcw_debut[MAX_MATERIALS];
static int   *sp_vcw_blob = NULL;
static int    s_vcw_len = 0, s_vcw_cap = 0;

static bool vcw_blob_pousser( int v )
{
	if( s_vcw_len == s_vcw_cap )
	{
		const int cap = s_vcw_cap ? s_vcw_cap * 2 : 1024;
		int *p = (int *)realloc( sp_vcw_blob, sizeof( int ) * cap );
		if( !p )
			return false;
		sp_vcw_blob = p;
		s_vcw_cap   = cap;
	}
	sp_vcw_blob[s_vcw_len++] = v;
	return true;
}

static int mat_vcw( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_vcw_debut[i];
	return -1;
}

// Bloc d'animation d'un maillage : les sommets qu'il REFERENCE et dont
// l'indice d'animation vaut 1..num_seqs. [SOURCE] XBox/NX/mesh.cpp:1469 ne
// garde de meme que les sommets du maillage ; un indice 0 = sommet fixe
// (mesh.cpp:384). Un indice au-dela de num_seqs lirait hors du tableau sur
// XBox (jamais vu dans les donnees, audit vita/tools) : on l'ignore.
static SVitaVcw *vcw_construire( int debut, const unsigned char *p_idx_sommets,
                                 int num_vertices, const unsigned short *p_indices,
                                 int num_indices, const unsigned char *p_colors )
{
	if(( debut < 0 ) || !p_idx_sommets || !p_indices )
		return NULL;
	const int *b = sp_vcw_blob + debut;
	const int num_seqs = b[0];
	if( num_seqs <= 0 )
		return NULL;
	int num_cles = 0;
	{
		const int *q = b + 1;
		for( int s = 0; s < num_seqs; ++s )
		{
			num_cles += q[1];
			q += 2 + 2 * q[1];
		}
	}
	unsigned char *p_vu = (unsigned char *)calloc( num_vertices, 1 );
	if( !p_vu )
		return NULL;
	int n = 0;
	for( int k = 0; k < num_indices; ++k )
	{
		const int v = p_indices[k];
		if(( v >= num_vertices ) || p_vu[v] )
			continue;
		p_vu[v] = 1;
		const int idx = p_idx_sommets[v];
		if(( idx >= 1 ) && ( idx <= num_seqs ))
			++n;
	}
	if( n == 0 )
	{
		free( p_vu );
		return NULL;
	}
	const size_t taille = sizeof( SVitaVcw ) + sizeof( SVitaVcwSeq ) * num_seqs
	                    + sizeof( SVitaVcwCle ) * num_cles + sizeof( unsigned short ) * n
	                    + n + 4 * n + 8;
	unsigned char *p_bloc = (unsigned char *)malloc( taille );
	if( !p_bloc )
	{
		free( p_vu );
		return NULL;
	}
	SVitaVcw *p = (SVitaVcw *)p_bloc;
	p->num_seqs    = num_seqs;
	p->num_cles    = num_cles;
	p->num_sommets = n;
	p->p_seqs      = (SVitaVcwSeq *)( p_bloc + sizeof( SVitaVcw ));
	p->p_cles      = (SVitaVcwCle *)( p->p_seqs + num_seqs );
	p->p_sommets   = (unsigned short *)( p->p_cles + num_cles );
	p->p_seq       = (unsigned char *)( p->p_sommets + n );
	p->p_orig      = p->p_seq + n;
	{
		const int *q = b + 1;
		int c = 0;
		for( int s = 0; s < num_seqs; ++s )
		{
			p->p_seqs[s].phase    = q[0];
			p->p_seqs[s].num_cles = q[1];
			p->p_seqs[s].premiere = c;
			for( int k = 0; k < q[1]; ++k, ++c )
			{
				p->p_cles[c].t = q[2 + 2 * k];
				memcpy( &p->p_cles[c].r, &q[3 + 2 * k], 4 );	// Image::RGBA : r, g, b, a
			}
			q += 2 + 2 * q[1];
		}
	}
	memset( p_vu, 0, num_vertices );
	int j = 0;
	for( int k = 0; k < num_indices; ++k )
	{
		const int v = p_indices[k];
		if(( v >= num_vertices ) || p_vu[v] )
			continue;
		p_vu[v] = 1;
		const int idx = p_idx_sommets[v];
		if(( idx < 1 ) || ( idx > num_seqs ))
			continue;
		p->p_sommets[j] = (unsigned short)v;
		p->p_seq[j]     = (unsigned char)( idx - 1 );
		if( p_colors )
			memcpy( &p->p_orig[4 * j], &p_colors[4 * v], 4 );
		else
			memset( &p->p_orig[4 * j], 255, 4 );
		++j;
	}
	free( p_vu );
	return p;
}

static uint32 nom_for_material( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_nom[i];
	return 0;
}

static uint32 texture_for_material( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_texture[i];
	}
	return 0;
}

// Rend la couleur de la passe 0, ou le neutre si le materiau est inconnu.
static void color_for_material( uint32 mat_checksum, float out[3] )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
		{
			out[0] = s_mat_color[i][0];
			out[1] = s_mat_color[i][1];
			out[2] = s_mat_color[i][2];
			return;
		}
	}
	out[0] = out[1] = out[2] = 0.5f;
}

// Speculaire de la passe 0 ; puissance 0 si le materiau est inconnu.
static void spec_for_material( uint32 mat_checksum, float out[4] )
{
	for( int i = 0; i < s_num_materials; ++i )
		if( s_mat_checksum[i] == mat_checksum )
		{
			memcpy( out, s_mat_spec[i], sizeof( float ) * 4 );
			return;
		}
	out[0] = out[1] = out[2] = out[3] = 0.0f;
}

static uint32 blend_for_material( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_blend[i];
	}
	return 0;
}

#define DEF_MAT_GETTER( nom, table, type, defaut )      \
	static type nom( uint32 mat_checksum )                 \
	{                                                      \
		for( int i = 0; i < s_num_materials; ++i )         \
			if( s_mat_checksum[i] == mat_checksum )        \
				return table[i];                           \
		return defaut;                                     \
	}

DEF_MAT_GETTER( mat_flags0,    s_mat_flags0,    uint32, 0 )
DEF_MAT_GETTER( mat_addr_u,    s_mat_au,        unsigned char, 0 )
DEF_MAT_GETTER( mat_addr_v,    s_mat_av,        unsigned char, 0 )
DEF_MAT_GETTER( mat_addr2_u,   s_mat_au2,       unsigned char, 0 )
DEF_MAT_GETTER( mat_addr2_v,   s_mat_av2,       unsigned char, 0 )
DEF_MAT_GETTER( mat_cutoff,    s_mat_cutoff,    uint32, 0 )
DEF_MAT_GETTER( mat_sorted,    s_mat_sorted,    uint32, 0 )
DEF_MAT_GETTER( mat_draworder, s_mat_draworder, float,  0.0f )
DEF_MAT_GETTER( mat_fixa,      s_mat_fixa,      unsigned char, 0 )
DEF_MAT_GETTER( mat_fixa2,     s_mat_fixa2,     unsigned char, 0 )

// Passes 2 et 3 d'un materiau (x = 0 ou 1). Rend false si le materiau est
// inconnu ; les champs sont alors laisses a leur valeur neutre.
static bool extra_pass_for_material( uint32 mat_checksum, int x, SVitaPasse *p_out )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
		{
			p_out->texture_checksum = s_mat_xtex[i][x];
			p_out->blend    = s_mat_xblend[i][x];
			p_out->flags    = s_mat_xflags[i][x];
			p_out->color[0] = s_mat_xcolor[i][x][0];
			p_out->color[1] = s_mat_xcolor[i][x][1];
			p_out->color[2] = s_mat_xcolor[i][x][2];
			p_out->fixed_alpha = s_mat_xfixa[i][x];
			p_out->addr_u   = s_mat_xau[i][x];
			p_out->addr_v   = s_mat_xav[i][x];
			p_out->env_tile[0] = s_mat_xenvtile[i][x][0];
			p_out->env_tile[1] = s_mat_xenvtile[i][x][1];
			return true;
		}
	}
	return false;
}

static void color2_for_material( uint32 mat_checksum, float out[3] )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
		{
			out[0] = s_mat_color2[i][0];
			out[1] = s_mat_color2[i][1];
			out[2] = s_mat_color2[i][2];
			return;
		}
	}
	out[0] = out[1] = out[2] = 0.5f;
}

static void envtile2_for_material( uint32 mat_checksum, float out[2], float out0[2] )
{
	out[0] = out[1] = out0[0] = out0[1] = 3.0f;		// defaut du moteur (material.cpp:70)
	for( int i = 0; i < s_num_materials; ++i )
		if( s_mat_checksum[i] == mat_checksum )
		{
			out[0] = s_mat_envtile2[i][0];
			out[1] = s_mat_envtile2[i][1];
			out0[0] = s_mat_envtile0[i][0];
			out0[1] = s_mat_envtile0[i][1];
			return;
		}
}

static uint32 layer2_texture( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_texture2[i];
	}
	return 0;
}

static uint32 layer2_blend( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_blend2[i];
	}
	return 0;
}

static uint32 layer2_flags( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_flags2[i];
	}
	return 0;
}

static unsigned char mat_nobfc( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_nobfc[i];
	return 1;		// inconnu : double face, comme avant
}

static unsigned char mat_zbias( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_zbias[i];
	return 0;
}

static void uvw_for_material( uint32 mat_checksum, unsigned char *p_masque, float (*p_par)[8] )
{
	*p_masque = 0;
	for( int i = 0; i < s_num_materials; ++i )
		if( s_mat_checksum[i] == mat_checksum )
		{
			*p_masque = s_mat_uvw[i];
			memcpy( p_par, s_mat_uvw_par[i], sizeof( s_mat_uvw_par[i] ));
			return;
		}
}

static uint32 passes_for_material( uint32 mat_checksum )
{
	for( int i = 0; i < s_num_materials; ++i )
	{
		if( s_mat_checksum[i] == mat_checksum )
			return s_mat_passes[i];
	}
	return 1;
}

static void skip_materials( void *p_file )
{
	s_num_materials = 0;
	s_vcw_len = 0;

	uint32 num_materials = rd_u32( p_file );
	VLOG( "SCN", "  materiaux : %u (offset %d)", (unsigned)num_materials,
	      File::Tell( p_file ));
	if( !check_count( "num_materials", (int)num_materials, 65536 ))
		return;

	for( uint32 i = 0; i < num_materials; ++i )
	{
		uint32 mat_checksum = rd_u32( p_file );
		// Checksum du NOM : celui que SetMaterialColor recoit du script (#58,
		// XBox/p_NxGeom.cpp:650 compare m_name_checksum, pas m_checksum).
		uint32 mat_nom = rd_u32( p_file );
		uint32 passes = rd_u32( p_file );
		// Trois champs qu'on sautait, et qui pilotent tout le rendu.
		uint32 alpha_cutoff = rd_u32( p_file );
		unsigned char sorted_flag = 0;
		rd( &sorted_flag, 1, 1, p_file );
		float draw_order = 0.0f;
		rd( &draw_order, sizeof( float ), 1, p_file );
		unsigned char single_sided_lu = 0;	// XBox/NX/material.cpp:675
		rd( &single_sided_lu, 1, 1, p_file );
		// no backface cull : [SOURCE] XBox/NX/material.cpp:306, RS_CULLMODE =
		// m_no_bfc ? D3DCULL_NONE : D3DCULL_CW. Issue #38.
		unsigned char no_bfc_lu = 0;
		rd( &no_bfc_lu, 1, 1, p_file );
		// z-bias : [SOURCE] XBox/NX/material.cpp:615, borne a 16, pose comme
		// D3DRS_ZBIAS (:308). C'est lui qui fait gagner les decalques (tags,
		// inscriptions, ombres plaquees) sur la surface ou ils sont colles.
		// On le sautait : z-fighting, decalques qui apparaissent/disparaissent.
		int32_t zbias_lu = 0;
		rd( &zbias_lu, 4, 1, p_file );

		unsigned char grassify = 0;
		rd( &grassify, 1, 1, p_file );
		if( grassify )
			skip( p_file, 4 + 4 );			// hauteur (float) + couches (int)

		float specular_power = 0.0f;
		rd( &specular_power, sizeof( float ), 1, p_file );
		float specular_col[3] = { 0.0f, 0.0f, 0.0f };
		if( specular_power > 0.0f )
			rd( specular_col, sizeof( float ), 3, p_file );	// couleur speculaire

		if( !check_count( "passes", (int)passes, 8 ))
			return;

		for( uint32 pass = 0; pass < passes; ++pass )
		{
			uint32 texture_checksum = rd_u32( p_file );
			uint32 flags            = rd_u32( p_file );

			// Seule la premiere passe nous interesse : c'est la couche de
			// base. Les suivantes sont des effets (reflets, detail) que le
			// pipeline fixe ne saurait pas composer de toute facon.
			skip( p_file, 1 );				// has_color (bool)
			// La couleur du materiau : trois flottants, neutre 0.5. On la
			// sautait, d'ou une teinte fausse partout ou elle ne l'est pas.
			float mat_color[3];
			rd( mat_color, sizeof( float ), 3, p_file );
			// Registre ALPHA : 24 bits de blend mode + 8 d'alpha fixe
			// (DX9/NX/render.cpp set_blend_mode). On le sautait en jetant la
			// valeur -- la meme erreur que pour le masque CAS.
			uint32 reg_alpha_lo = rd_u32( p_file );
			// [SOURCE] XBox/NX/material.cpp:665 : fixed_alpha = reg_alpha >> 32,
			// la MOITIE HAUTE (echelle 0-128). On prenait les bits 24-31 de la
			// moitie basse, toujours nuls : 1797 passes *_FIXED fausses.
			const uint32 reg_alpha_hi = rd_u32( p_file );
			const unsigned char fixa_lue =
			    (unsigned char)(( reg_alpha_hi > 255 ) ? 255 : reg_alpha_hi );
			// Toujours material.cpp:675 : un materiau translucide (passe 0
			// non DIFFUSE) est double face sauf s'il est marque simple face.
			if(( pass == 0 ) && !single_sided_lu && (( reg_alpha_lo & 0x00FFFFFF ) != 0 ))
				no_bfc_lu = 1;
			if(( pass == 0 ) && ( s_num_materials < MAX_MATERIALS ))
			{
				s_mat_checksum[s_num_materials] = mat_checksum;
				s_mat_nom[s_num_materials]      = mat_nom;
				s_mat_texture[s_num_materials]  = texture_checksum;
				s_mat_blend[s_num_materials]    = reg_alpha_lo & 0x00FFFFFF;
				s_mat_passes[s_num_materials]   = passes;
				s_mat_flags0[s_num_materials]    = flags;
				s_mat_cutoff[s_num_materials]    = alpha_cutoff;
				s_mat_nobfc[s_num_materials]     = no_bfc_lu ? 1 : 0;
				s_mat_zbias[s_num_materials]     = (unsigned char)(( zbias_lu < 0 ) ? 0 : ( zbias_lu > 16 ) ? 16 : zbias_lu );
				s_mat_vcw_debut[s_num_materials] = -1;
				s_mat_uvw[s_num_materials]       = 0;
				memset( s_mat_uvw_par[s_num_materials], 0, sizeof( s_mat_uvw_par[0] ));
				s_mat_sorted[s_num_materials]    = sorted_flag;
				s_mat_draworder[s_num_materials] = draw_order;
				s_mat_color[s_num_materials][0]  = mat_color[0];
				s_mat_color[s_num_materials][1]  = mat_color[1];
				s_mat_color[s_num_materials][2]  = mat_color[2];
				s_mat_spec[s_num_materials][0]   = specular_col[0];
				s_mat_spec[s_num_materials][1]   = specular_col[1];
				s_mat_spec[s_num_materials][2]   = specular_col[2];
				s_mat_spec[s_num_materials][3]   = ( specular_power > 0.0f ) ? specular_power : 0.0f;
				// Remises a zero : l'entree est indexee par la PASSE 0, la
				// couche 1 sera renseignee au tour suivant si elle existe.
				s_mat_texture2[s_num_materials] = 0;
				s_mat_blend2[s_num_materials]   = 0;
				s_mat_flags2[s_num_materials]   = 0;
				s_mat_fixa[s_num_materials]     = fixa_lue;
				s_mat_color2[s_num_materials][0] = 0.5f;
				s_mat_color2[s_num_materials][1] = 0.5f;
				s_mat_color2[s_num_materials][2] = 0.5f;
				s_mat_fixa2[s_num_materials]    = 0;
				for( int x = 0; x < 2; ++x )
				{
					s_mat_xtex[s_num_materials][x]   = 0;
					s_mat_xblend[s_num_materials][x] = 0;
					s_mat_xflags[s_num_materials][x] = 0;
					s_mat_xcolor[s_num_materials][x][0] = 0.5f;
					s_mat_xcolor[s_num_materials][x][1] = 0.5f;
					s_mat_xcolor[s_num_materials][x][2] = 0.5f;
					s_mat_xfixa[s_num_materials][x]  = 0;
					s_mat_xau[s_num_materials][x]    = 0;
					s_mat_xav[s_num_materials][x]    = 0;
					s_mat_xenvtile[s_num_materials][x][0] = 3.0f;	// material.cpp:70
					s_mat_xenvtile[s_num_materials][x][1] = 3.0f;
				}
				++s_num_materials;
			}
			// Couche 1 : la deuxieme unite de texture. On la range dans
			// l'entree creee par la passe 0, juste au-dessus.
			if(( pass == 1 ) && ( s_num_materials > 0 ))
			{
				s_mat_texture2[s_num_materials - 1] = texture_checksum;
				s_mat_blend2[s_num_materials - 1]   = reg_alpha_lo & 0x00FFFFFF;
				s_mat_flags2[s_num_materials - 1]   = flags;
				s_mat_color2[s_num_materials - 1][0] = mat_color[0];
				s_mat_color2[s_num_materials - 1][1] = mat_color[1];
				s_mat_color2[s_num_materials - 1][2] = mat_color[2];
				s_mat_fixa2[s_num_materials - 1]    = fixa_lue;
			}
			if((( pass == 2 ) || ( pass == 3 )) && ( s_num_materials > 0 ))
			{
				const int x = (int)pass - 2;
				s_mat_xtex[s_num_materials - 1][x]   = texture_checksum;
				s_mat_xblend[s_num_materials - 1][x] = reg_alpha_lo & 0x00FFFFFF;
				s_mat_xflags[s_num_materials - 1][x] = flags;
				s_mat_xcolor[s_num_materials - 1][x][0] = mat_color[0];
				s_mat_xcolor[s_num_materials - 1][x][1] = mat_color[1];
				s_mat_xcolor[s_num_materials - 1][x][2] = mat_color[2];
				s_mat_xfixa[s_num_materials - 1][x]  = fixa_lue;
			}
			// Adressage, un entier par axe. Etait saute.
			const uint32 addr_u = rd_u32( p_file );
			const uint32 addr_v = rd_u32( p_file );
			if((( pass == 2 ) || ( pass == 3 )) && ( s_num_materials > 0 ))
			{
				s_mat_xau[s_num_materials - 1][pass - 2] = (unsigned char)addr_u;
				s_mat_xav[s_num_materials - 1][pass - 2] = (unsigned char)addr_v;
			}
			if( s_num_materials > 0 )
			{
				if( pass == 0 )
				{
					s_mat_au[s_num_materials - 1] = (unsigned char)addr_u;
					s_mat_av[s_num_materials - 1] = (unsigned char)addr_v;
				}
				else if( pass == 1 )
				{
					s_mat_au2[s_num_materials - 1] = (unsigned char)addr_u;
					s_mat_av2[s_num_materials - 1] = (unsigned char)addr_v;
				}
			}
			// Tuilage envmap (issue #5) : etait saute. [SOURCE]
			// XBox/NX/material.cpp:415 -- echelle des coordonnees de reflet.
			{
				float tl[2];
				rd( tl, sizeof( float ), 2, p_file );
				if(( pass == 1 ) && ( s_num_materials > 0 ))
				{
					s_mat_envtile2[s_num_materials - 1][0] = tl[0];
					s_mat_envtile2[s_num_materials - 1][1] = tl[1];
				}
				if(( pass == 0 ) && ( s_num_materials > 0 ))
				{
					s_mat_envtile0[s_num_materials - 1][0] = tl[0];
					s_mat_envtile0[s_num_materials - 1][1] = tl[1];
				}
				if((( pass == 2 ) || ( pass == 3 )) && ( s_num_materials > 0 ))
				{
					s_mat_xenvtile[s_num_materials - 1][pass - 2][0] = tl[0];
					s_mat_xenvtile[s_num_materials - 1][pass - 2][1] = tl[1];
				}
			}
			skip( p_file, 4 );				// mode de filtrage

			// [SOURCE] XBox/NX/material.cpp:693-700 : 8 flottants bruts
			// (sUVWibbleParams sans sa matrice), passe par passe.
			if( flags & MATFLAG_UV_WIBBLE )
			{
				float par[8];
				rd( par, sizeof( float ), 8, p_file );
				if(( pass < 4 ) && ( s_num_materials > 0 ))
				{
					s_mat_uvw[s_num_materials - 1] |= (unsigned char)( 1u << pass );
					memcpy( s_mat_uvw_par[s_num_materials - 1][pass], par, sizeof( par ));
				}
			}

			if(( pass == 0 ) && ( flags & MATFLAG_VC_WIBBLE ))
			{
				uint32 num_seqs = rd_u32( p_file );
				if( !check_count( "num_seqs", (int)num_seqs, 4096 ))
					return;
				// Gardees (issue #45 V2) si le materiau a son entree ; au plus
				// 255 sequences (l'indice par sommet est un octet).
				const int ent = s_num_materials - 1;
				const int debut = s_vcw_len;
				bool garder = ( ent >= 0 ) && ( num_seqs <= 255 )
				              && vcw_blob_pousser( (int)num_seqs );
				for( uint32 seq = 0; seq < num_seqs; ++seq )
				{
					uint32 num_keys = rd_u32( p_file );
					const int phase = rd_int( p_file );
					if( !check_count( "num_keys", (int)num_keys, 65536 ))
						return;
					// sVCWibbleKeyframe = int + Image::RGBA = 8 octets.
					// Moins de deux cles : pas d'interpolation possible
					// (XBox asserte, material.cpp:186) -- materiau ignore.
					if( num_keys < 2 )
						garder = false;
					if( garder )
						garder = vcw_blob_pousser( phase ) && vcw_blob_pousser( (int)num_keys );
					for( uint32 k = 0; k < num_keys; ++k )
					{
						int cle[2];
						rd( cle, 4, 2, p_file );
						if( garder )
							garder = vcw_blob_pousser( cle[0] ) && vcw_blob_pousser( cle[1] );
					}
				}
				if( garder && !s_read_error )
					s_mat_vcw_debut[ent] = debut;
				else
					s_vcw_len = debut;
			}

			if( flags & MATFLAG_PASS_TEXTURE_ANIMATES )
			{
				int num_keyframes = rd_int( p_file );
				skip( p_file, 4 * 3 );		// periode, iterations, phase
				if( !check_count( "num_keyframes", num_keyframes, 65536 ))
					return;
				skip( p_file, 8 * num_keyframes );	// temps + checksum de texture
			}

			// Infos de mipmap : 4 uint32 dans les deux cas. Le code DX9 les
			// lit si la passe est texturee, et fait un Seek de 16 sinon --
			// meme decalage.
			skip( p_file, 16 );
		}
	}
}


// --- accumulation des maillages --------------------------------------------

static void mesh_list_push( SVitaSceneGeom *p_geom, const SVitaMesh *p_mesh )
{
	if( p_geom->num_meshes == p_geom->capacity )
	{
		int new_cap = p_geom->capacity ? ( p_geom->capacity * 2 ) : 64;
		SVitaMesh *p_new = (SVitaMesh *)realloc( p_geom->p_meshes,
		                                         new_cap * sizeof( SVitaMesh ));
		if( !p_new )
		{
			VLOG( "SCN", "!! plus de memoire pour la liste de maillages (%d)",
			      new_cap );
			s_read_error = true;
			return;
		}
		p_geom->p_meshes = p_new;
		p_geom->capacity = new_cap;
	}
	p_geom->p_meshes[p_geom->num_meshes++] = *p_mesh;
}


// --- billboards (issue #2) ---------------------------------------------------
//
// [SOURCE] XBox/NX/mesh.cpp:887 sMesh::SetBillboardData. Le quad est exporte
// dans une orientation quelconque ; on l'exprime dans SON repere (u, v, n),
// n etant la normale du premier triangle, puis le rendu le reconstruit chaque
// image dans le repere de la camera (billboard.cpp, BillboardScreenAlignedVS).
// Les indices du fichier designent les memes sommets que ceux du tampon de
// maillage XBox : la normale est la meme.
static void bb_norm( float *v )
{
	const float l = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
	if( l > 0.0f )
	{
		v[0] /= l; v[1] /= l; v[2] /= l;
	}
}
static void bb_cross( float *o, const float *a, const float *b )
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

static SVitaBillboard *bb_construire( uint32 type_fichier, const float *pivot,
                                      const float *axe, const float *p_pos,
                                      int num_vertices, const unsigned short *p_idx,
                                      int num_indices )
{
	// XBox l'impose par assertion (mesh.cpp:925) : 4 sommets, 4 indices.
	if(( num_vertices != 4 ) || ( num_indices < 3 ) || !p_idx )
	{
		VLOG( "SCN", "billboard ignore : %d sommets, %d indices", num_vertices, num_indices );
		return NULL;
	}
	for( int k = 0; k < 3; ++k )
		if( p_idx[k] >= 4 )
			return NULL;

	int type;
	if( type_fichier == 1 )
		type = 0;										// face a l'ecran
	else if( type_fichier == 2 )
		type = ( fabsf( axe[1] ) > 0.99f ) ? 1 : 2;	// axe Y, ou axe quelconque
	else
	{
		VLOG( "SCN", "billboard de type %u inconnu, laisse fige", (unsigned)type_fichier );
		return NULL;
	}

	SVitaBillboard *b = (SVitaBillboard *)malloc( sizeof( SVitaBillboard ));
	if( !b )
		return NULL;
	b->type = type;
	for( int k = 0; k < 3; ++k )
	{
		b->pivot[k] = pivot[k];
		b->axe[k]   = ( type == 2 ) ? axe[k] : 0.0f;
	}

	const float *v0 = &p_pos[3 * p_idx[0]];
	const float *v1 = &p_pos[3 * p_idx[1]];
	const float *v2 = &p_pos[3 * p_idx[2]];
	float e1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
	float e2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };
	float n[3], u[3], v[3];
	bb_cross( n, e1, e2 );
	bb_norm( n );
	static const float haut[3] = { 0.0f, 1.0f, 0.0f };
	bb_cross( u, n, ( type == 2 ) ? axe : haut );
	bb_norm( u );
	bb_cross( v, u, n );
	bb_norm( v );

	b->rayon = 0.0f;
	for( int i = 0; i < 4; ++i )
	{
		const float r[3] = { p_pos[3 * i + 0] - pivot[0],
		                     p_pos[3 * i + 1] - pivot[1],
		                     p_pos[3 * i + 2] - pivot[2] };
		b->rel[i][0] = r[0] * u[0] + r[1] * u[1] + r[2] * u[2];
		b->rel[i][1] = r[0] * v[0] + r[1] * v[1] + r[2] * v[2];
		b->rel[i][2] = r[0] * n[0] + r[1] * n[1] + r[2] * n[2];
		for( int c = 0; c < 3; ++c )
			b->orig[i][c] = p_pos[3 * i + c];
		const float d = sqrtf( r[0] * r[0] + r[1] * r[1] + r[2] * r[2] );
		if( d > b->rayon )
			b->rayon = d;
	}
	return b;
}


// --- un secteur ------------------------------------------------------------

static void load_sector( void *p_file, SVitaSceneGeom *p_geom )
{
	const uint32 sector_checksum = rd_u32( p_file );		// cle des scripts
	const int sector_bone = rd_int( p_file );	// index d'os du SECTEUR
	uint32 sector_flags = rd_u32( p_file );
	int    num_meshes   = (int)rd_u32( p_file );
	float sector_bb[6];						// boite englobante (#65)
	rd( sector_bb, sizeof( float ), 6, p_file );
	skip( p_file, 4 * 4 );					// sphere englobante

	// Billboard (issue #2) : type, origine, pivot, axe. [SOURCE]
	// XBox/p_nxsector.cpp:321 -- le Z du pivot est stocke inverse (celui de
	// l'origine aussi, inutilisee) ; l'axe est lu tel quel.
	uint32 bb_type = 0;
	float  bb_pivot[3] = { 0.0f, 0.0f, 0.0f };
	float  bb_axe[3]   = { 0.0f, 1.0f, 0.0f };
	const bool billboard = ( sector_flags & SECTOR_FLAG_BILLBOARD ) != 0;
	if( billboard )
	{
		bb_type = rd_u32( p_file );
		skip( p_file, 4 * 3 );				// origine
		rd( bb_pivot, sizeof( float ), 3, p_file );
		rd( bb_axe, sizeof( float ), 3, p_file );
		bb_pivot[2] = -bb_pivot[2];
	}

	int num_vertices       = rd_int( p_file );
	int vertex_data_stride = rd_int( p_file );
	(void)vertex_data_stride;

	if( !check_count( "num_vertices", num_vertices, 1 << 20 ))
		return;
	if( !check_count( "num_meshes", num_meshes, 1 << 16 ))
		return;

	// Positions : le seul flux qu'on garde.
	float *p_positions = (float *)malloc( sizeof( float ) * 3 * num_vertices );
	if( !p_positions )
	{
		VLOG( "SCN", "!! plus de memoire pour %d sommets", num_vertices );
		s_read_error = true;
		return;
	}
	rd( p_positions, sizeof( float ) * 3, num_vertices, p_file );

	// Les autres flux sont seulement traverses -- mais il FAUT les traverser :
	// une lecture omise decale tout ce qui suit.
	// Les normales servent a l'eclairage diffus : couleur = ambiante +
	// somme( diffuse_i * max( 0, N . dir_i )), la formule de
	// DX9/p_NxLight.cpp:64. Sans elles, tout est rendu a pleine luminosite et
	// le jeu parait plat. On les garde donc au lieu de les traverser.
	// On ne les garde que pour les maillages SKINNES.
	//
	// Les charger partout ajoutait 12 octets par sommet sur tout le decor -- et
	// un niveau, ce n'est pas un personnage : NJ compte 1350 secteurs et 4431
	// maillages. La lecture s'interrompait faute de memoire et le niveau
	// arrivait vide (« 0 maillages [LECTURE INTERROMPUE] »), alors que le ciel,
	// minuscule, passait.
	//
	// Le decor n'en a de toute facon aucun usage : son eclairage est CUIT dans
	// ses couleurs de sommets. Seuls les personnages passent par l'eclairage
	// materiel, et eux seuls portent des poids d'os.
	float *p_normals = NULL;
	if( sector_flags & SECTOR_FLAG_HAS_NORMALS )
	{
		// Lues dans tous les cas, mais GARDEES seulement si un maillage du
		// secteur en a l'usage (os, ou couche envmap -- issue #5) : sinon
		// liberees a la fin du secteur. Le pic memoire reste celui d'un seul
		// secteur, pas du niveau entier (la cause de l'echec documente plus
		// haut).
		p_normals = (float *)malloc( sizeof( float ) * 3 * num_vertices );

		if( p_normals )
			rd( p_normals, sizeof( float ) * 3, num_vertices, p_file );
		else
			skip( p_file, 4 * 3 * num_vertices );
	}

	// Maillage skinne : poids et indices d'os. On les GARDE -- sans eux, les
	// sommets d'un personnage restent exprimes par rapport aux os et la
	// geometrie s'effondre.
	unsigned int   *p_weights = NULL;
	unsigned short *p_bones   = NULL;
	if( sector_flags & SECTOR_FLAG_HAS_WEIGHTS )
	{
		p_weights = (unsigned int *)malloc( sizeof( unsigned int ) * num_vertices );
		if( p_weights )
			rd( p_weights, sizeof( unsigned int ), num_vertices, p_file );
		else
			skip( p_file, 4 * num_vertices );

		p_bones = (unsigned short *)malloc( sizeof( unsigned short ) * 4 * num_vertices );
		if( p_bones )
			rd( p_bones, sizeof( unsigned short ) * 4, num_vertices, p_file );
		else
			skip( p_file, 2 * 4 * num_vertices );
	}

	// Coordonnees de texture. On ne garde que le PREMIER jeu : les suivants
	// servent aux passes d'effets, que le pipeline fixe ne compose pas.
	float *p_uvs  = NULL;
	float *p_uvs2 = NULL;		// deuxieme jeu, pour la deuxieme unite
	float *p_uvs_x[2] = { NULL, NULL };	// jeux 3 et 4 (passes 2 et 3)
	if( sector_flags & SECTOR_FLAG_HAS_TEXCOORDS )
	{
		int num_tc_sets = rd_int( p_file );
		if( !check_count( "num_tc_sets", num_tc_sets, 8 ))
		{
			free( p_positions );
			return;
		}
		if( num_tc_sets > 0 )
		{
			// Combien de jeux d'UV ce maillage porte-t-il ?
			//
			// On prend TOUJOURS le premier (indices 0 et 1). Le decor s'affiche
			// correctement ainsi, mais l'humain rapporte que les personnages
			// ont visage et vetements � comme inverses �. Si les modeles de
			// personnages portent plusieurs jeux et que le bon n'est pas le
			// premier, cela expliquerait qu'eux seuls soient touches.
			//
			// On ne journalise QUE les maillages a plusieurs jeux, avec le nom
			// du modele : ce sont les seuls candidats pour le visage encore
			// faux, et le decor (1 jeu partout) n'a plus a noyer la trace.
			if( num_tc_sets > 1 )
			{
				static int s_n = 0;
				if( s_n < 40 )
				{
					++s_n;
					VLOG( "UV", "'%s' : %d jeux, %d sommets",
					      sp_current_file, num_tc_sets, num_vertices );
				}
			}
			int stride = 2 * num_tc_sets;
			float *p_all = (float *)malloc( sizeof( float ) * stride * num_vertices );
			if( p_all )
			{
				rd( p_all, sizeof( float ) * stride, num_vertices, p_file );
				p_uvs = (float *)malloc( sizeof( float ) * 2 * num_vertices );
				if( p_uvs )
				{
					for( int v = 0; v < num_vertices; ++v )
					{
						p_uvs[v * 2 + 0] = p_all[v * stride + 0];
						// V NON inverse -- test.
						//
						// L'inversion etait reprise du chemin des sprites 2D
						// (ligne 0 en haut cote D3D, en bas cote OpenGL). Elle
						// paraissait juste parce que le decor semblait correct
						// -- mais une texture de mur en briques inversee
						// verticalement ne se remarque pas, alors qu'un VISAGE
						// inverse saute aux yeux. L'humain rapporte justement
						// visages et vetements � comme inverses � sur tous les
						// personnages.
						//
						// Les textures de modeles sont peut-etre deja dans le
						// bon sens, contrairement aux sprites : le journal note
						// deja que l'orientation se decide FORMAT PAR FORMAT
						// (.img de bas en haut, .fnt de haut en bas), sans regle
						// globale. On tranche par l'image.
						p_uvs[v * 2 + 1] = p_all[v * stride + 1];
					}
				}

				// Deuxieme jeu, pour la deuxieme couche de texture. Il etait
				// lu puis jete : la deuxieme unite n'avait rien a
				// echantillonner, donc 80 % du decor sortait avec une seule
				// de ses deux couches.
				//
				// Cout mesure sur table : 61 946 sommets concernes dans New
				// Jersey, soit 0,47 Mo. A comparer aux 12 octets par sommet
				// des normales, qui avaient fait echouer la lecture du niveau
				// -- ici on est cinq fois moins cher, et seulement sur les
				// secteurs qui portent vraiment plusieurs jeux.
				if( num_tc_sets >= 2 )
				{
					p_uvs2 = (float *)malloc( sizeof( float ) * 2 * num_vertices );
					if( p_uvs2 )
					{
						for( int v = 0; v < num_vertices; ++v )
						{
							p_uvs2[v * 2 + 0] = p_all[v * stride + 2];
							p_uvs2[v * 2 + 1] = p_all[v * stride + 3];
						}
					}
				}
				// Jeux 3 et 4 : ceux des passes 2 et 3 (option B, etape 3).
				for( int x = 0; ( x < 2 ) && ( num_tc_sets >= 3 + x ); ++x )
				{
					p_uvs_x[x] = (float *)malloc( sizeof( float ) * 2 * num_vertices );
					if( p_uvs_x[x] )
					{
						for( int v = 0; v < num_vertices; ++v )
						{
							p_uvs_x[x][v * 2 + 0] = p_all[v * stride + 4 + 2 * x];
							p_uvs_x[x][v * 2 + 1] = p_all[v * stride + 5 + 2 * x];
						}
					}
				}
				free( p_all );
			}
			else
			{
				skip( p_file, 4 * stride * num_vertices );
			}
		}
	}

	// Couleurs de sommets : l'eclairage cuit du decor, et l'alpha des voiles.
	// Fichier : BGRA (D3DCOLOR), echelle 0..128 (128 = plein, heritage PS2,
	// mesuree sur les fichiers). On convertit une fois ici en RGBA 0..255
	// pour les donner telles quelles a glColorPointer.
	unsigned char *p_colors = NULL;
	if( sector_flags & SECTOR_FLAG_HAS_COLORS )
	{
		p_colors = (unsigned char *)malloc( 4 * num_vertices );
		if( p_colors )
		{
			rd( p_colors, 4, num_vertices, p_file );
			for( int v = 0; v < num_vertices; ++v )
			{
				unsigned char *c = &p_colors[v * 4];
				const unsigned char b = c[0];
				c[0] = c[2];			// B <-> R
				c[2] = b;
				for( int k = 0; k < 4; ++k )
				{
					int e = c[k] * 2;	// 0..128 -> 0..255 (129+ clampe)
					c[k] = ( e > 255 ) ? 255 : (unsigned char)e;
				}
			}
		}
		else
			skip( p_file, 4 * num_vertices );
	}

	// Indice d'animation de couleur par sommet (issue #45 V2) : 0 = fixe,
	// N = sequence N du materiau. [SOURCE] XBox/p_nxsector.cpp:143.
	unsigned char *p_vcw_idx = NULL;
	if( sector_flags & SECTOR_FLAG_HAS_VC_WIBBLE )
	{
		p_vcw_idx = (unsigned char *)malloc( num_vertices );
		if( p_vcw_idx )
			rd( p_vcw_idx, 1, num_vertices, p_file );
		else
			skip( p_file, 1 * num_vertices );
	}

	// Les maillages du secteur partagent tous ce meme tampon de sommets ; ils
	// n'en indexent que des morceaux. On duplique donc les positions dans
	// chaque maillage plutot que de gerer un comptage de references : c'est
	// plus lourd en memoire, mais lisible, et on mesurera avant d'optimiser.
	// Le premier maillage retenu du secteur possede les tampons de sommets.
	bool first_of_sector = true;
	bool normales_utiles = ( p_weights != NULL ) || g_vita_garder_normales;	// #5 ; modeles rigides eclaires
	const int premier_maillage = p_geom->num_meshes;

	for( int m = 0; m < num_meshes; ++m )
	{
		if( s_read_error )
			break;

		skip( p_file, 4 * 3 );				// centre de la sphere
		skip( p_file, 4 );					// rayon
		skip( p_file, 4 * 3 );				// coin inferieur
		skip( p_file, 4 * 3 );				// coin superieur
		// Drapeaux du maillage. Seul le bit 0x400 sert : pas d'ombre du
		// skater sur ce maillage. [SOURCE] XBox/p_nxsector.cpp:172 ->
		// MESH_FLAG_NO_SKATER_SHADOW, teste par render_shadow_meshes
		// (XBox/NX/render.cpp:2344). Issue #45 V2 / #4.
		const uint32 drapeaux_maillage = rd_u32( p_file );
		uint32 material_checksum = rd_u32( p_file );

		uint32 num_lod_levels = rd_u32( p_file );
		if( !check_count( "num_lod_levels", (int)num_lod_levels, 8 ))
			break;

		unsigned short *p_indices  = NULL;
		int             num_indices = 0;

		for( uint32 lod = 0; lod < num_lod_levels; ++lod )
		{
			int n = rd_int( p_file );
			if( !check_count( "num_indices", n, 1 << 20 ))
				break;

			// On ne garde que le LOD 0, le plus detaille.
			if(( lod == 0 ) && ( n > 0 ))
			{
				p_indices = (unsigned short *)malloc( sizeof( unsigned short ) * n );
				if( !p_indices )
				{
					VLOG( "SCN", "!! plus de memoire pour %d indices", n );
					s_read_error = true;
					break;
				}
				rd( p_indices, sizeof( unsigned short ), n, p_file );
				num_indices = n;
			}
			else
			{
				skip( p_file, 2 * n );
			}
		}

		if( s_read_error )
		{
			free( p_indices );
			break;
		}

		if( p_indices && ( num_indices >= 3 ))
		{
			// PARTAGE, pas copie. Tous les maillages du secteur pointent sur
			// les memes sommets ; seul le premier en est proprietaire et les
			// liberera. Auparavant chaque maillage avait sa copie, et le
			// skinning retransformait 18 fois les memes sommets par frame.
			SVitaMesh mesh;
			mesh.sector_checksum  = sector_checksum;
			for( int k = 0; k < 6; ++k )
				mesh.sector_bb[k] = sector_bb[k];
			mesh.mat_checksum     = material_checksum;
			mesh.mat_nom          = nom_for_material( material_checksum );
			mesh.sector_bone      = sector_bone;
			mesh.num_vertices     = num_vertices;
			mesh.num_indices      = num_indices;
			mesh.p_indices        = p_indices;
			mesh.p_positions      = p_positions;
			mesh.p_normals        = p_normals;
			mesh.p_uvs            = p_uvs;
			mesh.p_colors         = p_colors;
		// Index DANS LE SECTEUR, pas dans la scene entiere : c'est ce que les
		// tables CAS designent (DX9/p_nxsector.cpp:222).
		mesh.load_order       = m;
			mesh.p_weights        = p_weights;
			mesh.p_bones          = p_bones;
			mesh.owns_vertices    = ( first_of_sector ? 1 : 0 );
			mesh.texture_checksum = texture_for_material( material_checksum );
			mesh.blend_mode       = blend_for_material( material_checksum );
			mesh.num_passes       = passes_for_material( material_checksum );
			mesh.mat_flags0       = mat_flags0( material_checksum );
			mesh.addr_u           = mat_addr_u( material_checksum );
			mesh.addr_v           = mat_addr_v( material_checksum );
			mesh.addr2_u          = mat_addr2_u( material_checksum );
			mesh.addr2_v          = mat_addr2_v( material_checksum );
			mesh.alpha_cutoff     = mat_cutoff( material_checksum );
			mesh.zbias            = mat_zbias( material_checksum );
			uvw_for_material( material_checksum, &mesh.uvw, mesh.uvw_par );
			mesh.no_bfc           = mat_nobfc( material_checksum );
			mesh.no_ombre         = ( drapeaux_maillage & 0x400 ) ? 1 : 0;
			mesh.unlit            = ( drapeaux_maillage & 0x20000 ) ? 1 : 0;
			// [SOURCE] XBox/p_nxsector.cpp:220 : ( flags & 0x800 ) du MAILLAGE.
			mesh.p_vcw = ( drapeaux_maillage & 0x800 )
			             ? vcw_construire( mat_vcw( material_checksum ), p_vcw_idx,
			                               num_vertices, p_indices, num_indices, p_colors )
			             : NULL;
			mesh.p_bb = billboard
			            ? bb_construire( bb_type, bb_pivot, bb_axe, p_positions,
			                             num_vertices, p_indices, num_indices )
			            : NULL;
			mesh.mat_sorted       = mat_sorted( material_checksum );
			mesh.draw_order       = mat_draworder( material_checksum );
			color_for_material( material_checksum, mesh.mat_color );
			spec_for_material( material_checksum, mesh.spec );
			mesh.mat_flags2       = layer2_flags( material_checksum );
			color2_for_material( material_checksum, mesh.mat_color2 );
			mesh.fixed_alpha0     = mat_fixa( material_checksum );
			mesh.fixed_alpha2     = mat_fixa2( material_checksum );
			mesh.blend_mode2      = layer2_blend( material_checksum );
			// Une couche en environment mapping n'a pas de jeu d'UV propre :
			// ses coordonnees sont generees (mesh.cpp:1218). Lui donner le
			// jeu 1 plaquerait une texture au hasard. On la laisse de cote --
			// 47 materiaux sur 495 dans New Jersey.
			mesh.texture_checksum2 = ( mesh.mat_flags2 & MATFLAG_ENVIRONMENT )
			                         ? 0
			                         : layer2_texture( material_checksum );

			// Le pointeur est TOUJOURS recopie, meme quand la couche est
			// inutilisable : il porte la PROPRIETE du tampon, partage par tout
			// le secteur. Le mettre a NULL sur un maillage envmap ferait fuir
			// le tampon des lors que c'est lui qui le possede. C'est
			// texture_checksum2 == 0 qui dit au rendu de ne pas s'en servir.
			mesh.p_uvs2            = p_uvs2;
			// Couche 1 envmap (issue #5).
			mesh.texture_env2 = ( mesh.mat_flags2 & MATFLAG_ENVIRONMENT )
			                    ? layer2_texture( material_checksum ) : 0;
			envtile2_for_material( material_checksum, mesh.env_tiling2, mesh.env_tiling0 );
			mesh.env0 = ( mesh.mat_flags0 & MATFLAG_ENVIRONMENT ) ? 1 : 0;
			// [SOURCE] XBox/NX/mesh.cpp:1418-1434 : les jeux 0..N-1 sont
			// recopies UN POUR UN et l'etage p lit le jeu p
			// (D3DTSS_TCI_PASSTHRU | p, material.cpp:425). Une passe 0 en
			// reflet ignore son jeu 0 (nul dans les donnees), elle ne le cede
			// PAS a la passe 1 : celle-ci lit le jeu 1. Lui donner le jeu 0
			// echantillonnait le coin (0,0) de sa texture -- fenetres noires,
			// issue #41. Jeu 0 en dernier recours si le secteur n'en a qu'un.
			mesh.p_uvs_couche1 = p_uvs2 ? p_uvs2 : ( mesh.env0 ? p_uvs : NULL );
			if(( mesh.texture_env2 || mesh.env0 ) && p_normals )
				normales_utiles = true;
			mesh.p_uvs_x[0]        = p_uvs_x[0];
			mesh.p_uvs_x[1]        = p_uvs_x[1];

			// Passes 2 et 3, et le jeu d'UV de chaque passe. [SOURCE]
			// XBox/NX/mesh.cpp:1418 : la passe N lit le jeu N ; une passe en
			// environment mapping genere ses coordonnees et ignore le sien.
			{
				const float *sets[4] = { p_uvs, p_uvs2, p_uvs_x[0], p_uvs_x[1] };
				uint32 pflags[4] = { mesh.mat_flags0, mesh.mat_flags2, 0, 0 };
				for( int x = 0; x < 2; ++x )
				{
					SVitaPasse *px = &mesh.passe_x[x];
					px->texture_checksum = 0; px->blend = 0; px->flags = 0;
					px->color[0] = px->color[1] = px->color[2] = 0.5f;
					px->fixed_alpha = 0; px->addr_u = px->addr_v = 0;
					px->p_uvs = NULL;
					px->env_tile[0] = px->env_tile[1] = 3.0f;
					if(( mesh.num_passes > (unsigned int)( 2 + x ))
					    && extra_pass_for_material( material_checksum, x, px ))
						pflags[2 + x] = px->flags;
				}
				// Passe p -> jeu p, reflet ou non (meme regle que la couche 1).
				for( int p = 2; ( p < 4 ) && ( p < (int)mesh.num_passes ); ++p )
					if( !( pflags[p] & MATFLAG_ENVIRONMENT ))
						mesh.passe_x[p - 2].p_uvs = (float *)sets[p];
				// Une passe 2-3 en environment mapping garde sa texture, sans
				// jeu d'UV : le rendu la dessine en reflet (issue #72, � epx �),
				// d'ou les normales conservees. Les autres passes sans jeu
				// d'UV sont ecartees.
				for( int x = 0; x < 2; ++x )
				{
					if( mesh.passe_x[x].flags & MATFLAG_ENVIRONMENT )
					{
						if( mesh.passe_x[x].texture_checksum && p_normals )
							normales_utiles = true;
					}
					else if( !mesh.passe_x[x].p_uvs )
						mesh.passe_x[x].texture_checksum = 0;
				}
			}
			first_of_sector       = false;
			mesh_list_push( p_geom, &mesh );
		}
		else
		{
			free( p_indices );
		}
	}

	free( p_vcw_idx );

	// Normales sans usage dans ce secteur : liberees maintenant.
	if( p_normals && !normales_utiles )
	{
		for( int i = premier_maillage; i < p_geom->num_meshes; ++i )
			if( p_geom->p_meshes[i].p_normals == p_normals )
				p_geom->p_meshes[i].p_normals = NULL;
		free( p_normals );
		p_normals = NULL;
	}

	// Ne PAS liberer ici : les tampons sont desormais detenus par le premier
	// maillage du secteur. Si aucun maillage n'a ete retenu, personne ne les
	// possede -- on les libere alors.
	if( first_of_sector )
	{
		free( p_colors );
		free( p_bones );
		free( p_weights );
		free( p_uvs );
		free( p_uvs2 );
		free( p_uvs_x[0] );
		free( p_uvs_x[1] );
		free( p_normals );
		free( p_positions );
	}
}


// --- point d'entree --------------------------------------------------------

static bool load_scene_body( void *p_file, SVitaSceneGeom *p_geom )
{
	uint32 mat_version  = rd_u32( p_file );
	uint32 mesh_version = rd_u32( p_file );
	uint32 vert_version = rd_u32( p_file );
	(void)mat_version; (void)mesh_version; (void)vert_version;

	skip_materials( p_file );

	int num_sectors = rd_int( p_file );
	if( check_count( "num_sectors", num_sectors, 1 << 16 ))
	{
		for( int s = 0; s < num_sectors; ++s )
		{
			if( s_read_error )
				break;
			load_sector( p_file, p_geom );
		}
	}

	// HIERARCHIE, juste apres les secteurs, en FIN de fichier.
	//
	// [SOURCE] XBox/p_nx.cpp:583, « Read hierarchy information » : un entier
	// donnant le nombre d'objets, puis autant de CHierarchyObject bruts de
	// 80 octets, recopies tels quels — le moteur les relit avec ses propres
	// accesseurs.
	//
	// Sans cette lecture, GetHierarchy() rendait NULL,
	// CalculateCarHierarchyMatrices sortait sans rien poser, et les six
	// parties d'un vehicule se dessinaient empilees a l'origine du modele.
	if( !s_read_error )
	{
		const int n = rd_int( p_file );
		if(( n > 0 ) && ( n < 4096 ))
		{
			p_geom->p_hierarchy = malloc( (size_t)n * 80u );
			if( p_geom->p_hierarchy )
			{
				rd( p_geom->p_hierarchy, 80, n, p_file );
				if( s_read_error )
				{
					free( p_geom->p_hierarchy );
					p_geom->p_hierarchy = NULL;
				}
				else
				{
					p_geom->num_hierarchy = n;
					VLOG( "SCN", "hierarchie : %d objets", n );
				}
			}
		}

		// Pas d'objets, compte invraisemblable, ou fin de fichier atteinte :
		// ce modele n'est simplement pas hierarchique, et c'est le cas de la
		// grande majorite (tout le decor, veh_trolley...). La geometrie lue
		// avant reste valide — on ne fait pas echouer la scene pour cela.
		s_read_error = false;
	}

	return !s_read_error;
}


bool LoadSceneGeometry( const char *p_name, SVitaSceneGeom *p_geom )
{
	sp_current_file = p_name ? p_name : "?";
	memset( p_geom, 0, sizeof( SVitaSceneGeom ));
	s_read_error = false;
	s_mem        = NULL;

	void *p_file = File::Open( p_name, "rb" );
	if( !p_file )
	{
		VLOG( "SCN", "ouverture impossible : '%s'", p_name );
		return false;
	}

	load_scene_body( p_file, p_geom );

	File::Close( p_file );

	int total_indices = 0;
	int textured      = 0;
	// Couverture des couches de texture. Le pipeline fixe de vitaGL n'en
	// rendait que DEUX ; le shader materiau genere (option B, #13) les compose
	// toutes, jusqu'a quatre (issue #6).
	int couche2       = 0;		// maillages a deux couches ou plus
	int tronques      = 0;		// 3 ou 4 couches : par le shader materiau
	int envmap        = 0;		// couche 1 en environment mapping, sans UV
	for( int i = 0; i < p_geom->num_meshes; ++i )
	{
		const SVitaMesh *p_m = &p_geom->p_meshes[i];
		total_indices += p_m->num_indices;
		if( p_m->p_uvs && p_m->texture_checksum )
			++textured;
		if( p_m->texture_checksum2 )
			++couche2;
		if( p_m->num_passes > 2 )
			++tronques;
		if( p_m->mat_flags2 & MATFLAG_ENVIRONMENT )
			++envmap;
	}

	VLOG( "SCN", "'%s' : %d maillages (%d textures), %d indices, "
	             "%d materiaux%s",
	      p_name, p_geom->num_meshes, textured, total_indices,
	      s_num_materials, s_read_error ? "  [LECTURE INTERROMPUE]" : "" );

	// Ce qui n'est PAS rendu doit se voir dans le journal. Une troncature
	// silencieuse se lit, quelques semaines plus tard, comme une couverture
	// complete -- et on cherche alors le defaut partout ailleurs.
	if( couche2 || tronques || envmap )
	{
		VLOG( "SCN", "  couches de texture : %d maillages a 2 couches ou plus, "
		             "dont %d a 3-4 couches (shader materiau), %d envmap non geres (#5)",
		      couche2, tronques, envmap );
	}

	return ( !s_read_error ) && ( p_geom->num_meshes > 0 );
}


bool LoadSceneGeometryFromMemory( const void *p_data, int size,
                                  const char *p_label, SVitaSceneGeom *p_geom )
{
	sp_current_file = p_label ? p_label : "<memoire>";
	memset( p_geom, 0, sizeof( SVitaSceneGeom ));
	s_read_error = false;

	s_mem      = (const unsigned char *)p_data;
	s_mem_off  = 0;
	s_mem_size = (size_t)size;

	load_scene_body( NULL, p_geom );

	s_mem = NULL;

	VLOG( "SCN", "'%s' (memoire, %d octets) : %d maillages%s",
	      sp_current_file, size, p_geom->num_meshes,
	      s_read_error ? "  [LECTURE INTERROMPUE]" : "" );

	return ( !s_read_error ) && ( p_geom->num_meshes > 0 );
}


void FreeSceneGeometry( SVitaSceneGeom *p_geom )
{
	for( int i = 0; i < p_geom->num_meshes; ++i )
	{
		if( p_geom->p_meshes[i].owns_vertices )
		{
			free( p_geom->p_meshes[i].p_positions );
			free( p_geom->p_meshes[i].p_normals );
			free( p_geom->p_meshes[i].p_uvs );
			free( p_geom->p_meshes[i].p_uvs2 );
			free( p_geom->p_meshes[i].p_uvs_x[0] );
			free( p_geom->p_meshes[i].p_uvs_x[1] );
			free( p_geom->p_meshes[i].p_colors );
			free( p_geom->p_meshes[i].p_weights );
			free( p_geom->p_meshes[i].p_bones );
		}
		free( p_geom->p_meshes[i].p_indices );
		free( p_geom->p_meshes[i].p_vcw );
		free( p_geom->p_meshes[i].p_bb );
	}
	free( p_geom->p_meshes );
	memset( p_geom, 0, sizeof( SVitaSceneGeom ));
}

} // namespace NxVita
