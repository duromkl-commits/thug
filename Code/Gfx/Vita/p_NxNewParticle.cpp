///////////////////////////////////////////////////////////////////////////////
// p_NxNewParticle.cpp -- particules parametriques sur Vita (systemes NEWFLAT).
// Voir p_NxNewParticle.h pour le contrat et la repartition portable/plateforme.
//
// RENDU, d'apres le XBox ([SOURCE] XBox/p_nxnewparticle.cpp:170 et
// NX/ParticleNewFlatVS.vsh, NX/PixelShader0.psh) :
//   - un quad face camera par particule. Vecteurs d'ecran : droite = at x Y
//     du monde, haut = droite x at (la droite reste HORIZONTALE, ce n'est pas
//     le repere camera complet). Demi-cote = rayon courant (pos.w).
//     Coins : (-1,+1) uv (0,0) ; (+1,+1) (1,0) ; (+1,-1) (1,1) ; (-1,-1) (0,1).
//   - couleur de sommet = lerp( col0, col1, interpolateur ), puis
//     PixelShader0 : rgb = tex * 0.5 (couleur materiau) * 4 * v, a = tex * v * 2.
//     Soit tex * v * 2 sur les quatre canaux : 128 vaut 1.0, convention PS2.
//     Ici le x2 est porte par la couleur de sommet, bornee a 255 (voir
//     couleur_x2) : exact tant que la composante ne depasse pas 127.
//   - materiau : MATFLAG_TRANSPARENT, pas de culling (m_no_bfc), coupure
//     alpha 1, melange GetBlendMode( m_BlendMode ). La profondeur est TESTEE
//     mais pas ecrite (XBox/p_nx.cpp:396, RS_ZWRITEENABLE 0 autour de
//     render_particles et de RenderParticles). Brouillard : oui (oFog), noir
//     pour les modes additifs et soustractifs (render.cpp:1177).
//   - aucun tri : particules dessinees du plus vieux flux au plus recent.
//   - au-dela de 240 unites (dist^2 >= 57600) le XBox passe en point sprites,
//     bornes a 64 pixels a l'ecran. Reproduit en "prt 1" par un demi-cote
//     borne a dist / 15 (64 px sur 480 lignes, a la focale du jeu, voir
//     dessiner) ; "prt 2" garde les quads en perspective partout.
//
// GPU (un plantage GPU eteint la console) : rien n'est alloue cote GPU par ce
// fichier. Les sommets sont ecrits dans un tampon CPU fixe (alloue une fois,
// jamais libere ni agrandi) et passes a vitaGL en tableaux clients, qu'il
// recopie dans son propre pool a chaque dessin -- le chemin des modeles
// skinnes sur CPU (p_NxModel.cpp), eprouve. La texture est relue dans le
// dictionnaire de particules a chaque image, jamais memorisee : un
// identifiant GL conserve apres un vidage du dictionnaire serait un nom mort.
//
// DEUX CHEMINS DE CALCUL ("prt", voir p_NxNewParticle.h) :
//   - CPU (prt 1/2/3) : position, coins du quad et couleur calcules ici,
//     sommets complets (24 octets) passes au pipeline fixe de vitaGL.
//   - shader (prt 4) : comme le XBox, le CPU ne tire que les 4 nombres
//     aleatoires (entiers) et le temps de chaque particule ; le programme CG
//     s_src_prt_v refait ParticleNewFlatVS.vsh (position, coins, couleur) et
//     le brouillard des shaders du decor (BROUILLARD_VS). Sommet de 16
//     octets, voir SPrtSommetShd. Toujours vitaGL et tableaux clients : rien
//     n'est alloue cote GPU ici non plus. Si la compilation echoue, repli
//     automatique sur le chemin CPU.

#include <core/defines.h>
#include <core/allmath.h>
#include <gfx/NxTexMan.h>
#include <gfx/camera.h>
#include <gel/scripting/checksum.h>
#include <sys/timer.h>

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vita_log.h"
#include "p_NxTexture.h"
#include "p_NxNewParticle.h"
#include "p_world_render.h"
#include "p_shader_decor.h"

namespace NxVita
{
// "prt" : 2 par defaut (quads en perspective), valide contre xemu le
// 2026-10-03 (vapeur des tours de NJ). Le mode 1 bornait les particules
// lointaines a dist/15 et les rendait invisibles ; a cette distance la borne
// XBox de 64 px des point sprites ne joue pas.
int g_vita_particules = 4;	// shader (prt 4) par defaut depuis le 2026-10-03 : VC 4575 particules 8,4 -> 1,7 ms, image identique au mode CPU

// p_shader_decor.cpp : compilation CG avec cache disque.
GLuint CompilerShaderCache( GLenum type, const char *p_source, const char *p_nom );
}

