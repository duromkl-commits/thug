/*****************************************************************************
**  THUG-Vita â€” backend graphique                                            **
**  Code/Gfx/Vita/p_NxTexture.cpp                                           **
**                                                                          **
**  Chargement des dictionnaires de textures (.tex.xbx).                    **
**                                                                          **
**  Voir p_NxTexture.h pour la disposition du fichier. Deux points meritent **
**  d'etre retenus, parce qu'ils ne se devinent pas :                        **
**                                                                          **
**  1. ENTRELACEMENT. Les textures non compressees sont rangees en ordre de **
**     Morton (Â« swizzle Â» Xbox). Les copier telles quelles donne une image **
**     en damier reconnaissable -- symptome utile, d'ailleurs : si l'ecran  **
**     montre ce damier, c'est ici qu'il faut regarder.                      **
**                                                                          **
**  2. ORDRE DES COMPOSANTES. Un D3DCOLOR vaut 0xAARRGGBB, donc en petit-   **
**     boutien les octets sortent B, G, R, A. OpenGL attend R, G, B, A. Il  **
**     faut echanger rouge et bleu, sinon tout le jeu vire au bleu.          **
**                                                                          **
**  Choix assume : on ne televerse QUE le niveau 0. Les autres niveaux sont **
**  lus (il faut avancer dans le fichier) puis jetes. Les mipmaps sont un   **
**  gain de qualite et de bande passante, pas une condition pour voir       **
**  quelque chose ; on les ajoutera quand l'image sera juste.                **
*****************************************************************************/

#include <core/defines.h>
#include <sys/file/filesys.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Pool circulaire de vitaGL (vgl.c:132), exporte par la bibliotheque mais
// absent du header public. Voir envoi_dxt_prepare plus bas.
extern "C" uint8_t *vgl_reserve_data_pool( uint32_t size );

#include "vita_log.h"
#include "p_NxTexture.h"

namespace NxVita
{
void OublierAdressage( unsigned int tex );
extern unsigned int g_vita_gen_pre;	// p_world_render.cpp
// « dxa 0/1 » : realigner le pool circulaire de vitaGL avant chaque envoi
// compresse (issue #11, voir envoi_dxt_prepare).
bool g_vita_dxt_align = true;

// --- Mipmaps (issue #18) ----------------------------------------------------
//
// [MESURE] Les fichiers Xbox portent toute la chaine de mips, mais seul le
// niveau 0 etait televerse, filtre GL_LINEAR. Dans une ville vue de loin, un
// pixel lointain allait chercher ses texels dans la texture pleine taille :
// cache de textures du GPU ruine, bande passante gaspillee -- et scintillement.
//
// « mip N » : 0 = sans mips (ancien comportement), 1 = bilineaire + mip le
// plus proche, 2 = trilineaire. Applique a chaud a toutes les textures du
// decor qui ont une chaine de mips.
// Defaut 0 (comportement d'origine) : aucun gain mesure tant que les DXT1, la
// majorite du decor, n'ont pas leur chaine (le chemin compresse de vitaGL
// plante, voir PORTING_NOTES). Lu AU CHARGEMENT : « mip 1 » a chaud n'a
// d'effet que sur un niveau charge avec mip > 0.
int g_vita_mip = 0;
static GLuint *sp_tex_mip   = NULL;
static int     s_nb_tex_mip = 0, s_cap_tex_mip = 0;

static GLint filtre_min( void )
{
	return ( g_vita_mip == 2 ) ? GL_LINEAR_MIPMAP_LINEAR
	     : ( g_vita_mip == 1 ) ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
}

static void retenir_tex_mip( GLuint tex )
{
	if( s_nb_tex_mip == s_cap_tex_mip )
	{
		int cap = s_cap_tex_mip ? s_cap_tex_mip * 2 : 1024;
		GLuint *p = (GLuint *)realloc( sp_tex_mip, cap * sizeof( GLuint ));
		if( !p )
			return;
		sp_tex_mip = p;
		s_cap_tex_mip = cap;
	}
	sp_tex_mip[s_nb_tex_mip++] = tex;
}

void VitaAppliquerMip( int mode )
{
	g_vita_mip = mode;
	const GLint f = filtre_min();
	for( int i = 0; i < s_nb_tex_mip; ++i )
	{
		glBindTexture( GL_TEXTURE_2D, sp_tex_mip[i] );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f );
	}
	glBindTexture( GL_TEXTURE_2D, 0 );
	VLOG( "TEX", "mipmaps : mode %d sur %d textures", mode, s_nb_tex_mip );
}
}

