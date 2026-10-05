///////////////////////////////////////////////////////////////////////////////
// p_ombre.cpp -- ombre portee "detailed" du skater (issues #45 V2 et #4).
// Voir p_ombre.h pour le contrat XBox et le pourquoi de la methode.
//
// Chaine d'une image :
//   phase logique   CDetailedShadow::UpdatePosition -> OmbreCamera (lumiere)
//                   CVitaGeom::plat_render -> OmbreCapturer (racine + os)
//   image suivante  s_plat_pre_render -> OmbreCarteRendu : carte de la pose
//                   capturee, dans le FBO 256x256, avant la scene principale
//                   RenderWorld, fin de la passe opaque -> reception sur le
//                   decor, avec la camera de lumiere COURANTE
//
// Pourquoi la camera courante a la reception et non celle de la carte : la
// carte est rendue relativement a P (position de la camera de lumiere), et P
// suit la cible (pos + up x 36, shadowcomponent.cpp:212). Le contenu de la
// carte est donc invariant par translation du skater ; le recevoir autour du
// P courant recale l'ombre sur la position courante, seule la POSE a une image
// de retard. Avec la camera de la carte, toute l'ombre trainerait d'une image.
//
// GARDE-FOU : un plantage GPU eteint la console. On reste sur vitaGL (pas de
// chemin GXM direct), avec la meme structure de programme que le skinning
// eprouve (uB[192], s_src_peau_v de p_shader_decor.cpp), et tout est coupe
// par "omb 0".

#include <core/defines.h>
#include <gfx/image/imagebasic.h>
#include <gfx/NxModel.h>
#include <gfx/camera.h>

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <math.h>
#include <string.h>

#include "vita_log.h"
#include "p_NxModel.h"
#include "p_NxTexture.h"
#include "p_ombre.h"
#include <psp2/gxm.h>

extern "C"
{
	extern SceGxmContext *gxm_context;
	extern GLboolean      is_rendering_display;
}
namespace NxVita { bool GxmListeOuverte( void ); }
namespace NxVita { extern bool g_vita_zeq; }	// p_world_render.cpp, « zeq »