namespace Nx
{

/*****************************************************************************
**  Generateur pseudo-aleatoire du XBox, a l'identique                      **
*****************************************************************************/

// [SOURCE] XBox/p_nxnewparticle.cpp:62. Etat global, regraine par flux avant
// chaque parcours : la suite d'un flux est donc deterministe, et c'est ce qui
// permet de ne stocker aucune particule. Calcul en uint32 pour l'addition et
// la multiplication (le debordement signe est indefini en C++), decalages en
// int32 (decalage arithmetique, comme MSVC).
static int32 s_rand_seed;
static int32 s_rand_a = 314159265;
static int32 s_rand_b = 178453311;

static inline void seed_particle_rnd( uint32 s, uint32 a, uint32 b )
{
	s_rand_seed = (int32)s;
	s_rand_a    = (int32)a;
	s_rand_b    = (int32)b;
}

static inline void particle_rnd_pas( void )
{
	s_rand_seed = (int32)((uint32)s_rand_seed * (uint32)s_rand_a + (uint32)s_rand_b );
	s_rand_a    = (int32)((uint32)( s_rand_a ^ s_rand_seed ) + (uint32)( s_rand_seed >> 4 ));
	s_rand_b    = (int32)((uint32)s_rand_b + (uint32)(( s_rand_seed >> 3 ) - 0x10101010L ));
}

static inline int particle_rnd( int n )
{
	particle_rnd_pas();
	return (int)(( s_rand_seed & 0xffff ) * n ) >> 16;
}

void CVitaParticleStream::AdvanceSeed( int num_places )
{
	// Chaque particule tire 4 nombres (XBox :127).
	seed_particle_rnd( m_rand_seed, m_rand_a, m_rand_b );
	for( int i = 0; i < ( num_places * 4 ); i++ )
		particle_rnd_pas();
	m_rand_seed = (uint32)s_rand_seed;
	m_rand_a    = (uint32)s_rand_a;
	m_rand_b    = (uint32)s_rand_b;
}


/*****************************************************************************
**  Etat partage d'une image                                                **
*****************************************************************************/

// Particules par appel de dessin. 4 sommets chacune : 16 bits d'indices
// suffisent (4096 x 4 = 16384). Un systeme plus gros est dessine en plusieurs
// appels. Le plus gros systeme des scripts (skater_particlesys.qb, 1500 / s
// pendant 0,1 s, debit XBox divise par 2) en a ~75.
#define PRT_LOT		4096

struct SPrtSommet
{
	float			x, y, z;
	float			u, v;
	unsigned char	rgba[4];
};

static SPrtSommet *		sp_sommets = NULL;
static unsigned short *	sp_indices = NULL;
static bool				s_tampons_ok = false;

// Chemin shader (prt 4) : 16 octets par sommet, identiques pour les 4 coins
// sauf "coin". r : les 4 tirages bruts n = 0..16383 (r = 1 + n / 16384),
// lus en U16 NORMALISE (n / 65535, remis a l'echelle dans le shader) ;
// t : age de la particule ; coin : uv du coin (0 ou 255, U8 normalise), dont
// le shader deduit aussi le signe du decalage (cx = 2u - 1, cy = 1 - 2v).
// Ordre des champs = ordre des attributs (0, 1, 2) : vitaGL recopie alors le
// tableau d'un bloc (attributs "packed", custom_shaders.c).
struct SPrtSommetShd
{
	unsigned short	r[4];
	float			t;
	unsigned char	coin[4];
};

static SPrtSommetShd *	sp_sommets_shd = NULL;
static bool				s_tampons_shd_ok = false;
static bool				s_shader_image = false;	// chemin de l'image en cours

// Repere d'ecran et camera de l'image, poses par RenderVita.
static float s_droite[3], s_haut[3], s_cam[3];
static bool  s_image_ok = false;

// Journal [PRT].
static int			s_vivants = 0, s_crees = 0, s_detruits = 0;
static int			s_sys_dessines = 0, s_sys_hors_champ = 0, s_sans_tex = 0;
static int			s_particules = 0, s_appels = 0;
static SceUInt64	s_cout_us = 0;
static int			s_images = 0;

static bool assurer_tampons( void )
{
	if( s_tampons_ok )
		return true;
	// Alloues UNE fois, jamais liberes : memoire CPU, que vitaGL recopie.
	sp_sommets = (SPrtSommet *)malloc( sizeof( SPrtSommet ) * 4 * PRT_LOT );
	sp_indices = (unsigned short *)malloc( sizeof( unsigned short ) * 6 * PRT_LOT );
	if( !sp_sommets || !sp_indices )
	{
		free( sp_sommets );
		free( sp_indices );
		sp_sommets = NULL;
		sp_indices = NULL;
		VLOG( "PRT", "tampons de particules : allocation impossible" );
		return false;
	}
	for( int q = 0; q < PRT_LOT; ++q )
	{
		unsigned short *i = sp_indices + q * 6;
		const unsigned short b = (unsigned short)( q * 4 );
		i[0] = b; i[1] = b + 1; i[2] = b + 2;
		i[3] = b; i[4] = b + 2; i[5] = b + 3;
		// Coordonnees de texture fixes par coin : posees une fois.
		SPrtSommet *s = sp_sommets + q * 4;
		s[0].u = 0.0f; s[0].v = 0.0f;
		s[1].u = 1.0f; s[1].v = 0.0f;
		s[2].u = 1.0f; s[2].v = 1.0f;
		s[3].u = 0.0f; s[3].v = 1.0f;
	}
	s_tampons_ok = true;
	return true;
}

// [SOURCE] XBox/NX/render.cpp:119, GetBlendMode, ramene aux familles que le
// rendu distingue ici.
enum
{
	PRT_DIFFUSE, PRT_ADD, PRT_ADD_FIXE, PRT_SUB, PRT_SUB_FIXE, PRT_BLEND,
	PRT_BLEND_FIXE, PRT_MODULATE, PRT_MODULATE_FIXE, PRT_BRIGHTEN,
	PRT_BRIGHTEN_FIXE
};

static uint32 famille_melange( uint32 checksum )
{
	switch( checksum )
	{
		case 0x54628ed7: return PRT_BLEND;			// Blend
		case 0x02e58c18: return PRT_ADD;			// Add
		case 0xa7fd7d23:							// Sub
		case 0xdea7e576: return PRT_SUB;			// Subtract
		case 0x40f44b8a: return PRT_MODULATE;		// Modulate
		case 0x68e77f40: return PRT_BRIGHTEN;		// Brighten
		case 0x18b98905: return PRT_BLEND_FIXE;		// FixBlend
		case 0xa86285a1: return PRT_ADD_FIXE;		// FixAdd
		case 0x0d7a749a:							// FixSub
		case 0x0eea99ff: return PRT_SUB_FIXE;		// FixSubtract
		case 0x90b93703: return PRT_MODULATE_FIXE;	// FixModulate
		case 0xb8aa03c9: return PRT_BRIGHTEN_FIXE;	// FixBrighten
		default:         return PRT_DIFFUSE;		// Diffuse, None
	}
}

// Rend false si le mode ne dessine rien.
//
// Les modes "Fix*" melangent avec l'alpha CONSTANT du registre, pris dans
// les 8 bits hauts de la valeur de mode (render.cpp:1331). Or plat_build
// XBox pose m_reg_alpha[0] = GetBlendMode( m_BlendMode ) sans y glisser
// m_FixedAlpha (qui part dans m_color[0][3], que PixelShader0 n'utilise pas
// pour l'alpha) : la constante vaut 0 et ces systemes sont invisibles sur
// XBox. Aucun ParticleObject des scripts ne s'en sert (inventaire
// vita/tools/particules_qb.py : Blend et Add seulement) ; on reproduit.
static bool poser_melange( uint32 famille )
{
	switch( famille )
	{
		case PRT_ADD:
			glBlendEquation( GL_FUNC_ADD );
			glBlendFunc( GL_SRC_ALPHA, GL_ONE );
			return true;
		case PRT_SUB:
			glBlendEquation( GL_FUNC_REVERSE_SUBTRACT );
			glBlendFunc( GL_SRC_ALPHA, GL_ONE );
			return true;
		case PRT_BLEND:
			glBlendEquation( GL_FUNC_ADD );
			glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
			return true;
		case PRT_MODULATE:
			glBlendEquation( GL_FUNC_ADD );
			glBlendFunc( GL_ZERO, GL_SRC_ALPHA );
			return true;
		case PRT_BRIGHTEN:
			glBlendEquation( GL_FUNC_ADD );
			glBlendFunc( GL_DST_COLOR, GL_ONE );
			return true;
		case PRT_DIFFUSE:
			glBlendEquation( GL_FUNC_ADD );
			glBlendFunc( GL_ONE, GL_ZERO );
			return true;
		default:
			return false;
	}
}

static SceUInt64 s_boucle_us = 0;
// Particules calculees au plus par image (budget CPU). Le chemin shader ne
// coute au CPU que les tirages et 64 octets ecrits par particule : plafond
// releve a 6000 (Vancouver en demandait 4575 ; le chemin CPU les a dessinees
// sans incident avant le plafond, avec des sommets plus gros).
int g_vita_prt_budget = 2000;
int g_vita_prt_budget_shd = 6000;
static int s_budget_restant = 0;
static inline unsigned char couleur_x2( float c )
{
	// PixelShader0 : x2 sur la couleur de sommet (128 = 1.0). Borne a 255 :
	// le XBox, lui, multiplie avant de saturer (tex * v * 2), donc une
	// composante > 127 sur une texture sombre y sort un peu plus claire.
	// Alternative notee dans PORTING_NOTES : GL_COMBINE + GL_RGB_SCALE 2.
	const float v = c * 2.0f;
	if( v <= 0.0f )
		return 0;
	if( v >= 255.0f )
		return 255;
	return (unsigned char)( v + 0.5f );
}


/*****************************************************************************
**  Chemin shader (prt 4) : ParticleNewFlatVS.vsh en CG                     **
*****************************************************************************/

// [SOURCE] XBox/NX/ParticleNewFlatVS.vsh, ligne a ligne :
//   pos = p0 + t p1 + t^2 p2 + ( s0 + t s1 + t^2 s2 ) * r   (4 composantes)
//   demi-cote w = pos.w ; pos.w = 1
//   pos.xyz += cx * w * droite + cy * w * haut               (c12-c15, c4/c5)
//   oPos = WVP pos ; oT0 = uv du coin (c8-c11)
//   oD0 = lerp( v0, v1, interpolateur )                      (c17.y)
// Le XBox recevait l'interpolateur et les deux couleurs par particule
// (calcules sur CPU, :373-410). Ici le shader les deduit de t :
//   uT = ( temps du point milieu, 1 / mid, 1 / ( vie - mid ), decalage )
//   seg = ( t > mid ) ; ci = seg ? ( t - mid ) uT.z + uT.w : t uT.y
//   couleur = lerp( lerp( C0, C1, seg ), lerp( C1, C2, seg ), ci )
// Sans couleur de milieu (UseMidcolor faux) : mid = 1e30 (seg toujours 0),
// uT.y = 1 / vie, couleurs ( Color0, Color2, Color2 ). Couleurs deja x 2 / 255
// (PixelShader0, 128 = 1.0), saturees comme couleur_x2 sur CPU. Les cas
// limites (mid ou vie - mid nuls) suivent ceux du chemin CPU : voir
// dessiner_shader.
// La matrice passe en quatre lignes float4, comme le decor (p_shader_decor.cpp :
// une float4x4 arrivait decalee).
static const char *s_src_prt_v =
	"void main(\n"
	"	float4 aR,\n"
	"	float aT,\n"
	"	float2 aCoin,\n"
	"	uniform float4 uL0,\n"
	"	uniform float4 uL1,\n"
	"	uniform float4 uL2,\n"
	"	uniform float4 uL3,\n"
	"	uniform float4 uP0,\n"
	"	uniform float4 uP1,\n"
	"	uniform float4 uP2,\n"
	"	uniform float4 uS0,\n"
	"	uniform float4 uS1,\n"
	"	uniform float4 uS2,\n"
	"	uniform float4 uDroite,\n"
	"	uniform float4 uHaut,\n"
	"	uniform float4 uC0,\n"
	"	uniform float4 uC1,\n"
	"	uniform float4 uC2,\n"
	"	uniform float4 uT,\n"
	"	uniform float4 uFogP,\n"
	"	uniform float4 uFogC,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float4 out vColor : COLOR,\n"
	"	float4 out vFog : TEXCOORD4)\n"
	"{\n"
	"	float t = aT;\n"
	"	float tt = t * t;\n"
	"	float4 r = 1.f + aR * (65535.f / 16384.f);\n"
	"	float4 pos = uP0 + t * uP1 + tt * uP2 + (uS0 + t * uS1 + tt * uS2) * r;\n"
	"	float2 c = float2(aCoin.x * 2.f - 1.f, 1.f - aCoin.y * 2.f) * pos.w;\n"
	"	float4 q = float4(pos.xyz + c.x * uDroite.xyz + c.y * uHaut.xyz, 1.f);\n"
	"	vPosition = float4(dot(uL0, q), dot(uL1, q), dot(uL2, q), dot(uL3, q));\n"
	"	vTexcoord = aCoin;\n"
	"	float seg = 1.f - step(t, uT.x);\n"
	"	float ci = lerp(t * uT.y, (t - uT.x) * uT.z + uT.w, seg);\n"
	"	vColor = saturate(lerp(lerp(uC0, uC1, seg), lerp(uC1, uC2, seg), ci));\n"
	BROUILLARD_VS( "dot(uL3, q)" )
	"}\n";

// PixelShader0 : texture x couleur de sommet (le x2 est dans la couleur),
// puis le brouillard comme les shaders du decor.
static const char *s_src_prt_f =
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float4 vColor : COLOR,\n"
	"	float4 vFog : TEXCOORD4,\n"
	"	uniform sampler2D uTex)\n"
	"{\n"
	"	float4 c = tex2D(uTex, vTexcoord) * vColor;\n"
	"	return float4(c.rgb * vFog.a + vFog.rgb, c.a);\n"
	"}\n";

enum { PRT_ATTR_R = 0, PRT_ATTR_T = 1, PRT_ATTR_COIN = 2 };

static int    s_etat_shd = 0;		// 0 jamais tente, 1 pret, -1 echec
static GLuint s_prog_shd = 0;
static GLint  s_u_l[4], s_u_p[3], s_u_s[3], s_u_c[3], s_u_droite, s_u_haut, s_u_t;
static GLint  s_u_fogp, s_u_fogc;

static bool shader_prt_pret( void )
{
	if( s_etat_shd != 0 )
		return ( s_etat_shd > 0 );
	s_etat_shd = -1;

	GLuint vs = NxVita::CompilerShaderCache( GL_CG_VERTEX_SHADER_EXT, s_src_prt_v, "particules sommets" );
	GLuint fs = NxVita::CompilerShaderCache( GL_CG_FRAGMENT_SHADER_EXT, s_src_prt_f, "particules pixels" );
	if( !vs || !fs )
	{
		VLOG( "PRT", "!! shaders refuses : prt 4 retombe sur le calcul CPU" );
		return false;
	}
	s_prog_shd = glCreateProgram();
	glAttachShader( s_prog_shd, vs );
	glAttachShader( s_prog_shd, fs );
	glBindAttribLocation( s_prog_shd, PRT_ATTR_R,    "aR" );
	glBindAttribLocation( s_prog_shd, PRT_ATTR_T,    "aT" );
	glBindAttribLocation( s_prog_shd, PRT_ATTR_COIN, "aCoin" );
	glLinkProgram( s_prog_shd );
	GLint ok = 0;
	glGetProgramiv( s_prog_shd, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		VLOG( "PRT", "!! edition de liens refusee : prt 4 retombe sur le calcul CPU" );
		return false;
	}
	static const char *noms_l[4] = { "uL0", "uL1", "uL2", "uL3" };
	static const char *noms_p[3] = { "uP0", "uP1", "uP2" };
	static const char *noms_s[3] = { "uS0", "uS1", "uS2" };
	static const char *noms_c[3] = { "uC0", "uC1", "uC2" };
	for( int k = 0; k < 4; ++k )
		s_u_l[k] = glGetUniformLocation( s_prog_shd, noms_l[k] );
	for( int k = 0; k < 3; ++k )
	{
		s_u_p[k] = glGetUniformLocation( s_prog_shd, noms_p[k] );
		s_u_s[k] = glGetUniformLocation( s_prog_shd, noms_s[k] );
		s_u_c[k] = glGetUniformLocation( s_prog_shd, noms_c[k] );
	}
	s_u_droite = glGetUniformLocation( s_prog_shd, "uDroite" );
	s_u_haut   = glGetUniformLocation( s_prog_shd, "uHaut" );
	s_u_t      = glGetUniformLocation( s_prog_shd, "uT" );
	s_u_fogp   = glGetUniformLocation( s_prog_shd, "uFogP" );
	s_u_fogc   = glGetUniformLocation( s_prog_shd, "uFogC" );
	GLint prog_avant = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog_avant );
	glUseProgram( s_prog_shd );
	glUniform1i( glGetUniformLocation( s_prog_shd, "uTex" ), 0 );
	glUseProgram( prog_avant );
	VLOG( "PRT", "programme particules pret (prt 4 : calcul par vertex shader)" );
	s_etat_shd = 1;
	return true;
}