namespace Nx
{

// Issue #11 -- textures DXT1 noires apres une reconstruction du skater.
//
// [SOURCE] vitaGL utils/gpu_utils.c:663, gpu_alloc_compressed_texture : un
// envoi DXT1 n'est PAS une copie CPU (contrairement au RGBA, gpu_utils.c:386,
// vgl_fast_memcpy). Les donnees sont posees dans le POOL CIRCULAIRE
// (vgl_reserve_data_pool, vgl.c:132), puis un sceGxmTransferCopy au format
// RAW64 les range dans la texture -- et sa valeur de retour est ignoree.
//
// [VERIFIE sur console] Le pointeur du pool avance de la taille exacte de
// chaque reservation ; nos tableaux de sommets (multiples de 4) le laissent
// parfois a 4 modulo 8. Mesure par relecture du premier bloc apres
// sceGxmTransferFinish : 320 envois reussis dont 235 decales d'un multiple
// de 16, et 38 echecs, TOUS decales de 4. Apres realignement : 1600 envois,
// zero echec. Une texture ratee garde le contenu anterieur de sa memoire :
// des zeros (DXT1 nul = NOIR : corps, t-shirt, pantalon, yeux) ou une
// ancienne texture (le cou multicolore).
//
// Au demarrage le pool est neuf et aligne, d'ou un jeu correct jusqu'a la
// premiere reconstruction en pleine partie.
// Alpha MINIMAL de chaque texture, par nom GL (issue #18). 0 = inconnu ou
// transparent : le decor garde alors son test alpha. Voir p_world_render.cpp,
// seuil_effectif : un discard qui ne retire jamais rien prive quand meme le
// GPU (PowerVR) de son rejet precoce des surfaces cachees.
unsigned char g_vita_tex_alpha_min[16384];

static void noter_alpha_min( GLuint tex, unsigned int a )
{
	if(( tex > 0 ) && ( tex < 16384 ))
		g_vita_tex_alpha_min[tex] = (unsigned char)a;
}

static unsigned int alpha_min_rgba( const unsigned char *p, int num_pixels )
{
	unsigned int m = 255;
	for( int i = 0; i < num_pixels; ++i )
		if( p[i * 4 + 3] < m )
			m = p[i * 4 + 3];
	return m;
}

// DXT1 lu en RGBA : un bloc en mode 3 couleurs (c0 <= c1) rend l'index 3
// TRANSPARENT. Sinon tout est opaque.
// Texture SOMBRE (desktop, ombres du soleil) : couleur moyenne ponderee par
// l'alpha sous 40 -- les decalques d'ombre cuite du decor (texture noire a
// alpha, melange BLEND), que les ombres du soleil remplacent. Meme regle que
// le convertisseur de niveaux (thug_to_skate.py, texture_is_dark).
unsigned char g_vita_tex_sombre[16384];

static void noter_sombre( GLuint tex, bool sombre )
{
	if(( tex > 0 ) && ( tex < 16384 ))
		g_vita_tex_sombre[tex] = sombre ? 1 : 0;
}

static bool sombre_rgba( const unsigned char *p, int num_pixels )
{
	double s = 0.0, w = 0.0;
	for( int i = 0; i < num_pixels; ++i )
	{
		const double a = p[i * 4 + 3] / 255.0;
		s += a * ( p[i * 4] + p[i * 4 + 1] + p[i * 4 + 2] ) / 3.0;
		w += a;
	}
	return ( w > 0.0 ) && ( s / w < 40.0 );
}

// Blocs DXT : moyenne des deux couleurs extremes de chaque bloc (couleur sur
// 8 octets a `decalage` dans un bloc de `pas` octets).
static bool sombre_dxt( const unsigned char *p, unsigned int taille, unsigned int pas, unsigned int decalage )
{
	double s = 0.0;
	unsigned int n = 0;
	for( unsigned int o = 0; o + pas <= taille; o += pas, ++n )
	{
		const unsigned char *b = p + o + decalage;
		const unsigned int c[2] = { (unsigned)( b[0] | ( b[1] << 8 )), (unsigned)( b[2] | ( b[3] << 8 )) };
		for( int k = 0; k < 2; ++k )
			s += ((( c[k] >> 11 ) & 31 ) * 255.0 / 31.0 + (( c[k] >> 5 ) & 63 ) * 255.0 / 63.0
			      + ( c[k] & 31 ) * 255.0 / 31.0 ) / 6.0;
	}
	return n && ( s / n < 40.0 );
}

static unsigned int alpha_min_dxt1( const unsigned char *p, unsigned int taille )
{
	for( unsigned int o = 0; o + 8 <= taille; o += 8 )
	{
		const unsigned int c0 = p[o] | ( p[o + 1] << 8 );
		const unsigned int c1 = p[o + 2] | ( p[o + 3] << 8 );
		if( c0 > c1 )
			continue;
		const unsigned int bits = p[o + 4] | ( p[o + 5] << 8 ) | ( p[o + 6] << 16 ) | ( (unsigned int)p[o + 7] << 24 );
		for( int k = 0; k < 16; ++k )
			if((( bits >> ( 2 * k )) & 3 ) == 3 )
				return 0;
	}
	return 255;
}

static void envoi_dxt_prepare()
{
	const unsigned mis = (unsigned)( (uintptr_t)vgl_reserve_data_pool( 0 ) & 63u );
	if( NxVita::g_vita_dxt_align && mis )
		vgl_reserve_data_pool( 64u - mis );
}

// --- CVitaTexture ----------------------------------------------------------

// Comptes de diagnostic (issue #32) : textures et dictionnaires vivants, et
// texels des textures vivantes. Releves a chaque vidage du monde.
static int          s_tex_vivantes = 0;
static unsigned int s_texels_vivants = 0;
static int          s_dicts_vivants = 0;

CVitaTexture::CVitaTexture()
	: m_gl_texture( 0 ), mp_img_path( NULL ), m_util_w( 0 ), m_util_h( 0 ),
	  m_width( 0 ), m_height( 0 ), m_transparent( false )
{
	++s_tex_vivantes;
}

// Trace de diagnostic : un identifiant GL detruit est RECYCLE par le pilote.
// Or les maillages COPIENT cet identifiant (p_NxModel.cpp, p_dst->texture) au
// lieu de garder une reference. Si une texture meurt apres qu'un maillage l'a
// notee, le maillage dessine avec l'identifiant d'une AUTRE texture -- celle
// qui aura recupere le numero. C'est le scenario a confirmer ou refuter ici.
static int s_detruites = 0;

// Commande « texsum N » : empreinte du contenu d'une texture GL, pour savoir
// si ses donnees changent sous nos pieds (enquete sur le fond du menu qui vire
// a la couleur d'effacement apres le mode demo, alors que la texture n'est
// jamais detruite).
}	// namespace Nx
#include <zlib.h>
namespace NxVita
{
void VitaTexSum( unsigned int name )
{
	glBindTexture( GL_TEXTURE_2D, name );
	const unsigned char *p_data = (const unsigned char *)vglGetTexDataPointer( GL_TEXTURE_2D );
	SceGxmTexture *p_gxm = vglGetGxmTexture( GL_TEXTURE_2D );
	if( !p_data || !p_gxm )
	{
		VLOG( "TEX", "texsum %u : pas de donnees (ptr=%p gxm=%p)", name, p_data, p_gxm );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return;
	}
	const unsigned w = sceGxmTextureGetWidth( p_gxm );
	const unsigned h = sceGxmTextureGetHeight( p_gxm );
	// 16 Ko suffisent a dire si le contenu a change, quel que soit le format.
	const unsigned long crc = crc32( 0L, p_data, 16384 );
	// Pour les formats 32 bits : combien de pixels ont un alpha nul ? Un sprite
	// detoure qui perd son alpha au rechargement se voit ici d'un coup d'oeil.
	unsigned alpha0 = 0;
	const unsigned fmt = (unsigned)sceGxmTextureGetFormat( p_gxm );
	if(( fmt & 0xFF000000u ) == 0x0C000000u )
		for( unsigned i = 0; i < w * h; ++i )
			if( p_data[i * 4 + 3] == 0 )
				++alpha0;
	VLOG( "TEX", "texsum %u : ptr=%p %ux%u format=0x%08x crc16k=%08lx debut=%02x%02x%02x%02x alpha0=%u/%u",
	      name, p_data, w, h, fmt, crc,
	      p_data[0], p_data[1], p_data[2], p_data[3], alpha0, w * h );
	glBindTexture( GL_TEXTURE_2D, 0 );
}
}	// namespace NxVita
namespace Nx
{

CVitaTexture::~CVitaTexture()
{
	if( m_gl_texture )
		if( m_gl_texture && ( s_detruites < 40 ))
	{
		VLOG( "TEX", "DESTRUCTION texture GL %u (%dx%d)",
		      (unsigned)m_gl_texture, (int)m_width, (int)m_height );
		++s_detruites;
	}
	--s_tex_vivantes;
	s_texels_vivants -= (unsigned)m_width * m_height;
	NxVita::OublierAdressage( m_gl_texture );
	noter_alpha_min( m_gl_texture, 0 );		// le nom pourra etre reutilise
	noter_sombre( m_gl_texture, false );
	glDeleteTextures( 1, &m_gl_texture );
	free( mp_img_path );
}

void CVitaTexture::SetGLTextureKeepChecksum( GLuint tex, uint16 w, uint16 h,
                                             bool transparent )
{
	s_texels_vivants += (unsigned)w * h - (unsigned)m_width * m_height;
	m_gl_texture  = tex;
	m_width       = w;
	m_height      = h;
	m_transparent = transparent;
}

void CVitaTexture::SetGLTexture( GLuint tex, uint16 w, uint16 h, bool transparent,
                                 uint32 checksum )
{
	s_texels_vivants += (unsigned)w * h - (unsigned)m_width * m_height;
	m_gl_texture  = tex;
	m_width       = w;
	m_height      = h;
	m_transparent = transparent;
	m_checksum    = checksum;
}


// --- desentrelacement ------------------------------------------------------
//
// Transcrit du backend DX9 (DX9/NX/texture.cpp:207). L'algorithme entrelace
// les bits des coordonnees X et Y ; on le deroule tel quel plutot que de le
// reecrire Â« plus proprement Â» -- une reecriture non verifiee sur un format
// binaire ne se distingue d'un bug que par l'image produite.
// Decodage LOGICIEL du DXT5.
//
// vitaGL accepte GL_COMPRESSED_RGBA_S3TC_DXT5_EXT sans erreur, mais l'alpha
// n'arrive pas au rendu : mesure sur table, la texture du voile du menu a un
// alpha moyen de 1/255 et le quad sortait blanc OPAQUE. On decode donc les
// blocs nous-memes -- couleurs interpolees comme DXT1 (mode c0>c1 force) plus
// les huit niveaux d'alpha du bloc.
//
// Cout : quatre octets par texel au lieu d'un, uniquement pour les textures
// DXT5. Les DXT1, majoritaires (decor), restent compressees.
static void decode_dxt5( unsigned char *p_dst, const unsigned char *p_src,
                         int width, int height )
{
	const unsigned char *p = p_src;
	for( int by = 0; by < height; by += 4 )
	{
		for( int bx = 0; bx < width; bx += 4 )
		{
			// --- bloc alpha : deux extremes + trois bits par texel ---------
			const unsigned int a0 = p[0], a1 = p[1];
			unsigned long long abits = 0;
			for( int k = 0; k < 6; ++k )
				abits |= (unsigned long long)p[2 + k] << ( 8 * k );
			unsigned int at[8];
			at[0] = a0;
			at[1] = a1;
			if( a0 > a1 )
			{
				for( int k = 1; k < 7; ++k )
					at[k + 1] = (( 7 - k ) * a0 + k * a1 ) / 7;
			}
			else
			{
				for( int k = 1; k < 5; ++k )
					at[k + 1] = (( 5 - k ) * a0 + k * a1 ) / 5;
				at[6] = 0;
				at[7] = 255;
			}

			// --- bloc couleur ----------------------------------------------
			const unsigned int c0 = p[8]  | ( p[9]  << 8 );
			const unsigned int c1 = p[10] | ( p[11] << 8 );
			unsigned int cbits = 0;
			for( int k = 0; k < 4; ++k )
				cbits |= (unsigned int)p[12 + k] << ( 8 * k );
			p += 16;

			unsigned char col[4][3];
			col[0][0] = (unsigned char)(((( c0 >> 11 ) & 0x1F ) * 255 ) / 31 );
			col[0][1] = (unsigned char)(((( c0 >>  5 ) & 0x3F ) * 255 ) / 63 );
			col[0][2] = (unsigned char)(((  c0         & 0x1F ) * 255 ) / 31 );
			col[1][0] = (unsigned char)(((( c1 >> 11 ) & 0x1F ) * 255 ) / 31 );
			col[1][1] = (unsigned char)(((( c1 >>  5 ) & 0x3F ) * 255 ) / 63 );
			col[1][2] = (unsigned char)(((  c1         & 0x1F ) * 255 ) / 31 );
			for( int k = 0; k < 3; ++k )
			{
				col[2][k] = (unsigned char)(( 2 * col[0][k] + col[1][k] ) / 3 );
				col[3][k] = (unsigned char)(( col[0][k] + 2 * col[1][k] ) / 3 );
			}

			for( int ty = 0; ty < 4; ++ty )
			{
				for( int tx = 0; tx < 4; ++tx )
				{
					const int x = bx + tx, y = by + ty;
					if(( x >= width ) || ( y >= height ))
						continue;
					const int i = ty * 4 + tx;
					const unsigned int ci = ( cbits >> ( 2 * i )) & 3;
					const unsigned int ai = (unsigned int)(( abits >> ( 3 * i )) & 7 );
					unsigned char *d = &p_dst[( y * width + x ) * 4];
					d[0] = col[ci][0];
					d[1] = col[ci][1];
					d[2] = col[ci][2];
					d[3] = (unsigned char)at[ai];
				}
			}
		}
	}
}


static void unswizzle( char *p_out, const char *p_in, int width, int height,
                       int byte_depth )
{
	int pixel = 0;
	int in_off = 0;
	int total = width * height;

	while( pixel < total )
	{
		int half_w = width / 2;
		int half_h = height / 2;

		int x = 0, y = 0;
		int bx = 1, by = 1, bit = 1;

		while( half_w || half_h )
		{
			if( half_w )
			{
				half_w /= 2;
				if( pixel & bit )
					x |= bx;
				bx  *= 2;
				bit *= 2;
			}
			if( half_h )
			{
				half_h /= 2;
				if( pixel & bit )
					y |= by;
				by  *= 2;
				bit *= 2;
			}
		}
		memcpy( &p_out[byte_depth * ( x + y * width )], &p_in[in_off], byte_depth );
		in_off += byte_depth;
		++pixel;
	}
}


// D3DCOLOR (0xAARRGGBB) -> RGBA8 attendu par OpenGL.
static inline void argb_to_rgba( unsigned char *p, int num_pixels )
{
	for( int i = 0; i < num_pixels; ++i )
	{
		unsigned char b = p[i * 4 + 0];
		unsigned char r = p[i * 4 + 2];
		p[i * 4 + 0] = r;
		p[i * 4 + 2] = b;
	}
}


// A1R5G5B5 -> RGBA8. On developpe plutot que de chercher un format GL exact :
// 2 octets economises par texel ne valent pas le risque de se tromper de
// format sur un materiel qu'on connait mal.
static void a1rgb5_to_rgba8( unsigned char *p_dst, const unsigned char *p_src,
                             int num_pixels )
{
	for( int i = 0; i < num_pixels; ++i )
	{
		unsigned short v = (unsigned short)( p_src[i * 2] | ( p_src[i * 2 + 1] << 8 ));
		unsigned char  r = (unsigned char)((( v >> 10 ) & 0x1F ) * 255 / 31 );
		unsigned char  g = (unsigned char)((( v >>  5 ) & 0x1F ) * 255 / 31 );
		unsigned char  b = (unsigned char)((  v         & 0x1F ) * 255 / 31 );
		unsigned char  a = ( v & 0x8000 ) ? 255 : 0;
		p_dst[i * 4 + 0] = r;
		p_dst[i * 4 + 1] = g;
		p_dst[i * 4 + 2] = b;
		p_dst[i * 4 + 3] = a;
	}
}


// --- lecture du fichier ----------------------------------------------------

static bool s_error = false;

// Le meme contenu arrive tantot d'un fichier, tantot d'un bloc memoire deja
// charge (dictionnaires embarques dans une archive). Plutot que de dupliquer
// le parseur -- deux copies qui divergent au premier correctif -- on lit a
// travers ce petit lecteur.
struct SReader
{
	void                *p_file;	// NULL si lecture en memoire
	const unsigned char *p_mem;
};

static void rd( void *p_dst, int size, int count, SReader *p_rd )
{
	if( s_error )
		return;

	int bytes = size * count;
	if( p_rd->p_file )
	{
		if( File::Read( p_dst, size, count, p_rd->p_file ) != (size_t)bytes )
			s_error = true;
	}
	else
	{
		memcpy( p_dst, p_rd->p_mem, bytes );
		p_rd->p_mem += bytes;
	}
}

static uint32 rd_u32( SReader *p_rd )
{
	uint32 v = 0;
	rd( &v, sizeof( uint32 ), 1, p_rd );
	return v;
}


// Corps commun : le fichier et le bloc memoire ont EXACTEMENT la meme
// disposition, seule la source change.
static int load_texture_stream( SReader *p_rd, const char *p_label,
                                Lst::HashTable< Nx::CTexture > *p_table );


int LoadVitaTextureFile( const char *p_filename,
                         Lst::HashTable< Nx::CTexture > *p_table )
{
	if( !p_table )
		return 0;

	void *p_file = File::Open( p_filename, "rb" );
	if( !p_file )
	{
		VLOG( "TEX", "introuvable : '%s'", p_filename );
		return 0;
	}

	SReader reader;
	reader.p_file = p_file;
	reader.p_mem  = NULL;

	int n = load_texture_stream( &reader, p_filename, p_table );

	File::Close( p_file );
	return n;
}


int LoadVitaTextureMemory( const void *p_data,
                           Lst::HashTable< Nx::CTexture > *p_table )
{
	if( !p_table || !p_data )
		return 0;

	SReader reader;
	reader.p_file = NULL;
	reader.p_mem  = (const unsigned char *)p_data;

	return load_texture_stream( &reader, "<memoire>", p_table );
}


static int load_texture_stream( SReader *p_rd, const char *p_label,
                                Lst::HashTable< Nx::CTexture > *p_table )
{
	s_error = false;

	int version      = (int)rd_u32( p_rd );
	int num_textures = (int)rd_u32( p_rd );

	if(( num_textures < 0 ) || ( num_textures > 8192 ))
	{
		VLOG( "TEX", "!! '%s' : %d textures annoncees, hors bornes",
		      p_label, num_textures );
		return 0;
	}

	int created = 0;
	int skipped = 0;

	for( int t = 0; ( t < num_textures ) && !s_error; ++t )
	{
		uint32 checksum       = rd_u32( p_rd );
		uint32 base_width     = rd_u32( p_rd );
		uint32 base_height    = rd_u32( p_rd );
		uint32 levels         = rd_u32( p_rd );
		uint32 texel_depth    = rd_u32( p_rd );
		uint32 palette_depth  = rd_u32( p_rd );
		uint32 dxt            = rd_u32( p_rd );
		uint32 palette_size   = rd_u32( p_rd );
		(void)palette_depth;

		if(( base_width == 0 ) || ( base_height == 0 )
		   || ( base_width > 4096 ) || ( base_height > 4096 )
		   || ( levels == 0 ) || ( levels > 16 )
		   || ( palette_size > 4096 ))
		{
			VLOG( "TEX", "!! '%s' texture %d incoherente (%ux%u, %u niveaux) "
			             "-- lecture desynchronisee", p_label, t,
			      (unsigned)base_width, (unsigned)base_height, (unsigned)levels );
			s_error = true;
			break;
		}

		unsigned char palette[4096];
		if( palette_size )
			rd( palette, (int)palette_size, 1, p_rd );

		GLuint gl_tex = 0;
		int    mips_envoyes = 0;		// niveaux DXT televerses au-dela du 0

		uint32 mip_w = base_width;
		uint32 mip_h = base_height;

		for( uint32 level = 0; ( level < levels ) && !s_error; ++level )
		{
			uint32 data_size = rd_u32( p_rd );
			if( data_size > ( 8u << 20 ))
			{
				s_error = true;
				break;
			}

			unsigned char *p_src = (unsigned char *)malloc( data_size );
			if( !p_src )
			{
				s_error = true;
				break;
			}
			rd( p_src, (int)data_size, 1, p_rd );

			// Niveaux suivants d'une texture DXT (hors DXT5, decodee) : envoyes
			// tels quels, dans le meme format que le niveau 0. Les blocs DXT
			// font 4x4 : on s'arrete sous cette taille. Les autres formats
			// sont entrelaces et palettes niveau par niveau : leur chaine est
			// regeneree par vitaGL apres la boucle.
			//
			// [VERIFIE] Plantage dans gpu_alloc_compressed_texture (copie) quand
			// la taille du niveau ne correspondait pas a celle que vitaGL
			// alloue : on n'envoie que des niveaux de la taille DXT1 EXACTE
			// (8 octets par bloc 4x4) d'une texture en puissance de deux.
			const bool pot = (( base_width & ( base_width - 1 )) == 0 )
			              && (( base_height & ( base_height - 1 )) == 0 );
			const uint32 taille_dxt1 = (( mip_w + 3 ) / 4 ) * (( mip_h + 3 ) / 4 ) * 8;
			// [DESACTIVE] gpu_alloc_compressed_texture plante sur ces envois
			// (copie de taille negative, voir PORTING_NOTES) : a refaire en
			// construisant la chaine nous-memes.
			if( false && ( level > 0 ) && gl_tex && ( dxt > 0 ) && ( dxt != 5 ) && pot
			   && ( data_size == taille_dxt1 )
			   && ( NxVita::g_vita_mip > 0 ) && ( mip_w >= 4 ) && ( mip_h >= 4 )
			   && ( mips_envoyes == (int)level - 1 ))
			{
				glBindTexture( GL_TEXTURE_2D, gl_tex );
				envoi_dxt_prepare();
				while( glGetError() != GL_NO_ERROR ) {}
				glCompressedTexImage2D( GL_TEXTURE_2D, (GLint)level,
				                        GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
				                        (GLsizei)mip_w, (GLsizei)mip_h, 0,
				                        (GLsizei)data_size, p_src );
				if( glGetError() == GL_NO_ERROR )
					++mips_envoyes;
			}

			// Le niveau 0 cree la texture.
			if(( level == 0 ) && !s_error )
			{
				glGenTextures( 1, &gl_tex );
				noter_alpha_min( gl_tex, 0 );
				noter_sombre( gl_tex, false );
				glBindTexture( GL_TEXTURE_2D, gl_tex );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );

				if( dxt == 5 )
				{
					// DXT5 : decode en logiciel, vitaGL n'en restitue pas
					// l'alpha (voir decode_dxt5).
					unsigned char *p_rgba5 =
						(unsigned char *)malloc( mip_w * mip_h * 4 );
					if( p_rgba5 )
					{
						decode_dxt5( p_rgba5, p_src, (int)mip_w, (int)mip_h );
						noter_alpha_min( gl_tex, alpha_min_rgba( p_rgba5, (int)( mip_w * mip_h )));
						noter_sombre( gl_tex, sombre_rgba( p_rgba5, (int)( mip_w * mip_h )));
						glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA,
						              (GLsizei)mip_w, (GLsizei)mip_h, 0,
						              GL_RGBA, GL_UNSIGNED_BYTE, p_rgba5 );
						free( p_rgba5 );
					}
				}
				else if( dxt > 0 )
				{
					// Blocs 4x4 compresses, non entrelaces : televersement direct.
					GLenum fmt = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
					// Purge des erreurs precedentes, pour attribuer la
					// suivante a CET upload.
					while( glGetError() != GL_NO_ERROR ) {}
					envoi_dxt_prepare();
					noter_alpha_min( gl_tex, ( dxt == 1 ) ? alpha_min_dxt1( p_src, data_size ) : 0 );
					noter_sombre( gl_tex, ( dxt == 1 ) ? sombre_dxt( p_src, data_size, 8, 0 )
					                                   : sombre_dxt( p_src, data_size, 16, 8 ));
					glCompressedTexImage2D( GL_TEXTURE_2D, 0, fmt,
					                        (GLsizei)mip_w, (GLsizei)mip_h, 0,
					                        (GLsizei)data_size, p_src );
					// Un upload compresse refuse laisse une texture VIDE qui
					// sample blanc opaque -- exactement la tete du rectangle
					// blanc du menu (deux quads-panneaux en DXT5). On compte
					// par format pour trancher.
					{
						const GLenum err = glGetError();
						if( err != GL_NO_ERROR )
						{
							static int s_warn = 0;
							if( s_warn++ < 12 )
								VLOG( "TEX", "!! upload DXT%u %ux%u refuse "
								             "(glGetError=0x%x)",
								      (unsigned)dxt, (unsigned)mip_w,
								      (unsigned)mip_h, (unsigned)err );
						}
					}
				}
				else
				{
					int num_pixels = (int)( mip_w * mip_h );
					unsigned char *p_rgba =
						(unsigned char *)malloc( num_pixels * 4 );

					if( p_rgba )
					{
						if( texel_depth == 8 )
						{
							// Indices sur un octet : desentrelacer, puis
							// developper via la palette.
							unsigned char *p_idx =
								(unsigned char *)malloc( num_pixels );
							if( p_idx )
							{
								unswizzle( (char *)p_idx, (const char *)p_src,
								           (int)mip_w, (int)mip_h, 1 );
								for( int i = 0; i < num_pixels; ++i )
									memcpy( &p_rgba[i * 4],
									        &palette[p_idx[i] * 4], 4 );
								free( p_idx );
								argb_to_rgba( p_rgba, num_pixels );
							}
						}
						else if( texel_depth == 16 )
						{
							unsigned char *p_lin =
								(unsigned char *)malloc( num_pixels * 2 );
							if( p_lin )
							{
								unswizzle( (char *)p_lin, (const char *)p_src,
								           (int)mip_w, (int)mip_h, 2 );
								a1rgb5_to_rgba8( p_rgba, p_lin, num_pixels );
								free( p_lin );
							}
						}
						else	// 32 bits
						{
							unswizzle( (char *)p_rgba, (const char *)p_src,
							           (int)mip_w, (int)mip_h, 4 );
							argb_to_rgba( p_rgba, num_pixels );
						}

						noter_alpha_min( gl_tex, alpha_min_rgba( p_rgba, num_pixels ));
						noter_sombre( gl_tex, sombre_rgba( p_rgba, num_pixels ));
						glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA,
						              (GLsizei)mip_w, (GLsizei)mip_h, 0,
						              GL_RGBA, GL_UNSIGNED_BYTE, p_rgba );
						free( p_rgba );
					}
				}
			}