namespace NxVita
{

// p_shader_decor.cpp : compilation CG avec le cache disque ux0:data/thug/shd.
GLuint CompilerShaderCache( GLenum type, const char *p_source, const char *p_nom );

int g_vita_ombre = 1;	// valide sur console le 2026-10-03 (Manhattan, xemu), #45			// a 1 une fois valide a l'ecran
float g_vita_ombre_pf = -2.0f;		// XBox render.cpp:2328, SLOPESCALE
float g_vita_ombre_pu = -4.0f;		// XBox render.cpp:2327, ZOFFSET
// Auto-ombrage du skater (#45, p_ombre.h, "aom"). Coupe tant que non valide
// a l'ecran. Part d'ombre : XBox instance.cpp:547.
// "omd X" : decalage du PCF 4 points en texels (#69), 0 = echantillon unique.
float g_vita_ombre_douce = 0.0f;	// xemu : bords NETS (creneles), pas plus doux -- laisse a 0
// "omc 0/1" (#70) : decoupe de la reception aux 4 bords lateraux de la boite
// de lumiere par plans de decoupe materiels (sorties CLP du programme de
// sommets). Sans elle, chaque maillage recepteur -- des sols entiers a
// Vancouver -- etait redessine sur toute sa surface a l'ecran, avec 5
// lectures de texture par pixel, pour un resultat k = 1 (aucun effet) hors
// de la carte : ~9 ms de GPU a Vancouver, ~5 a Hawaii. Exact : hors de la
// carte, le shader rend deja k = 1 (masque "hors").
// COUPEE PAR DEFAUT (2026-10-05) : en jeu, les plans CLP provoquent des
// paquets de 3 a 5 images de ~200 ms passees a attendre le GPU dans
// vglSwapBuffers (traceur [ACC] : CPU ~15 ms, swap ~185 ms). A/B en partie
// automatique a New Jersey : 24 images de 200 ms sur 6 parties de 90 s avec
// la decoupe, 0 sur 2 parties sans (et 0 sans ombre du tout).
int   g_vita_ombre_decoupe = 0;
// "omr 0/1" (#73) : la meme economie que #70 sans plans CLP -- la reception
// n'est rasterisee que dans le rectangle ecran de la boite de lumiere, par la
// region de decoupe en tuiles de GXM (sceGxmSetRegionClip, granularite 32x32,
// cout nul). Pas de glScissor : vitaGL le realise par deux quads de stencil
// a chaque changement. Hors de la carte le shader rend deja k = 1 : un
// rectangle trop large reste exact.
int   g_vita_ombre_region = 1;
static bool s_region_posee = false;
static int  s_region_w = 0, s_region_h = 0;
static unsigned int s_region_n = 0, s_region_plein = 0, s_region_pix = 0;
int   g_vita_auto_ombre = 1;	// valide sur console le 2026-10-05 (Manhattan, aom 2 sans acne, cout nul)
float g_vita_aom_b0 = 1.5f;
float g_vita_aom_b1 = 1.0f;
float g_vita_aom_k  = 0.25f;

// [SOURCE] shadow.cpp:42 : sCreateRenderTargetTexture( 256, 256, 16, 16 ).
#define OMB_TAILLE		256
// [SOURCE] XBox/NX/render.cpp:1789 : XGMatrixOrthoRH( 96, 96, 1, 128 ).
#define OMB_DEMI		48.0f
#define OMB_PRES		1.0f
#define OMB_LOIN		128.0f
#define OMB_MAX_PROJ	4		// table texture -> modele
#define OMB_MAX_CAPT	32		// geoms captures (le skater en a ~15-20)
#define OMB_MAX_JEUX	8		// jeux d'os copies (un par modele et par image)
#define OMB_MAX_OS		64		// = PEAU_MAX_OS, uB[192]


/*****************************************************************************
**  Table des projections (XBox : pTextureProjectionDetailsTable)            **
*****************************************************************************/

struct SProjection
{
	Nx::CTexture *	p_tex;		// NULL = entree libre
	GLuint			gl_tex;
	GLuint			fbo;
	int				fbo_etat;	// 0 jamais tente, 1 pret, -1 echec
	Nx::CModel *	p_modele;
	bool			cam_ok;
	float			P[3], r[3], u[3], f[3];
	unsigned int	carte_img;	// image ou la carte a ete rendue (0 = jamais)
	int				n_geoms;	// geoms dessines dans la derniere carte
	// Auto-ombrage (#45) : lignes espace modele -> ( u, v, profondeur ) de la
	// carte, celles du premier geom dessine dans la derniere carte.
	float			aom[12];
	bool			aom_ok;
};

static SProjection s_proj[OMB_MAX_PROJ];

// Numero d'image : incremente au debut de chaque image (OmbreCarteRendu).
// Une capture porte le numero de l'image pendant laquelle elle a ete faite.
static unsigned int s_image = 1;

static SProjection *trouver( const Nx::CTexture *p_tex )
{
	if( !p_tex )
		return NULL;
	for( int i = 0; i < OMB_MAX_PROJ; ++i )
		if( s_proj[i].p_tex == p_tex )
			return &s_proj[i];
	return NULL;
}


/*****************************************************************************
**  Captures de pose                                                         **
*****************************************************************************/

struct SJeuOs
{
	const float *	p_src;		// adresse d'origine (cle de partage)
	unsigned int	img;
	int				n;
	float			m[OMB_MAX_OS * 16];
};

struct SCapture
{
	const void *			p_geom;		// NULL = libre
	const Nx::CModel *		p_modele;
	const Nx::CVitaMesh *	p_mesh;
	float					racine[16];
	int						jeu;		// indice dans s_jeux
	unsigned int			img;
};

static SCapture s_capt[OMB_MAX_CAPT];
static SJeuOs   s_jeux[OMB_MAX_JEUX];

bool OmbreModeleSuivi( const Nx::CModel *p_modele )
{
	if( !g_vita_ombre || !p_modele )
		return false;
	for( int i = 0; i < OMB_MAX_PROJ; ++i )
		if( s_proj[i].p_tex && ( s_proj[i].p_modele == p_modele ))
			return true;
	return false;
}

void OmbreCapturer( const void *p_geom, const Nx::CModel *p_modele,
                    const Nx::CVitaMesh *p_mesh, const float *p_racine,
                    const float *p_os, int num_os )
{
	if( !p_geom || !p_mesh || !p_racine || !p_os || ( num_os <= 0 ) || ( num_os > OMB_MAX_OS ))
		return;

	// Jeu d'os : les geoms d'un meme modele partagent le tableau de matrices
	// du squelette, on ne le copie qu'une fois par image.
	int jeu = -1;
	for( int j = 0; j < OMB_MAX_JEUX; ++j )
		if(( s_jeux[j].img == s_image ) && ( s_jeux[j].p_src == p_os ) && ( s_jeux[j].n == num_os ))
		{
			jeu = j;
			break;
		}
	if( jeu < 0 )
	{
		// Le plus ancien qui ne sert pas a cette image.
		unsigned int plus_vieux = 0xFFFFFFFFu;
		for( int j = 0; j < OMB_MAX_JEUX; ++j )
			if(( s_jeux[j].img != s_image ) && ( s_jeux[j].img < plus_vieux ))
			{
				plus_vieux = s_jeux[j].img;
				jeu = j;
			}
		if( jeu < 0 )
			return;		// plus de place : ce geom manquera a la carte
		s_jeux[jeu].p_src = p_os;
		s_jeux[jeu].img   = s_image;
		s_jeux[jeu].n     = num_os;
		memcpy( s_jeux[jeu].m, p_os, sizeof( float ) * 16 * num_os );
	}

	// Emplacement : celui du geom, sinon un libre ou perime.
	int k = -1;
	for( int i = 0; i < OMB_MAX_CAPT; ++i )
		if( s_capt[i].p_geom == p_geom )
		{
			k = i;
			break;
		}
	if( k < 0 )
		for( int i = 0; i < OMB_MAX_CAPT; ++i )
			if( !s_capt[i].p_geom || (( s_image - s_capt[i].img ) > 2 ))
			{
				k = i;
				break;
			}
	if( k < 0 )
		return;
	SCapture *c = &s_capt[k];
	c->p_geom   = p_geom;
	c->p_modele = p_modele;
	c->p_mesh   = p_mesh;
	memcpy( c->racine, p_racine, sizeof( c->racine ));
	c->jeu      = jeu;
	c->img      = s_image;
}

void OmbreOublierGeom( const void *p_geom )
{
	for( int i = 0; i < OMB_MAX_CAPT; ++i )
		if( s_capt[i].p_geom == p_geom )
			s_capt[i].p_geom = NULL;
}


/*****************************************************************************
**  Contrat CEngine                                                          **
*****************************************************************************/

Nx::CTexture *OmbreCreerCible( int largeur, int hauteur )
{
	// Toujours 256x256 : c'est la seule taille demandee par le moteur
	// (shadow.cpp:42), et la projection suppose une carte carree.
	(void)largeur; (void)hauteur;

	GLint act_avant = 0, tex_avant = 0;
	glGetIntegerv( GL_ACTIVE_TEXTURE, &act_avant );
	glActiveTexture( GL_TEXTURE0 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex_avant );
	GLuint t = 0;
	glGenTextures( 1, &t );
	glBindTexture( GL_TEXTURE_2D, t );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, OMB_TAILLE, OMB_TAILLE, 0,
	              GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	// LINEAR : le filtrage bilineaire de la carte tient lieu du PCF 2x2 de la
	// Xbox (D3DTSS_MAGFILTER/MINFILTER LINEAR, render.cpp:2286). CLAMP : le
	// "bord blanc" XBox (BORDERCOLOR 0xffffffff, hors carte = eclaire) est
	// fait dans le shader de reception.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, tex_avant );
	glActiveTexture( act_avant );

	Nx::CVitaTexture *p_tex = new Nx::CVitaTexture();
	p_tex->SetGLTexture( t, OMB_TAILLE, OMB_TAILLE, false, 0 );
	VLOG( "OMB", "cible d'ombre creee : texture GL %u, %dx%d (demande %dx%d)",
	      (unsigned)t, OMB_TAILLE, OMB_TAILLE, largeur, hauteur );
	return p_tex;
}

void OmbreProjeter( Nx::CTexture *p_tex, Nx::CModel *p_modele )
{
	if( !p_tex )
		return;
	SProjection *p = trouver( p_tex );
	if( !p )
		for( int i = 0; i < OMB_MAX_PROJ; ++i )
			if( !s_proj[i].p_tex )
			{
				p = &s_proj[i];
				break;
			}
	if( !p )
	{
		VLOG( "OMB", "!! table des projections pleine (%d) : ombre ignoree", OMB_MAX_PROJ );
		return;
	}
	memset( p, 0, sizeof( *p ));
	p->p_tex    = p_tex;
	p->gl_tex   = static_cast<Nx::CVitaTexture *>( p_tex )->GetGLTexture();
	p->p_modele = p_modele;
	VLOG( "OMB", "projection : texture GL %u -> modele %p (%d geoms)",
	      (unsigned)p->gl_tex, (void *)p_modele, p_modele ? p_modele->GetNumGeoms() : 0 );
}

static void normaliser( float v[3] )
{
	const float n = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
	if( n > 1e-12f )
	{
		v[0] /= n; v[1] /= n; v[2] /= n;
	}
}

static void produit_vectoriel( float o[3], const float a[3], const float b[3] )
{
	o[0] = a[1] * b[2] - a[2] * b[1];
	o[1] = a[2] * b[0] - a[0] * b[2];
	o[2] = a[0] * b[1] - a[1] * b[0];
}

void OmbreCamera( Nx::CTexture *p_tex, Gfx::Camera *p_cam )
{
	SProjection *p = trouver( p_tex );
	if( !p || !p_cam )
		return;

	// [SOURCE] XBox/p_nx.cpp:993 : at = pos + matrice[Z] ; puis
	// set_texture_projection_camera (render.cpp:1773) : LookAtRH( pos, at,
	// up ), up = (0,0,1) si la direction est verticale, (0,1,0) sinon.
	Mth::Vector &pos = p_cam->GetPos();
	Mth::Matrix &m   = p_cam->GetMatrix();
	float f[3] = { m[Z][X], m[Z][Y], m[Z][Z] };
	normaliser( f );
	// XBox teste l'egalite exacte (pos.x == at.x && pos.z == at.z) ; la
	// tolerance couvre en plus le quasi-vertical, ou up = Y rendrait une
	// base degeneree.
	const bool verticale = ( fabsf( f[0] ) < 1e-4f ) && ( fabsf( f[2] ) < 1e-4f );
	const float up[3] = { 0.0f, verticale ? 0.0f : 1.0f, verticale ? 1.0f : 0.0f };
	// LookAtRH : zaxis = normal( eye - at ) = -f ; xaxis = normal( up x zaxis ) ;
	// yaxis = zaxis x xaxis.
	const float za[3] = { -f[0], -f[1], -f[2] };
	float r[3], u[3];
	produit_vectoriel( r, up, za );
	normaliser( r );
	produit_vectoriel( u, za, r );

	p->P[0] = pos[X]; p->P[1] = pos[Y]; p->P[2] = pos[Z];
	memcpy( p->f, f, sizeof( f ));
	memcpy( p->r, r, sizeof( r ));
	memcpy( p->u, u, sizeof( u ));
	if( !p->cam_ok )
		VLOG( "OMB", "camera de lumiere : P (%.0f %.0f %.0f) f (%.2f %.2f %.2f)%s",
		      p->P[0], p->P[1], p->P[2], f[0], f[1], f[2], verticale ? " verticale" : "" );
	p->cam_ok = true;
}

void OmbreArreter( Nx::CTexture *p_tex )
{
	SProjection *p = trouver( p_tex );
	if( !p )
		return;
	// [SOURCE] XBox destroy_texture_projection_details (render.cpp:1755).
	// Le FBO part ici ; la texture, avec le CVitaTexture (~CDetailedShadow
	// le detruit juste apres). vitaGL differe les liberations GPU.
	if( p->fbo )
		glDeleteFramebuffers( 1, &p->fbo );
	VLOG( "OMB", "projection arretee : texture GL %u", (unsigned)p->gl_tex );
	memset( p, 0, sizeof( *p ));
}


/*****************************************************************************
**  Programmes                                                               **
*****************************************************************************/

// Ecriture de la carte. Skinning IDENTIQUE a s_src_peau_v (p_shader_decor.cpp)
// -- meme tableau uB[192], memes 3 os par sommet --, mais les lignes uL0..3
// sont la matrice ortho de la lumiere x la racine du modele. Sortie :
// r = profondeur 0..1 ( (f.(w-P) - 1) / 127, le z D3D de l'ortho XBox ),
// g = 1 (couverture). Carte effacee a (0,0,0,0).
static const char *s_src_carte_v =
	"void main(\n"
	"	float3 aPos,\n"
	"	float3 aPoids,\n"
	"	float3 aOs,\n"
	"	uniform float4 uL0,\n"
	"	uniform float4 uL1,\n"
	"	uniform float4 uL2,\n"
	"	uniform float4 uL3,\n"
	"	uniform float4 uB[192],\n"
	"	float4 out vPosition : POSITION,\n"
	"	float out vD : TEXCOORD0)\n"
	"{\n"
	"	float4 p = float4(aPos, 1.f);\n"
	"	int3 o = (int3)min(aOs, float3(63.f, 63.f, 63.f)) * 3;\n"
	"	float3 s = aPoids.x * float3(dot(uB[o.x], p), dot(uB[o.x + 1], p), dot(uB[o.x + 2], p))\n"
	"	         + aPoids.y * float3(dot(uB[o.y], p), dot(uB[o.y + 1], p), dot(uB[o.y + 2], p))\n"
	"	         + aPoids.z * float3(dot(uB[o.z], p), dot(uB[o.z + 1], p), dot(uB[o.z + 2], p));\n"
	"	float4 q = float4(s, 1.f);\n"
	"	float4 c = float4(dot(uL0, q), dot(uL1, q), dot(uL2, q), dot(uL3, q));\n"
	"	vPosition = c;\n"
	"	vD = c.z * 0.5f + 0.5f;\n"
	"}\n";

static const char *s_src_carte_f =
	"float4 main(\n"
	"	float vD : TEXCOORD0)\n"
	"{\n"
	"	return float4(vD, 1.f, 0.f, 1.f);\n"
	"}\n";

// Reception sur le decor. Position : exactement le calcul du decor
// (dot(uL0..3, p) avec s_pv_shader). vS = coordonnees dans la carte (x, y) et
// profondeur du point vue de la lumiere (z), par uS0..2.
// vClip (#70) : plans de decoupe utilisateur (semantique CLP, comme le
// pipeline fixe de vitaGL, ffp_v.h) -- le materiel coupe chaque triangle
// recepteur aux bords de la carte, 0 <= vS.x, vS.y <= 1, a uDecoupe pres
// (marge de quelques texels ; 1e4 = decoupe coupee, "omc 0").
//
// [SOURCE] ShadowBufferStaticGeomPS.psh (XBox/NX) :
//   r0.rgb = t1.rgb + 0.6        (t1 = 1 eclaire, 0 ombre ; sature)
//   r0.a   = ( 1 - t1.b ) x t0.a (alpha test a 1 : pas d'ombre sur les
//                                 texels transparents du materiau)
// puis melange MODULATE_COLOR : dst x r0.rgb. Ici s = part d'ombre 0..1,
// k = min( 1, 1.6 - s ), ecrit en couleur et multiplie par glBlendFunc(
// GL_ZERO, GL_SRC_COLOR ). Pas de discard : k = 1 laisse le pixel intact.
//
// Comparaison : m.r / m.g = profondeur moyenne des texels couverts autour du
// point (filtrage bilineaire, m.g = couverture) ; ombre si le point est plus
// loin que l'occultant de plus d'une unite (1/127).
static const char *s_src_rec_v =
	"void main(\n"
	"	float3 aPosition,\n"
	"	float2 aTexcoord,\n"
	"	uniform float4 uL0,\n"
	"	uniform float4 uL1,\n"
	"	uniform float4 uL2,\n"
	"	uniform float4 uL3,\n"
	"	uniform float4 uS0,\n"
	"	uniform float4 uS1,\n"
	"	uniform float4 uS2,\n"
	"	uniform float uDecoupe,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float3 out vS : TEXCOORD1,\n"
	"	float out vClip[4] : CLP0)\n"
	"{\n"
	"	float4 p = float4(aPosition, 1.f);\n"
	"	vPosition = float4(dot(uL0, p), dot(uL1, p), dot(uL2, p), dot(uL3, p));\n"
	"	vTexcoord = aTexcoord;\n"
	"	vS = float3(dot(uS0, p), dot(uS1, p), dot(uS2, p));\n"
	"	vClip[0] = vS.x + uDecoupe;\n"
	"	vClip[1] = 1.f - vS.x + uDecoupe;\n"
	"	vClip[2] = vS.y + uDecoupe;\n"
	"	vClip[3] = 1.f - vS.y + uDecoupe;\n"
	"}\n";

static const char *s_src_rec_f =
		// PCF 4 points (#69) : bord adouci comme la reference xemu ; uDoux = 0 ->
	// l'ancien echantillon unique (commande "omd 0").
	"	float ombre_tap(sampler2D t, float2 uv, float z) {\n"
	"		float4 m = tex2D(t, uv);\n"
	"		float occ = m.r / max(m.g, 1.f / 255.f);\n"
	"		return m.g * step(occ + 1.f / 127.f, z);\n"
	"	}\n"
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float3 vS : TEXCOORD1,\n"
	"	uniform sampler2D uTex,\n"
	"	uniform sampler2D uOmbre,\n"
	"	uniform float uMasque,\n"
	"	uniform float uDoux)\n"
	"{\n"
	"	float z = saturate(vS.z);\n"
	"	float2 d = float2(uDoux / 256.f, uDoux / 256.f);\n"
	"	float s = 0.25f * (ombre_tap(uOmbre, vS.xy + float2(-d.x, -d.y), z) + ombre_tap(uOmbre, vS.xy + float2(d.x, -d.y), z)\n"
	"	                 + ombre_tap(uOmbre, vS.xy + float2(-d.x, d.y), z) + ombre_tap(uOmbre, vS.xy + float2(d.x, d.y), z));\n"
	"	float2 hors = step(float2(1.f, 1.f), vS.xy) + step(vS.xy, float2(0.f, 0.f));\n"
	"	s = s * (1.f - saturate(hors.x + hors.y));\n"
	"	float k = min(1.f, 1.6f - s);\n"
	"	float a = tex2D(uTex, vTexcoord).a;\n"
	"	float garde = max(1.f - uMasque, step(1.f / 255.f, s * a));\n"
	"	k = lerp(1.f, k, garde);\n"
	"	return float4(k, k, k, 1.f);\n"
	"}\n";

// Vue de la carte ("omb 2/3") : r = profondeur, g = couverture.
static const char *s_src_vue_v =
	"void main(\n"
	"	float2 aPosition,\n"
	"	float2 aTexcoord,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0)\n"
	"{\n"
	"	vPosition = float4(aPosition, 0.f, 1.f);\n"
	"	vTexcoord = aTexcoord;\n"
	"}\n";

static const char *s_src_vue_f =
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	uniform sampler2D uImage)\n"
	"{\n"
	"	float4 t = tex2D(uImage, vTexcoord);\n"
	"	return float4(t.r, t.g, t.g * 0.25f, 1.f);\n"
	"}\n";