static bool assurer_tampons_shd( void )
{
	if( s_tampons_shd_ok )
		return true;
	// Alloue UNE fois, jamais libere : memoire CPU, que vitaGL recopie.
	sp_sommets_shd = (SPrtSommetShd *)malloc( sizeof( SPrtSommetShd ) * 4 * PRT_LOT );
	if( !sp_sommets_shd )
	{
		VLOG( "PRT", "tampon shader de particules : allocation impossible" );
		return false;
	}
	memset( sp_sommets_shd, 0, sizeof( SPrtSommetShd ) * 4 * PRT_LOT );
	// Coins fixes, poses une fois : uv (0,0) (1,0) (1,1) (0,1), comme le
	// chemin CPU.
	static const unsigned char uv[4][2] = { { 0, 0 }, { 255, 0 }, { 255, 255 }, { 0, 255 } };
	for( int q = 0; q < PRT_LOT; ++q )
		for( int k = 0; k < 4; ++k )
		{
			sp_sommets_shd[q * 4 + k].coin[0] = uv[k][0];
			sp_sommets_shd[q * 4 + k].coin[1] = uv[k][1];
		}
	s_tampons_shd_ok = true;
	return true;
}

// Couleur Image::RGBA -> float4 x 2 / 255 (le x2 de PixelShader0).
static inline void couleur_shd( const Image::RGBA &c, float out[4] )
{
	const float k = 2.0f / 255.0f;
	out[0] = (float)c.r * k;
	out[1] = (float)c.g * k;
	out[2] = (float)c.b * k;
	out[3] = (float)c.a * k;
}