			free( p_src );

			if( mip_w > 1 ) mip_w >>= 1;
			if( mip_h > 1 ) mip_h >>= 1;
		}

		if( s_error )
		{
			if( gl_tex )
				glDeleteTextures( 1, &gl_tex );
			break;
		}

		// Chaine de mips : celle du fichier (DXT), ou regeneree par vitaGL.
		if( gl_tex && ( levels > 1 ) && ( NxVita::g_vita_mip > 0 ))
		{
			glBindTexture( GL_TEXTURE_2D, gl_tex );
			bool chaine = ( mips_envoyes > 0 );
			if(( dxt == 0 ) || ( dxt == 5 ))
			{
				glGenerateMipmap( GL_TEXTURE_2D );
				chaine = true;
			}
			if( chaine )
			{
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, NxVita::filtre_min());
				NxVita::retenir_tex_mip( gl_tex );
			}
		}

		// Deux checksums que le backend PC ecarte explicitement ; on fait
		// pareil faute de savoir pourquoi (Â« hack from PC Â», p_nxtexture.cpp:698).
		if(( checksum == 0x749DB390 ) || ( checksum == 0xD1CE9FFC ))
		{
			glDeleteTextures( 1, &gl_tex );
			++skipped;
			continue;
		}

		CVitaTexture *p_texture = new CVitaTexture;
		p_texture->SetGLTexture( gl_tex, (uint16)base_width, (uint16)base_height,
		                         ( dxt == 5 ) || ( texel_depth == 32 ), checksum );
		p_table->PutItem( checksum, p_texture );
		++created;
	}

	glBindTexture( GL_TEXTURE_2D, 0 );

	VLOG( "TEX", "'%s' v%d : %d/%d textures%s%s", p_label, version,
	      created, num_textures,
	      skipped ? " (2 ecartees)" : "",
	      s_error ? "  [LECTURE INTERROMPUE]" : "" );

	return created;
}