enum { CA_POS = 0, CA_POIDS = 1, CA_OS = 2 };
enum { RE_POS = 0, RE_UV = 1 };
enum { VU_POS = 0, VU_UV = 1 };

static GLuint lier( const char *p_vs, const char *p_fs, const char *p_nom,
                    const char **pp_attr, const int *p_loc, int n_attr )
{
	char nom[64];
	snprintf( nom, sizeof( nom ), "%s sommets", p_nom );
	GLuint vs = CompilerShaderCache( GL_CG_VERTEX_SHADER_EXT, p_vs, nom );
	snprintf( nom, sizeof( nom ), "%s pixels", p_nom );
	GLuint fs = CompilerShaderCache( GL_CG_FRAGMENT_SHADER_EXT, p_fs, nom );
	if( !vs || !fs )
	{
		VLOG( "OMB", "!! shaders '%s' refuses", p_nom );
		return 0;
	}
	GLuint prog = glCreateProgram();
	glAttachShader( prog, vs );
	glAttachShader( prog, fs );
	for( int i = 0; i < n_attr; ++i )
		glBindAttribLocation( prog, p_loc[i], pp_attr[i] );
	glLinkProgram( prog );
	GLint ok = 0;
	glGetProgramiv( prog, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		VLOG( "OMB", "!! edition de liens '%s' refusee", p_nom );
		return 0;
	}
	return prog;
}