/*****************************************************************************
**  CVitaNewParticle                                                        **
*****************************************************************************/

CVitaNewParticle::CVitaNewParticle( void )
{
	m_emitting			= false;
	m_max_streams		= 0;
	m_num_streams		= 0;
	mp_stream			= NULL;
	mp_newest_stream	= NULL;
	mp_oldest_stream	= NULL;
	m_blend				= PRT_DIFFUSE;
	++s_vivants;
	++s_crees;
}

CVitaNewParticle::~CVitaNewParticle( void )
{
	plat_destroy();
	--s_vivants;
	++s_detruits;
}

// [SOURCE] XBox/p_nxnewparticle.cpp:653, update_position.
void CVitaNewParticle::update_position( void )
{
	const float t1 = m_params.m_Lifetime * m_params.m_MidpointPct * 0.01f;
	const float t2 = m_params.m_Lifetime;
	Mth::Vector u, a_;

	Mth::Vector x0 = m_params.m_BoxPos[0];
	x0[3]          = m_params.m_Radius[0];
	Mth::Vector x1 = m_params.m_BoxPos[1];
	x1[3]          = m_params.m_Radius[1];
	Mth::Vector x2 = m_params.m_BoxPos[2];
	x2[3]          = m_params.m_Radius[2];

	if( m_params.m_UseMidpoint )
	{
		u  = ( t2 * t2 * ( x1 - x0 ) - t1 * t1 * ( x2 - x0 )) / ( t1 * t2 * ( t2 - t1 ));
		a_ = ( t1 * ( x2 - x0 ) - t2 * ( x1 - x0 )) / ( t1 * t2 * ( t2 - t1 ));
	}
	else
	{
		u  = ( x2 - x0 ) / t2;
		a_.Set( 0.0f, 0.0f, 0.0f, 0.0f );
	}

	m_p0    = x0 - 1.5f * m_s0;
	m_p1    = u  - 1.5f * m_s1;
	m_p2    = a_ - 1.5f * m_s2;
	m_p0[3] = x0[3] - 1.5f * m_s0[3];
	m_p1[3] = u[3]  - 1.5f * m_s1[3];
	m_p2[3] = a_[3] - 1.5f * m_s2[3];
}

void CVitaNewParticle::plat_update( void )
{
	if( m_params.m_LocalCoord )
		update_position();
}

// [SOURCE] XBox/p_nxnewparticle.cpp:707, plat_build. Le materiau XBox n'a pas
// d'equivalent ici : le mode de melange est resolu une fois, la texture est
// relue a chaque image (voir l'en-tete).
void CVitaNewParticle::plat_build( void )
{
	// "Reduce emit rate selectively to improve performance." (XBox). Garde :
	// c'est ce debit que montre xemu, reference visuelle.
	m_params.m_EmitRate = m_params.m_EmitRate * 0.5f;

	// 5 flux, quel que soit m_MaxStreams (XBox l'ignore aussi).
	m_max_streams		= 5;
	m_num_streams		= 0;
	mp_stream			= new CVitaParticleStream[m_max_streams];
	memset( mp_stream, 0, sizeof( CVitaParticleStream ) * m_max_streams );
	mp_newest_stream	= mp_stream + m_max_streams - 1;
	mp_oldest_stream	= mp_stream;
	m_emitting			= false;

	m_blend = famille_melange( m_params.m_BlendMode );

	// 3 points -> PVA, pour l'etendue des boites (et l'ecart de rayon en w).
	const float t1 = m_params.m_Lifetime * m_params.m_MidpointPct * 0.01f;
	const float t2 = m_params.m_Lifetime;
	Mth::Vector x0, x1, x2, u, a_;

	x0    = m_params.m_BoxDims[0];
	x0[3] = m_params.m_RadiusSpread[0];
	x1    = m_params.m_BoxDims[1];
	x1[3] = m_params.m_RadiusSpread[1];
	x2    = m_params.m_BoxDims[2];
	x2[3] = m_params.m_RadiusSpread[2];

	if( m_params.m_UseMidpoint )
	{
		u  = ( t2 * t2 * ( x1 - x0 ) - t1 * t1 * ( x2 - x0 )) / ( t1 * t2 * ( t2 - t1 ));
		a_ = ( t1 * ( x2 - x0 ) - t2 * ( x1 - x0 )) / ( t1 * t2 * ( t2 - t1 ));
	}
	else
	{
		u  = ( x2 - x0 ) / t2;
		a_.Set( 0.0f, 0.0f, 0.0f, 0.0f );
	}

	m_s0 = x0;
	m_s1 = u;
	m_s2 = a_;

	// Puis les centres (m_p*), recentres de -1,5 x l'etendue : les nombres
	// aleatoires valent [1, 2], donc le decalage final couvre [-0,5, +0,5].
	update_position();

	{
		Mth::Vector c = m_bsphere;
		VLOG( "PRT", "systeme %s : tex %s, melange %08x, %.1f/s (XBox /2), vie %.2f s, "
		             "rayons %.0f %.0f %.0f, sphere (%.0f %.0f %.0f) r %.0f%s",
		      Script::FindChecksumName( m_params.m_Name ),
		      Script::FindChecksumName( m_params.m_Texture ),
		      (unsigned)m_params.m_BlendMode, m_params.m_EmitRate, m_params.m_Lifetime,
		      m_params.m_Radius[0], m_params.m_Radius[1], m_params.m_Radius[2],
		      c[X], c[Y], c[Z], c[W], m_params.m_LocalCoord ? ", local" : "" );
	}
}