// --- CVitaTexDict ----------------------------------------------------------

CVitaTexDict::CVitaTexDict( uint32 checksum )
	: CTexDict( checksum ), m_vita_liberable( false )
{
	++s_dicts_vivants;
	// Ne charge rien : dictionnaire rempli plus tard par le moteur.
}

CVitaTexDict::CVitaTexDict( const char *p_tex_dict_name )
	: CTexDict( p_tex_dict_name, true ),		// true : il FAUT la table de lookup
	  m_vita_liberable( false )
{
	++s_dicts_vivants;
	LoadVitaTextureFile( p_tex_dict_name, mp_texture_lookup );
}

CVitaTexDict::~CVitaTexDict()
{
	--s_dicts_vivants;
}


// --- une texture isolee (.img.xbx) -----------------------------------------
//
// Format distinct de celui des dictionnaires (DX9/NX/texture.cpp:273) :
//
//   uint32 version, checksum, largeur, hauteur, profondeur, profondeur CLUT
//   uint16 largeur d'origine, hauteur d'origine
//   uint32 taille de la palette
//   [palette]
//   donnees
//
// Les profondeurs sont des valeurs de REGISTRE PS2, pas des nombres de bits :
// PSMCT32=0x00, PSMCT16=0x02, PSMT8=0x13. Les prendre pour des bits donne des
// tailles absurdes.
#define PSMCT32	0x00
#define PSMCT16	0x02
#define PSMT8	0x13