static int    s_etat_carte = 0;		// 0 jamais tente, 1 pret, -1 echec
static GLuint s_prog_carte = 0;
static GLint  s_ca_l[4], s_ca_b;

static int    s_etat_rec = 0;
static GLuint s_prog_rec = 0;
static GLint  s_re_l[4], s_re_s[3], s_re_masque, s_re_doux = -1, s_re_decoupe = -1;

static int    s_etat_vue = 0;
static GLuint s_prog_vue = 0;
static GLuint s_vbo_vue  = 0;

static const char *s_noms_l[4] = { "uL0", "uL1", "uL2", "uL3" };

static bool carte_prete( void )
{
	if( s_etat_carte != 0 )
		return ( s_etat_carte > 0 );
	s_etat_carte = -1;
	const char *attr[3] = { "aPos", "aPoids", "aOs" };
	const int   loc[3]  = { CA_POS, CA_POIDS, CA_OS };
	s_prog_carte = lier( s_src_carte_v, s_src_carte_f, "ombre carte", attr, loc, 3 );
	if( !s_prog_carte )
		return false;
	for( int k = 0; k < 4; ++k )
		s_ca_l[k] = glGetUniformLocation( s_prog_carte, s_noms_l[k] );
	s_ca_b = glGetUniformLocation( s_prog_carte, "uB" );
	VLOG( "OMB", "programme de la carte pret" );
	s_etat_carte = 1;
	return true;
}

static bool reception_prete( void )
{
	if( s_etat_rec != 0 )
		return ( s_etat_rec > 0 );
	s_etat_rec = -1;
	const char *attr[2] = { "aPosition", "aTexcoord" };
	const int   loc[2]  = { RE_POS, RE_UV };
	s_prog_rec = lier( s_src_rec_v, s_src_rec_f, "ombre reception", attr, loc, 2 );
	if( !s_prog_rec )
		return false;
	for( int k = 0; k < 4; ++k )
		s_re_l[k] = glGetUniformLocation( s_prog_rec, s_noms_l[k] );
	s_re_s[0]   = glGetUniformLocation( s_prog_rec, "uS0" );
	s_re_s[1]   = glGetUniformLocation( s_prog_rec, "uS1" );
	s_re_s[2]   = glGetUniformLocation( s_prog_rec, "uS2" );
	s_re_masque = glGetUniformLocation( s_prog_rec, "uMasque" );
	s_re_doux   = glGetUniformLocation( s_prog_rec, "uDoux" );
	s_re_decoupe = glGetUniformLocation( s_prog_rec, "uDecoupe" );
	GLint prog_avant = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog_avant );
	glUseProgram( s_prog_rec );
	glUniform1i( glGetUniformLocation( s_prog_rec, "uTex" ), 0 );
	glUniform1i( glGetUniformLocation( s_prog_rec, "uOmbre" ), 1 );
	glUseProgram( prog_avant );
	VLOG( "OMB", "programme de reception pret (decoupe CLP : uniforme %d)", (int)s_re_decoupe );
	s_etat_rec = 1;
	return true;
}