void CVitaNewParticle::plat_destroy( void )
{
	if( mp_stream )
	{
		delete [] mp_stream;
		mp_stream = NULL;
	}
	mp_newest_stream = NULL;
	mp_oldest_stream = NULL;
	m_num_streams    = 0;
}

void CVitaNewParticle::plat_hide( bool should_hide )
{
	// Rien : CNewParticle::Render ne dessine deja plus un systeme cache, et
	// le XBox ne surcharge pas cette fonction (le stub portable ne fait
	// qu'imprimer).
}

// [SOURCE] XBox/p_nxnewparticle.cpp:136-232. Le pas de temps est celui de
// l'image (Tmr::FrameLength) au lieu du 1/60 fixe du XBox, qui supposait une
// image par vsync : a 30 i/s la vapeur monterait a mi-vitesse. A 60 i/s, les
// deux sont egaux. Rend false s'il n'y a rien a dessiner.
bool CVitaNewParticle::avancer_flux( float dt )
{
	if( !mp_stream )
		return false;

	if( m_params.m_EmitRate && ( !m_emitting || ( m_params.m_EmitRate != mp_newest_stream->m_rate )))
	{
		if( m_num_streams < m_max_streams )
		{
			m_num_streams++;
			mp_newest_stream++;
			if( mp_newest_stream == mp_stream + m_max_streams )
				mp_newest_stream = mp_stream;

			mp_newest_stream->m_rate			= m_params.m_EmitRate;
			mp_newest_stream->m_interval		= 1.0f / m_params.m_EmitRate;
			mp_newest_stream->m_oldest_age		= 0.0f;
			mp_newest_stream->m_num_particles	= 0;
			mp_newest_stream->m_rand_seed		= (uint32)rand();
			mp_newest_stream->m_rand_a			= 314159265;
			mp_newest_stream->m_rand_b			= 178453311;
			m_emitting = true;
		}
		else
		{
			m_emitting = false;
		}
	}
	else
	{
		m_emitting = ( m_params.m_EmitRate != 0.0f );
	}

	if( !m_num_streams )
		return false;

	// Vieillissement de tous les flux.
	CVitaParticleStream *p_stream = mp_oldest_stream;
	for( int i = 0; i < m_num_streams; ++i )
	{
		p_stream->m_oldest_age += dt;
		p_stream++;
		if( p_stream == mp_stream + m_max_streams )
			p_stream = mp_stream;
	}

	// Naissances dans le flux le plus recent.
	if( m_emitting )
		mp_newest_stream->m_num_particles = (int)( mp_newest_stream->m_oldest_age * mp_newest_stream->m_rate + 1.0f );

	// Morts dans le plus ancien.
	if( mp_oldest_stream->m_oldest_age > m_params.m_Lifetime )
	{
		const int particles_dead = (int)(( mp_oldest_stream->m_oldest_age - m_params.m_Lifetime ) * mp_oldest_stream->m_rate + 1.0f );

		mp_oldest_stream->m_num_particles -= particles_dead;

		if( mp_oldest_stream->m_num_particles > 0 || ( m_num_streams == 1 && m_emitting ))
		{
			mp_oldest_stream->m_oldest_age -= (float)particles_dead * mp_oldest_stream->m_interval;
			mp_oldest_stream->AdvanceSeed( particles_dead );
		}
		else
		{
			m_num_streams--;
			mp_oldest_stream++;
			if( mp_oldest_stream == mp_stream + m_max_streams )
				mp_oldest_stream = mp_stream;
			if( !m_num_streams )
				return false;
		}
	}
	return true;
}

void CVitaNewParticle::plat_render( void )
{
	if( !NxVita::g_vita_particules || !s_image_ok )
		return;

	// Simulation AVANT le test de visibilite, comme XBox : un systeme hors
	// champ continue de vivre.
	float dt = Tmr::FrameLength();
	if( dt < 0.0f )		dt = 0.0f;
	if( dt > 0.1f )		dt = 0.1f;
	if( !avancer_flux( dt ))
		return;

	// Systemes fixes : test de la sphere englobante (XBox :234). Les systemes
	// en coordonnees locales bougent, leur sphere ne veut rien dire.
	if( !m_params.m_LocalCoord
	    && !NxVita::SphereVisible( m_bsphere[X], m_bsphere[Y], m_bsphere[Z], m_bsphere[W] ))
	{
		++s_sys_hors_champ;
		return;
	}

	dessiner();
}