static bool is_power_of_two( uint32 v )
{
	return ( v != 0 ) && (( v & ( v - 1 )) == 0 );
}


bool LoadVitaImgInto( CVitaTexture *p_texture, const char *p_texture_name,
                      GLuint reuse_tex )
{
	char filename[288];
	snprintf( filename, sizeof( filename ), "%s.img.xbx", p_texture_name );

	void *p_file = File::Open( filename, "rb" );
	if( !p_file )
	{
		// Bornee : ScriptLoadTexture est appele des centaines de fois.
		static int s_missed = 0;
		if( s_missed++ < 20 )
			VLOG( "TEX", "sprite ECHEC : '%s'", filename );
		return false;
	}

	s_error = false;

	SReader reader;
	reader.p_file = p_file;
	reader.p_mem  = NULL;
	SReader *p_rd = &reader;

	uint32 version       = rd_u32( p_rd );
	uint32 checksum      = rd_u32( p_rd );
	uint32 width         = rd_u32( p_rd );
	uint32 height        = rd_u32( p_rd );
	uint32 bit_depth     = rd_u32( p_rd );
	uint32 clut_depth    = rd_u32( p_rd );
	uint16 orig_w = 0, orig_h = 0;
	rd( &orig_w, sizeof( uint16 ), 1, p_rd );
	rd( &orig_h, sizeof( uint16 ), 1, p_rd );
	uint32 palette_size  = rd_u32( p_rd );
	(void)version;

	// Registres PS2 -> bits par texel.
	uint32 bpp = 0;
	if( bit_depth == PSMCT32 )		bpp = 32;
	else if( bit_depth == PSMCT16 )	bpp = 16;
	else if( bit_depth == PSMT8 )	bpp = 8;

	if(( bpp == 0 ) || ( width == 0 ) || ( height == 0 )
	   || ( width > 4096 ) || ( height > 4096 ) || ( palette_size > 4096 ))
	{
		VLOG( "TEX", "!! '%s' en-tete incoherent (%ux%u, profondeur 0x%x)",
		      filename, (unsigned)width, (unsigned)height, (unsigned)bit_depth );
		File::Close( p_file );
		return false;
	}

	unsigned char palette[4096];
	bool has_clut = ( bpp < 16 ) && ( palette_size > 0 );
	if( has_clut )
		rd( palette, (int)palette_size, 1, p_rd );
	else if( palette_size )
		rd( palette, (int)palette_size, 1, p_rd );
	(void)clut_depth;

	// Les dimensions non puissances de deux ne sont PAS entrelacees.
	bool arbitrary = ( !is_power_of_two( width ) || !is_power_of_two( height ));

	uint32 num_bytes = (( width * height * ( bpp >> 3 )) + 3 ) & 0xFFFFFFFCu;
	unsigned char *p_src = (unsigned char *)malloc( num_bytes );
	if( !p_src )
	{
		File::Close( p_file );
		return false;
	}
	rd( p_src, (int)num_bytes, 1, p_rd );
	File::Close( p_file );

	if( s_error )
	{
		free( p_src );
		return false;
	}

	int num_pixels = (int)( width * height );

	unsigned char *p_packed = p_src;
	unsigned char *p_tmp    = NULL;
	if( !arbitrary )
	{
		p_tmp = (unsigned char *)malloc( num_bytes );
		if( p_tmp )
		{
			unswizzle( (char *)p_tmp, (const char *)p_src,
			           (int)width, (int)height, (int)( bpp >> 3 ));
			p_packed = p_tmp;
		}
	}

	unsigned char *p_rgba = (unsigned char *)malloc( num_pixels * 4 );
	if( !p_rgba )
	{
		free( p_tmp );
		free( p_src );
		return false;
	}

	if( has_clut )
	{
		for( int i = 0; i < num_pixels; ++i )
			memcpy( &p_rgba[i * 4], &palette[p_packed[i] * 4], 4 );
		argb_to_rgba( p_rgba, num_pixels );
	}
	else if( bpp == 16 )
	{
		a1rgb5_to_rgba8( p_rgba, p_packed, num_pixels );
	}
	else
	{
		memcpy( p_rgba, p_packed, num_pixels * 4 );
		argb_to_rgba( p_rgba, num_pixels );
	}

	GLuint gl_tex = reuse_tex;
	if( !gl_tex )
		glGenTextures( 1, &gl_tex );
	glBindTexture( GL_TEXTURE_2D, gl_tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	// Sur un identifiant existant, glTexImage2D respecifie entierement la
	// texture (dimensions et format compris) : l'ancienne image, DXT ou non,
	// est remplacee.
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE, p_rgba );
	glBindTexture( GL_TEXTURE_2D, 0 );

	free( p_rgba );
	free( p_tmp );
	free( p_src );

	p_texture->SetGLTextureKeepChecksum( gl_tex, (uint16)width, (uint16)height, true );
	// XBox texture.cpp:271-274 : Base = taille de la texture, Actual = original.
	p_texture->SetUtile(( orig_w && orig_w <= width ) ? orig_w : 0,
	                    ( orig_h && orig_h <= height ) ? orig_h : 0 );
	(void)checksum;

	static int s_traced = 0;
	if( s_traced < 40 )
	{
		++s_traced;
		VLOG( "TEX", "sprite '%s' %ux%u %u bits%s", filename,
		      (unsigned)width, (unsigned)height, (unsigned)bpp,
		      arbitrary ? " (non entrelacee)" : "" );
	}

	return true;
}