static bool vue_prete( void )
{
	if( s_etat_vue != 0 )
		return ( s_etat_vue > 0 );
	s_etat_vue = -1;
	const char *attr[2] = { "aPosition", "aTexcoord" };
	const int   loc[2]  = { VU_POS, VU_UV };
	s_prog_vue = lier( s_src_vue_v, s_src_vue_f, "ombre vue", attr, loc, 2 );
	if( !s_prog_vue )
		return false;
	GLint prog_avant = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog_avant );
	glUseProgram( s_prog_vue );
	glUniform1i( glGetUniformLocation( s_prog_vue, "uImage" ), 0 );
	glUseProgram( prog_avant );

	// Coin haut droit, 128x128 pixels sur 960x544, marge 8 pixels. Bandes :
	// 4 sommets V normal, puis 4 V retourne ("omb 3"). x, y, u, v.
	const float x1 = 1.0f - 16.0f / 960.0f, x0 = x1 - 256.0f / 960.0f;
	const float y1 = 1.0f - 16.0f / 544.0f, y0 = y1 - 256.0f / 544.0f;
	const float quad[2][16] = {
		{ x0, y0, 0.f, 0.f,   x1, y0, 1.f, 0.f,   x0, y1, 0.f, 1.f,   x1, y1, 1.f, 1.f },
		{ x0, y0, 0.f, 1.f,   x1, y0, 1.f, 1.f,   x0, y1, 0.f, 0.f,   x1, y1, 1.f, 0.f } };
	GLint buf_avant = 0;
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buf_avant );
	glGenBuffers( 1, &s_vbo_vue );
	glBindBuffer( GL_ARRAY_BUFFER, s_vbo_vue );
	glBufferData( GL_ARRAY_BUFFER, sizeof( quad ), quad, GL_STATIC_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, buf_avant );
	s_etat_vue = 1;
	return true;
}

static bool fbo_pret( SProjection *p )
{
	if( p->fbo_etat != 0 )
		return ( p->fbo_etat > 0 );
	p->fbo_etat = -1;
	if( !p->gl_tex )
		return false;
	GLint fb_avant = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fb_avant );
	glGenFramebuffers( 1, &p->fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, p->fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p->gl_tex, 0 );
	const GLenum statut = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	glBindFramebuffer( GL_FRAMEBUFFER, fb_avant );
	if( statut != GL_FRAMEBUFFER_COMPLETE )
	{
		VLOG( "OMB", "!! FBO d'ombre incomplet (0x%x) : pas d'ombre", (unsigned)statut );
		glDeleteFramebuffers( 1, &p->fbo );
		p->fbo = 0;
		return false;
	}
	VLOG( "OMB", "FBO d'ombre pret (texture GL %u)", (unsigned)p->gl_tex );
	p->fbo_etat = 1;
	return true;
}


/*****************************************************************************
**  Passe 1 : la carte (render_shadow_targets, render.cpp:2119)              **
*****************************************************************************/

static unsigned long long s_us_carte = 0, s_us_rec = 0, s_us_rec_t = 0;
static int s_n_rec = 0, s_n_rec_t = 0;

void OmbreCompterReception( unsigned long long us, int maillages, bool translucides )
{
	if( translucides )
	{
		s_us_rec_t += us;
		s_n_rec_t  += maillages;
		return;
	}
	s_us_rec += us;
	s_n_rec  += maillages;
}

// Lignes de la matrice ortho de la lumiere sur un point MONDE homogene :
// clip = ( r.(w-P)/48, u.(w-P)/48, 2d - 1, 1 ), d = ( f.(w-P) - 1 ) / 127.
static void lignes_lumiere( const SProjection *p, float C[4][4] )
{
	const float rP = p->r[0] * p->P[0] + p->r[1] * p->P[1] + p->r[2] * p->P[2];
	const float uP = p->u[0] * p->P[0] + p->u[1] * p->P[1] + p->u[2] * p->P[2];
	const float fP = p->f[0] * p->P[0] + p->f[1] * p->P[1] + p->f[2] * p->P[2];
	const float kz = 2.0f / ( OMB_LOIN - OMB_PRES );
	for( int a = 0; a < 3; ++a )
	{
		C[0][a] = p->r[a] / OMB_DEMI;
		C[1][a] = p->u[a] / OMB_DEMI;
		C[2][a] = p->f[a] * kz;
		C[3][a] = 0.0f;
	}
	C[0][3] = -rP / OMB_DEMI;
	C[1][3] = -uP / OMB_DEMI;
	C[2][3] = -( fP + OMB_PRES ) * kz - 1.0f;
	C[3][3] = 1.0f;
}

static float s_os_lignes[OMB_MAX_OS * 3 * 4];