void CVitaNewParticle::dessiner( void )
{
	// Texture : relue a chaque image (voir l'en-tete). Sans elle le XBox
	// echantillonnait une unite vide ; on ne dessine rien et on le compte.
	GLuint gl_tex = 0;
	if( CTexDictManager::sp_particle_tex_dict )
	{
		CTexture *p_tex = CTexDictManager::sp_particle_tex_dict->GetTexture( m_params.m_Texture );
		if( p_tex )
			gl_tex = static_cast<CVitaTexture *>( p_tex )->GetGLTexture();
	}
	if( !gl_tex )
	{
		++s_sans_tex;
		return;
	}
	if( !poser_melange( m_blend ))
		return;
	glBindTexture( GL_TEXTURE_2D, gl_tex );
	if( s_shader_image )
	{
		dessiner_shader();
		return;
	}
	NxVita::BrouillardFixeNoir(( m_blend == PRT_ADD ) || ( m_blend == PRT_SUB ));

	// Limite des point sprites XBox (prt 1) : distance du centre de la
	// sphere a la camera, comme le choix de technique du XBox (:252).
	bool borne = false;
	if( NxVita::g_vita_particules == 1 )
	{
		const float dx = m_bsphere[X] - s_cam[0];
		const float dy = m_bsphere[Y] - s_cam[1];
		const float dz = m_bsphere[Z] - s_cam[2];
		borne = ( dx * dx + dy * dy + dz * dz ) >= 57600.0f;
	}

	const SceUInt64 t_boucle = sceKernelGetProcessTimeWide();
	const float midpoint_time = m_params.m_Lifetime * ( m_params.m_ColorMidpointPct * 0.01f );
	int n_lot = 0;
	++s_sys_dessines;

	CVitaParticleStream *p_stream = mp_oldest_stream;
	for( int i = 0; i < m_num_streams; i++, p_stream++ )
	{
		if( p_stream == mp_stream + m_max_streams )
			p_stream = mp_stream;

		float t = p_stream->m_oldest_age;
		// Etat du generateur en registres (meme suite que particle_rnd, XBox
		// :373) : la version a variables statiques coutait ~1 us/particule.
		uint32 rs = p_stream->m_rand_seed, ra = p_stream->m_rand_a, rb = p_stream->m_rand_b;
		const float inv_mid  = ( midpoint_time > 1e-6f ) ? 1.0f / midpoint_time : 0.0f;
		const float d_fin    = m_params.m_Lifetime - midpoint_time;
		const float inv_fin  = ( d_fin > 1e-6f ) ? 1.0f / d_fin : 0.0f;
		const float inv_vie  = ( m_params.m_Lifetime > 1e-6f ) ? 1.0f / m_params.m_Lifetime : 0.0f;
		const float k_rnd    = 1.0f / 16384.0f;

		for( int p = 0; p < p_stream->m_num_particles; ++p )
		{
			// Plafond par image (calcul CPU ~1 us/particule sur Cortex-A9 :
			// 4575 particules a Vancouver coutaient 4,5 ms). Leve quand le
			// calcul passera dans le vertex shader, comme sur XBox.
			if( s_budget_restant <= 0 )
				break;
			--s_budget_restant;
			// 4 nombres dans [1, 2].
			float r[4];
			for( int k = 0; k < 4; ++k )
			{
				rs = rs * ra + rb;
				ra = ( ra ^ rs ) + ( (uint32)( (int32)rs >> 4 ));
				rb = rb + ( (uint32)( (int32)rs >> 3 ) - 0x10101010u );
				r[k] = 1.0f + (float)((int)(( rs & 0xffff ) * 16384 ) >> 16 ) * k_rnd;
			}

			float ci;
			const Image::RGBA *c0, *c1;
			if( m_params.m_UseMidcolor )
			{
				if( t > midpoint_time )
				{
					ci = ( d_fin > 1e-6f ) ? ( t - midpoint_time ) * inv_fin : 1.0f;
					c0 = &m_params.m_Color[1];
					c1 = &m_params.m_Color[2];
				}
				else
				{
					ci = t * inv_mid;
					c0 = &m_params.m_Color[0];
					c1 = &m_params.m_Color[1];
				}
			}
			else
			{
				ci = t * inv_vie;
				c0 = &m_params.m_Color[0];
				c1 = &m_params.m_Color[2];
			}

			// ParticleNewFlatVS.vsh : pos = p0 + t p1 + t^2 p2
			//                             + ( s0 + t s1 + t^2 s2 ) * r, w = rayon.
			const float tt = t * t;
			float pos[4];
			for( int k = 0; k < 4; ++k )
				pos[k] = m_p0[k] + t * m_p1[k] + tt * m_p2[k]
				       + ( m_s0[k] + t * m_s1[k] + tt * m_s2[k] ) * r[k];
			float w = pos[3];

			if( borne )
			{
				const float dx = pos[0] - s_cam[0];
				const float dy = pos[1] - s_cam[1];
				const float dz = pos[2] - s_cam[2];
				const float lim = sqrtf( dx * dx + dy * dy + dz * dz ) * ( 1.0f / 15.0f );
				if( w > lim )		w = lim;
				else if( w < -lim )	w = -lim;
			}

			uint32 coul;
			{
				unsigned char rgba[4];
				rgba[0] = couleur_x2( c0->r + ( (float)c1->r - (float)c0->r ) * ci );
				rgba[1] = couleur_x2( c0->g + ( (float)c1->g - (float)c0->g ) * ci );
				rgba[2] = couleur_x2( c0->b + ( (float)c1->b - (float)c0->b ) * ci );
				rgba[3] = couleur_x2( c0->a + ( (float)c1->a - (float)c0->a ) * ci );
				memcpy( &coul, rgba, 4 );
			}

			const float rx = s_droite[0] * w, ry = s_droite[1] * w, rz = s_droite[2] * w;
			const float hx = s_haut[0] * w,   hy = s_haut[1] * w,   hz = s_haut[2] * w;

			SPrtSommet *s = sp_sommets + n_lot * 4;
			s[0].x = pos[0] - rx + hx; s[0].y = pos[1] - ry + hy; s[0].z = pos[2] - rz + hz;
			s[1].x = pos[0] + rx + hx; s[1].y = pos[1] + ry + hy; s[1].z = pos[2] + rz + hz;
			s[2].x = pos[0] + rx - hx; s[2].y = pos[1] + ry - hy; s[2].z = pos[2] + rz - hz;
			s[3].x = pos[0] - rx - hx; s[3].y = pos[1] - ry - hy; s[3].z = pos[2] - rz - hz;
			// u, v : fixes, poses une fois a l'allocation (assurer_tampons).
			memcpy( s[0].rgba, &coul, 4 ); memcpy( s[1].rgba, &coul, 4 );
			memcpy( s[2].rgba, &coul, 4 ); memcpy( s[3].rgba, &coul, 4 );

			if( ++n_lot == PRT_LOT )
			{
				if( NxVita::g_vita_particules != 3 )	// 3 : calcul seul (mesure)
					glDrawElements( GL_TRIANGLES, n_lot * 6, GL_UNSIGNED_SHORT, sp_indices );
				s_particules += n_lot;
				++s_appels;
				n_lot = 0;
			}

			t -= p_stream->m_interval;
		}
	}

	s_boucle_us += sceKernelGetProcessTimeWide() - t_boucle;
	if( n_lot )
	{
		if( NxVita::g_vita_particules != 3 )	// 3 : calcul seul (mesure)
			glDrawElements( GL_TRIANGLES, n_lot * 6, GL_UNSIGNED_SHORT, sp_indices );
		s_particules += n_lot;
		++s_appels;
	}
}