bool CVitaTexture::plat_load_texture( const char *p_texture_name,
                                      bool sprite, bool alloc_vram )
{
	if( !LoadVitaImgInto( this, p_texture_name ))
		return false;

	free( mp_img_path );
	mp_img_path = strdup( p_texture_name );
	return true;
}


// Appele par CTexDict::ReplaceTexture (NxTexture.cpp:930) : la nouvelle
// texture vient d'etre chargee par nom, elle sera DETRUITE juste apres. Les
// scripts CAS s'en servent pour la peau, les yeux et surtout le visage :
// cas_skater_m.q:888 remplace Â« CS_NSN_facemap.png Â», une GRILLE de reperage
// UV, par le vrai visage choisi. Sans cette fonction (stub de la classe de
// base, qui rendait false), c'est la grille qui restait a l'ecran.
bool CVitaTexture::plat_replace_texture( CTexture *p_texture )
{
	CVitaTexture *p_new = static_cast< CVitaTexture * >( p_texture );

	if( !p_new || !p_new->mp_img_path || !m_gl_texture )
	{
		VLOG( "TEX", "!! remplacement impossible : %s",
		      !p_new ? "pas de texture" :
		      !p_new->mp_img_path ? "source hors .img" : "cible sans image GL" );
		return false;
	}

	// La transparence est une propriete du materiau tel que le modele a ete
	// construit (tri, blend) ; on ne la laisse pas changer avec l'image.
	const bool transparent = m_transparent;
	if( !LoadVitaImgInto( this, p_new->mp_img_path, m_gl_texture ))
	{
		VLOG( "TEX", "!! remplacement : relecture de '%s' echouee",
		      p_new->mp_img_path );
		return false;
	}
	m_transparent = transparent;
	// Les lots precalcules ont COPIE le descripteur de texture (adresse des
	// donnees comprise) : apres re-televersement, il pointe sur l'ancienne
	// image, liberee. On les fait reconstruire (#32).
	++NxVita::g_vita_gen_pre;

	static int s_traced = 0;
	if( s_traced++ < 40 )
		VLOG( "TEX", "remplacement %08x <- '%s' (GL %u, %ux%u)",
		      (unsigned)m_checksum, p_new->mp_img_path,
		      (unsigned)m_gl_texture, (unsigned)m_width, (unsigned)m_height );
	return true;
}


CTexture *CVitaTexDict::plat_load_texture( const char *p_texture_name,
                                           bool sprite, bool alloc_vram )
{
	// On passe par CTexture::LoadTexture, PAS directement par le chargeur :
	// c'est cette couche qui prefixe Â« images/ Â» et qui calcule le checksum
	// a partir du nom de fichier. La court-circuiter (premiere version) donne
	// 315 chargements de sprite qui echouent tous, en silence.
	CVitaTexture *p_texture = new CVitaTexture;
	if( !p_texture->LoadTexture( p_texture_name, sprite, alloc_vram ))
	{
		delete p_texture;
		return NULL;
	}
	return p_texture;
}

} // namespace Nx


namespace NxVita { void CompteTextures( int *p_tex, unsigned int *p_texels, int *p_dicts ); }
void NxVita::CompteTextures( int *p_tex, unsigned int *p_texels, int *p_dicts )
{
	*p_tex    = Nx::s_tex_vivantes;
	*p_texels = Nx::s_texels_vivants;
	*p_dicts  = Nx::s_dicts_vivants;
}