static int dessiner_carte( SProjection *p, unsigned int img_capt )
{
	float C[4][4];
	lignes_lumiere( p, C );
	int n = 0;
	for( int i = 0; i < OMB_MAX_CAPT; ++i )
	{
		const SCapture *c = &s_capt[i];
		if( !c->p_geom || ( c->img != img_capt ) || ( c->p_modele != p->p_modele ))
			continue;
		const SJeuOs *j = &s_jeux[c->jeu];
		if( j->img != img_capt )
			continue;
		const Nx::SGpuMesh *pieces = c->p_mesh->Pieces();
		const int np = c->p_mesh->NumPieces();
		if( !pieces || ( np <= 0 ))
			continue;
		// Pieces rigides (vehicules) : pas par ce chemin.
		bool ok = true;
		for( int k = 0; ( k < np ) && ok; ++k )
			if( pieces[k].sector_bone >= 0 )
				ok = false;
		if( !ok )
			continue;

		// Lignes = lumiere x racine : la racine Mth (lignes = axes du modele en
		// coordonnees monde, translation en ligne 3) amene q (espace modele)
		// en monde : w_c = sum_a q_a R[a][c] + R[3][c].
		const float *R = c->racine;
		float LL[4][4];
		for( int l = 0; l < 4; ++l )
		{
			float *L = LL[l];
			for( int a = 0; a < 4; ++a )
			{
				float s = 0.0f;
				for( int cc = 0; cc < 3; ++cc )
					s += C[l][cc] * R[a * 4 + cc];
				L[a] = s;
			}
			L[3] += C[l][3];
			glUniform4f( s_ca_l[l], L[0], L[1], L[2], L[3] );
		}
		// Auto-ombrage (#45) : les memes lignes, ramenees aux coordonnees de
		// texture de la carte -- u = 0,5 x + 0,5, v = 0,5 y + 0,5 (convention
		// des FBO vitaGL, celle de la reception), profondeur = vD du shader
		// de la carte. w = 1 (ortho), d'ou le 0,5 en translation.
		if( n == 0 )
		{
			for( int l = 0; l < 3; ++l )
				for( int a = 0; a < 4; ++a )
					p->aom[l * 4 + a] = 0.5f * LL[l][a] + (( a == 3 ) ? 0.5f : 0.0f );
			p->aom_ok = true;
		}
		// Os : meme rangement que ShaderPeauDebut (p_shader_decor.cpp).
		memset( s_os_lignes, 0, sizeof( s_os_lignes ));
		for( int b = 0; b < j->n; ++b )
		{
			const float *m = &j->m[b * 16];
			for( int cc = 0; cc < 3; ++cc )
			{
				float *l = &s_os_lignes[( b * 3 + cc ) * 4];
				l[0] = m[0 * 4 + cc];
				l[1] = m[1 * 4 + cc];
				l[2] = m[2 * 4 + cc];
				l[3] = m[3 * 4 + cc];
			}
		}
		glUniform4fv( s_ca_b, OMB_MAX_OS * 3, s_os_lignes );

		for( int k = 0; k < np; ++k )
		{
			const Nx::SGpuMesh *q = &pieces[k];
			if( !q->vbo_repos || !q->vbo_poids || !q->vbo_os || !q->ibo || ( q->num_indices <= 0 ))
				continue;
			if( q->decal )		// passe 1 (#55) : meme geometrie que sa base
				continue;
			glBindBuffer( GL_ARRAY_BUFFER, q->vbo_repos );
			glVertexAttribPointer( CA_POS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
			glBindBuffer( GL_ARRAY_BUFFER, q->vbo_poids );
			glVertexAttribPointer( CA_POIDS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
			glBindBuffer( GL_ARRAY_BUFFER, q->vbo_os );
			glVertexAttribPointer( CA_OS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
			glBindBuffer( GL_ARRAY_BUFFER, 0 );
			glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, q->ibo );
			glDrawElements( GL_TRIANGLE_STRIP, q->num_indices, GL_UNSIGNED_SHORT, NULL );
		}
		++n;
	}
	return n;
}

void OmbreCarteRendu( void )
{
	const unsigned int img_capt = s_image;	// captures de la phase logique passee
	++s_image;
	if( s_image == 0 )
		s_image = 1;
	if( !g_vita_ombre )
		return;

	const SceUInt64 t0 = sceKernelGetProcessTimeWide();
	bool etat_sauve = false;
	GLint vp[4] = { 0, 0, 960, 544 }, fb_avant = 0, prog_avant = 0, act_avant = 0;

	for( int i = 0; i < OMB_MAX_PROJ; ++i )
	{
		SProjection *p = &s_proj[i];
		if( !p->p_tex || !p->p_modele || !p->cam_ok )
			continue;
		// Y a-t-il une pose ? (modele non dessine a l'image passee : rien)
		bool pose = false;
		for( int k = 0; ( k < OMB_MAX_CAPT ) && !pose; ++k )
			if( s_capt[k].p_geom && ( s_capt[k].img == img_capt ) && ( s_capt[k].p_modele == p->p_modele ))
				pose = true;
		if( !pose )
			continue;
		if( !carte_prete() || !fbo_pret( p ))
			continue;

		if( !etat_sauve )
		{
			etat_sauve = true;
			glGetIntegerv( GL_VIEWPORT, vp );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fb_avant );
			glGetIntegerv( GL_CURRENT_PROGRAM, &prog_avant );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &act_avant );
		}

		// [SOURCE] render.cpp:2150-2172 : cible, effacement, pas de culling
		// (vRENDER_NO_CULLING), pas de brouillard (le programme n'en a pas).
		// Pas de polygon offset : il servait l'auto-ombrage du skater, que le
		// decalage d'une unite de la reception remplace ici.
		glBindFramebuffer( GL_FRAMEBUFFER, p->fbo );
		glViewport( 0, 0, OMB_TAILLE, OMB_TAILLE );
		glDisable( GL_BLEND );
		glDisable( GL_ALPHA_TEST );
		glDisable( GL_CULL_FACE );
		glDisable( GL_POLYGON_OFFSET_FILL );
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LESS );
		glDepthMask( GL_TRUE );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

		glUseProgram( s_prog_carte );
		glEnableVertexAttribArray( CA_POS );
		glEnableVertexAttribArray( CA_POIDS );
		glEnableVertexAttribArray( CA_OS );
		p->n_geoms = dessiner_carte( p, img_capt );
		glDisableVertexAttribArray( CA_POS );
		glDisableVertexAttribArray( CA_POIDS );
		glDisableVertexAttribArray( CA_OS );
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
		if( p->n_geoms > 0 )
			p->carte_img = s_image;
	}

	if( etat_sauve )
	{
		// Etat rendu. Le viewport est repose AVANT de quitter le FBO : vitaGL
		// le reapplique a l'ouverture de la scene suivante (scene_reset). La
		// couleur d'effacement est reposee par s_plat_pre_render juste apres.
		glUseProgram( prog_avant );
		glActiveTexture( act_avant );
		glViewport( vp[0], vp[1], vp[2], vp[3] );
		glBindFramebuffer( GL_FRAMEBUFFER, fb_avant );
	}

	s_us_carte += sceKernelGetProcessTimeWide() - t0;

	// Journal : camera et cible toutes les 120 images, cout toutes les 600.
	static int s_n = 0;
	++s_n;
	if(( s_n % 120 ) == 0 )
		for( int i = 0; i < OMB_MAX_PROJ; ++i )
		{
			const SProjection *p = &s_proj[i];
			if( !p->p_tex )
				continue;
			VLOG( "OMB", "ombre %d : P (%.0f %.0f %.0f) f (%.2f %.2f %.2f) cible (%.0f %.0f %.0f), "
			             "carte %s (%d geoms)",
			      i, p->P[0], p->P[1], p->P[2], p->f[0], p->f[1], p->f[2],
			      p->P[0] + p->f[0] * 72.0f, p->P[1] + p->f[1] * 72.0f, p->P[2] + p->f[2] * 72.0f,
			      ( p->carte_img == s_image ) ? "rendue" : "ABSENTE", p->n_geoms );
		}
	if(( s_n % 600 ) == 0 )
	{
		VLOG( "OMB", "cout processeur : carte %.3f ms/image, reception %.3f ms/image (%d maillages/image), "
		             "translucides %.3f ms/image (%d.%02d maillages/image), decoupe CLP %s",
		      (float)s_us_carte / 600000.0f, (float)s_us_rec / 600000.0f, s_n_rec / 600,
		      (float)s_us_rec_t / 600000.0f, s_n_rec_t / 600, ( s_n_rec_t % 600 ) / 6,
		      g_vita_ombre_decoupe ? "MARCHE" : "ARRET" );
		s_us_carte = s_us_rec = s_us_rec_t = 0;
		s_n_rec = s_n_rec_t = 0;
	}
}