// Chemin shader (prt 4). Programme, matrice, repere d'ecran et tableaux de
// sommets sont poses par RenderVita ; texture et melange par dessiner. Ici :
// les uniformes du systeme, puis les tirages et le temps de chaque particule.
void CVitaNewParticle::dessiner_shader( void )
{
	// Brouillard : noir pour Add et Sub, comme le chemin CPU (render.cpp:1177).
	{
		float P[4], C[4];
		NxVita::BrouillardUniformes(( m_blend == PRT_ADD ) || ( m_blend == PRT_SUB ), false, P, C );
		glUniform4fv( s_u_fogp, 1, P );
		glUniform4fv( s_u_fogc, 1, C );
	}
	{
		const Mth::Vector *p[3] = { &m_p0, &m_p1, &m_p2 };
		const Mth::Vector *s[3] = { &m_s0, &m_s1, &m_s2 };
		for( int k = 0; k < 3; ++k )
		{
			glUniform4f( s_u_p[k], (*p[k])[X], (*p[k])[Y], (*p[k])[Z], (*p[k])[W] );
			glUniform4f( s_u_s[k], (*s[k])[X], (*s[k])[Y], (*s[k])[Z], (*s[k])[W] );
		}
	}
	// Couleur : memes cas que la boucle CPU (dessiner).
	{
		const float midpoint_time = m_params.m_Lifetime * ( m_params.m_ColorMidpointPct * 0.01f );
		float c[3][4], T[4];
		if( m_params.m_UseMidcolor )
		{
			const float d_fin = m_params.m_Lifetime - midpoint_time;
			couleur_shd( m_params.m_Color[0], c[0] );
			couleur_shd( m_params.m_Color[1], c[1] );
			couleur_shd( m_params.m_Color[2], c[2] );
			T[0] = midpoint_time;
			T[1] = ( midpoint_time > 1e-6f ) ? 1.0f / midpoint_time : 0.0f;
			// CPU : ci = 1 quand la seconde moitie est de duree nulle.
			T[2] = ( d_fin > 1e-6f ) ? 1.0f / d_fin : 0.0f;
			T[3] = ( d_fin > 1e-6f ) ? 0.0f : 1.0f;
		}
		else
		{
			couleur_shd( m_params.m_Color[0], c[0] );
			couleur_shd( m_params.m_Color[2], c[1] );
			couleur_shd( m_params.m_Color[2], c[2] );
			T[0] = 1.0e30f;		// seg toujours 0
			T[1] = ( m_params.m_Lifetime > 1e-6f ) ? 1.0f / m_params.m_Lifetime : 0.0f;
			T[2] = 0.0f;
			T[3] = 0.0f;
		}
		for( int k = 0; k < 3; ++k )
			glUniform4fv( s_u_c[k], 1, c[k] );
		glUniform4fv( s_u_t, 1, T );
	}

	const SceUInt64 t_boucle = sceKernelGetProcessTimeWide();
	int n_lot = 0;
	++s_sys_dessines;

	CVitaParticleStream *p_stream = mp_oldest_stream;
	for( int i = 0; i < m_num_streams; i++, p_stream++ )
	{
		if( p_stream == mp_stream + m_max_streams )
			p_stream = mp_stream;

		float t = p_stream->m_oldest_age;
		uint32 rs = p_stream->m_rand_seed, ra = p_stream->m_rand_a, rb = p_stream->m_rand_b;

		for( int p = 0; p < p_stream->m_num_particles; ++p )
		{
			if( s_budget_restant <= 0 )
				break;
			--s_budget_restant;
			// Les 4 tirages, entiers bruts 0..16383 (meme suite que dessiner).
			uint32 n[4];
			for( int k = 0; k < 4; ++k )
			{
				rs = rs * ra + rb;
				ra = ( ra ^ rs ) + ( (uint32)( (int32)rs >> 4 ));
				rb = rb + ( (uint32)( (int32)rs >> 3 ) - 0x10101010u );
				n[k] = (uint32)((int)(( rs & 0xffff ) * 16384 ) >> 16 );
			}
			const unsigned short r16[4] = { (unsigned short)n[0], (unsigned short)n[1],
			                                (unsigned short)n[2], (unsigned short)n[3] };

			SPrtSommetShd *s = sp_sommets_shd + n_lot * 4;
			for( int k = 0; k < 4; ++k )
			{
				// Le coin (s[k].coin) est fixe, pose a l'allocation.
				memcpy( s[k].r, r16, sizeof( r16 ));
				s[k].t = t;
			}

			if( ++n_lot == PRT_LOT )
			{
				glDrawRangeElements( GL_TRIANGLES, 0, n_lot * 4 - 1, n_lot * 6, GL_UNSIGNED_SHORT, sp_indices );
				s_particules += n_lot;
				++s_appels;
				n_lot = 0;
			}

			t -= p_stream->m_interval;
		}
	}

	s_boucle_us += sceKernelGetProcessTimeWide() - t_boucle;
	if( n_lot )
	{
		glDrawRangeElements( GL_TRIANGLES, 0, n_lot * 4 - 1, n_lot * 6, GL_UNSIGNED_SHORT, sp_indices );
		s_particules += n_lot;
		++s_appels;
	}
}


/*****************************************************************************
**  CVitaNewParticleManager                                                 **
*****************************************************************************/

CNewParticle *CVitaNewParticleManager::plat_create_particle( void )
{
	return new CVitaNewParticle;
}

static void normaliser( float v[3] )
{
	const float l = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
	if( l > 1e-6f )
	{
		v[0] /= l; v[1] /= l; v[2] /= l;
	}
}

// Repere d'ecran d'une image, a partir de la vue du monde. Partage avec les
// anciennes particules (p_NxParticle.cpp), qui suivent la meme regle XBox
// (p_nxparticleflat.cpp:198 : droite = at x Y, haut = droite x at).
static void repere_ecran( const float v[16], float droite[3], float haut[3], float at[3], float cam[3] )
{
	// Repere d'ecran. La vue (p_world_render.cpp, RenderWorld) a pour
	// LIGNES les axes camera : droite (v0 v4 v8), haut (v1 v5 v9), arriere
	// (v2 v6 v10), regard = -arriere (OpenGL).
	const float ref_droite[3] = { v[0], v[4], v[8] };
	const float ref_haut[3]   = { v[1], v[5], v[9] };
	at[0] = -v[2]; at[1] = -v[6]; at[2] = -v[10];

	// XBox :259 : droite = at x Y du monde, haut = droite x at. Le sens
	// depend de la main du repere : on l'aligne sur celui de l'ecran, pour
	// que l'uv (0,0) tombe en haut a gauche.
	droite[0] = -at[2];	// at x (0,1,0)
	droite[1] = 0.0f;
	droite[2] = at[0];
	if(( droite[0] * droite[0] + droite[2] * droite[2] ) < 1e-8f )
	{
		// Regard vertical : le XBox produirait un vecteur nul. Repere camera.
		memcpy( droite, ref_droite, 3 * sizeof( float ));
	}
	normaliser( droite );
	if(( droite[0] * ref_droite[0] + droite[1] * ref_droite[1] + droite[2] * ref_droite[2] ) < 0.0f )
	{
		droite[0] = -droite[0]; droite[1] = -droite[1]; droite[2] = -droite[2];
	}
	haut[0] = droite[1] * at[2] - droite[2] * at[1];
	haut[1] = droite[2] * at[0] - droite[0] * at[2];
	haut[2] = droite[0] * at[1] - droite[1] * at[0];
	normaliser( haut );
	if(( haut[0] * ref_haut[0] + haut[1] * ref_haut[1] + haut[2] * ref_haut[2] ) < 0.0f )
	{
		haut[0] = -haut[0]; haut[1] = -haut[1]; haut[2] = -haut[2];
	}

	// Position camera = -R^T t.
	for( int c = 0; c < 3; ++c )
		cam[c] = -( v[c * 4 + 0] * v[12] + v[c * 4 + 1] * v[13] + v[c * 4 + 2] * v[14] );
}

void CVitaNewParticleManager::RenderVita( void )
{
	const SceUInt64 t0 = sceKernelGetProcessTimeWide();
	s_image_ok = false;
	s_shader_image = false;

	float v[16];
	if( NxVita::g_vita_particules && assurer_tampons() && NxVita::GetViewMatrix( v ))
	{
		float at[3];
		repere_ecran( v, s_droite, s_haut, at, s_cam );

		// prt 4 : chemin shader, si le programme et son tampon sont prets ;
		// sinon repli sur le calcul CPU (comme prt 2).
		s_shader_image = ( NxVita::g_vita_particules == 4 )
		                 && shader_prt_pret() && assurer_tampons_shd();

		// Etat commun. Projection : celle du monde, laissee par RenderWorld ;
		// on la repose quand meme, elle ne coute qu'un chargement de matrice.
		NxVita::SetWorldProjection();
		glMatrixMode( GL_MODELVIEW );
		glPushMatrix();
		glLoadMatrixf( v );

		// Fonction de test : celle que laisse RenderWorld, non modifiee (les
		// translucides des modeles, dessines juste apres, en heritent).
		glEnable( GL_DEPTH_TEST );
		glDepthMask( GL_FALSE );		// XBox/p_nx.cpp:396
		glDisable( GL_CULL_FACE );		// m_no_bfc
		glDisable( GL_ALPHA_TEST );
		glDisable( GL_LIGHTING );
		glEnable( GL_BLEND );

		glActiveTexture( GL_TEXTURE0 );
		glClientActiveTexture( GL_TEXTURE0 );
		glEnable( GL_TEXTURE_2D );
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );

		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
		if( s_shader_image )
		{
			// mvp = projection x vue, en colonnes ; passee en quatre lignes.
			float proj[16], mvp[16];
			glGetFloatv( GL_PROJECTION_MATRIX, proj );
			for( int c = 0; c < 4; ++c )
				for( int r = 0; r < 4; ++r )
				{
					float s = 0.0f;
					for( int k = 0; k < 4; ++k )
						s += proj[k * 4 + r] * v[c * 4 + k];
					mvp[c * 4 + r] = s;
				}
			glUseProgram( s_prog_shd );
			for( int r = 0; r < 4; ++r )
				glUniform4f( s_u_l[r], mvp[r], mvp[4 + r], mvp[8 + r], mvp[12 + r] );
			glUniform4f( s_u_droite, s_droite[0], s_droite[1], s_droite[2], 0.0f );
			glUniform4f( s_u_haut, s_haut[0], s_haut[1], s_haut[2], 0.0f );
			glEnableVertexAttribArray( PRT_ATTR_R );
			glEnableVertexAttribArray( PRT_ATTR_T );
			glEnableVertexAttribArray( PRT_ATTR_COIN );
			glVertexAttribPointer( PRT_ATTR_R, 4, GL_UNSIGNED_SHORT, GL_TRUE,
			                       sizeof( SPrtSommetShd ), sp_sommets_shd[0].r );
			glVertexAttribPointer( PRT_ATTR_T, 1, GL_FLOAT, GL_FALSE,
			                       sizeof( SPrtSommetShd ), &sp_sommets_shd[0].t );
			glVertexAttribPointer( PRT_ATTR_COIN, 2, GL_UNSIGNED_BYTE, GL_TRUE,
			                       sizeof( SPrtSommetShd ), sp_sommets_shd[0].coin );
			s_budget_restant = g_vita_prt_budget_shd;
		}
		else
		{
			glEnableClientState( GL_VERTEX_ARRAY );
			glEnableClientState( GL_TEXTURE_COORD_ARRAY );
			glEnableClientState( GL_COLOR_ARRAY );
			glVertexPointer( 3, GL_FLOAT, sizeof( SPrtSommet ), &sp_sommets[0].x );
			glTexCoordPointer( 2, GL_FLOAT, sizeof( SPrtSommet ), &sp_sommets[0].u );
			glColorPointer( 4, GL_UNSIGNED_BYTE, sizeof( SPrtSommet ), sp_sommets[0].rgba );
			s_budget_restant = g_vita_prt_budget;
			NxVita::BrouillardFixe( 1 );
		}
		s_image_ok = true;

		RenderParticles();

		s_image_ok = false;
		if( s_shader_image )
		{
			glDisableVertexAttribArray( PRT_ATTR_R );
			glDisableVertexAttribArray( PRT_ATTR_T );
			glDisableVertexAttribArray( PRT_ATTR_COIN );
			glUseProgram( 0 );
		}
		else
		{
			NxVita::BrouillardFixe( 0 );
			glDisableClientState( GL_COLOR_ARRAY );
			glDisableClientState( GL_TEXTURE_COORD_ARRAY );
			glDisableClientState( GL_VERTEX_ARRAY );
		}
		glBindTexture( GL_TEXTURE_2D, 0 );
		glDisable( GL_TEXTURE_2D );
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glDisable( GL_BLEND );
		glDepthMask( GL_TRUE );
		glMatrixMode( GL_MODELVIEW );
		glPopMatrix();
	}

	s_cout_us += sceKernelGetProcessTimeWide() - t0;
	if(( ++s_images % 600 ) == 0 )
	{
		VLOG( "PRT", "prt %d (%s, plafond %d) : systemes vivants %d (crees %d, detruits %d) ; "
		             "par image : %.1f dessines, %.1f hors champ, %.1f sans texture, "
		             "%.1f particules, %.1f appels ; %.3f ms/image",
		      NxVita::g_vita_particules,
		      !NxVita::g_vita_particules ? "arret" : ( s_shader_image ? "shader" : "cpu" ),
		      s_shader_image ? g_vita_prt_budget_shd : g_vita_prt_budget,
		      s_vivants, s_crees, s_detruits,
		      s_sys_dessines / 600.0f, s_sys_hors_champ / 600.0f, s_sans_tex / 600.0f,
		      s_particules / 600.0f, s_appels / 600.0f, s_cout_us / 600000.0f );
		VLOG( "PRT", "  dont boucles de particules %.3f ms/image", s_boucle_us / 600000.0f );
		s_boucle_us = 0;
		s_sys_dessines = s_sys_hors_champ = s_sans_tex = s_particules = s_appels = 0;
		s_cout_us = 0;
	}
}

} // namespace Nx

/*****************************************************************************
**  Outils partages avec les anciennes particules (p_NxParticle.cpp)       **
*****************************************************************************/

namespace NxVita
{
uint32 PrtFamilleMelange( uint32 checksum )		{ return Nx::famille_melange( checksum ); }
bool   PrtPoserMelange( uint32 famille )		{ return Nx::poser_melange( famille ); }
bool   PrtMelangeNoir( uint32 famille )			{ return ( famille == Nx::PRT_ADD ) || ( famille == Nx::PRT_SUB ); }
unsigned char PrtCouleurX2( float c )			{ return Nx::couleur_x2( c ); }
void   PrtRepereEcran( const float v[16], float droite[3], float haut[3], float at[3], float cam[3] )
{
	Nx::repere_ecran( v, droite, haut, at, cam );
}
} // namespace NxVita