bool OmbreAutoOmbrage( const Nx::CModel *p_modele, float O[16], unsigned int *p_tex )
{
	if( !g_vita_ombre || !g_vita_auto_ombre || !p_modele || !O || !p_tex )
		return false;
	for( int i = 0; i < OMB_MAX_PROJ; ++i )
	{
		const SProjection *p = &s_proj[i];
		if(( p->p_modele != p_modele ) || !p->p_tex || !p->gl_tex || !p->aom_ok
		   || ( p->carte_img != s_image ) || ( p->fbo_etat <= 0 ))
			continue;
		memcpy( O, p->aom, sizeof( p->aom ));
		const float kz = 1.0f / ( OMB_LOIN - OMB_PRES );
		O[13] = g_vita_aom_b0 * kz;
		O[14] = g_vita_aom_b1 * kz;
		*p_tex = p->gl_tex;
		return true;
	}
	return false;
}


/*****************************************************************************
**  Passe 2 : la reception (render_shadow_meshes, render.cpp:2239)           **
*****************************************************************************/

static int s_rec_proj[OMB_MAX_PROJ];	// OmbreReceptions -> indice dans s_proj
static int s_rec_n = 0;

int OmbreReceptions( SOmbreReception *p_out, int max )
{
	s_rec_n = 0;
	if( !g_vita_ombre )
		return 0;
	for( int i = 0; ( i < OMB_MAX_PROJ ) && ( s_rec_n < max ); ++i )
	{
		const SProjection *p = &s_proj[i];
		if( !p->p_tex || !p->cam_ok || ( p->carte_img != s_image ) || ( p->fbo_etat <= 0 ))
			continue;
		SOmbreReception *o = &p_out[s_rec_n];
		memcpy( o->P, p->P, sizeof( o->P ));
		memcpy( o->r, p->r, sizeof( o->r ));
		memcpy( o->u, p->u, sizeof( o->u ));
		memcpy( o->f, p->f, sizeof( o->f ));
		// Boite de lumiere : P + f x {1, 128} +- 48 r +- 48 u, ses 8 coins.
		for( int a = 0; a < 3; ++a )
		{
			o->bmin[a] =  1e30f;
			o->bmax[a] = -1e30f;
		}
		for( int c = 0; c < 8; ++c )
		{
			const float z  = ( c & 1 ) ? OMB_LOIN : OMB_PRES;
			const float sr = ( c & 2 ) ? OMB_DEMI : -OMB_DEMI;
			const float su = ( c & 4 ) ? OMB_DEMI : -OMB_DEMI;
			for( int a = 0; a < 3; ++a )
			{
				const float v = p->P[a] + p->f[a] * z + p->r[a] * sr + p->u[a] * su;
				if( v < o->bmin[a] ) o->bmin[a] = v;
				if( v > o->bmax[a] ) o->bmax[a] = v;
			}
		}
		s_rec_proj[s_rec_n++] = i;
	}
	return s_rec_n;
}

static GLuint s_rec_tex_ombre = 0;

bool OmbreReceptionDebut( int i, const float *pv )
{
	if(( i < 0 ) || ( i >= s_rec_n ) || !reception_prete())
		return false;
	const SProjection *p = &s_proj[s_rec_proj[i]];

	glUseProgram( s_prog_rec );
	// Lignes de pv (colonnes OpenGL) : ligne r = ( m[r], m[4+r], m[8+r], m[12+r] ).
	for( int r = 0; r < 4; ++r )
		glUniform4f( s_re_l[r], pv[r], pv[4 + r], pv[8 + r], pv[12 + r] );

	// vS : u = 0.5 + r.(p-P)/96, v = 0.5 + u.(p-P)/96 (convention GL des FBO
	// vitaGL : y clip = +1 ecrit en t = 1 ; "omb 3" inverse pour verifier),
	// z = ( f.(p-P) - 1 ) / 127.
	const float rP = p->r[0] * p->P[0] + p->r[1] * p->P[1] + p->r[2] * p->P[2];
	const float uP = p->u[0] * p->P[0] + p->u[1] * p->P[1] + p->u[2] * p->P[2];
	const float fP = p->f[0] * p->P[0] + p->f[1] * p->P[1] + p->f[2] * p->P[2];
	const float sv = ( g_vita_ombre == 3 ) ? -1.0f : 1.0f;
	const float k  = 1.0f / ( 2.0f * OMB_DEMI );
	const float kz = 1.0f / ( OMB_LOIN - OMB_PRES );
	glUniform4f( s_re_s[0], p->r[0] * k, p->r[1] * k, p->r[2] * k, 0.5f - rP * k );
	glUniform4f( s_re_s[1], sv * p->u[0] * k, sv * p->u[1] * k, sv * p->u[2] * k, 0.5f - sv * uP * k );
	glUniform4f( s_re_s[2], p->f[0] * kz, p->f[1] * kz, p->f[2] * kz, -( fP + OMB_PRES ) * kz );
	// Decoupe (#70) : marge = 1 texel + l'ecart du PCF ("omd"), en unites de
	// carte ; hors de [0,1] le pixel vaut k = 1, la marge n'est que prudence.
	if( s_re_decoupe >= 0 )
		glUniform1f( s_re_decoupe, g_vita_ombre_decoupe
		             ? ( 1.0f + fabsf( g_vita_ombre_douce )) / (float)OMB_TAILLE : 1e4f );

	// Carte sur l'unite 1, texture du materiau sur l'unite 0.
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, p->gl_tex );
	glActiveTexture( GL_TEXTURE0 );
	s_rec_tex_ombre = p->gl_tex;

	// [SOURCE] render.cpp:2324-2331 et :2594 : pas d'ecriture de profondeur,
	// melange MODULATE_COLOR = dst x src.rgb, pas de culling.
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glDepthMask( GL_FALSE );
	glDisable( GL_CULL_FACE );
	glDisable( GL_ALPHA_TEST );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ZERO, GL_SRC_COLOR );
	glBlendEquation( GL_FUNC_ADD );

	glEnableVertexAttribArray( RE_POS );
	glEnableVertexAttribArray( RE_UV );

	// Region de tuiles (#73) : projection des 8 coins de la boite de lumiere.
	// Un coin derriere la camera (w <= 0) : pas de rectangle sur, on garde
	// l'ecran entier.
	s_region_posee = false;
	if( g_vita_ombre_region && !NxVita::GxmListeOuverte() && !glIsEnabled( GL_SCISSOR_TEST ))
	{
		GLint vp[4];
		glGetIntegerv( GL_VIEWPORT, vp );
		float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
		bool ok = ( vp[2] > 0 ) && ( vp[3] > 0 );
		for( int c = 0; ok && ( c < 8 ); ++c )
		{
			const float z  = ( c & 1 ) ? OMB_LOIN : OMB_PRES;
			const float sr = ( c & 2 ) ? OMB_DEMI : -OMB_DEMI;
			const float su = ( c & 4 ) ? OMB_DEMI : -OMB_DEMI;
			float q[3];
			for( int a = 0; a < 3; ++a )
				q[a] = p->P[a] + p->f[a] * z + p->r[a] * sr + p->u[a] * su;
			const float cx = pv[0] * q[0] + pv[4] * q[1] + pv[8]  * q[2] + pv[12];
			const float cy = pv[1] * q[0] + pv[5] * q[1] + pv[9]  * q[2] + pv[13];
			const float cw = pv[3] * q[0] + pv[7] * q[1] + pv[11] * q[2] + pv[15];
			if( cw <= 1.0f )
			{
				ok = false;
				break;
			}
			const float nx = cx / cw, ny = cy / cw;
			if( nx < x0 ) x0 = nx;
			if( nx > x1 ) x1 = nx;
			if( ny < y0 ) y0 = ny;
			if( ny > y1 ) y1 = ny;
		}
		if( ok )
		{
			// NDC -> pixels GL (origine en bas a gauche), marge d'un pixel.
			int px0 = (int)floorf( vp[0] + ( x0 * 0.5f + 0.5f ) * vp[2] ) - 1;
			int px1 = (int)ceilf ( vp[0] + ( x1 * 0.5f + 0.5f ) * vp[2] ) + 1;
			int py0 = (int)floorf( vp[1] + ( y0 * 0.5f + 0.5f ) * vp[3] ) - 1;
			int py1 = (int)ceilf ( vp[1] + ( y1 * 0.5f + 0.5f ) * vp[3] ) + 1;
			const int W = vp[0] + vp[2], H = vp[1] + vp[3];
			if( px0 < 0 ) px0 = 0;
			if( py0 < 0 ) py0 = 0;
			if( px1 > W ) px1 = W;
			if( py1 > H ) py1 = H;
			++s_region_n;
			if(( px1 <= px0 ) || ( py1 <= py0 ))
			{
				// Boite hors champ : rien a recevoir a l'ecran.
				px0 = py0 = 0;
				px1 = py1 = 1;
			}
			s_region_pix += (unsigned)(( px1 - px0 ) * ( py1 - py0 ));
			// FBO vitaGL retournes (pas de HAVE_UNFLIPPED_FBOS) : y GL = y GXM ;
			// ecran : y inverse. Bornes inclusives.
			const int gy0 = is_rendering_display ? ( H - py1 ) : py0;
			sceGxmSetRegionClip( gxm_context, SCE_GXM_REGION_CLIP_OUTSIDE,
			                     px0, gy0, px1 - 1, gy0 + ( py1 - py0 ) - 1 );
			s_region_posee = true;
			s_region_w = W;
			s_region_h = H;
		}
		else
			++s_region_plein;
	}
	return true;
}

void OmbreReceptionMaillage( unsigned int vbo, unsigned int uvbo, unsigned int ibo,
                             int num_indices, unsigned int texture, bool masque )
{
	const bool m = masque && texture && uvbo;
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glVertexAttribPointer( RE_POS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	// Sans jeu d'UV, l'attribut lit les positions : valeur ignoree (uMasque 0).
	glBindBuffer( GL_ARRAY_BUFFER, m ? uvbo : vbo );
	glVertexAttribPointer( RE_UV, 2, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	// Toujours une texture valide sur l'unite 0 : la carte elle-meme a defaut.
	glBindTexture( GL_TEXTURE_2D, m ? texture : s_rec_tex_ombre );
	glUniform1f( s_re_masque, m ? 1.0f : 0.0f );
	if( s_re_doux >= 0 ) glUniform1f( s_re_doux, g_vita_ombre_douce );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, ibo );
	glDrawElements( GL_TRIANGLE_STRIP, num_indices, GL_UNSIGNED_SHORT, NULL );
}

void OmbreReceptionFin( void )
{
	if( s_region_posee )
	{
		// Region pleine, comme vitaGL la laisse hors glScissor (tests.c).
		sceGxmSetRegionClip( gxm_context, SCE_GXM_REGION_CLIP_OUTSIDE,
		                     0, 0, s_region_w - 1, s_region_h - 1 );
		s_region_posee = false;
	}
	if( s_region_n + s_region_plein >= 1200 )
	{
		VLOG( "OMB", "region de tuiles (omr %d) : %u receptions, %u plein ecran, %u pixels en moyenne",
		      g_vita_ombre_region, s_region_n, s_region_plein,
		      s_region_n ? s_region_pix / s_region_n : 0 );
		s_region_n = s_region_plein = s_region_pix = 0;
	}
	glDisableVertexAttribArray( RE_POS );
	glDisableVertexAttribArray( RE_UV );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glUseProgram( 0 );
	// Etat de la passe opaque, la ou RenderWorld l'attend (test du decor :
	// LEQUAL comme XBox, « zeq »).
	glDisable( GL_BLEND );
	glDepthFunc( g_vita_zeq ? GL_LEQUAL : GL_LESS );
	glDepthMask( GL_TRUE );
}


/*****************************************************************************
**  Vue de la carte ("omb 2/3")                                              **
*****************************************************************************/

void OmbreVueCarte( void )
{
	if( g_vita_ombre < 2 )
		return;
	const SProjection *p = NULL;
	for( int i = 0; i < OMB_MAX_PROJ; ++i )
		if( s_proj[i].p_tex && ( s_proj[i].carte_img == s_image ))
		{
			p = &s_proj[i];
			break;
		}
	if( !p || !vue_prete())
		return;

	GLint prog_avant = 0, act_avant = 0, tex_avant = 0, buf_avant = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog_avant );
	glGetIntegerv( GL_ACTIVE_TEXTURE, &act_avant );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buf_avant );
	glActiveTexture( GL_TEXTURE0 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex_avant );
	const GLboolean dt = glIsEnabled( GL_DEPTH_TEST );
	const GLboolean cf = glIsEnabled( GL_CULL_FACE );
	const GLboolean bl = glIsEnabled( GL_BLEND );
	if( dt ) glDisable( GL_DEPTH_TEST );
	if( cf ) glDisable( GL_CULL_FACE );
	if( bl ) glDisable( GL_BLEND );

	glUseProgram( s_prog_vue );
	glBindTexture( GL_TEXTURE_2D, p->gl_tex );
	glBindBuffer( GL_ARRAY_BUFFER, s_vbo_vue );
	glEnableVertexAttribArray( VU_POS );
	glEnableVertexAttribArray( VU_UV );
	glVertexAttribPointer( VU_POS, 2, GL_FLOAT, GL_FALSE, 4 * sizeof( float ), (void *)0 );
	glVertexAttribPointer( VU_UV,  2, GL_FLOAT, GL_FALSE, 4 * sizeof( float ),
	                       (void *)( 2 * sizeof( float )));
	glDrawArrays( GL_TRIANGLE_STRIP, ( g_vita_ombre == 3 ) ? 4 : 0, 4 );
	glDisableVertexAttribArray( VU_POS );
	glDisableVertexAttribArray( VU_UV );

	glBindBuffer( GL_ARRAY_BUFFER, buf_avant );
	glBindTexture( GL_TEXTURE_2D, tex_avant );
	glActiveTexture( act_avant );
	glUseProgram( prog_avant );
	if( dt ) glEnable( GL_DEPTH_TEST );
	if( cf ) glEnable( GL_CULL_FACE );
	if( bl ) glEnable( GL_BLEND );
}

} // namespace NxVita
