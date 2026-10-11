/*****************************************************************************
**  THUG-Vita -- backend graphique                                          **
**  Code/Gfx/Vita/p_shader_decor.cpp                                        **
**                                                                          **
**  Rendu du decor par shader CG. Voir p_shader_decor.h pour le pourquoi.   **
**                                                                          **
**  Conventions reprises de vitaGL, pas devinees :                          **
**  - langage CG natif (GL_CG_*_SHADER_EXT) : compile par libshacccg sans   **
**    passer par le traducteur GLSL (custom_shaders.c:1846) ;               **
**  - matrices : vitaGL range les siennes en lignes et fait mul(M, v)       **
**    (shaders/ffp_v.h:164). Nos matrices OpenGL sont en colonnes : on les  **
**    envoie avec transpose = GL_TRUE et le shader fait mul(M, v) lui aussi ;**
**  - tableaux de sommets : l'etat des shaders (cur_vao) est distinct de    **
**    celui du pipeline fixe (ffp_vertex_attrib_state, ffp.c:1833), on peut **
**    donc alterner les deux dans une meme image.                           **
*****************************************************************************/

#include <core/defines.h>

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/gxm.h>
#include <stdlib.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "vita_log.h"
#include "p_shader_decor.h"

namespace NxVita
{

// Brouillard (issue #45) : macro BROUILLARD_VS, dans p_shader_decor.h (les
// particules du p_NxNewParticle.cpp s'en servent aussi).

// Par defaut depuis le 2026-09-27 : sur toutes les captures comparees de New
// Jersey, le pipeline fixe laisse l'herbe en taches noires et des facades
// noires, que la formule Xbox corrige. Â« shd 0 Â» garde l'ancien rendu.
int g_vita_shader_decor = 1;

enum { ATTR_POSITION = 0, ATTR_UV = 1, ATTR_COULEUR = 2, ATTR_UV2 = 3 };

// Ce que le pipeline fixe calcule aujourd'hui pour un lot du decor :
// GL_MODULATE (texture x couleur de sommet), puis GL_ALPHA_TEST en GEQUAL.
// La matrice passe en QUATRE LIGNES float4, pas en float4x4 : mesure
// (mode 8 de diagnostic) -- une float4x4 envoyee par glUniformMatrix4fv
// arrivait decalee dans le shader, alors que les uniformes float y arrivent
// intacts. Quatre produits scalaires ne laissent aucune question de rangement.
static const char *s_source_sommets =
	"void main(\n"
	"	float3 aPosition,\n"
	"	float2 aTexcoord,\n"
	"	float4 aColor,\n"
	"	uniform float4 uL0,\n"
	"	uniform float4 uL1,\n"
	"	uniform float4 uL2,\n"
	"	uniform float4 uL3,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float4 out vColor : COLOR)\n"
	"{\n"
	"	float4 p = float4(aPosition, 1.f);\n"
	"	vPosition = float4(dot(uL0, p), dot(uL1, p), dot(uL2, p), dot(uL3, p));\n"
	"	vTexcoord = aTexcoord;\n"
	"	vColor = aColor;\n"
	"}\n";

static const char *s_source_pixels =
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float4 vColor : COLOR,\n"
	"	uniform sampler2D uTex,\n"
	"	uniform float uSeuil,\n"
	"	uniform float uMagenta)\n"
	"{\n"
	"	float4 c = tex2D(uTex, vTexcoord) * vColor;\n"
	"	if (uMagenta > 0.5f)\n"
	"		return float4(1.f, 0.f, 1.f, 1.f);\n"
	"	if (c.a < uSeuil)\n"
	"		discard;\n"
	"	return c;\n"
	"}\n";

static int   s_etat = 0;		// 0 = jamais tente, 1 = pret, -1 = echec
static GLuint s_prog = 0;
static GLint  s_loc_l[4] = { -1, -1, -1, -1 };
static GLint  s_loc_seuil = -1;
static GLint  s_loc_magenta = -1;
static float  s_seuil = -1.0f;

// --- Cache disque des shaders compiles (issue #17) --------------------------
//
// libshacccg met 270 a 340 ms par variante (mesure : 60 variantes dans New
// Jersey, 17 s). Le binaire GXP ne depend que du source : on le range sous
// son empreinte, et les chargements suivants le relisent en quelques ms.
// vitaGL serialise et recharge lui-meme (vglGetShaderBinary / glShaderBinary,
// custom_shaders.c:660/1976). Invalider = effacer ux0:data/thug/shd/.
#define CACHE_SHD "ux0:data/thug/shd"
static unsigned char s_tampon_bin[64 * 1024];

static unsigned long long empreinte( const char *s, GLenum type )
{
	unsigned long long h = 1469598103934665603ULL ^ (unsigned long long)type;
	for( ; *s; ++s )
	{
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

static GLuint charger_cache( GLenum type, unsigned long long h )
{
	char chemin[96];
	snprintf( chemin, sizeof( chemin ), CACHE_SHD "/%016llx.gxp", h );
	SceUID fd = sceIoOpen( chemin, SCE_O_RDONLY, 0 );
	if( fd < 0 )
		return 0;
	const int n = sceIoRead( fd, s_tampon_bin, sizeof( s_tampon_bin ));
	sceIoClose( fd );
	if( n <= 0 )
		return 0;
	GLuint sh = glCreateShader( type );
	glShaderBinary( 1, &sh, 0, s_tampon_bin, n );
	return sh;
}

static void ecrire_cache( GLuint sh, unsigned long long h )
{
	GLsizei n = 0;
	vglGetShaderBinary( sh, sizeof( s_tampon_bin ), &n, s_tampon_bin );
	if(( n <= 0 ) || ( n > (GLsizei)sizeof( s_tampon_bin )))
		return;
	sceIoMkdir( CACHE_SHD, 0777 );
	char chemin[96];
	snprintf( chemin, sizeof( chemin ), CACHE_SHD "/%016llx.gxp", h );
	SceUID fd = sceIoOpen( chemin, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	if( fd < 0 )
		return;
	sceIoWrite( fd, s_tampon_bin, n );
	sceIoClose( fd );
}

static GLuint compiler( GLenum type, const char *p_source, const char *p_nom )
{
	const unsigned long long h = empreinte( p_source, type );
	GLuint sh = charger_cache( type, h );
	if( sh )
		return sh;

	sh = glCreateShader( type );
	glShaderSource( sh, 1, &p_source, NULL );
	glCompileShader( sh );
	GLint ok = 0;
	glGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		char log[512];
		GLsizei n = 0;
		log[0] = 0;
		glGetShaderInfoLog( sh, sizeof( log ) - 1, &n, log );
		log[( n > 0 && n < (GLsizei)sizeof( log )) ? n : 0] = 0;
		VLOG( "SHD", "!! compilation '%s' refusee : %s", p_nom, log );
		glDeleteShader( sh );
		return 0;
	}
	ecrire_cache( sh, h );
	return sh;
}

// Meme compilation, cache disque compris, pour le reste du backend
// (p_gamma.cpp, passe de la rampe gamma).
GLuint CompilerShaderCache( GLenum type, const char *p_source, const char *p_nom )
{
	return compiler( type, p_source, p_nom );
}

bool ShaderDecorPret()
{
	if( s_etat != 0 )
		return ( s_etat > 0 );

	s_etat = -1;
	GLuint vs = compiler( GL_CG_VERTEX_SHADER_EXT, s_source_sommets, "sommets" );
	GLuint fs = compiler( GL_CG_FRAGMENT_SHADER_EXT, s_source_pixels, "pixels" );
	if( !vs || !fs )
		return false;

	s_prog = glCreateProgram();
	glAttachShader( s_prog, vs );
	glAttachShader( s_prog, fs );
	glBindAttribLocation( s_prog, ATTR_POSITION, "aPosition" );
	glBindAttribLocation( s_prog, ATTR_UV,       "aTexcoord" );
	glBindAttribLocation( s_prog, ATTR_COULEUR,  "aColor" );
	glLinkProgram( s_prog );

	GLint ok = 0;
	glGetProgramiv( s_prog, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		VLOG( "SHD", "!! edition de liens refusee" );
		return false;
	}

	s_loc_l[0]  = glGetUniformLocation( s_prog, "uL0" );
	s_loc_l[1]  = glGetUniformLocation( s_prog, "uL1" );
	s_loc_l[2]  = glGetUniformLocation( s_prog, "uL2" );
	s_loc_l[3]  = glGetUniformLocation( s_prog, "uL3" );
	s_loc_seuil = glGetUniformLocation( s_prog, "uSeuil" );
	s_loc_magenta = glGetUniformLocation( s_prog, "uMagenta" );
	VLOG( "SHD", "programme decor pret" );
	s_etat = 1;
	return true;
}

void ShaderDecorDebut( const float *mvp )
{
	glUseProgram( s_prog );
	// Lignes de la matrice : mvp est en colonnes (OpenGL), la ligne r est
	// donc ( m[r], m[4 + r], m[8 + r], m[12 + r] ).
	for( int r = 0; r < 4; ++r )
		glUniform4f( s_loc_l[r], mvp[r], mvp[4 + r], mvp[8 + r], mvp[12 + r] );
	s_seuil = -1.0f;
	ShaderDecorSeuil( 0.0f );
	// Diagnostic : mode 5 = lots peints en magenta.
	glUniform1f( s_loc_magenta, ( g_vita_shader_decor == 5 ) ? 1.0f : 0.0f );

	glActiveTexture( GL_TEXTURE0 );
	glEnableVertexAttribArray( ATTR_POSITION );
	glEnableVertexAttribArray( ATTR_UV );
	glEnableVertexAttribArray( ATTR_COULEUR );
}

void ShaderDecorSeuil( float seuil )
{
	if( seuil == s_seuil )
		return;
	s_seuil = seuil;
	glUniform1f( s_loc_seuil, seuil );
}

void ShaderDecorTampons( unsigned int vbo, unsigned int uvbo, unsigned int cbo )
{
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glVertexAttribPointer( ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, uvbo );
	glVertexAttribPointer( ATTR_UV, 2, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, cbo );
	glVertexAttribPointer( ATTR_COULEUR, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
}

void ShaderDecorFin()
{
	glDisableVertexAttribArray( ATTR_POSITION );
	glDisableVertexAttribArray( ATTR_UV );
	glDisableVertexAttribArray( ATTR_COULEUR );
	glUseProgram( 0 );
}


// --- Materiau jusqu'a 4 passes : shaders GENERES, comme le moteur Xbox ------
//
// [SOURCE] XBox/NX/render.cpp:302-1024, get_pixel_shader. Pour la passe p :
//     couleur_p = texture_p x c_p x v0 x 4      (xmma_x4, saturee)
//     alpha_p   = texture_p.a x v0.a x 2        (ou x c4.a = 0.5 si la passe
//                                               ignore l'alpha de sommet)
// puis chaque passe p >= 1 est combinee au resultat selon SON mode, par la
// meme table pour les passes 1, 2 et 3 (render.cpp:420, :623, :907). Nos
// couleurs de sommets valent deja 2 x v0 : 4 x v0 x c = (2 v0) x (2 c).
//
// Deux details du source, repris tels quels :
//  - BLEND_PREVIOUS_MASK prend comme masque l'alpha de la passe PRECEDENTE
//    (r0.a pour la passe 1, r1.a pour la 2, t2.a pour la 3) ;
//  - MODULATE_FIXED n'a pas de break et CHUTE dans BRIGHTEN (r x alpha de la
//    passe + r), pas dans BRIGHTEN_FIXED (render.cpp:660-667).
// L'alpha final est celui de la passe 0.
//
// POURQUOI GENERER. Un shader unique qui teste le mode de chaque passe a
// chaque pixel coutait, mesure dans New Jersey a camera fixe, 16,7 -> 41-46 ms
// par image, le processeur n'ayant que +0,4 ms : le GPU sature. Le moteur
// d'origine fait la meme chose que nous ici : une cle par combinaison
// (render.cpp:322), un programme par cle, en cache. Chaque variante ne porte
// que ses operations ; le discard n'y figure que si le materiau a un seuil
// alpha (un discard, meme jamais pris, prive le GPU du rejet precoce).

// Operation de combinaison d'une passe, ecrite dans le source genere.
// r = resultat, q/qa = couleur et alpha de la passe, f = alpha fixe, m = masque.
static const char *op_combinaison( unsigned int mode )
{
	switch( mode )
	{
		case 1:  return "r = saturate(r + q * qa);";
		case 2:  return "r = saturate(r + q * f);";
		case 3:  return "r = saturate(r - q * qa);";
		case 4:  return "r = saturate(r - q * f);";
		case 5:  return "r = lerp(r, q, qa);";
		case 6:  return "r = lerp(r, q, f);";
		case 7:  return "r = r * qa;";
		case 8:  return "r = r * f; r = saturate(r * qa + r);";
		case 9:  return "r = saturate(r * qa + r);";
		case 10: return "r = saturate(r * f + r);";
		case 12: return "r = lerp(r, q, m);";
		case 13: return "r = lerp(q, r, m);";
		default: return "";		// DIFFUSE, GLOSS_MAP : sans effet sur r
	}
}

struct SGxmVariante;

struct SVarianteMateriau
{
	uint32 cle;
	GLuint prog;
	GLuint vs_gl, fs_gl;	// shaders vitaGL, dont on tire les binaires GXM
	int    passes;
	SGxmVariante *gxm;	// chemin GXM direct, construit au premier usage
	unsigned int env;	// passes en reflet (issue #5)
	unsigned int wib;	// passes a UV wibble (issue #43)
	GLint  w[4];		// uW0..3, decalage d'UV par passe (-1 : absent)
	GLint  l[4];		// lignes de la matrice
	GLint  c[4];		// couleurs de passe
	GLint  seuil;
	float  c_pose[4][4];	// dernieres couleurs envoyees (evite les renvois)
	GLint  fogp, fogc;		// uFogP, uFogC (issue #45)
	float  fog_pose[8];		// dernieres valeurs envoyees
};

// La table n'est jamais videe d'un niveau a l'autre : 128 debordait apres
// quelques changements de niveau (NJ, Tampa, Slam City) -- plantage, voir
// ShaderMateriauMaillage.
#define MAX_VARIANTES 512
static SVarianteMateriau s_var[MAX_VARIANTES];
static int    s_num_var = 0;
static GLuint s_vs_m[5][16][16];	// un vertex shader par nombre de passes, masque de reflet et de wibble
static const SVarianteMateriau *sp_var_cour = NULL;
static const float *sp_mvp = NULL;
static bool   s_echec_m = false;
static bool   s_en_prechargement = false;
static int    s_uv_actifs = 0;	// attributs d'UV actuellement actives

enum { ATTR_UV3 = 4, ATTR_UV4 = 5 };

// Cle : passes (3 bits), modes 1..3 (4 bits chacun), alphas ignores (4 bits),
// test alpha (1 bit).
unsigned int ShaderMateriauCle( const SShaderMateriau *p )
{
	uint32 k = (uint32)p->passes;
	for( int i = 1; i < 4; ++i )
		k |= ( i < p->passes ? ( p->mode[i] & 0xF ) : 0 ) << ( 3 + 4 * ( i - 1 ));
	for( int i = 0; i < 4; ++i )
		if(( i < p->passes ) && p->ignore_alpha[i] )
			k |= 1u << ( 15 + i );
	if( p->seuil > 0.0f )
		k |= 1u << 19;
	k |= ( p->env & (( 1u << p->passes ) - 1 ) & 0xF ) << 20;	// passes en reflet (#5)
	k |= ( p->wib & ~p->env & (( 1u << p->passes ) - 1 ) & 0xF ) << 24;	// UV wibble (#43)
	if( p->fixe0 )
		k |= 1u << 28;		// alpha de sortie = alpha fixe (modes *_FIXED)
	return k;
}

static GLuint vertex_shader_m( int passes, unsigned int env, unsigned int wib )
{
	env &= 0xF;
	wib &= ~env & 0xF;		// une passe en reflet n'a pas d'UV a decaler
	if( s_vs_m[passes][env][wib] )
		return s_vs_m[passes][env][wib];
	char src[4096];
	int n = snprintf( src, sizeof( src ),
		"void main(\n"
		"	float3 aPosition,\n"
		"	float4 aColor,\n" );
	static const char *noms_in[4]  = { "aTexcoord", "aTexcoord2", "aTexcoord3", "aTexcoord4" };
	for( int k = 0; k < passes; ++k )
		if( !( env & ( 1u << k )))
			n += snprintf( src + n, sizeof( src ) - n, "	float2 %s,\n", noms_in[k] );
	if( env )
	{
		n += snprintf( src + n, sizeof( src ) - n,
			"	float3 aNormal,\n"
			"	uniform float4 uV0,\n	uniform float4 uV1,\n	uniform float4 uV2,\n" );
		for( int k = 0; k < passes; ++k )
			if( env & ( 1u << k ))
				n += snprintf( src + n, sizeof( src ) - n, "	uniform float4 uE%d,\n", k );
	}
	for( int k = 0; k < passes; ++k )
		if( wib & ( 1u << k ))
			n += snprintf( src + n, sizeof( src ) - n, "	uniform float4 uW%d,\n", k );
	n += snprintf( src + n, sizeof( src ) - n,
		"	uniform float4 uL0,\n	uniform float4 uL1,\n"
		"	uniform float4 uL2,\n	uniform float4 uL3,\n"
		"	uniform float4 uFogP,\n	uniform float4 uFogC,\n"
		"	float4 out vPosition : POSITION,\n" );
	for( int k = 0; k < passes; ++k )
		n += snprintf( src + n, sizeof( src ) - n,
		               "	float2 out vT%d : TEXCOORD%d,\n", k, k );
	n += snprintf( src + n, sizeof( src ) - n,
		"	float4 out vFog : TEXCOORD4,\n"
		"	float4 out vColor : COLOR)\n"
		"{\n"
		"	float4 p = float4(aPosition, 1.f);\n"
		"	vPosition = float4(dot(uL0, p), dot(uL1, p), dot(uL2, p), dot(uL3, p));\n"
		"	vColor = aColor;\n"
		BROUILLARD_VS( "dot(uL3, p)" ));
	// UV wibble (issue #43) : XBox pose la translation dans la matrice de
	// texture de la passe (D3DTS_TEXTUREn._31/_32, XBox/NX/material.cpp:444,
	// D3DTTFF_COUNT2) -- u' = u + uoff, v' = v + voff.
	for( int k = 0; k < passes; ++k )
		if( !( env & ( 1u << k )))
		{
			if( wib & ( 1u << k ))
				n += snprintf( src + n, sizeof( src ) - n, "	vT%d = %s + uW%d.xy;\n", k, noms_in[k], k );
			else
				n += snprintf( src + n, sizeof( src ) - n, "	vT%d = %s;\n", k, noms_in[k] );
		}
	if( env )
	{
		// Reflet en espace camera, comme D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR
		// (XBox/NX/material.cpp:420) : R = 2 (N.E) N - E, E vers l'oeil.
		// Le passage main gauche (D3D) -> main droite (GL) ne change que z,
		// que la projection n'utilise pas. Puis la matrice de texture Xbox :
		// u = 0,5 tu Rx + 0,5 ; v = 0,5 tv Ry + 0,5 (material.cpp:287, :415).
		n += snprintf( src + n, sizeof( src ) - n,
			"	float3 pe = float3(dot(uV0, p), dot(uV1, p), dot(uV2, p));\n"
			"	float3 ne = normalize(float3(dot(uV0.xyz, aNormal), dot(uV1.xyz, aNormal), dot(uV2.xyz, aNormal)));\n"
			"	float3 e = normalize(-pe);\n"
			"	float3 r = 2.0f * dot(ne, e) * ne - e;\n" );
		for( int k = 0; k < passes; ++k )
			if( env & ( 1u << k ))
				n += snprintf( src + n, sizeof( src ) - n,
					"	vT%d = float2(0.5f * uE%d.x * r.x + 0.5f, 0.5f * uE%d.y * r.y + 0.5f);\n", k, k, k );
	}
	snprintf( src + n, sizeof( src ) - n, "}\n" );
	s_vs_m[passes][env][wib] = compiler( GL_CG_VERTEX_SHADER_EXT, src, "materiau sommets" );
	return s_vs_m[passes][env][wib];
}

// Issue #69 : table de hachage des variantes ("vhs 0/1", p_siodev.cpp).
// variante() est appelee a CHAQUE dessin GXM non precalcule (maillages seuls,
// lots non precalcules) et par la serie vitaGL. Le parcours lineaire de s_var
// lit une entree de ~190 octets par cle comparee, soit une ligne de cache par
// sonde ; la table n'est jamais videe d'un niveau a l'autre (voir
// MAX_VARIANTES), donc les variantes du niveau courant sont rangees APRES
// celles de tous les niveaux deja visites, et le parcours s'allonge au fil de
// la partie. Ici : adressage ouvert, sondage lineaire, index + 1 (0 = vide).
// Toute variante creee y est inseree, quel que soit le reglage : une cle est
// dans la table si et seulement si le parcours lineaire la trouverait, et la
// MEME entree de s_var est rendue (image identique par construction).
bool g_vita_var_hash = true;
#define VAR_HASH_N 2048		// puissance de 2, > 3 x MAX_VARIANTES
static short s_var_hash[VAR_HASH_N];
static int   s_vh_recherches = 0, s_vh_sondes = 0, s_vh_poses_vs = 0;
static int   s_vh_poses_evitees = 0;		// issue #69, « vpr »
static inline unsigned int var_hash_pos( uint32 k )
{
	return ( k * 2654435761u ) >> ( 32 - 11 );		// 11 bits : VAR_HASH_N
}
static void var_hash_inserer( uint32 k, int idx )
{
	unsigned int h = var_hash_pos( k );
	for( int n = 0; ( n < VAR_HASH_N ) && s_var_hash[h]; ++n )
		h = ( h + 1 ) & ( VAR_HASH_N - 1 );
	if( !s_var_hash[h] )
		s_var_hash[h] = (short)( idx + 1 );
}

void ShaderMateriauStats( int *p_nb_var, int *p_recherches, int *p_sondes, int *p_poses_vs )
{
	if( p_nb_var )     *p_nb_var     = s_num_var;
	if( p_recherches ) *p_recherches = s_vh_recherches;
	if( p_sondes )     *p_sondes     = s_vh_sondes;
	if( p_poses_vs )   *p_poses_vs   = s_vh_poses_vs;
	s_vh_recherches = s_vh_sondes = s_vh_poses_vs = 0;
}

int ShaderMateriauPosesEvitees( void )
{
	const int n = s_vh_poses_evitees;
	s_vh_poses_evitees = 0;
	return n;
}

static const SVarianteMateriau *variante( const SShaderMateriau *p )
{
	const uint32 k = ShaderMateriauCle( p );
	++s_vh_recherches;
	if( g_vita_var_hash )
	{
		unsigned int h = var_hash_pos( k );
		for( int n = 0; n < VAR_HASH_N; ++n )
		{
			const int e = s_var_hash[h];
			++s_vh_sondes;
			if( !e )
				break;
			if( s_var[e - 1].cle == k )
				return &s_var[e - 1];
			h = ( h + 1 ) & ( VAR_HASH_N - 1 );
		}
	}
	else
	{
		int i;
		for( i = 0; i < s_num_var; ++i )
			if( s_var[i].cle == k )
				break;
		s_vh_sondes += ( i < s_num_var ) ? ( i + 1 ) : s_num_var;	// compte hors de la boucle
		if( i < s_num_var )
			return &s_var[i];
	}
	if( s_num_var >= MAX_VARIANTES )
	{
		static int s_dit = 0;
		if( s_dit++ < 5 )
			VLOG( "SHD", "!! table des variantes pleine (%d) : cle %08x non dessinee", MAX_VARIANTES, (unsigned)k );
		return NULL;
	}
	const SceUInt64 t_debut = sceKernelGetProcessTimeWide();

	// Source du fragment shader, specialise pour cette cle.
	char src[4096];
	int n = snprintf( src, sizeof( src ), "float4 main(\n" );
	for( int q = 0; q < p->passes; ++q )
		n += snprintf( src + n, sizeof( src ) - n, "	float2 vT%d : TEXCOORD%d,\n", q, q );
	n += snprintf( src + n, sizeof( src ) - n, "	float4 vColor : COLOR,\n"
	                                           "	float4 vFog : TEXCOORD4,\n" );
	for( int q = 0; q < p->passes; ++q )
		n += snprintf( src + n, sizeof( src ) - n,
		               "	uniform sampler2D uTex%d,\n	uniform float4 uC%d,\n", q, q );
	n += snprintf( src + n, sizeof( src ) - n,
		"	uniform float uSeuil)\n"
		"{\n"
		"	half4 v = vColor;\n"
		"	half4 t0 = tex2D(uTex0, vT0);\n"
		"	half3 r = saturate(t0.rgb * (2.0 * (half3)uC0.rgb) * v.rgb);\n"
		"	half a0 = %s;\n"
		"	half m = a0;\n",
		p->ignore_alpha[0] ? "t0.a" : "saturate(t0.a * v.a)" );
	for( int q = 1; q < p->passes; ++q )
	{
		n += snprintf( src + n, sizeof( src ) - n,
			"	{\n"
			"		half4 t = tex2D(uTex%d, vT%d);\n"
			"		half3 q = saturate(t.rgb * (2.0 * (half3)uC%d.rgb) * v.rgb);\n"
			"		half qa = %s;\n"
			"		half f = uC%d.a;\n"
			"		%s\n"
			"		m = qa;\n"
			"	}\n",
			q, q, q,
			p->ignore_alpha[q] ? "t.a" : "saturate(t.a * v.a)",
			q, op_combinaison( p->mode[q] ));
	}
	if( p->seuil > 0.0f )
		n += snprintf( src + n, sizeof( src ) - n, "	if (a0 < uSeuil)\n		discard;\n" );
	n += snprintf( src + n, sizeof( src ) - n, "	r = r * (half)vFog.a + (half3)vFog.rgb;\n" );
	snprintf( src + n, sizeof( src ) - n, p->fixe0 ? "	return float4(r, saturate((half)uC0.a));\n}\n"
	                                               : "	return float4(r, a0);\n}\n" );

	const unsigned int masque_passes = ( 1u << p->passes ) - 1;
	GLuint vs = vertex_shader_m( p->passes, p->env & masque_passes,
	                             p->wib & ~p->env & masque_passes );
	GLuint fs = compiler( GL_CG_FRAGMENT_SHADER_EXT, src, "materiau pixels" );
	if( !vs || !fs )
	{
		VLOG( "SHD", "!! variante %08x refusee", (unsigned)k );
		s_echec_m = true;
		return NULL;
	}

	SVarianteMateriau *v = &s_var[s_num_var];
	v->cle  = k;
	v->vs_gl = vs;
	v->fs_gl = fs;
	v->passes = p->passes;
	v->env    = p->env & (( 1u << p->passes ) - 1 );
	v->wib    = p->wib & ~v->env & (( 1u << p->passes ) - 1 );
	v->gxm = NULL;
	v->prog = glCreateProgram();
	glAttachShader( v->prog, vs );
	glAttachShader( v->prog, fs );
	glBindAttribLocation( v->prog, ATTR_POSITION, "aPosition" );
	glBindAttribLocation( v->prog, ATTR_COULEUR,  "aColor" );
	static const char *noms_in[4]  = { "aTexcoord", "aTexcoord2", "aTexcoord3", "aTexcoord4" };
	static const int   attr_uv[4]  = { ATTR_UV, ATTR_UV2, ATTR_UV3, ATTR_UV4 };
	for( int q = 0; q < p->passes; ++q )
		if( !( p->env & ( 1u << q )))
			glBindAttribLocation( v->prog, attr_uv[q], noms_in[q] );
	glLinkProgram( v->prog );
	GLint ok = 0;
	glGetProgramiv( v->prog, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		VLOG( "SHD", "!! variante %08x : edition de liens refusee", (unsigned)k );
		s_echec_m = true;
		return NULL;
	}
	static const char *noms_l[4] = { "uL0", "uL1", "uL2", "uL3" };
	static const char *noms_c[4] = { "uC0", "uC1", "uC2", "uC3" };
	static const char *noms_t[4] = { "uTex0", "uTex1", "uTex2", "uTex3" };
	for( int q = 0; q < 4; ++q )
	{
		v->l[q] = glGetUniformLocation( v->prog, noms_l[q] );
		v->c[q] = ( q < p->passes ) ? glGetUniformLocation( v->prog, noms_c[q] ) : -1;
	}
	v->seuil = glGetUniformLocation( v->prog, "uSeuil" );
	v->fogp  = glGetUniformLocation( v->prog, "uFogP" );
	v->fogc  = glGetUniformLocation( v->prog, "uFogC" );
	for( int k = 0; k < 8; ++k )
		v->fog_pose[k] = -1.0f;
	{
		static const char *noms_w[4] = { "uW0", "uW1", "uW2", "uW3" };
		for( int q = 0; q < 4; ++q )
			v->w[q] = ( v->wib & ( 1u << q )) ? glGetUniformLocation( v->prog, noms_w[q] ) : -1;
	}
	for( int q = 0; q < 4; ++q )
		for( int w = 0; w < 4; ++w )
			v->c_pose[q][w] = -1.0f;
	glUseProgram( v->prog );
	for( int q = 0; q < p->passes; ++q )
		glUniform1i( glGetUniformLocation( v->prog, noms_t[q] ), q );
	var_hash_inserer( k, s_num_var );		// issue #69, avant de la rendre visible
	++s_num_var;
	VLOG( "SHD", "variante materiau %d : cle %08x (%d passes), compilee en %.1f ms%s",
	      s_num_var, (unsigned)k, p->passes,
	      (float)( sceKernelGetProcessTimeWide() - t_debut ) / 1000.0f,
	      s_en_prechargement ? "" : " -- PENDANT LE JEU" );
	return v;
}

// Compile au chargement la variante qu'un dessin demandera (issue #17) : sans
// cela, la premiere apparition d'un materiau figeait une image 160-180 ms.
void ShaderMateriauPrecompiler( const SShaderMateriau *p )
{
	if( s_echec_m )
		return;
	s_en_prechargement = true;
	variante( p );
	s_en_prechargement = false;
}

int ShaderMateriauNombreVariantes()
{
	return s_num_var;
}

bool ShaderMateriauPret()
{
	return !s_echec_m;
}

void ShaderMateriauDebut( const float *mvp )
{
	sp_mvp = mvp;
	sp_var_cour = NULL;
	s_uv_actifs = -1;
	glEnableVertexAttribArray( ATTR_POSITION );
	glEnableVertexAttribArray( ATTR_COULEUR );
}

bool ShaderMateriauMaillage( const SShaderMateriau *p )
{
	const SVarianteMateriau *v = variante( p );
	if( !v )
		return false;		// l'appelant ne doit PAS dessiner (pipeline fixe incoherent)
	if( v != sp_var_cour )
	{
		glUseProgram( v->prog );
		for( int r = 0; r < 4; ++r )
			glUniform4f( v->l[r], sp_mvp[r], sp_mvp[4 + r], sp_mvp[8 + r], sp_mvp[12 + r] );
		sp_var_cour = v;
	}
	SVarianteMateriau *vm = (SVarianteMateriau *)v;
	for( int k = 0; k < p->passes; ++k )
	{
		if( memcmp( vm->c_pose[k], p->c[k], sizeof( float ) * 4 ) != 0 )
		{
			glUniform4f( v->c[k], p->c[k][0], p->c[k][1], p->c[k][2], p->c[k][3] );
			memcpy( vm->c_pose[k], p->c[k], sizeof( float ) * 4 );
		}
	}
	if( p->seuil > 0.0f )
		glUniform1f( v->seuil, p->seuil );
	{
		float f[8];
		BrouillardUniformes( p->fog_noir, false, f, f + 4 );
		if( memcmp( vm->fog_pose, f, sizeof( f )) != 0 )
		{
			glUniform4f( v->fogp, f[0], f[1], f[2], f[3] );
			glUniform4f( v->fogc, f[4], f[5], f[6], f[7] );
			memcpy( vm->fog_pose, f, sizeof( f ));
		}
	}
	// UV wibble (issue #43) : change a chaque image, pose a chaque dessin.
	for( int k = 0; k < p->passes; ++k )
		if( v->w[k] >= 0 )
			glUniform4f( v->w[k], p->wib_uv[k][0], p->wib_uv[k][1], 0.0f, 0.0f );

	static const int attr_uv[4] = { ATTR_UV, ATTR_UV2, ATTR_UV3, ATTR_UV4 };
	glBindBuffer( GL_ARRAY_BUFFER, p->vbo );
	glVertexAttribPointer( ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	if( p->passes != s_uv_actifs )
	{
		for( int k = 0; k < 4; ++k )
		{
			if( k < p->passes )
				glEnableVertexAttribArray( attr_uv[k] );
			else
				glDisableVertexAttribArray( attr_uv[k] );
		}
		s_uv_actifs = p->passes;
	}
	for( int k = 0; k < p->passes; ++k )
	{
		glBindBuffer( GL_ARRAY_BUFFER, p->uvbo[k] );
		glVertexAttribPointer( attr_uv[k], 2, GL_FLOAT, GL_FALSE, 0, NULL );
	}
	glBindBuffer( GL_ARRAY_BUFFER, p->cbo );
	glVertexAttribPointer( ATTR_COULEUR, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	return true;
}

void ShaderMateriauFin()
{
	static const int attr_uv[4] = { ATTR_UV, ATTR_UV2, ATTR_UV3, ATTR_UV4 };
	glDisableVertexAttribArray( ATTR_POSITION );
	glDisableVertexAttribArray( ATTR_COULEUR );
	for( int k = 0; k < 4; ++k )
		glDisableVertexAttribArray( attr_uv[k] );
	glUseProgram( 0 );
	sp_var_cour = NULL;
}


// --- Personnages : skinning par le GPU (issue #18) --------------------------
//
// Mesure : CModel::Render = 4,3-4,5 ms par image en zone dense, dont 2,3 ms
// de skinning CPU (p_NxModel.cpp, DrawSkinned) puis un dessin depuis des
// tableaux cote CPU que vitaGL recopie a chaque appel. Ici les sommets de
// repos, poids et indices d'os sont statiques sur le GPU ; seules les
// matrices d'os changent. Meme calcul que le CPU : jusqu'a 3 os par sommet,
// poids non renormalises, os hors squelette = contribution nulle.
//
// Os : 3 float4 par os -- la colonne c de la matrice Mth (rangee par lignes,
// translation en ligne 3) : ( m[0][c], m[1][c], m[2][c], m[3][c] ). Pas de
// float4x4 (arrive decalee, voir le decor).

#define PEAU_MAX_OS 64

static const char *s_src_peau_v =
	"void main(\n"
	"	float3 aPos,\n"
	"	float3 aPoids,\n"
	"	float3 aOs,\n"
	"	float2 aTexcoord,\n"
	"	uniform float4 uL0,\n"
	"	uniform float4 uL1,\n"
	"	uniform float4 uL2,\n"
	"	uniform float4 uL3,\n"
	"	uniform float4 uB[192],\n"
	"	uniform float4 uFogP,\n"
	"	uniform float4 uFogC,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float4 out vFog : TEXCOORD4)\n"
	"{\n"
	"	float4 p = float4(aPos, 1.f);\n"
	"	int3 o = (int3)min(aOs, float3(63.f, 63.f, 63.f)) * 3;\n"
	"	float3 s = aPoids.x * float3(dot(uB[o.x], p), dot(uB[o.x + 1], p), dot(uB[o.x + 2], p))\n"
	"	         + aPoids.y * float3(dot(uB[o.y], p), dot(uB[o.y + 1], p), dot(uB[o.y + 2], p))\n"
	"	         + aPoids.z * float3(dot(uB[o.z], p), dot(uB[o.z + 1], p), dot(uB[o.z + 2], p));\n"
	"	float4 q = float4(s, 1.f);\n"
	"	vPosition = float4(dot(uL0, q), dot(uL1, q), dot(uL2, q), dot(uL3, q));\n"
	"	vTexcoord = aTexcoord;\n"
	BROUILLARD_VS( "dot(uL3, q)" )
	"}\n";

static const char *s_src_peau_f =
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float4 vFog : TEXCOORD4,\n"
	"	uniform sampler2D uTex,\n"
	"	uniform float4 uTeinte,\n"
	"	uniform float uAvecTexture)\n"
	"{\n"
	"	float4 t = tex2D(uTex, vTexcoord);\n"
	"	float4 c = lerp(float4(1.f, 1.f, 1.f, 1.f), t, uAvecTexture) * uTeinte;\n"
	"	clip(c.a - 0.5f / 255.f);\n"
	"	return float4(c.rgb * vFog.a + vFog.rgb, c.a);\n"
	"}\n";

enum { ATTR_PEAU_POS = 0, ATTR_PEAU_POIDS = 1, ATTR_PEAU_OS = 2, ATTR_PEAU_UV = 3 };

static int    s_etat_peau = 0;
static GLuint s_prog_peau = 0;
static GLint  s_pe_l[4], s_pe_b, s_pe_teinte, s_pe_avec, s_pe_fogp, s_pe_fogc;
static float  s_os_lignes[PEAU_MAX_OS * 3 * 4];

bool ShaderPeauPret()
{
	if( s_etat_peau != 0 )
		return ( s_etat_peau > 0 );
	s_etat_peau = -1;
	GLuint vs = compiler( GL_CG_VERTEX_SHADER_EXT, s_src_peau_v, "peau sommets" );
	GLuint fs = compiler( GL_CG_FRAGMENT_SHADER_EXT, s_src_peau_f, "peau pixels" );
	if( !vs || !fs )
		return false;
	s_prog_peau = glCreateProgram();
	glAttachShader( s_prog_peau, vs );
	glAttachShader( s_prog_peau, fs );
	glBindAttribLocation( s_prog_peau, ATTR_PEAU_POS,   "aPos" );
	glBindAttribLocation( s_prog_peau, ATTR_PEAU_POIDS, "aPoids" );
	glBindAttribLocation( s_prog_peau, ATTR_PEAU_OS,    "aOs" );
	glBindAttribLocation( s_prog_peau, ATTR_PEAU_UV,    "aTexcoord" );
	glLinkProgram( s_prog_peau );
	GLint ok = 0;
	glGetProgramiv( s_prog_peau, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		VLOG( "SHD", "!! peau : edition de liens refusee" );
		return false;
	}
	static const char *noms_l[4] = { "uL0", "uL1", "uL2", "uL3" };
	for( int k = 0; k < 4; ++k )
		s_pe_l[k] = glGetUniformLocation( s_prog_peau, noms_l[k] );
	s_pe_b      = glGetUniformLocation( s_prog_peau, "uB" );
	s_pe_teinte = glGetUniformLocation( s_prog_peau, "uTeinte" );
	s_pe_avec   = glGetUniformLocation( s_prog_peau, "uAvecTexture" );
	s_pe_fogp   = glGetUniformLocation( s_prog_peau, "uFogP" );
	s_pe_fogc   = glGetUniformLocation( s_prog_peau, "uFogC" );
	glUseProgram( s_prog_peau );
	glUniform1i( glGetUniformLocation( s_prog_peau, "uTex" ), 0 );
	glUseProgram( 0 );
	VLOG( "SHD", "programme peau pret" );
	s_etat_peau = 1;
	return true;
}

int ShaderPeauMaxOs()
{
	return PEAU_MAX_OS;
}

void ShaderPeauDebut( const float *mvp, const float *p_os, int num_os )
{
	glUseProgram( s_prog_peau );
	for( int r = 0; r < 4; ++r )
		glUniform4f( s_pe_l[r], mvp[r], mvp[4 + r], mvp[8 + r], mvp[12 + r] );
	// p_os : num_os matrices Mth (16 flottants, rangees par lignes).
	memset( s_os_lignes, 0, sizeof( s_os_lignes ));
	for( int b = 0; ( b < num_os ) && ( b < PEAU_MAX_OS ); ++b )
	{
		const float *m = &p_os[b * 16];
		for( int c = 0; c < 3; ++c )
		{
			float *l = &s_os_lignes[( b * 3 + c ) * 4];
			l[0] = m[0 * 4 + c];
			l[1] = m[1 * 4 + c];
			l[2] = m[2 * 4 + c];
			l[3] = m[3 * 4 + c];
		}
	}
	glUniform4fv( s_pe_b, PEAU_MAX_OS * 3, s_os_lignes );
	{
		float P[4], C[4];
		BrouillardUniformes( false, false, P, C );
		glUniform4fv( s_pe_fogp, 1, P );
		glUniform4fv( s_pe_fogc, 1, C );
	}
	glEnableVertexAttribArray( ATTR_PEAU_POS );
	glEnableVertexAttribArray( ATTR_PEAU_POIDS );
	glEnableVertexAttribArray( ATTR_PEAU_OS );
	glEnableVertexAttribArray( ATTR_PEAU_UV );
}

void ShaderPeauPiece( unsigned int vbo_repos, unsigned int vbo_poids, unsigned int vbo_os,
                      unsigned int uvbo, bool avec_texture, const float teinte[4] )
{
	glBindBuffer( GL_ARRAY_BUFFER, vbo_repos );
	glVertexAttribPointer( ATTR_PEAU_POS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, vbo_poids );
	glVertexAttribPointer( ATTR_PEAU_POIDS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, vbo_os );
	glVertexAttribPointer( ATTR_PEAU_OS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	// Sans jeu d'UV, l'attribut lit la pose de repos : valeur ignoree.
	glBindBuffer( GL_ARRAY_BUFFER, uvbo ? uvbo : vbo_repos );
	glVertexAttribPointer( ATTR_PEAU_UV, 2, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glUniform4f( s_pe_teinte, teinte[0], teinte[1], teinte[2], teinte[3] );
	glUniform1f( s_pe_avec, avec_texture ? 1.0f : 0.0f );
}

void ShaderPeauFin()
{
	glDisableVertexAttribArray( ATTR_PEAU_POS );
	glDisableVertexAttribArray( ATTR_PEAU_POIDS );
	glDisableVertexAttribArray( ATTR_PEAU_OS );
	glDisableVertexAttribArray( ATTR_PEAU_UV );
	glUseProgram( 0 );
}



// --- Personnages ECLAIRES, chemin vitaGL (issue #4) -------------------------
//
// [SOURCE] XBox/NX/WeightedMeshVS_VXC_3Weight.vsh:47-90 et PixelShader0.psh :
//
//   N   = normale skinnee par les memes os que la position (dp3 x MAT0..2)
//   r11 = ambiante + somme_i couleur_i x max( 0, -N.dir_i )    (vsh:70-82)
//   oD0 = sat( r11 x couleur_de_sommet )                        (vsh:84, le
//         registre de couleur du vertex shader est sature a [0,1])
//   rgb = sat( sat( texture x c0 ) x oD0 x 4 )                  (psh, mul_x4)
//
// dir_i = -direction de la lumiere (XBox/p_NxLight.cpp:92), ramenee dans
// l'espace du modele par la matrice racine (anim.cpp:353) ; couleurs et
// ambiante = couleur / 128 x luminosite (p_NxLight.cpp:69,87). c0 = couleur
// du materiau, 0,5 au neutre, ou la teinte du geom / 255 (XBox/p_NxGeom.cpp:597).
// Notre teinte vaut rgba / 128, soit 2 x c0 ; la couleur de materiau est deja
// cuite dans le cbo (p_NxModel.cpp, x2), d'ou ici :
//
//   rgb = sat( texture x teinte x min( r11 x v, 1 ) x 2 ),   v = cbo / 255
//
// Au neutre (v = 128/255) : texture x min( r11, ~2 ). Mesure sur table
// (skaterparts.prx) : 786 materiaux de peau sur 789 a 0,5, et des couleurs
// de sommets a 128 sauf l'occlusion cuite (sacs a dos, planche : 14..128).
// Alpha : celui de la texture x teinte, comme le chemin non eclaire (XBox le
// module aussi par oD0.a x 2, que nous ignorons).
// Une piece sans normales garde la lumiere neutre (uSansNormale = 1).
//
// SPECULAIRE (issue #45). [SOURCE] XBox/NX/WeightedMeshVS_VXC_Specular_1Weight.vsh
// (choisi par mesh.cpp:1519 si MATFLAG_SPECULAR) : Blinn sur la SEULE
// lumiere 0, en espace modele (camera relative au modele, anim.cpp:378) :
//   V = normalize( cam - pos ), H = normalize( V - dir0 ),
//   oD1 = couleur_spec x ( N.dir0 < 0 et N.H > 0 ? (N.H)^puissance : 0 )  (lit)
// ni la couleur de la lumiere ni celle des sommets n'y entrent. Puis le
// combineur final (PixelShader0.psh, render.cpp:979 : xfc ..., sum, ...)
// AJOUTE v1 au resultat r0, avant le brouillard : sum = r0 + v1, sature.
// uSpec = 0 hors materiau speculaire (5 materiaux du jeu, skaterparts.prx).
// GLOSS_MAP (issue #45) : [SOURCE] XBox/NX/render.cpp:481-484, une passe 1 en
// vBLEND_MODE_GLOSS_MAP fait "mul v1.rgb,v1.rgb,t1.a" -- la speculaire est
// multipliee par l'alpha BRUT de la texture de la passe 1 (jeu d'UV 1, oT1 de
// WeightedMeshVS_VXC_Specular_*.vsh). Unite 2, attribut aTexcoord1 ; hors
// gloss, texture blanche 1x1 : speculaire inchangee.
//
// AUTO-OMBRAGE (issue #45, p_ombre.h). [SOURCE] XBox/NX/instance.cpp:494-668,
// PixelShader_ShadowBuffer.psh, WeightedMeshVS_VXC_*Weight_SBPassThru.vsh :
// second dessin du skater en MODULATE_COLOR, couleur = ( 1 - k ) + k x
// eclaire. Ici : vOmb = point skinne dans la carte (lignes uO0..2 de
// OmbreAutoOmbrage), vOmb.w = biais ; la carte (unite 1) donne la profondeur
// moyenne des texels couverts autour (filtrage bilineaire, comme la
// reception) ; part d'ombre en rampe sur une unite au-dela du biais (PCF
// approche) ; sortie x ( 1 - uAomK x ombre ), brouillard compris comme la
// passe XBox (MODULATE sur le pixel deja embrume). Biais de pente : la
// normale skinnee contre la direction de la lumiere (uO2.xyz, en espace
// modele). uAomK = 0 (defaut, carte neutre 1x1 sur l'unite 1) : sortie
// inchangee.
static const char *s_src_peau_vgl_lum_v =
	"void main(\n"
	"	float3 aPos,\n"
	"	float3 aPoids,\n"
	"	float3 aOs,\n"
	"	float2 aTexcoord,\n"
	"	float3 aNormal,\n"
	"	float4 aCouleur,\n"
	"	float2 aTexcoord1,\n"
	"	uniform float4 uL0,\n"
	"	uniform float4 uL1,\n"
	"	uniform float4 uL2,\n"
	"	uniform float4 uL3,\n"
	"	uniform float4 uB[192],\n"
	"	uniform float4 uAmb,\n"
	"	uniform float4 uLd0,\n"
	"	uniform float4 uLc0,\n"
	"	uniform float4 uLd1,\n"
	"	uniform float4 uLc1,\n"
	"	uniform float4 uLd2,\n"
	"	uniform float4 uLc2,\n"
	"	uniform float uAvecCouleur,\n"
	"	uniform float uLumK,\n"
	"	uniform float uAlphaX2,\n"
	"	uniform float uSansNormale,\n"
	"	uniform float4 uCam,\n"
	"	uniform float4 uSpec,\n"
	"	uniform float4 uO0,\n"
	"	uniform float4 uO1,\n"
	"	uniform float4 uO2,\n"
	"	uniform float4 uOB,\n"
	"	uniform float4 uFogP,\n"
	"	uniform float4 uFogC,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float4 out vLum : TEXCOORD1,\n"
	"	float3 out vSpec : TEXCOORD2,\n"
	"	float4 out vOmb : TEXCOORD3,\n"
	"	float4 out vFog : TEXCOORD4,\n"
	"	float2 out vTex1 : TEXCOORD5)\n"
	"{\n"
	"	float4 p = float4(aPos, 1.f);\n"
	"	int3 o = (int3)min(aOs, float3(63.f, 63.f, 63.f)) * 3;\n"
	"	float3 s = aPoids.x * float3(dot(uB[o.x], p), dot(uB[o.x + 1], p), dot(uB[o.x + 2], p))\n"
	"	         + aPoids.y * float3(dot(uB[o.y], p), dot(uB[o.y + 1], p), dot(uB[o.y + 2], p))\n"
	"	         + aPoids.z * float3(dot(uB[o.z], p), dot(uB[o.z + 1], p), dot(uB[o.z + 2], p));\n"
	"	float3 n = aPoids.x * float3(dot(uB[o.x].xyz, aNormal), dot(uB[o.x + 1].xyz, aNormal), dot(uB[o.x + 2].xyz, aNormal))\n"
	"	         + aPoids.y * float3(dot(uB[o.y].xyz, aNormal), dot(uB[o.y + 1].xyz, aNormal), dot(uB[o.y + 2].xyz, aNormal))\n"
	"	         + aPoids.z * float3(dot(uB[o.z].xyz, aNormal), dot(uB[o.z + 1].xyz, aNormal), dot(uB[o.z + 2].xyz, aNormal));\n"
	"	n = n * rsqrt(max(dot(n, n), 1e-8f));\n"
	"	float3 l = uAmb.xyz + uLc0.xyz * max(0.f, -dot(n, uLd0.xyz))\n"
	"	                    + uLc1.xyz * max(0.f, -dot(n, uLd1.xyz))\n"
	"	                    + uLc2.xyz * max(0.f, -dot(n, uLd2.xyz));\n"
	// aCouleur vient du cbo des modeles, DEJA double au chargement (echelle
	// 0..128 -> 0..255, p_scene_load.cpp) : on revient a l'echelle XBox du
	// registre v0 (128 = 0,5). Sans ce 0,5 le skater sortait 2 fois trop
	// clair (mesure contre xemu, Manhattan : pantalon 135 au lieu de ~50).
	"	float3 v = lerp(float3(128.f / 255.f, 128.f / 255.f, 128.f / 255.f), aCouleur.rgb * 0.5f, uAvecCouleur);\n"
	"	vLum.xyz = lerp(min(l * v, float3(1.f, 1.f, 1.f)) * uLumK, float3(1.f, 1.f, 1.f), uSansNormale);\n"
	// XBox (WeightedMeshVS_VXC : oD0 = r11 * couleur ; PixelShader0 : mul_x2 sur
	// l'alpha) : alpha = 2 x sat(r11.a x alpha sommet) x alpha texture, r11.a =
	// 1 (ambiante) + somme des N.L (alphas des lumieres a 1). Les calques
	// translucides des personnages sortent donc bien plus opaques (#46).
	"	float na = 1.f + max(0.f, -dot(n, uLd0.xyz)) + max(0.f, -dot(n, uLd1.xyz)) + max(0.f, -dot(n, uLd2.xyz));\n"
	"	float va = lerp(128.f / 255.f, aCouleur.a * 0.5f, uAvecCouleur);\n"
	"	vLum.w = lerp(1.f, 2.f * saturate(na * va), uAlphaX2 * (1.f - uSansNormale));\n"
	// rsqrt borne plutot que normalize : un vecteur nul (oeil inconnu, sommet
	// a l'origine) donnerait NaN, et 0 x NaN = NaN sur TOUS les personnages.
	"	float3 hv = uCam.xyz - s;\n"
	"	hv = hv * rsqrt(max(dot(hv, hv), 1e-8f)) - uLd0.xyz;\n"
	"	hv = hv * rsqrt(max(dot(hv, hv), 1e-8f));\n"
	"	float nh = max(dot(n, hv), 1e-6f);\n"
	"	float gs = (dot(n, uLd0.xyz) < 0.f) ? (1.f - uSansNormale) : 0.f;\n"
	"	vSpec = uSpec.rgb * (gs * pow(nh, uSpec.w));\n"
	"	float4 q = float4(s, 1.f);\n"
	"	vPosition = float4(dot(uL0, q), dot(uL1, q), dot(uL2, q), dot(uL3, q));\n"
	"	vTexcoord = aTexcoord;\n"
	"	vTex1 = aTexcoord1;\n"
	// Auto-ombrage (#45). Lignes nulles (coupe) : vOmb.xy = 0, hors carte.
	"	vOmb.xyz = float3(dot(uO0, q), dot(uO1, q), dot(uO2, q));\n"
	"	float3 fo = uO2.xyz * rsqrt(max(dot(uO2.xyz, uO2.xyz), 1e-12f));\n"
	"	float co = clamp(abs(dot(n, fo)), 0.2f, 1.f);\n"
	"	vOmb.w = uOB.x + uOB.y * min(sqrt(1.f - co * co) / co, 4.f);\n"
	BROUILLARD_VS( "dot(uL3, q)" )
	"}\n";

static const char *s_src_peau_vgl_lum_f =
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float4 vLum : TEXCOORD1,\n"
	"	float3 vSpec : TEXCOORD2,\n"
	"	float4 vOmb : TEXCOORD3,\n"
	"	float4 vFog : TEXCOORD4,\n"
	"	float2 vTex1 : TEXCOORD5,\n"
	"	uniform sampler2D uTex,\n"
	"	uniform sampler2D uOmbre,\n"
	"	uniform sampler2D uGloss,\n"
	"	uniform float4 uTeinte,\n"
	"	uniform float uAvecTexture,\n"
	"	uniform float uAomK)\n"
	"{\n"
	"	float4 t = tex2D(uTex, vTexcoord);\n"
	"	float4 c = lerp(float4(1.f, 1.f, 1.f, 1.f), t, uAvecTexture) * uTeinte;\n"
	"	float a = saturate(c.a * vLum.w);\n"
	"	clip(a - 0.5f / 255.f);\n"
	"	float4 mo = tex2D(uOmbre, vOmb.xy);\n"
	"	float occ = mo.r / max(mo.g, 1.f / 255.f);\n"
	"	float so = mo.g * saturate((saturate(vOmb.z) - occ - vOmb.w) * 127.f);\n"
	"	float2 ho = step(float2(1.f, 1.f), vOmb.xy) + step(vOmb.xy, float2(0.f, 0.f));\n"
	"	so = so * (1.f - saturate(ho.x + ho.y));\n"
	"	float ko = 1.f - uAomK * so;\n"
	"	float gm = tex2D(uGloss, vTex1).a;\n"
	"	return float4((saturate(saturate(c.rgb * vLum.xyz) + vSpec * gm) * vFog.a + vFog.rgb) * ko, a);\n"
	"}\n";

enum { ATTR_PEAU_NORMALE = 4, ATTR_PEAU_COULEUR = 5, ATTR_PEAU_UV1 = 6 };

static int    s_etat_peau_lum = 0;
static GLuint s_prog_peau_lum = 0;
static GLint  s_pl_l[4], s_pl_b, s_pl_teinte, s_pl_avec, s_pl_fogp, s_pl_fogc;
static GLint  s_pl_amb, s_pl_ld[3], s_pl_lc[3], s_pl_avec_coul, s_pl_sans_n, s_pl_lumk;
// "lmk N" : facteur final de la lumiere des personnages (XBox PixelShader0 : mul_x4
// sur v0 sature ; 2 = ancien reglage). #46.
float g_vita_lum_k = 2.0f;
// "ax2 0/1" : alpha des personnages double comme XBox (#46).
int g_vita_alpha_x2 = 1;
static GLint s_pl_ax2 = -1;

// "puc 0/1" : uniformes de SOMMETS de la peau eclairee poses seulement quand
// leur valeur change (issue #18, phase logique).
//
// [SOURCE] vitaGL custom_shaders.c : tout glUniform* sur un uniforme de
// sommets met dirty_shader_vert_unifs a vrai (vgl_fill_uniform_data), et le
// dessin suivant recopie ALORS TOUT le tampon d'uniformes de sommets du
// programme (upload_uniforms : vglReserveVertexUniformBuffer + memcpy de
// unif_buf_size) -- ici 192 float4 d'os + matrice + lumieres, ~3,3 Ko.
// ShaderPeauEclaireePiece posait uAvecCouleur et uSansNormale (deux uniformes
// de SOMMETS) a chaque piece : chaque glDrawElements d'une piece recopiait
// donc les 64 os, alors qu'ils ne changent qu'a ShaderPeauEclaireeDebut.
// La version non eclairee (ShaderPeauPiece) ne pose que des uniformes de
// fragments (uTeinte, uAvecTexture) et n'avait pas ce defaut.
//
// La valeur d'un uniforme vit dans le programme (p->unif_vbuffer) et survit
// a glUseProgram : si la derniere valeur posee est la meme, le programme
// l'a deja. Seule ShaderPeauEclaireePiece ecrit ces deux uniformes ; le
// cache est remis a -1 a la creation du programme. Meme image au pixel.
// Defaut 0 tant que l'A/B n'est pas fait (protocole AGENTS.md section 4).
bool g_vita_peau_uc = true;	// mesure NJ V1 2026-10-04 : logique -0,2/-0,3 ms
static float s_pl_der_avec_coul = -1.0f;
static float s_pl_der_sans_n    = -1.0f;
// Speculaire (issue #45) : uniformes de SOMMETS eux aussi, poses seulement
// quand ils changent (meme raison que ci-dessus) ; l'oeil en espace modele
// est recalcule a chaque ShaderPeauEclaireeDebut.
static GLint s_pl_cam = -1, s_pl_spec = -1;
static float s_pl_der_spec[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
static bool  s_pl_oeil_ok = false;
// Auto-ombrage (#45) : lignes vers la carte et biais (SOMMETS, poses
// seulement quand il est actif pour le modele), part d'ombre (fragments).
// Carte neutre 1x1 (couverture nulle) sur l'unite 1 hors auto-ombrage : le
// sampler doit toujours lire une texture valide.
static GLint  s_pl_o[3] = { -1, -1, -1 }, s_pl_ob = -1, s_pl_aomk = -1;
static float  s_pl_der_aomk = -1.0f;
static GLuint s_tex_aom_neutre = 0;
// GLOSS_MAP (#45) : texture blanche 1x1 (alpha 1) sur l'unite 2 hors gloss,
// et texture actuellement liee sur l'unite 2 (remise a la blanche par Debut).
static GLuint s_tex_gloss_neutre = 0;
static GLuint s_pl_der_gloss = 0;

bool ShaderPeauEclaireePret()
{
	if( s_etat_peau_lum != 0 )
		return ( s_etat_peau_lum > 0 );
	s_etat_peau_lum = -1;
	GLuint vs = compiler( GL_CG_VERTEX_SHADER_EXT, s_src_peau_vgl_lum_v, "peau eclairee sommets" );
	GLuint fs = compiler( GL_CG_FRAGMENT_SHADER_EXT, s_src_peau_vgl_lum_f, "peau eclairee pixels" );
	if( !vs || !fs )
		return false;
	s_prog_peau_lum = glCreateProgram();
	glAttachShader( s_prog_peau_lum, vs );
	glAttachShader( s_prog_peau_lum, fs );
	glBindAttribLocation( s_prog_peau_lum, ATTR_PEAU_POS,     "aPos" );
	glBindAttribLocation( s_prog_peau_lum, ATTR_PEAU_POIDS,   "aPoids" );
	glBindAttribLocation( s_prog_peau_lum, ATTR_PEAU_OS,      "aOs" );
	glBindAttribLocation( s_prog_peau_lum, ATTR_PEAU_UV,      "aTexcoord" );
	glBindAttribLocation( s_prog_peau_lum, ATTR_PEAU_NORMALE, "aNormal" );
	glBindAttribLocation( s_prog_peau_lum, ATTR_PEAU_COULEUR, "aCouleur" );
	glBindAttribLocation( s_prog_peau_lum, ATTR_PEAU_UV1,     "aTexcoord1" );
	glLinkProgram( s_prog_peau_lum );
	GLint ok = 0;
	glGetProgramiv( s_prog_peau_lum, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		VLOG( "SHD", "!! peau eclairee : edition de liens refusee" );
		return false;
	}
	static const char *noms_l[4] = { "uL0", "uL1", "uL2", "uL3" };
	for( int k = 0; k < 4; ++k )
		s_pl_l[k] = glGetUniformLocation( s_prog_peau_lum, noms_l[k] );
	s_pl_b         = glGetUniformLocation( s_prog_peau_lum, "uB" );
	s_pl_teinte    = glGetUniformLocation( s_prog_peau_lum, "uTeinte" );
	s_pl_avec      = glGetUniformLocation( s_prog_peau_lum, "uAvecTexture" );
	s_pl_fogp      = glGetUniformLocation( s_prog_peau_lum, "uFogP" );
	s_pl_fogc      = glGetUniformLocation( s_prog_peau_lum, "uFogC" );
	s_pl_amb       = glGetUniformLocation( s_prog_peau_lum, "uAmb" );
	s_pl_ld[0]     = glGetUniformLocation( s_prog_peau_lum, "uLd0" );
	s_pl_ld[1]     = glGetUniformLocation( s_prog_peau_lum, "uLd1" );
	s_pl_lc[0]     = glGetUniformLocation( s_prog_peau_lum, "uLc0" );
	s_pl_lc[1]     = glGetUniformLocation( s_prog_peau_lum, "uLc1" );
	s_pl_ld[2]     = glGetUniformLocation( s_prog_peau_lum, "uLd2" );	// lumiere de scene
	s_pl_lc[2]     = glGetUniformLocation( s_prog_peau_lum, "uLc2" );
	s_pl_avec_coul = glGetUniformLocation( s_prog_peau_lum, "uAvecCouleur" );
	s_pl_sans_n    = glGetUniformLocation( s_prog_peau_lum, "uSansNormale" );
	s_pl_lumk      = glGetUniformLocation( s_prog_peau_lum, "uLumK" );
	s_pl_ax2       = glGetUniformLocation( s_prog_peau_lum, "uAlphaX2" );
	s_pl_cam       = glGetUniformLocation( s_prog_peau_lum, "uCam" );
	s_pl_spec      = glGetUniformLocation( s_prog_peau_lum, "uSpec" );
	s_pl_o[0]      = glGetUniformLocation( s_prog_peau_lum, "uO0" );
	s_pl_o[1]      = glGetUniformLocation( s_prog_peau_lum, "uO1" );
	s_pl_o[2]      = glGetUniformLocation( s_prog_peau_lum, "uO2" );
	s_pl_ob        = glGetUniformLocation( s_prog_peau_lum, "uOB" );
	s_pl_aomk      = glGetUniformLocation( s_prog_peau_lum, "uAomK" );
	if(( s_pl_b < 0 ) || ( s_pl_amb < 0 ) || ( s_pl_ld[0] < 0 ) || ( s_pl_lc[0] < 0 )
	   || ( s_pl_ld[1] < 0 ) || ( s_pl_lc[1] < 0 ))
	{
		VLOG( "SHD", "!! peau eclairee : uniforme absent" );
		return false;
	}
	glUseProgram( s_prog_peau_lum );
	glUniform1i( glGetUniformLocation( s_prog_peau_lum, "uTex" ), 0 );
	glUniform1i( glGetUniformLocation( s_prog_peau_lum, "uOmbre" ), 1 );
	glUniform1i( glGetUniformLocation( s_prog_peau_lum, "uGloss" ), 2 );
	glUseProgram( 0 );
	if( !s_tex_aom_neutre || !s_tex_gloss_neutre )
	{
		GLint act_avant = 0, tex_avant = 0;
		glGetIntegerv( GL_ACTIVE_TEXTURE, &act_avant );
		glActiveTexture( GL_TEXTURE0 );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex_avant );
		static const unsigned char nul[4]   = { 0, 0, 0, 0 };
		static const unsigned char blanc[4] = { 255, 255, 255, 255 };
		GLuint *pt[2] = { &s_tex_aom_neutre, &s_tex_gloss_neutre };
		const unsigned char *px[2] = { nul, blanc };
		for( int k = 0; k < 2; ++k )
		{
			if( *pt[k] )
				continue;
			glGenTextures( 1, pt[k] );
			glBindTexture( GL_TEXTURE_2D, *pt[k] );
			glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px[k] );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
			glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		}
		glBindTexture( GL_TEXTURE_2D, tex_avant );
		glActiveTexture( act_avant );
	}
	s_pl_der_aomk = -1.0f;
	VLOG( "SHD", "programme peau eclairee pret" );
	s_pl_der_avec_coul = -1.0f;
	s_pl_der_sans_n    = -1.0f;
	for( int k = 0; k < 4; ++k )
		s_pl_der_spec[k] = -1.0f;
	s_etat_peau_lum = 1;
	return true;
}

void ShaderPeauEclaireeDebut( const float *mvp, const float *p_os, int num_os,
                              const float *lum )
{
	glUseProgram( s_prog_peau_lum );
	for( int r = 0; r < 4; ++r )
		glUniform4f( s_pl_l[r], mvp[r], mvp[4 + r], mvp[8 + r], mvp[12 + r] );
	memset( s_os_lignes, 0, sizeof( s_os_lignes ));
	for( int b = 0; ( b < num_os ) && ( b < PEAU_MAX_OS ); ++b )
	{
		const float *m = &p_os[b * 16];
		for( int c = 0; c < 3; ++c )
		{
			float *l = &s_os_lignes[( b * 3 + c ) * 4];
			l[0] = m[0 * 4 + c];
			l[1] = m[1 * 4 + c];
			l[2] = m[2 * 4 + c];
			l[3] = m[3 * 4 + c];
		}
	}
	glUniform4fv( s_pl_b, PEAU_MAX_OS * 3, s_os_lignes );
	glUniform4f( s_pl_amb, lum[0], lum[1], lum[2], 0.0f );
	glUniform1f( s_pl_lumk, g_vita_lum_k );
	glUniform1f( s_pl_ax2, g_vita_alpha_x2 ? 1.0f : 0.0f );
	// Oeil en espace MODELE, pour la speculaire : le point que la projection
	// envoie en x = y = w = 0 (lignes 0, 1 et 3 de mvp). XBox le calcule
	// depuis la matrice racine (anim.cpp:378) ; ici mvp suffit, et le chemin
	// differe (SPeauReportee) n'a que lui. Projection orthogonale : pas de
	// solution, speculaire coupee.
	if( s_pl_cam >= 0 )
	{
		const float a0[3] = { mvp[0], mvp[4], mvp[8] };
		const float a1[3] = { mvp[1], mvp[5], mvp[9] };
		const float a3[3] = { mvp[3], mvp[7], mvp[11] };
		const float b0 = -mvp[12], b1 = -mvp[13], b3 = -mvp[15];
		const float c13[3] = { a1[1] * a3[2] - a1[2] * a3[1], a1[2] * a3[0] - a1[0] * a3[2], a1[0] * a3[1] - a1[1] * a3[0] };
		const float c30[3] = { a3[1] * a0[2] - a3[2] * a0[1], a3[2] * a0[0] - a3[0] * a0[2], a3[0] * a0[1] - a3[1] * a0[0] };
		const float c01[3] = { a0[1] * a1[2] - a0[2] * a1[1], a0[2] * a1[0] - a0[0] * a1[2], a0[0] * a1[1] - a0[1] * a1[0] };
		const float det = a0[0] * c13[0] + a0[1] * c13[1] + a0[2] * c13[2];
		const float n0 = sqrtf( a0[0] * a0[0] + a0[1] * a0[1] + a0[2] * a0[2] );
		const float n1 = sqrtf( a1[0] * a1[0] + a1[1] * a1[1] + a1[2] * a1[2] );
		const float n3 = sqrtf( a3[0] * a3[0] + a3[1] * a3[1] + a3[2] * a3[2] );
		s_pl_oeil_ok = fabsf( det ) > 1e-6f * n0 * n1 * n3;
		float e[3] = { 0.0f, 0.0f, 0.0f };
		if( s_pl_oeil_ok )
			for( int k = 0; k < 3; ++k )
				e[k] = ( b0 * c13[k] + b1 * c30[k] + b3 * c01[k] ) / det;
		glUniform4f( s_pl_cam, e[0], e[1], e[2], 0.0f );
	}
	for( int k = 0; k < 3; ++k )
	{
		if(( s_pl_ld[k] < 0 ) || ( s_pl_lc[k] < 0 ))
			continue;
		glUniform4f( s_pl_ld[k], lum[3 + k * 6], lum[4 + k * 6], lum[5 + k * 6], 0.0f );
		glUniform4f( s_pl_lc[k], lum[6 + k * 6], lum[7 + k * 6], lum[8 + k * 6], 0.0f );
	}
	{
		float P[4], C[4];
		BrouillardUniformes( false, false, P, C );
		glUniform4fv( s_pl_fogp, 1, P );
		glUniform4fv( s_pl_fogc, 1, C );
	}
	// Auto-ombrage (#45) : neutre, ShaderPeauEclaireeAutoOmbre l'active.
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, s_tex_aom_neutre );
	glActiveTexture( GL_TEXTURE2 );		// GLOSS_MAP (#45) : neutre
	glBindTexture( GL_TEXTURE_2D, s_tex_gloss_neutre );
	s_pl_der_gloss = s_tex_gloss_neutre;
	glActiveTexture( GL_TEXTURE0 );
	if(( s_pl_aomk >= 0 ) && ( s_pl_der_aomk != 0.0f ))
	{
		glUniform1f( s_pl_aomk, 0.0f );
		s_pl_der_aomk = 0.0f;
	}
	glEnableVertexAttribArray( ATTR_PEAU_POS );
	glEnableVertexAttribArray( ATTR_PEAU_POIDS );
	glEnableVertexAttribArray( ATTR_PEAU_OS );
	glEnableVertexAttribArray( ATTR_PEAU_UV );
	glEnableVertexAttribArray( ATTR_PEAU_NORMALE );
	glEnableVertexAttribArray( ATTR_PEAU_COULEUR );
	glEnableVertexAttribArray( ATTR_PEAU_UV1 );
}

void ShaderPeauEclaireeAutoOmbre( const float *O, unsigned int tex, float k, float b0, float b1 )
{
	if( !O || !tex || ( s_pl_aomk < 0 ) || ( s_pl_ob < 0 )
	    || ( s_pl_o[0] < 0 ) || ( s_pl_o[1] < 0 ) || ( s_pl_o[2] < 0 ))
		return;
	for( int l = 0; l < 3; ++l )
		glUniform4f( s_pl_o[l], O[l * 4], O[l * 4 + 1], O[l * 4 + 2], O[l * 4 + 3] );
	glUniform4f( s_pl_ob, b0, b1, 0.0f, 0.0f );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, tex );
	glActiveTexture( GL_TEXTURE0 );
	glUniform1f( s_pl_aomk, k );
	s_pl_der_aomk = k;
}

void ShaderPeauEclaireePiece( unsigned int vbo_repos, unsigned int vbo_poids, unsigned int vbo_os,
                              unsigned int uvbo, bool avec_texture, const float teinte[4],
                              unsigned int vbo_normales, unsigned int cbo, const float *spec,
                              unsigned int gloss_tex, unsigned int gloss_uvbo, int gloss_clamp )
{
	// GLOSS_MAP (#45) : jeu d'UV 1 et texture de la passe 1 (unite 2) ; hors
	// gloss, l'attribut lit un tampon valide quelconque et la texture blanche
	// donne un facteur 1.
	const bool gloss = ( gloss_tex && gloss_uvbo );
	glBindBuffer( GL_ARRAY_BUFFER, gloss ? gloss_uvbo : ( uvbo ? uvbo : vbo_repos ));
	glVertexAttribPointer( ATTR_PEAU_UV1, 2, GL_FLOAT, GL_FALSE, 0, NULL );
	{
		const GLuint tg = gloss ? (GLuint)gloss_tex : s_tex_gloss_neutre;
		if( gloss || ( tg != s_pl_der_gloss ))
		{
			glActiveTexture( GL_TEXTURE2 );
			glBindTexture( GL_TEXTURE_2D, tg );
			if( gloss )
			{
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ( gloss_clamp & 1 ) ? GL_CLAMP_TO_EDGE : GL_REPEAT );
				glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, ( gloss_clamp & 2 ) ? GL_CLAMP_TO_EDGE : GL_REPEAT );
			}
			glActiveTexture( GL_TEXTURE0 );
			s_pl_der_gloss = tg;
		}
	}
	glBindBuffer( GL_ARRAY_BUFFER, vbo_repos );
	glVertexAttribPointer( ATTR_PEAU_POS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, vbo_poids );
	glVertexAttribPointer( ATTR_PEAU_POIDS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, vbo_os );
	glVertexAttribPointer( ATTR_PEAU_OS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, uvbo ? uvbo : vbo_repos );
	glVertexAttribPointer( ATTR_PEAU_UV, 2, GL_FLOAT, GL_FALSE, 0, NULL );
	// Absents : l'attribut lit la pose de repos (12 octets par sommet, donc
	// toujours dans le tampon) et l'uniforme correspondant l'ignore.
	glBindBuffer( GL_ARRAY_BUFFER, vbo_normales ? vbo_normales : vbo_repos );
	glVertexAttribPointer( ATTR_PEAU_NORMALE, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, cbo ? cbo : vbo_repos );
	glVertexAttribPointer( ATTR_PEAU_COULEUR, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glUniform4f( s_pl_teinte, teinte[0], teinte[1], teinte[2], teinte[3] );
	glUniform1f( s_pl_avec, avec_texture ? 1.0f : 0.0f );
	// Uniformes de SOMMETS : voir g_vita_peau_uc plus haut. Avec puc 0, on
	// les pose comme avant mais on tient le cache a jour, pour que basculer
	// a chaud reste exact.
	const float avec_coul = cbo ? 1.0f : 0.0f;
	const float sans_n    = vbo_normales ? 0.0f : 1.0f;
	if( !g_vita_peau_uc || ( avec_coul != s_pl_der_avec_coul ))
	{
		glUniform1f( s_pl_avec_coul, avec_coul );
		s_pl_der_avec_coul = avec_coul;
	}
	if( !g_vita_peau_uc || ( sans_n != s_pl_der_sans_n ))
	{
		glUniform1f( s_pl_sans_n, sans_n );
		s_pl_der_sans_n = sans_n;
	}
	if( s_pl_spec >= 0 )
	{
		float sp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		if( spec && ( spec[3] > 0.0f ) && s_pl_oeil_ok )
			memcpy( sp, spec, sizeof( sp ));
		if( !g_vita_peau_uc || memcmp( sp, s_pl_der_spec, sizeof( sp )))
		{
			glUniform4fv( s_pl_spec, 1, sp );
			memcpy( s_pl_der_spec, sp, sizeof( sp ));
		}
	}
}

void ShaderPeauEclaireeFin()
{
	glActiveTexture( GL_TEXTURE1 );		// auto-ombrage (#45) : unite 1 rendue
	glBindTexture( GL_TEXTURE_2D, 0 );
	glActiveTexture( GL_TEXTURE2 );		// GLOSS_MAP (#45) : unite 2 rendue
	glBindTexture( GL_TEXTURE_2D, 0 );
	s_pl_der_gloss = 0;
	glActiveTexture( GL_TEXTURE0 );
	glDisableVertexAttribArray( ATTR_PEAU_UV1 );
	glDisableVertexAttribArray( ATTR_PEAU_NORMALE );
	glDisableVertexAttribArray( ATTR_PEAU_COULEUR );
	ShaderPeauFin();
}


// --- Modeles RIGIDES eclaires, chemin vitaGL (issue #67) --------------------
//
// Remplace le calcul CPU d'eclairer_piece (p_NxModel.cpp) et son
// glBufferSubData, formule IDENTIQUE, sommet par sommet :
//
//   l   = ambiante + somme_i couleur_i x max( 0, N.dir_i )     (N brute, dir_i
//         telles que lumieres_rigides les donne : pas de signe moins, pas de
//         normalisation de N ni de la 3e direction)
//   col = min( cbo x k x l, 1 ), k = 0,5 avec texture, 1 sans ; alpha = cbo
//
// puis le pixel, comme le pipeline fixe de vitaGL le faisait sur ces
// couleurs (shaders/ffp_f.h, tex_env.h) :
//   - texture : GL_COMBINE MODULATE, GL_RGB_SCALE 2, sature (combine_src) :
//     rgb = sat( t x col x 2 ), a = sat( t.a x col.a ) ;
//   - sans texture : la couleur de sommet seule (le pipeline fixe ignore
//     glColor quand GL_COLOR_ARRAY est actif : has_colors = 1, LtintColor
//     inutilise) -- d'ou la texture blanche et uX2 = 1 ;
//   - test alpha GL_GREATER 0,35 des pieces melangees (uSeuil, < 0 = coupe) ;
//   - brouillard : BROUILLARD_VS, celui des autres shaders (le glFog de
//     vitaGL n'agit pas sur un programme). Seul ecart connu : au-dela de
//     proche + plage, XBox (et ce shader) plafonnent a la densite, le
//     pipeline fixe continuait de monter (voir BrouillardFixe).
// Ecart d'arrondi : le CPU tronquait chaque couleur a l'octet, ici elle reste
// flottante (au plus 1/255).
static const char *s_src_rig_lum_v =
	"void main(\n"
	"	float3 aPos,\n"
	"	float3 aNormal,\n"
	"	float4 aCouleur,\n"
	"	float2 aTexcoord,\n"
	"	uniform float4 uL0,\n"
	"	uniform float4 uL1,\n"
	"	uniform float4 uL2,\n"
	"	uniform float4 uL3,\n"
	"	uniform float4 uAmb,\n"
	"	uniform float4 uLd0,\n"
	"	uniform float4 uLc0,\n"
	"	uniform float4 uLd1,\n"
	"	uniform float4 uLc1,\n"
	"	uniform float4 uLd2,\n"
	"	uniform float4 uLc2,\n"
	"	uniform float uEch,\n"
	"	uniform float4 uFogP,\n"
	"	uniform float4 uFogC,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float4 out vCouleur : TEXCOORD1,\n"
	"	float4 out vFog : TEXCOORD4)\n"
	"{\n"
	"	float4 q = float4(aPos, 1.f);\n"
	"	float3 l = uAmb.xyz + uLc0.xyz * max(0.f, dot(aNormal, uLd0.xyz))\n"
	"	                    + uLc1.xyz * max(0.f, dot(aNormal, uLd1.xyz))\n"
	"	                    + uLc2.xyz * max(0.f, dot(aNormal, uLd2.xyz));\n"
	"	vCouleur = float4(min(aCouleur.rgb * uEch * l, float3(1.f, 1.f, 1.f)), aCouleur.a);\n"
	"	vPosition = float4(dot(uL0, q), dot(uL1, q), dot(uL2, q), dot(uL3, q));\n"
	"	vTexcoord = aTexcoord;\n"
	BROUILLARD_VS( "dot(uL3, q)" )
	"}\n";

static const char *s_src_rig_lum_f =
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float4 vCouleur : TEXCOORD1,\n"
	"	float4 vFog : TEXCOORD4,\n"
	"	uniform sampler2D uTex,\n"
	"	uniform float uX2,\n"
	"	uniform float uSeuil)\n"
	"{\n"
	"	float4 c = saturate(tex2D(uTex, vTexcoord) * vCouleur * float4(uX2, uX2, uX2, 1.f));\n"
	"	clip(c.a - uSeuil);\n"
	"	return float4(c.rgb * vFog.a + vFog.rgb, c.a);\n"
	"}\n";

// Emplacements d'attributs propres a ce programme (les attributs actives
// sont communs a tous les programmes : Fin les coupe).
enum { ATTR_RIG_POS = 0, ATTR_RIG_NORMALE = 1, ATTR_RIG_COULEUR = 2, ATTR_RIG_UV = 3 };

static int    s_etat_rig = 0;
static GLuint s_prog_rig = 0;
static GLint  s_rg_l[4], s_rg_amb, s_rg_ld[3], s_rg_lc[3], s_rg_ech, s_rg_fogp, s_rg_fogc;
static GLint  s_rg_x2, s_rg_seuil;
static float  s_rg_der_x2 = -1.0f, s_rg_der_seuil = -2.0f;
static GLuint s_tex_blanc_rig = 0;

bool ShaderRigideEclairePret()
{
	if( s_etat_rig != 0 )
		return ( s_etat_rig > 0 );
	s_etat_rig = -1;
	GLuint vs = compiler( GL_CG_VERTEX_SHADER_EXT, s_src_rig_lum_v, "rigide eclaire sommets" );
	GLuint fs = compiler( GL_CG_FRAGMENT_SHADER_EXT, s_src_rig_lum_f, "rigide eclaire pixels" );
	if( !vs || !fs )
		return false;
	s_prog_rig = glCreateProgram();
	glAttachShader( s_prog_rig, vs );
	glAttachShader( s_prog_rig, fs );
	glBindAttribLocation( s_prog_rig, ATTR_RIG_POS,     "aPos" );
	glBindAttribLocation( s_prog_rig, ATTR_RIG_NORMALE, "aNormal" );
	glBindAttribLocation( s_prog_rig, ATTR_RIG_COULEUR, "aCouleur" );
	glBindAttribLocation( s_prog_rig, ATTR_RIG_UV,      "aTexcoord" );
	glLinkProgram( s_prog_rig );
	GLint ok = 0;
	glGetProgramiv( s_prog_rig, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		VLOG( "SHD", "!! rigide eclaire : edition de liens refusee" );
		return false;
	}
	static const char *noms_l[4] = { "uL0", "uL1", "uL2", "uL3" };
	for( int k = 0; k < 4; ++k )
		s_rg_l[k] = glGetUniformLocation( s_prog_rig, noms_l[k] );
	s_rg_amb   = glGetUniformLocation( s_prog_rig, "uAmb" );
	s_rg_ld[0] = glGetUniformLocation( s_prog_rig, "uLd0" );
	s_rg_lc[0] = glGetUniformLocation( s_prog_rig, "uLc0" );
	s_rg_ld[1] = glGetUniformLocation( s_prog_rig, "uLd1" );
	s_rg_lc[1] = glGetUniformLocation( s_prog_rig, "uLc1" );
	s_rg_ld[2] = glGetUniformLocation( s_prog_rig, "uLd2" );
	s_rg_lc[2] = glGetUniformLocation( s_prog_rig, "uLc2" );
	s_rg_ech   = glGetUniformLocation( s_prog_rig, "uEch" );
	s_rg_fogp  = glGetUniformLocation( s_prog_rig, "uFogP" );
	s_rg_fogc  = glGetUniformLocation( s_prog_rig, "uFogC" );
	s_rg_x2    = glGetUniformLocation( s_prog_rig, "uX2" );
	s_rg_seuil = glGetUniformLocation( s_prog_rig, "uSeuil" );
	bool manque = ( s_rg_amb < 0 ) || ( s_rg_ech < 0 ) || ( s_rg_fogp < 0 ) || ( s_rg_fogc < 0 )
	              || ( s_rg_x2 < 0 ) || ( s_rg_seuil < 0 );
	for( int k = 0; k < 4; ++k )
		manque = manque || ( s_rg_l[k] < 0 );
	for( int k = 0; k < 3; ++k )
		manque = manque || ( s_rg_ld[k] < 0 ) || ( s_rg_lc[k] < 0 );
	if( manque )
	{
		VLOG( "SHD", "!! rigide eclaire : uniforme absent" );
		return false;
	}
	glUseProgram( s_prog_rig );
	glUniform1i( glGetUniformLocation( s_prog_rig, "uTex" ), 0 );
	glUseProgram( 0 );
	if( !s_tex_blanc_rig )
	{
		GLint act_avant = 0, tex_avant = 0;
		glGetIntegerv( GL_ACTIVE_TEXTURE, &act_avant );
		glActiveTexture( GL_TEXTURE0 );
		glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex_avant );
		static const unsigned char blanc[4] = { 255, 255, 255, 255 };
		glGenTextures( 1, &s_tex_blanc_rig );
		glBindTexture( GL_TEXTURE_2D, s_tex_blanc_rig );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, blanc );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, tex_avant );
		glActiveTexture( act_avant );
	}
	s_rg_der_x2    = -1.0f;
	s_rg_der_seuil = -2.0f;
	VLOG( "SHD", "programme rigide eclaire pret" );
	s_etat_rig = 1;
	return true;
}

void ShaderRigideEclaireDebut( const float *lum )
{
	glUseProgram( s_prog_rig );
	glActiveTexture( GL_TEXTURE0 );
	glUniform4f( s_rg_amb, lum[0], lum[1], lum[2], 0.0f );
	for( int k = 0; k < 3; ++k )
	{
		glUniform4f( s_rg_ld[k], lum[3 + k * 6], lum[4 + k * 6], lum[5 + k * 6], 0.0f );
		glUniform4f( s_rg_lc[k], lum[6 + k * 6], lum[7 + k * 6], lum[8 + k * 6], 0.0f );
	}
	glEnableVertexAttribArray( ATTR_RIG_POS );
	glEnableVertexAttribArray( ATTR_RIG_NORMALE );
	glEnableVertexAttribArray( ATTR_RIG_COULEUR );
	glEnableVertexAttribArray( ATTR_RIG_UV );
}

void ShaderRigideEclairePiece( const float *mvp, unsigned int vbo, unsigned int nbo,
                               unsigned int cbo, unsigned int uvbo, bool avec_texture,
                               float seuil_alpha, bool fog_noir )
{
	glBindBuffer( GL_ARRAY_BUFFER, vbo );
	glVertexAttribPointer( ATTR_RIG_POS, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, nbo );
	glVertexAttribPointer( ATTR_RIG_NORMALE, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, cbo );
	glVertexAttribPointer( ATTR_RIG_COULEUR, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, NULL );
	// Sans jeu d'UV, l'attribut lit les positions (12 octets par sommet,
	// toujours dans le tampon) : la texture blanche ignore la valeur.
	glBindBuffer( GL_ARRAY_BUFFER, ( avec_texture && uvbo ) ? uvbo : vbo );
	glVertexAttribPointer( ATTR_RIG_UV, 2, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	if( !avec_texture )
		glBindTexture( GL_TEXTURE_2D, s_tex_blanc_rig );
	for( int r = 0; r < 4; ++r )
		glUniform4f( s_rg_l[r], mvp[r], mvp[4 + r], mvp[8 + r], mvp[12 + r] );
	glUniform1f( s_rg_ech, avec_texture ? 0.5f : 1.0f );
	{
		float P[4], C[4];
		BrouillardUniformes( fog_noir, false, P, C );
		glUniform4fv( s_rg_fogp, 1, P );
		glUniform4fv( s_rg_fogc, 1, C );
	}
	// Uniformes de FRAGMENTS : poses seulement quand ils changent (leur
	// valeur vit dans le programme, voir g_vita_peau_uc).
	const float x2 = avec_texture ? 2.0f : 1.0f;
	if( x2 != s_rg_der_x2 )
	{
		glUniform1f( s_rg_x2, x2 );
		s_rg_der_x2 = x2;
	}
	if( seuil_alpha != s_rg_der_seuil )
	{
		glUniform1f( s_rg_seuil, seuil_alpha );
		s_rg_der_seuil = seuil_alpha;
	}
}

void ShaderRigideEclaireFin()
{
	glDisableVertexAttribArray( ATTR_RIG_POS );
	glDisableVertexAttribArray( ATTR_RIG_NORMALE );
	glDisableVertexAttribArray( ATTR_RIG_COULEUR );
	glDisableVertexAttribArray( ATTR_RIG_UV );
	glUseProgram( 0 );
}



static unsigned int s_fog_gen = 1;	// generation des uniformes de brouillard (aussi utilisee hors GXM)
#ifndef THUG_DESKTOP
// --- Chemin GXM direct pour les lots de decor (issue #18) --------------------
//
// [MESURE] Un lot par vitaGL coute ~15 us de CPU a 444 MHz (preparation ~6,
// glDrawElements ~8), pour 500 a 1300 lots par image en zone dense : le GPU
// attend le CPU. Chaque dessin vitaGL repasse par toute sa machinerie
// generique (programmes patches, attributs, uniformes, textures).
//
// Ici, pour un lot : changement de programme seulement si la variante change,
// matrice envoyee une fois par changement de programme, couleurs de passe,
// textures et flux de sommets, sceGxmDraw. Rien d'autre.
//
// Cohabitation avec vitaGL : il applique ses etats GXM (profondeur, culling,
// viewport) immediatement, nos dessins en heritent. Il faut seulement que sa
// scene soit ouverte (scene_reset) avant, et lui faire tout recharger apres
// (programmes, uniformes) : c'est GxmMateriauFin.


extern "C"
{
	extern SceGxmContext       *gxm_context;
	extern SceGxmShaderPatcher *gxm_shader_patcher;
	extern SceGxmMultisampleMode msaa_mode;
	extern GLboolean dirty_shader_vert_unifs, dirty_shader_frag_unifs;
	extern GLboolean ffp_dirty_vert, ffp_dirty_frag;
	extern uint16_t  dirty_vert_unifs;
	extern uint32_t  dirty_frag_unifs;
	void scene_reset( void );
	extern uint32_t vgl_framecount;
	void *vgl_memalign( size_t alignment, size_t size, vglMemType type );
}

struct SGxmVariante
{
	SceGxmProgram         *p_vs, *p_fs;
	SceGxmShaderPatcherId  vs_id, fs_id;
	SceGxmVertexProgram   *vp;
	SceGxmFragmentProgram *fp;			// opaque, sans melange
	SceGxmFragmentProgram *fp_m[7];		// par famille de melange (translucides)
	const SceGxmProgramParameter *pL[4], *pC[4], *pSeuil;
	int unite[4];
	int passes;
	bool ok;
	// Issue #5 : flux de chaque passe (-1 = passe en reflet, sans UV), flux des
	// normales, et uniformes de vue et de tuilage.
	unsigned int env;
	int flux_uv[4];
	int flux_n;
	const SceGxmProgramParameter *pV[3], *pE[4];
	// Issue #43 : passes a UV wibble et leur uniforme de decalage.
	unsigned int wib;
	const SceGxmProgramParameter *pW[4];
	// Issue #45 : brouillard, uniformes de sommets.
	const SceGxmProgramParameter *pFogP, *pFogC;
};

// « gxd 0/1 » : lots opaques par le chemin GXM direct.
bool g_vita_gxm_direct = true;

static unsigned char s_bin[64 * 1024];

// Binaire vitaGL d'un shader -> SceGxmProgram enregistre aupres du patcher.
// Le binaire vitaGL est un en-tete (uniformes matrices, ...) suivi du
// programme GXM, qui commence par la signature « GXP\0 ».
static SceGxmProgram *programme_gxm( GLuint sh, SceGxmShaderPatcherId *p_id )
{
	GLsizei n = 0;
	vglGetShaderBinary( sh, sizeof( s_bin ), &n, s_bin );
	if(( n <= 0 ) || ( n > (GLsizei)sizeof( s_bin )))
		return NULL;
	int o = -1;
	for( int i = 0; i + 4 <= n; i += 4 )
		if(( s_bin[i] == 'G' ) && ( s_bin[i + 1] == 'X' ) && ( s_bin[i + 2] == 'P' ) && ( s_bin[i + 3] == 0 ))
		{
			o = i;
			break;
		}
	if( o < 0 )
		return NULL;
	const int taille = n - o;
	SceGxmProgram *p = (SceGxmProgram *)memalign( 16, taille );
	if( !p )
		return NULL;
	memcpy( p, s_bin + o, taille );
	if(( sceGxmProgramCheck( p ) != 0 )
	    || ( sceGxmShaderPatcherRegisterProgram( gxm_shader_patcher, p, p_id ) != 0 ))
	{
		free( p );
		return NULL;
	}
	return p;
}

// « vpr 0/1 » (issue #69) : programme de sommets PARTAGE entre variantes.
// Le vertex shader vitaGL est deja unique par (passes, reflet, wibble)
// (vertex_shader_m, s_vs_m), mais chaque variante en tirait SA copie du
// binaire GXP, enregistree a part : autant d'identifiants, donc de
// SceGxmVertexProgram distincts, que de variantes, et poser_variante_e
// reposait programme + tampon d'uniformes de sommets a chaque changement de
// variante (210 fois par image a Hawaii). Partage : une copie, un programme
// de sommets par vs_gl. Memes passes et meme reflet => memes attributs et
// memes flux (calcules ci-dessous a l'identique), donc le programme cree
// pour la premiere variante convient a toutes. Les variantes creees avec
// « vpr 0 » gardent leur copie (comportement d'avant).
bool g_vita_vpr = true;
struct SVsPartage
{
	GLuint                 vs_gl;
	SceGxmProgram         *p_vs;
	SceGxmShaderPatcherId  vs_id;
	SceGxmVertexProgram   *vp;
};
#define MAX_VS_PARTAGES 64
static SVsPartage s_vs_partage[MAX_VS_PARTAGES];
static int        s_nb_vs_partage = 0;
static SVsPartage *vs_partage_trouver( GLuint vs_gl )
{
	for( int i = 0; i < s_nb_vs_partage; ++i )
		if( s_vs_partage[i].vs_gl == vs_gl )
			return &s_vs_partage[i];
	return NULL;
}

static SGxmVariante *gxm_variante( SVarianteMateriau *v )
{
	if( v->gxm )
		return v->gxm->ok ? v->gxm : NULL;
	SGxmVariante *g = (SGxmVariante *)calloc( 1, sizeof( SGxmVariante ));
	v->gxm = g;
	if( !g )
		return NULL;
	g->passes = v->passes;
	const SVsPartage *ps = g_vita_vpr ? vs_partage_trouver( v->vs_gl ) : NULL;
	if( ps )
	{
		g->p_vs  = ps->p_vs;
		g->vs_id = ps->vs_id;
	}
	else
		g->p_vs = programme_gxm( v->vs_gl, &g->vs_id );
	g->p_fs = programme_gxm( v->fs_gl, &g->fs_id );
	if( !g->p_vs || !g->p_fs )
	{
		VLOG( "GXD", "!! variante %08x : binaire GXM introuvable", (unsigned)v->cle );
		return NULL;
	}

	// Attributs : un flux par tableau, comme nos VBO separes.
	static const char *noms_in[4] = { "aTexcoord", "aTexcoord2", "aTexcoord3", "aTexcoord4" };
	SceGxmVertexAttribute att[6];
	SceGxmVertexStream    flux[6];
	int na = 0;
	struct { const char *nom; SceGxmAttributeFormat fmt; int comp; int pas; } d[6] = {
		{ "aPosition", SCE_GXM_ATTRIBUTE_FORMAT_F32, 3, 12 },
		{ "aColor",    SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4, 4 },
	};
	int nd = 2;
	g->env = v->env;
	g->wib = v->wib;
	g->flux_n = -1;
	for( int k = 0; k < g->passes; ++k )
	{
		g->flux_uv[k] = -1;
		if( g->env & ( 1u << k ))
			continue;			// reflet : coordonnees generees, pas de flux
		g->flux_uv[k] = nd;
		d[nd].nom = noms_in[k]; d[nd].fmt = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		d[nd].comp = 2; d[nd].pas = 8; ++nd;
	}
	if( g->env )
	{
		g->flux_n = nd;
		d[nd].nom = "aNormal"; d[nd].fmt = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		d[nd].comp = 3; d[nd].pas = 12; ++nd;
	}
	for( int i = 0; i < nd; ++i )
	{
		const SceGxmProgramParameter *pp = sceGxmProgramFindParameterByName( g->p_vs, d[i].nom );
		if( !pp )
		{
			VLOG( "GXD", "!! variante %08x : attribut %s absent", (unsigned)v->cle, d[i].nom );
			return NULL;
		}
		att[na].streamIndex    = (unsigned short)i;
		att[na].offset         = 0;
		att[na].format         = (unsigned char)d[i].fmt;
		att[na].componentCount = (unsigned char)d[i].comp;
		att[na].regIndex       = (unsigned short)sceGxmProgramParameterGetResourceIndex( pp );
		flux[na].stride        = (unsigned short)d[i].pas;
		flux[na].indexSource   = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
		++na;
	}
	if( ps )
		g->vp = ps->vp;		// issue #69 : meme programme de sommets
	else if( sceGxmShaderPatcherCreateVertexProgram( gxm_shader_patcher, g->vs_id, att, na,
	                                                 flux, na, &g->vp ) != 0 )
	{
		VLOG( "GXD", "!! variante %08x : programme de sommets refuse", (unsigned)v->cle );
		return NULL;
	}
	else if( g_vita_vpr && ( s_nb_vs_partage < MAX_VS_PARTAGES ))
	{
		SVsPartage *n = &s_vs_partage[s_nb_vs_partage++];
		n->vs_gl = v->vs_gl;
		n->p_vs  = g->p_vs;
		n->vs_id = g->vs_id;
		n->vp    = g->vp;
	}
	// Opaque : pas de melange (blend NULL), sortie RGBA8.
	if( sceGxmShaderPatcherCreateFragmentProgram( gxm_shader_patcher, g->fs_id,
	        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, msaa_mode, NULL, g->p_vs, &g->fp ) != 0 )
	{
		VLOG( "GXD", "!! variante %08x : programme de pixels refuse", (unsigned)v->cle );
		return NULL;
	}

	static const char *noms_l[4] = { "uL0", "uL1", "uL2", "uL3" };
	static const char *noms_c[4] = { "uC0", "uC1", "uC2", "uC3" };
	static const char *noms_t[4] = { "uTex0", "uTex1", "uTex2", "uTex3" };
	for( int q = 0; q < 4; ++q )
	{
		g->pL[q] = sceGxmProgramFindParameterByName( g->p_vs, noms_l[q] );
		g->pC[q] = ( q < g->passes ) ? sceGxmProgramFindParameterByName( g->p_fs, noms_c[q] ) : NULL;
		const SceGxmProgramParameter *pt = ( q < g->passes )
		    ? sceGxmProgramFindParameterByName( g->p_fs, noms_t[q] ) : NULL;
		g->unite[q] = pt ? (int)sceGxmProgramParameterGetResourceIndex( pt ) : q;
	}
	g->pSeuil = sceGxmProgramFindParameterByName( g->p_fs, "uSeuil" );
	{
		static const char *noms_v[3] = { "uV0", "uV1", "uV2" };
		static const char *noms_e[4] = { "uE0", "uE1", "uE2", "uE3" };
		for( int r = 0; r < 3; ++r )
			g->pV[r] = g->env ? sceGxmProgramFindParameterByName( g->p_vs, noms_v[r] ) : NULL;
		for( int q = 0; q < 4; ++q )
			g->pE[q] = ( g->env & ( 1u << q )) ? sceGxmProgramFindParameterByName( g->p_vs, noms_e[q] ) : NULL;
		if( g->env && ( !g->pV[0] || !g->pV[1] || !g->pV[2] ))
		{
			VLOG( "GXD", "!! variante %08x : uniformes de reflet absents", (unsigned)v->cle );
			return NULL;
		}
		static const char *noms_w[4] = { "uW0", "uW1", "uW2", "uW3" };
		for( int q = 0; q < 4; ++q )
		{
			g->pW[q] = ( g->wib & ( 1u << q )) ? sceGxmProgramFindParameterByName( g->p_vs, noms_w[q] ) : NULL;
			if(( g->wib & ( 1u << q )) && !g->pW[q] )
			{
				VLOG( "GXD", "!! variante %08x : uniforme %s absent", (unsigned)v->cle, noms_w[q] );
				return NULL;
			}
		}
	}
	for( int q = 0; q < 4; ++q )
		if( !g->pL[q] )
		{
			VLOG( "GXD", "!! variante %08x : uniforme %s absent", (unsigned)v->cle, noms_l[q] );
			return NULL;
		}
	g->pFogP = sceGxmProgramFindParameterByName( g->p_vs, "uFogP" );
	g->pFogC = sceGxmProgramFindParameterByName( g->p_vs, "uFogC" );
	if( !g->pFogP || !g->pFogC )
	{
		VLOG( "GXD", "!! variante %08x : uniformes de brouillard absents", (unsigned)v->cle );
		return NULL;
	}
	g->ok = true;
	VLOG( "GXD", "variante %08x prete (%d passes, %d attributs%s)", (unsigned)v->cle, g->passes, na,
	      ps ? ", programme de sommets partage" : "" );
	return g;
}

// Etat GXM d'un fil de dessin (issue #18, listes de commandes sur plusieurs
// coeurs). Le thread principal utilise s_etat_princ ; le travailleur des lots
// passe le sien explicitement (pas de stockage local de thread : il est emule
// par pthread, et un thread cree par sceKernelCreateThread y plantait).
// Reutilisation du tampon d'uniformes de la peau (GxmPeauDebut) : valable
// seulement si AUCUN autre dessin GXM n'a touche programmes, etat precalcule
// ou flux depuis. vitaGL n'en sait rien -- ses drapeaux sales restent a vrai --
// donc chaque chemin direct l'invalide explicitement. Sans cela un personnage
// pouvait etre dessine avec le programme du DECOR et des flux d'un ancien lot
// (eventuellement libere) : plantage GPU au chargement de Tampa (#32).
static bool s_gp_ub_ok = false;

struct SGxmEtat
{
	SceGxmContext                *ctx;		// NULL = gxm_context de vitaGL
	const SGxmVariante           *cour;
	const SceGxmFragmentProgram  *fp_cour;
	bool                          pre_actif;
	const float                  *mvp;
	const float                  *vue;
	const SGxmVariante           *env_ub;
	float                         tile[4][2];
	float                         wib[4][2];		// issue #43
	// Issue #45 : brouillard du tampon de sommets en place (noir ou non, et
	// generation de l'etat global), pour le programme et pour le tampon a
	// reflet/wibble.
	bool                          fog_noir, env_noir;
	unsigned int                  fog_gen, env_gen;
};
static SGxmEtat s_etat_princ;

static inline void poser_brouillard_vs( void *vb, const SceGxmProgramParameter *pP,
                                        const SceGxmProgramParameter *pC, bool noir )
{
	float P[4], C[4];
	BrouillardUniformes( noir, false, P, C );
	sceGxmSetUniformDataF( vb, pP, 0, 4, P );
	sceGxmSetUniformDataF( vb, pC, 0, 4, C );
}
static inline SGxmEtat *etat( void )
{
	return &s_etat_princ;
}
#define E_CTX ( E->ctx ? E->ctx : gxm_context )
// Voir poser_uniformes_env (issue #5).

// Familles de melange, MEME table que dessiner_lot (p_world_render.cpp) :
//   0 opaque (aucun melange)       1 alpha (src_a, 1 - src_a), cas par defaut
//   2 ajout (src_a, 1)             3 soustraction inverse (src_a, 1)
//   4 modulation (0, src_col)      5 (dst_col, 1)
int GxmFamilleMelange( unsigned int blend, bool translucide )
{
	if( !translucide )
		return 0;
	switch( blend )
	{
		case 1: case 2:  return 2;
		case 3: case 4:  return 3;
		case 7: case 8:  return 4;
		case 9:          return 5;
		case 10:         return 6;		// BRIGHTEN_FIXED, alpha de sortie = fixe
		default:         return 1;
	}
}

static SceGxmFragmentProgram *programme_melange( SGxmVariante *g, int famille )
{
	if( famille == 0 )
		return g->fp;
	if( g->fp_m[famille] )
		return g->fp_m[famille];
	SceGxmBlendInfo b;
	memset( &b, 0, sizeof( b ));
	b.colorMask = SCE_GXM_COLOR_MASK_ALL;
	b.colorFunc = b.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
	switch( famille )
	{
		case 2: b.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; b.colorDst = SCE_GXM_BLEND_FACTOR_ONE; break;
		case 3: b.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; b.colorDst = SCE_GXM_BLEND_FACTOR_ONE;
		        b.colorFunc = b.alphaFunc = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT; break;
		// [SOURCE] XBox/NX/render.cpp, set_blend_mode : MODULATE = (ZERO, SRCALPHA)
		// et non (ZERO, SRC_COLOR) ; BRIGHTEN_FIXED = (DESTCOLOR, CONSTANTALPHA).
		case 4: b.colorSrc = SCE_GXM_BLEND_FACTOR_ZERO;      b.colorDst = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; break;
		case 5: b.colorSrc = SCE_GXM_BLEND_FACTOR_DST_COLOR; b.colorDst = SCE_GXM_BLEND_FACTOR_ONE; break;
		case 6: b.colorSrc = SCE_GXM_BLEND_FACTOR_DST_COLOR; b.colorDst = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; break;
		default: b.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; b.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
	}
	b.alphaSrc = b.colorSrc;
	b.alphaDst = b.colorDst;
	if( sceGxmShaderPatcherCreateFragmentProgram( gxm_shader_patcher, g->fs_id,
	        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, msaa_mode, &b, g->p_vs, &g->fp_m[famille] ) != 0 )
	{
		g->fp_m[famille] = NULL;
		return NULL;
	}
	return g->fp_m[famille];
}
// Un etat fragment precalcule est en place : le chemin immediat doit le
// retirer avant de reserver ses propres uniformes.

void GxmMateriauDebut( const float *mvp )
{
	SGxmEtat *E = etat();
	E->env_ub   = NULL;
	E->mvp  = mvp;
	E->cour = NULL;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	E->fp_cour = NULL;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	scene_reset();
	// Etat de sommets NON precalcule : la matrice est posee par
	// sceGxmReserveVertexDefaultUniformBuffer au changement de programme.
	sceGxmSetPrecomputedVertexState( E_CTX, NULL );
}

// Programmes et matrice : seulement quand ils changent.
static inline void poser_variante_e( SGxmEtat *E, SGxmVariante *g, const SceGxmFragmentProgram *fp,
                                     bool noir )
{
	SceGxmContext *ctx = E_CTX;
	if( fp != E->fp_cour )
	{
		sceGxmSetFragmentProgram( ctx, fp );
		E->fp_cour = fp;
		s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	}
	const bool fog_ok = ( E->fog_noir == noir ) && ( E->fog_gen == s_fog_gen );
	if(( g == E->cour ) && fog_ok )
		return;
	// Issue #69 (« vpr ») : autre variante, MEME programme de sommets (voir
	// gxm_variante). Le contexte a deja ce programme, et le tampon d'uniformes
	// de sommets reserve pour E->cour contient exactement ce que g y mettrait :
	// meme programme donc memes emplacements, meme matrice (E->mvp, change
	// seulement par GxmMateriauDebut qui remet E->cour a NULL), meme
	// brouillard (fog_ok). Tout ce qui pose un autre programme ou reserve un
	// autre tampon remet E->cour a NULL (GxmMateriauDebut/Fin, GxmPeauDebut,
	// GxmListeDebut, diff_fin_executer) : E->cour non nul garantit que l'etat
	// est encore le sien -- la meme garantie que le saut « g == E->cour »
	// ci-dessus. Variantes a reflet / wibble exclues : leur tampon est pose
	// par dessin (poser_uniformes_env_e).
	const bool meme_vp = g_vita_vpr && E->cour && ( E->cour->vp == g->vp )
	    && !g->env && !g->wib && !E->cour->env && !E->cour->wib;
	if( meme_vp && fog_ok )
	{
		E->cour = g;
		++s_vh_poses_evitees;
		return;
	}
	++s_vh_poses_vs;		// issue #69 : programme de sommets / matrice reposes
	E->fog_noir = noir;
	E->fog_gen  = s_fog_gen;
	if(( g != E->cour ) && !meme_vp )
		sceGxmSetVertexProgram( ctx, g->vp );
	// Variante a reflet ou a UV wibble : ses uniformes de sommets sont poses a
	// chaque dessin (poser_uniformes_env), inutile d'en reserver ici.
	if( !g->env && !g->wib )
	{
		E->env_ub = NULL;
		void *vb = NULL;
		sceGxmReserveVertexDefaultUniformBuffer( ctx, &vb );
		for( int r = 0; r < 4; ++r )
		{
			const float ligne[4] = { E->mvp[r], E->mvp[4 + r], E->mvp[8 + r], E->mvp[12 + r] };
			sceGxmSetUniformDataF( vb, g->pL[r], 0, 4, ligne );
		}
		poser_brouillard_vs( vb, g->pFogP, g->pFogC, noir );
	}
	E->cour = g;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
}
static inline void poser_variante( SGxmVariante *g, const SceGxmFragmentProgram *fp, bool noir )
{
	poser_variante_e( etat(), g, fp, noir );
}

// Adresse GPU des donnees d'un tampon vitaGL : son nom EST l'adresse de sa
// structure vbo, dont le premier champ est le pointeur de donnees
// (buffers.c, glGenBuffers ; shared.h, struct vbo).
static inline const unsigned char *donnees_tampon( unsigned int nom )
{
	return nom ? *(const unsigned char * const *)nom : NULL;
}

// --- Etats precalcules ------------------------------------------------------
//
// Pour un lot dont tout est constant d'une image a l'autre -- textures,
// couleurs de passe, seuil, tampons, plage d'index -- GXM sait tout preparer
// UNE fois : l'etat fragment (textures + uniformes) et le dessin (flux +
// index). Par image, il reste sceGxmSetPrecomputedFragmentState et
// sceGxmDrawPrecomputed. Les blocs sont lus par le GPU : memoire de vitaGL
// (VGL_MEM_RAM), jamais modifiee une fois preparee.
// --- Passes en reflet (issue #5) ---------------------------------------------
void GxmMateriauVue( const float *view )
{
	SGxmEtat *E = etat();
	E->vue = view;
}

// Tampon d'uniformes de sommets propre a ce dessin : matrice, vue, tuilage.
// Le tuilage depend du materiau, pas de la variante. sp_gxm_cour reste sur la
// variante : une autre variante reposera de toute facon son propre tampon.
// Tampon de sommets a reflet en place : reutilise tant que la variante et le
// tuilage sont les memes et que personne n'a reserve d'autre tampon entre-temps
// (sp_env_ub remis a NULL a chaque autre reservation). [MESURE] Manhattan M1 :
// le reposer a chaque lot coutait ~1,2 ms par image.
// Issue #43 : meme tampon pour les decalages d'UV wibble (wib), qui changent
// a chaque image -- reutilise seulement entre dessins de memes decalages.
static bool poser_uniformes_env_e( SGxmEtat *E, const SGxmVariante *g, const float (*tile)[2],
                                   const float (*wib)[2], bool noir )
{
	if( g->env && !E->vue )
		return false;
	if(( E->env_ub == g ) && !memcmp( E->tile, tile, sizeof( E->tile ))
	    && !memcmp( E->wib, wib, sizeof( E->wib ))
	    && ( E->env_noir == noir ) && ( E->env_gen == s_fog_gen ))
		return true;
	E->env_ub = g;
	E->env_noir = noir;
	E->env_gen  = s_fog_gen;
	memcpy( E->tile, tile, sizeof( E->tile ));
	memcpy( E->wib, wib, sizeof( E->wib ));
	SceGxmContext *ctx = E_CTX;
	void *vb = NULL;
	sceGxmReserveVertexDefaultUniformBuffer( ctx, &vb );
	for( int r = 0; r < 4; ++r )
	{
		const float l[4] = { E->mvp[r], E->mvp[4 + r], E->mvp[8 + r], E->mvp[12 + r] };
		sceGxmSetUniformDataF( vb, g->pL[r], 0, 4, l );
	}
	if( g->env )
		for( int r = 0; r < 3; ++r )
		{
			const float l[4] = { E->vue[r], E->vue[4 + r], E->vue[8 + r], E->vue[12 + r] };
			sceGxmSetUniformDataF( vb, g->pV[r], 0, 4, l );
		}
	for( int k = 0; k < 4; ++k )
		if( g->pE[k] )
		{
			const float e[4] = { tile[k][0], tile[k][1], 0.0f, 0.0f };
			sceGxmSetUniformDataF( vb, g->pE[k], 0, 4, e );
		}
	for( int k = 0; k < 4; ++k )
		if( g->pW[k] )
		{
			const float w[4] = { wib[k][0], wib[k][1], 0.0f, 0.0f };
			sceGxmSetUniformDataF( vb, g->pW[k], 0, 4, w );
		}
	poser_brouillard_vs( vb, g->pFogP, g->pFogC, noir );
	return true;
}
static bool poser_uniformes_env( const SGxmVariante *g, const float (*tile)[2],
                                 const float (*wib)[2], bool noir )
{
	return poser_uniformes_env_e( etat(), g, tile, wib, noir );
}

// Flux de sommets selon la table de la variante (identite sans reflet).
static void poser_flux( const SGxmVariante *g, const SShaderMateriau *p, const void **flux )
{
	flux[0] = donnees_tampon( p->vbo );
	flux[1] = donnees_tampon( p->cbo );
	for( int k = 0; k < g->passes; ++k )
	{
		const int f = g->env ? g->flux_uv[k] : 2 + k;
		if( f >= 0 )
			flux[f] = donnees_tampon( p->uvbo[k] );
	}
	if( g->env && ( g->flux_n >= 0 ))
		flux[g->flux_n] = donnees_tampon( p->nbo );
}

struct SGxmLotPre
{
	SceGxmPrecomputedFragmentState fs;
	SceGxmPrecomputedDraw          dr;
	SGxmVariante                  *g;
	const SceGxmFragmentProgram   *fp;
	void                          *mem;
	float                          tile[4][2];	// passes en reflet (#5)
	float                          wib[4][2];	// toujours nul : un lot ne wibble pas (#43)
	bool                           fog_noir;	// #45 : brouillard noir (ADD/SUB)
};

// Issue #32 : ces blocs n'etaient JAMAIS rendus. Chaque chargement de niveau
// laissait derriere lui la memoire vitaGL de tous ses lots precalcules.
// vglLazyFree et non vgl_free : l'image en cours de rendu peut encore les lire.
void GxmMateriauLibererPre( void *p_pre )
{
	SGxmLotPre *pre = (SGxmLotPre *)p_pre;
	if( !pre )
		return;
	if( pre->mem )
		vglLazyFree( pre->mem );
	free( pre );
}

void *GxmMateriauPreparer( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                           int first, int count, unsigned int ibo, int famille )
{
	const SVarianteMateriau *v0 = variante( p );
	if( !v0 )
		return NULL;
	SGxmVariante *g = gxm_variante( (SVarianteMateriau *)v0 );
	if( !g )
		return NULL;
	SceGxmFragmentProgram *fp = programme_melange( g, famille );
	if( !fp )
		return NULL;

	const unsigned taille_fs = sceGxmGetPrecomputedFragmentStateSize( fp );
	const unsigned taille_dr = sceGxmGetPrecomputedDrawSize( g->vp );
	const unsigned taille_ub = sceGxmProgramGetDefaultUniformBufferSize( g->p_fs );
	const unsigned a16 = 15;
	const unsigned o_dr = ( taille_fs + a16 ) & ~a16;
	const unsigned o_ub = ( o_dr + taille_dr + a16 ) & ~a16;
	const unsigned total = o_ub + (( taille_ub + a16 ) & ~a16 ) + 16;

	SGxmLotPre *pre = (SGxmLotPre *)calloc( 1, sizeof( SGxmLotPre ));
	unsigned char *mem = (unsigned char *)vgl_memalign( 64, total, VGL_MEM_RAM );
	if( !pre || !mem )
	{
		free( pre );
		return NULL;
	}
	memset( mem, 0, total );
	pre->mem = mem;
	pre->g   = g;

	pre->fp = fp;
	if( sceGxmPrecomputedFragmentStateInit( &pre->fs, fp, mem ) != 0 )
	{
		GxmMateriauLibererPre( pre );
		return NULL;
	}
	for( int k = 0; k < g->passes; ++k )
		sceGxmPrecomputedFragmentStateSetTexture( &pre->fs, g->unite[k], p_tex[k] );
	void *ub = mem + o_ub;
	for( int k = 0; k < g->passes; ++k )
		if( g->pC[k] )
			sceGxmSetUniformDataF( ub, g->pC[k], 0, 4, p->c[k] );
	if( g->pSeuil )
		sceGxmSetUniformDataF( ub, g->pSeuil, 0, 1, &p->seuil );
	sceGxmPrecomputedFragmentStateSetDefaultUniformBuffer( &pre->fs, ub );

	if( sceGxmPrecomputedDrawInit( &pre->dr, g->vp, mem + o_dr ) != 0 )
	{
		GxmMateriauLibererPre( pre );
		return NULL;
	}
	const void *flux[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
	poser_flux( g, p, flux );
	memcpy( pre->tile, p->env_tile, sizeof( pre->tile ));
	memcpy( pre->wib, p->wib_uv, sizeof( pre->wib ));
	pre->fog_noir = p->fog_noir;
	sceGxmPrecomputedDrawSetAllVertexStreams( &pre->dr, flux );
	sceGxmPrecomputedDrawSetParams( &pre->dr, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
	                                (const unsigned short *)donnees_tampon( ibo ) + first, count );
	return pre;
}

static void lot_pre_e( SGxmEtat *E, SGxmLotPre *pre )
{
	poser_variante_e( E, pre->g, pre->fp, pre->fog_noir );
	if(( pre->g->env || pre->g->wib ) && !poser_uniformes_env_e( E, pre->g, pre->tile, pre->wib, pre->fog_noir ))
		return;
	sceGxmSetPrecomputedFragmentState( E_CTX, &pre->fs );
	sceGxmDrawPrecomputed( E_CTX, &pre->dr );
	E->pre_actif = true;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
}

void GxmMateriauLotPre( void *p_pre )
{
	lot_pre_e( etat(), (SGxmLotPre *)p_pre );
}


bool GxmMateriauLot( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                     const int *p_first, const int *p_count, int n_plages,
                     unsigned int ibo, int famille )
{
	SGxmEtat *E = etat();
	const SVarianteMateriau *v0 = variante( p );
	if( !v0 )
		return false;
	SGxmVariante *g = gxm_variante( (SVarianteMateriau *)v0 );
	if( !g )
		return false;
	const SceGxmFragmentProgram *fp = programme_melange( g, famille );
	if( !fp )
		return false;
	SceGxmContext *ctx = E_CTX;

	if( E->pre_actif )
	{
		sceGxmSetPrecomputedFragmentState( ctx, NULL );
		E->pre_actif = false;
		s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	}
	poser_variante( g, fp, p->fog_noir );
	void *fb = NULL;
	sceGxmReserveFragmentDefaultUniformBuffer( ctx, &fb );
	for( int k = 0; k < g->passes; ++k )
		if( g->pC[k] )
			sceGxmSetUniformDataF( fb, g->pC[k], 0, 4, p->c[k] );
	if( g->pSeuil )
		sceGxmSetUniformDataF( fb, g->pSeuil, 0, 1, &p->seuil );

	for( int k = 0; k < g->passes; ++k )
		sceGxmSetFragmentTexture( ctx, g->unite[k], p_tex[k] );

	{
		const void *flux[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
		poser_flux( g, p, flux );
		for( int f = 0; f < 8; ++f )
			if( flux[f] )
				sceGxmSetVertexStream( ctx, f, flux[f] );
	}
	if(( g->env || g->wib ) && !poser_uniformes_env( g, p->env_tile, p->wib_uv, p->fog_noir ))
		return false;

	const unsigned short *idx = (const unsigned short *)donnees_tampon( ibo );
	for( int i = 0; i < n_plages; ++i )
		sceGxmDraw( ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
		            idx + p_first[i], p_count[i] );
	return true;
}

bool GxmMateriauMaillage( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                          unsigned int ibo, int num_indices, int famille )
{
	SGxmEtat *E = etat();
	const SVarianteMateriau *v0 = variante( p );
	if( !v0 )
		return false;
	SGxmVariante *g = gxm_variante( (SVarianteMateriau *)v0 );
	if( !g )
		return false;
	const SceGxmFragmentProgram *fp = programme_melange( g, famille );
	if( !fp )
		return false;
	SceGxmContext *ctx = E_CTX;
	if( E->pre_actif )
	{
		sceGxmSetPrecomputedFragmentState( ctx, NULL );
		E->pre_actif = false;
		s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	}
	poser_variante( g, fp, p->fog_noir );
	void *fb = NULL;
	sceGxmReserveFragmentDefaultUniformBuffer( ctx, &fb );
	for( int k = 0; k < g->passes; ++k )
		if( g->pC[k] )
			sceGxmSetUniformDataF( fb, g->pC[k], 0, 4, p->c[k] );
	if( g->pSeuil )
		sceGxmSetUniformDataF( fb, g->pSeuil, 0, 1, &p->seuil );
	for( int k = 0; k < g->passes; ++k )
		sceGxmSetFragmentTexture( ctx, g->unite[k], p_tex[k] );
	{
		const void *flux[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
		poser_flux( g, p, flux );
		for( int f = 0; f < 8; ++f )
			if( flux[f] )
				sceGxmSetVertexStream( ctx, f, flux[f] );
	}
	if(( g->env || g->wib ) && !poser_uniformes_env( g, p->env_tile, p->wib_uv, p->fog_noir ))
		return false;
	sceGxmDraw( ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16,
	            donnees_tampon( ibo ), num_indices );
	return true;
}

void GxmMateriauFin()
{
	SGxmEtat *E = etat();
	// Rendre a vitaGL un contexte en mode NON precalcule.
	sceGxmSetPrecomputedFragmentState( E_CTX, NULL );
	sceGxmSetPrecomputedVertexState( E_CTX, NULL );
	E->pre_actif = false;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	E->fp_cour = NULL;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	E->env_ub = NULL;
	// vitaGL doit tout recharger au prochain dessin : ses programmes, et ses
	// tampons d'uniformes, puisque ceux que GXM a en main sont les notres.
	dirty_shader_vert_unifs = GL_TRUE;
	dirty_shader_frag_unifs = GL_TRUE;
	ffp_dirty_vert = GL_TRUE;
	ffp_dirty_frag = GL_TRUE;
	dirty_vert_unifs = 0xFFFF;
	dirty_frag_unifs = 0xFFFFFFFF;
	E->cour = NULL;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
}


// --- Personnages en GXM direct (issue #18) ----------------------------------
//
// [MESURE] V1 : sauter le dessin des modeles (« mdl 0 ») fait passer la phase
// logique de 7,0 a 4,2 ms. Le skinning est deja sur le GPU : ce qui coute est
// la soumission par vitaGL -- 192 float4 d'os recopies dans le tampon
// d'uniformes a CHAQUE glDrawElements, plus programme, attributs et uniformes
// relus a chaque piece. Ici : un jeu d'uniformes de sommets par geom (les os
// utilises seulement), puis par piece un petit tampon fragment, la texture,
// quatre flux et sceGxmDraw. Memes shaders, meme calcul.

// COUPE PAR DEFAUT depuis le 2026-10-03 (#32) : avec ce chemin, New Jersey ->
// Quit -> menu -> Tampa plantait le GPU a coup sur (console eteinte) ; avec
// gpk 0 seul, plus de plantage (bissection : gxd/gxp/gxs actifs). Cause exacte
// non trouvee. Ne pas reactiver sans avoir rejoue ce scenario.
bool g_vita_gxm_peau = false;

// Variante ECLAIREE (issue #4), formule de la reference
// (XBox/NX/WeightedMeshVS_VXC_3Weight.vsh) : normale skinnee par les memes os,
// lumieres ramenees dans l'espace du modele (anim.cpp:353), contribution
// max(0, -N.L) avec L la direction stockee inversee (p_NxLight.cpp:92), plus
// l'ambiante. Le registre de couleur sature a 1 et le pixel shader multiplie
// par 4 x 0,5 x 0,5 au neutre : texture x min(lumiere, 2).
static const char *s_src_peau_lum_v =
	"void main(\n"
	"	float3 aPos,\n"
	"	float3 aPoids,\n"
	"	float3 aOs,\n"
	"	float2 aTexcoord,\n"
	"	float3 aNormal,\n"
	"	uniform float4 uL0,\n"
	"	uniform float4 uL1,\n"
	"	uniform float4 uL2,\n"
	"	uniform float4 uL3,\n"
	"	uniform float4 uB[192],\n"
	"	uniform float4 uAmb,\n"
	"	uniform float4 uLd0,\n"
	"	uniform float4 uLc0,\n"
	"	uniform float4 uLd1,\n"
	"	uniform float4 uLc1,\n"
	"	uniform float4 uFogP,\n"
	"	uniform float4 uFogC,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0,\n"
	"	float3 out vLum : TEXCOORD1,\n"
	"	float4 out vFog : TEXCOORD4)\n"
	"{\n"
	"	float4 p = float4(aPos, 1.f);\n"
	"	int3 o = (int3)min(aOs, float3(63.f, 63.f, 63.f)) * 3;\n"
	"	float3 s = aPoids.x * float3(dot(uB[o.x], p), dot(uB[o.x + 1], p), dot(uB[o.x + 2], p))\n"
	"	         + aPoids.y * float3(dot(uB[o.y], p), dot(uB[o.y + 1], p), dot(uB[o.y + 2], p))\n"
	"	         + aPoids.z * float3(dot(uB[o.z], p), dot(uB[o.z + 1], p), dot(uB[o.z + 2], p));\n"
	"	float3 n = aPoids.x * float3(dot(uB[o.x].xyz, aNormal), dot(uB[o.x + 1].xyz, aNormal), dot(uB[o.x + 2].xyz, aNormal))\n"
	"	         + aPoids.y * float3(dot(uB[o.y].xyz, aNormal), dot(uB[o.y + 1].xyz, aNormal), dot(uB[o.y + 2].xyz, aNormal))\n"
	"	         + aPoids.z * float3(dot(uB[o.z].xyz, aNormal), dot(uB[o.z + 1].xyz, aNormal), dot(uB[o.z + 2].xyz, aNormal));\n"
	"	n = normalize(n);\n"
	"	float3 l = uAmb.xyz + uLc0.xyz * max(0.f, -dot(n, uLd0.xyz))\n"
	"	                    + uLc1.xyz * max(0.f, -dot(n, uLd1.xyz));\n"
	"	vLum = min(l, float3(2.f, 2.f, 2.f));\n"
	"	float4 q = float4(s, 1.f);\n"
	"	vPosition = float4(dot(uL0, q), dot(uL1, q), dot(uL2, q), dot(uL3, q));\n"
	"	vTexcoord = aTexcoord;\n"
	BROUILLARD_VS( "dot(uL3, q)" )
	"}\n";

static const char *s_src_peau_lum_f =
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	float3 vLum : TEXCOORD1,\n"
	"	float4 vFog : TEXCOORD4,\n"
	"	uniform sampler2D uTex,\n"
	"	uniform float4 uTeinte,\n"
	"	uniform float uAvecTexture)\n"
	"{\n"
	"	float4 t = tex2D(uTex, vTexcoord);\n"
	"	float4 c = lerp(float4(1.f, 1.f, 1.f, 1.f), t, uAvecTexture) * uTeinte;\n"
	"	return float4(saturate(c.rgb * vLum) * vFog.a + vFog.rgb, c.a);\n"
	"}\n";

struct SPeauProg
{
	int                    etat;		// 0 a construire, 1 pret, -1 echec
	SceGxmProgram         *vs, *fs;
	SceGxmShaderPatcherId  vs_id, fs_id;
	SceGxmVertexProgram   *vp;
	SceGxmFragmentProgram *fp;
	const SceGxmProgramParameter *l[4], *b, *teinte, *avec;
	const SceGxmProgramParameter *amb, *ld[2], *lc[2];
	const SceGxmProgramParameter *fogp, *fogc;		// issue #45
	int                    unite;
};
static SPeauProg s_peau[2];		// [0] sans eclairage, [1] eclairee (#4)
static SPeauProg *sp_peau_cour = &s_peau[0];

static bool construire_peau( SPeauProg *g, bool eclaire )
{
	if( g->etat != 0 )
		return ( g->etat > 0 );
	g->etat = -1;
	GLuint vs = compiler( GL_CG_VERTEX_SHADER_EXT, eclaire ? s_src_peau_lum_v : s_src_peau_v,
	                      eclaire ? "peau eclairee sommets (gxm)" : "peau sommets (gxm)" );
	GLuint fs = compiler( GL_CG_FRAGMENT_SHADER_EXT, eclaire ? s_src_peau_lum_f : s_src_peau_f,
	                      eclaire ? "peau eclairee pixels (gxm)" : "peau pixels (gxm)" );
	if( !vs || !fs )
		return false;
	g->vs = programme_gxm( vs, &g->vs_id );
	g->fs = programme_gxm( fs, &g->fs_id );
	if( !g->vs || !g->fs )
	{
		VLOG( "GXD", "!! peau : binaire GXM introuvable" );
		return false;
	}
	struct { const char *nom; int comp; int pas; } d[5] = {
		{ "aPos", 3, 12 }, { "aPoids", 3, 12 }, { "aOs", 3, 12 }, { "aTexcoord", 2, 8 },
		{ "aNormal", 3, 12 } };
	const int na = eclaire ? 5 : 4;
	SceGxmVertexAttribute att[5];
	SceGxmVertexStream    flux[5];
	for( int i = 0; i < na; ++i )
	{
		const SceGxmProgramParameter *pp = sceGxmProgramFindParameterByName( g->vs, d[i].nom );
		if( !pp )
		{
			VLOG( "GXD", "!! peau : attribut %s absent", d[i].nom );
			return false;
		}
		att[i].streamIndex    = (unsigned short)i;
		att[i].offset         = 0;
		att[i].format         = (unsigned char)SCE_GXM_ATTRIBUTE_FORMAT_F32;
		att[i].componentCount = (unsigned char)d[i].comp;
		att[i].regIndex       = (unsigned short)sceGxmProgramParameterGetResourceIndex( pp );
		flux[i].stride        = (unsigned short)d[i].pas;
		flux[i].indexSource   = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	}
	if( sceGxmShaderPatcherCreateVertexProgram( gxm_shader_patcher, g->vs_id, att, na,
	                                            flux, na, &g->vp ) != 0 )
	{
		VLOG( "GXD", "!! peau : programme de sommets refuse" );
		return false;
	}
	// Opaque, comme le chemin vitaGL (plat_render coupe le melange).
	if( sceGxmShaderPatcherCreateFragmentProgram( gxm_shader_patcher, g->fs_id,
	        SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, msaa_mode, NULL, g->vs, &g->fp ) != 0 )
	{
		VLOG( "GXD", "!! peau : programme de pixels refuse" );
		return false;
	}
	static const char *noms_l[4] = { "uL0", "uL1", "uL2", "uL3" };
	for( int r = 0; r < 4; ++r )
		g->l[r] = sceGxmProgramFindParameterByName( g->vs, noms_l[r] );
	g->b      = sceGxmProgramFindParameterByName( g->vs, "uB" );
	g->teinte = sceGxmProgramFindParameterByName( g->fs, "uTeinte" );
	g->avec   = sceGxmProgramFindParameterByName( g->fs, "uAvecTexture" );
	g->amb    = eclaire ? sceGxmProgramFindParameterByName( g->vs, "uAmb" ) : NULL;
	g->ld[0]  = eclaire ? sceGxmProgramFindParameterByName( g->vs, "uLd0" ) : NULL;
	g->ld[1]  = eclaire ? sceGxmProgramFindParameterByName( g->vs, "uLd1" ) : NULL;
	g->lc[0]  = eclaire ? sceGxmProgramFindParameterByName( g->vs, "uLc0" ) : NULL;
	g->lc[1]  = eclaire ? sceGxmProgramFindParameterByName( g->vs, "uLc1" ) : NULL;
	g->fogp   = sceGxmProgramFindParameterByName( g->vs, "uFogP" );
	g->fogc   = sceGxmProgramFindParameterByName( g->vs, "uFogC" );
	const SceGxmProgramParameter *pt = sceGxmProgramFindParameterByName( g->fs, "uTex" );
	g->unite = pt ? (int)sceGxmProgramParameterGetResourceIndex( pt ) : 0;
	if( !g->l[0] || !g->l[1] || !g->l[2] || !g->l[3] || !g->b || !g->teinte || !g->fogp || !g->fogc
	    || ( eclaire && ( !g->amb || !g->ld[0] || !g->ld[1] || !g->lc[0] || !g->lc[1] )))
	{
		VLOG( "GXD", "!! peau : uniforme absent" );
		return false;
	}
	VLOG( "GXD", "peau GXM%s prete (unite de texture %d)", eclaire ? " eclairee" : "", g->unite );
	g->etat = 1;
	return true;
}

bool GxmPeauPret()
{
	return construire_peau( &s_peau[0], false );
}

bool GxmPeauEclaireePret()
{
	return construire_peau( &s_peau[1], true );
}

// Reutilisation du tampon d'uniformes de sommets entre geoms d'un MEME modele
// (meme squelette, meme matrice) : ecrire 55 os dans la memoire GPU non cachee
// coute ~25 us (mesure), et un skater a 9 geoms. Valable seulement si
// personne n'a dessine entre-temps : vitaGL remet a faux ses drapeaux
// « dirty » des qu'il dessine, et GxmPeauFin les laisse tous a vrai.
static const float *sp_gp_os_der = NULL;
static int          s_gp_n_der = -1;
static float        s_gp_mvp_der[16];
static float        s_gp_lum_der[15];
static const SPeauProg *sp_gp_prog_der = NULL;
static uint32_t     s_gp_image_der = 0xFFFFFFFF;
int g_vita_gp_reutil[2] = { 0, 0 };		// reutilises, ecrits

void GxmPeauDebut( const float *mvp, const float *p_os, int num_os, bool remplir_os,
                   const float *lum )
{
	SGxmEtat *E = etat();
	SceGxmContext *ctx = E_CTX;
	scene_reset();
	SPeauProg *g = ( lum && ( s_peau[1].etat > 0 )) ? &s_peau[1] : &s_peau[0];
	sp_peau_cour = g;
	const bool intact = dirty_shader_vert_unifs && dirty_shader_frag_unifs && ffp_dirty_vert
	                    && ( dirty_vert_unifs == 0xFFFF );
	// REUTILISATION COUPEE (#32) : elle supposait le tampon d'uniformes reserve
	// au geom precedent encore valide. Il ne l'est plus apres un changement de
	// scene GXM sans changement d'image (rendu dans une texture, scene_reset,
	// glFinish pendant un chargement) -- adresse d'uniformes perimee pour le GPU.
	if( false && s_gp_ub_ok && intact && ( p_os == sp_gp_os_der ) && ( num_os == s_gp_n_der )
	    && ( g == sp_gp_prog_der )
	    && ( vgl_framecount == s_gp_image_der ) && !memcmp( mvp, s_gp_mvp_der, sizeof( s_gp_mvp_der ))
	    && ( !lum || !memcmp( lum, s_gp_lum_der, sizeof( s_gp_lum_der ))))
	{
		// Programmes, etats precalcules et tampon de sommets : ceux laisses
		// par le geom precedent, inchanges.
		++g_vita_gp_reutil[0];
		return;
	}
	++g_vita_gp_reutil[1];
	E->env_ub = NULL;
	sceGxmSetPrecomputedVertexState( ctx, NULL );
	sceGxmSetPrecomputedFragmentState( ctx, NULL );
	E->pre_actif = false;
	sceGxmSetVertexProgram( ctx, g->vp );
	sceGxmSetFragmentProgram( ctx, g->fp );
	// Les programmes en place ne sont plus ceux du chemin des lots.
	E->cour = NULL;
	E->fp_cour = NULL;

	void *vb = NULL;
	sceGxmReserveVertexDefaultUniformBuffer( ctx, &vb );
	for( int r = 0; r < 4; ++r )
	{
		const float ligne[4] = { mvp[r], mvp[4 + r], mvp[8 + r], mvp[12 + r] };
		sceGxmSetUniformDataF( vb, g->l[r], 0, 4, ligne );
	}
	poser_brouillard_vs( vb, g->fogp, g->fogc, false );
	if( g == &s_peau[1] )
	{
		// lum : ambiante (3), puis par lumiere direction (3) et couleur (3),
		// directions deja dans l'espace du modele.
		const float a[4] = { lum[0], lum[1], lum[2], 0.0f };
		sceGxmSetUniformDataF( vb, g->amb, 0, 4, a );
		for( int k = 0; k < 2; ++k )
		{
			const float d[4] = { lum[3 + k * 6], lum[4 + k * 6], lum[5 + k * 6], 0.0f };
			const float c[4] = { lum[6 + k * 6], lum[7 + k * 6], lum[8 + k * 6], 0.0f };
			sceGxmSetUniformDataF( vb, g->ld[k], 0, 4, d );
			sceGxmSetUniformDataF( vb, g->lc[k], 0, 4, c );
		}
		memcpy( s_gp_lum_der, lum, sizeof( s_gp_lum_der ));
	}
	// Seulement les os du squelette. Les autres emplacements restent tels que
	// le tampon les rend ; le chemin vitaGL les mettait a zero pour annuler la
	// contribution d'un indice d'os hors squelette -- remplir_os le demande
	// quand le maillage en contient (ou qu'on ne le sait pas).
	int n = ( num_os < PEAU_MAX_OS ) ? num_os : PEAU_MAX_OS;
	if( remplir_os )
		memset( s_os_lignes, 0, sizeof( s_os_lignes ));
	for( int b = 0; b < n; ++b )
	{
		const float *m = &p_os[b * 16];
		float *l = &s_os_lignes[b * 12];
		l[0] = m[0];  l[1] = m[4];  l[2]  = m[8];  l[3]  = m[12];
		l[4] = m[1];  l[5] = m[5];  l[6]  = m[9];  l[7]  = m[13];
		l[8] = m[2];  l[9] = m[6];  l[10] = m[10]; l[11] = m[14];
	}
	memcpy( (float *)vb + sceGxmProgramParameterGetResourceIndex( g->b ), s_os_lignes,
	        ( remplir_os ? PEAU_MAX_OS : n ) * 12 * sizeof( float ));

	sp_gp_os_der   = p_os;
	s_gp_n_der     = num_os;
	sp_gp_prog_der = g;
	s_gp_image_der = vgl_framecount;
	memcpy( s_gp_mvp_der, mvp, sizeof( s_gp_mvp_der ));
	// Un tampon rempli partiellement ne vaut que pour un maillage qui n'a pas
	// besoin du reste : on ne le reutilise pas pour un « remplir ».
	s_gp_ub_ok = !remplir_os;
}

void GxmPeauPiece( unsigned int vbo_repos, unsigned int vbo_poids, unsigned int vbo_os,
                   unsigned int uvbo, void *p_tex, bool avec_texture, const float teinte[4],
                   unsigned int ibo, int num_indices, unsigned int vbo_normales )
{
	SGxmEtat *E = etat();
	SceGxmContext *ctx = E_CTX;
	const SPeauProg *g = sp_peau_cour;
	void *fb = NULL;
	sceGxmReserveFragmentDefaultUniformBuffer( ctx, &fb );
	sceGxmSetUniformDataF( fb, g->teinte, 0, 4, teinte );
	if( g->avec )
	{
		const float avec = avec_texture ? 1.0f : 0.0f;
		sceGxmSetUniformDataF( fb, g->avec, 0, 1, &avec );
	}
	sceGxmSetFragmentTexture( ctx, g->unite, (const SceGxmTexture *)p_tex );
	sceGxmSetVertexStream( ctx, 0, donnees_tampon( vbo_repos ));
	sceGxmSetVertexStream( ctx, 1, donnees_tampon( vbo_poids ));
	sceGxmSetVertexStream( ctx, 2, donnees_tampon( vbo_os ));
	sceGxmSetVertexStream( ctx, 3, donnees_tampon( uvbo ? uvbo : vbo_repos ));
	if( g == &s_peau[1] )
		sceGxmSetVertexStream( ctx, 4, donnees_tampon( vbo_normales ));
	sceGxmDraw( ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16,
	            donnees_tampon( ibo ), num_indices );
}

void GxmPeauFin()
{
	GxmMateriauFin();
}


// --- Listes de commandes GXM (contexte differe), issue #18 ------------------
//
// Etape 0 : enregistrer une serie de dessins dans une liste puis l'executer,
// sur le meme thread -- valider la mecanique (memoire, etats) avant de
// paralleliser. L'etat de vitaGL est recopie dans la liste en pointant
// temporairement sa globale gxm_context sur le contexte differe : ses propres
// fonctions (profondeur, viewport, culling) ecrivent alors dans la liste.
extern "C"
{
	void change_depth_func( void );
	void validate_viewport( void );
	void change_cull_mode( void );
	void update_polygon_offset( void );
}

bool g_vita_listes_gxm = false;

struct SAnneauGxm { unsigned char *base; unsigned int taille, pos; };
// Un contexte differe et ses anneaux memoire (un jeu par image, 3 images).
struct SDiff
{
	SAnneauGxm        ann[3][3];		// [image % 3][vdm, sommets, fragments]
	int               img;
	SceGxmContext    *ctx;
	int               etat;				// 0 a creer, 1 pret, -1 echec
	SceGxmCommandList liste;
};
static SDiff s_diff0;		// etape 0 (gcl) : enregistrement sur le meme thread
static SDiff s_diff_trav;	// lots opaques enregistres par le travailleur

static void *ann_prendre( SDiff *d, int type, SceSize demande, SceSize *taille )
{
	SAnneauGxm *a = &d->ann[d->img < 0 ? 0 : d->img][type];
	SceSize t = ( demande > 16384 ) ? demande : 16384;
	t = ( t + 255 ) & ~255u;
	if( a->pos + t > a->taille )
	{
		t = a->taille - a->pos;
		if( t < demande )
		{
			*taille = 0;
			return NULL;
		}
	}
	void *p = a->base + a->pos;
	a->pos += t;
	*taille = t;
	return p;
}
static void *cb_vdm( void *d, SceSize n, SceSize *t )  { return ann_prendre( (SDiff *)d, 0, n, t ); }
static void *cb_vert( void *d, SceSize n, SceSize *t ) { return ann_prendre( (SDiff *)d, 1, n, t ); }
static void *cb_frag( void *d, SceSize n, SceSize *t ) { return ann_prendre( (SDiff *)d, 2, n, t ); }

static bool diff_creer( SDiff *d )
{
	if( d->etat != 0 )
		return d->etat > 0;
	d->etat = -1;
	d->img  = -1;
	static const unsigned int tailles[3] = { 128 * 1024, 1024 * 1024, 512 * 1024 };
	for( int f = 0; f < 3; ++f )
		for( int k = 0; k < 3; ++k )
		{
			d->ann[f][k].base = (unsigned char *)vgl_memalign( 4096, tailles[k], VGL_MEM_RAM );
			d->ann[f][k].taille = tailles[k];
			d->ann[f][k].pos = 0;
			if( !d->ann[f][k].base )
			{
				VLOG( "GXD", "!! listes : pas de memoire GPU pour les anneaux" );
				return false;
			}
		}
	SceGxmDeferredContextParams prm;
	memset( &prm, 0, sizeof( prm ));
	prm.hostMem          = memalign( 16, 4 * SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE );
	prm.hostMemSize      = 4 * SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
	prm.vdmCallback      = cb_vdm;
	prm.vertexCallback   = cb_vert;
	prm.fragmentCallback = cb_frag;
	prm.callbackData     = d;
	const int r = sceGxmCreateDeferredContext( &prm, &d->ctx );
	if( r < 0 )
	{
		VLOG( "GXD", "!! listes : sceGxmCreateDeferredContext -> 0x%08x", (unsigned)r );
		return false;
	}
	VLOG( "GXD", "contexte GXM differe pret" );
	d->etat = 1;
	return true;
}

// Debut d'une liste sur d, depuis le thread PRINCIPAL : anneaux de l'image,
// puis l'etat de vitaGL recopie dans la liste (sa globale gxm_context pointe
// le temps de le faire sur le contexte differe).
static bool diff_debut( SDiff *d )
{
	if( !diff_creer( d ))
		return false;
	const int img = (int)( vgl_framecount % 3 );
	if( img != d->img )
	{
		d->img = img;
		for( int k = 0; k < 3; ++k )
			d->ann[img][k].pos = 0;
	}
	if( sceGxmBeginCommandList( d->ctx ) < 0 )
		return false;
	SceGxmContext *imm = gxm_context;
	gxm_context = d->ctx;
	change_depth_func();
	validate_viewport();
	change_cull_mode();
	update_polygon_offset();
	sceGxmSetPrecomputedVertexState( d->ctx, NULL );
	sceGxmSetPrecomputedFragmentState( d->ctx, NULL );
	gxm_context = imm;
	return true;
}

// Fin : la liste est executee sur le contexte immediat, dont vitaGL reprend
// ensuite l'etat.
static void diff_fin_executer( SDiff *d )
{
	const int r = sceGxmEndCommandList( d->ctx, &d->liste );
	if( r >= 0 )
		sceGxmExecuteCommandList( gxm_context, &d->liste );
	else
		VLOG( "GXD", "!! listes : sceGxmEndCommandList -> 0x%08x", (unsigned)r );
	change_depth_func();
	validate_viewport();
	change_cull_mode();
	update_polygon_offset();
	SGxmEtat *E = &s_etat_princ;
	E->cour = NULL;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	E->fp_cour = NULL;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	E->env_ub = NULL;
	E->pre_actif = false;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
}

// --- Travailleur : lots opaques enregistres sur un autre coeur ---------------
//
// Le principal garde le parcours (champ, occultation, plages, creations
// paresseuses) et dessine lui-meme les lots non precalcules ; il passe les
// lots PRECALCULES au travailleur par une file sans verrou (un producteur, un
// consommateur). Le travailleur les enregistre dans sa liste, executee a la
// fin de la passe. L'ordre des opaques est sans effet : la profondeur trie.
static SGxmEtat s_etat_trav;
bool g_vita_lots_mt = false;

#define TRAV_FILE 4096
static void *volatile  s_trav_file[TRAV_FILE];
static volatile int    s_trav_ecrit = 0, s_trav_lu = 0;
static volatile int    s_trav_fini = 0;
static SceUID          s_trav_go = -1, s_trav_rendu = -1;
static int             s_trav_th = 0;		// 0 a creer, 1 pret, -1 echec
static bool            s_trav_en_cours = false;
static int             s_trav_n = 0, s_trav_direct = 0;
static SceUInt64       s_trav_us_att = 0, s_trav_us_exe = 0;

static int thread_lots( SceSize, void * )
{
	for( ;; )
	{
		if( sceKernelWaitSema( s_trav_go, 1, NULL ) < 0 )
			return 0;
		for( ;; )
		{
			const int e = s_trav_ecrit;
			if( s_trav_lu == e )
			{
				if( s_trav_fini && ( s_trav_lu == s_trav_ecrit ))
					break;
				continue;	// attente active : le coeur est a nous
			}
			__sync_synchronize();
			while( s_trav_lu != e )
			{
				lot_pre_e( &s_etat_trav, (SGxmLotPre *)s_trav_file[s_trav_lu % TRAV_FILE] );
				++s_trav_lu;
			}
		}
		sceKernelSignalSema( s_trav_rendu, 1 );
	}
	return 0;
}

static bool trav_pret( void )
{
	if( s_trav_th != 0 )
		return s_trav_th > 0;
	s_trav_th = -1;
	s_trav_go    = sceKernelCreateSema( "thug_lots_go", 0, 0, 1, NULL );
	s_trav_rendu = sceKernelCreateSema( "thug_lots_rendu", 0, 0, 1, NULL );
	if(( s_trav_go < 0 ) || ( s_trav_rendu < 0 ))
		return false;
	// Meme priorite que thug_listes, qui a fini son travail quand les lots
	// opaques commencent.
	SceUID th = sceKernelCreateThread( "thug_lots", thread_lots, 0x10000100 - 0x20, 0x8000,
	                                   0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, NULL );
	if(( th < 0 ) || ( sceKernelStartThread( th, 0, NULL ) < 0 ))
		return false;
	VLOG( "GXD", "lots opaques : travailleur sur les coeurs 1-2" );
	s_trav_th = 1;
	return true;
}

bool GxmTravDebut( const float *mvp, const float *vue )
{
	if( !g_vita_lots_mt || s_trav_en_cours || !trav_pret() || !diff_debut( &s_diff_trav ))
		return false;
	memset( &s_etat_trav, 0, sizeof( s_etat_trav ));
	s_etat_trav.ctx = s_diff_trav.ctx;
	s_etat_trav.mvp = mvp;
	s_etat_trav.vue = vue;
	s_trav_ecrit = 0;
	s_trav_lu    = 0;
	s_trav_fini  = 0;
	__sync_synchronize();
	s_trav_en_cours = true;
	sceKernelSignalSema( s_trav_go, 1 );
	return true;
}

bool GxmTravPousser( void *p_pre )
{
	if( !s_trav_en_cours )
		return false;
	const int e = s_trav_ecrit;
	if(( e - s_trav_lu ) >= TRAV_FILE )
	{
		++s_trav_direct;
		return false;		// file pleine : le principal dessine lui-meme
	}
	s_trav_file[e % TRAV_FILE] = p_pre;
	__sync_synchronize();
	s_trav_ecrit = e + 1;
	++s_trav_n;
	return true;
}

void GxmTravFin( void )
{
	if( !s_trav_en_cours )
		return;
	const SceUInt64 t0 = sceKernelGetProcessTimeWide();
	__sync_synchronize();
	s_trav_fini = 1;
	sceKernelWaitSema( s_trav_rendu, 1, NULL );
	s_trav_en_cours = false;
	const SceUInt64 t1 = sceKernelGetProcessTimeWide();
	diff_fin_executer( &s_diff_trav );
	s_trav_us_att += t1 - t0;
	s_trav_us_exe += sceKernelGetProcessTimeWide() - t1;
	static int s_img = 0;
	if(( ++s_img % 120 ) == 0 )
	{
		VLOG( "GXD", "lots sur l'autre coeur : %d/image, attente %.2f ms, execution %.2f ms, file pleine %d",
		      s_trav_n / 120, (float)s_trav_us_att / 120000.0f,
		      (float)s_trav_us_exe / 120000.0f, s_trav_direct );
		s_trav_n = 0; s_trav_direct = 0; s_trav_us_att = 0; s_trav_us_exe = 0;
	}
}

// Etape 0 (« gcl ») : les dessins GXM suivants du thread principal sont
// enregistres dans une liste, executee a GxmListeFin.
static bool s_liste_ouverte = false;
static SceGxmContext *sp_ctx_imm = NULL;

bool GxmListeDebut( void )
{
	if( !g_vita_listes_gxm || s_liste_ouverte || !diff_debut( &s_diff0 ))
		return false;
	sp_ctx_imm  = gxm_context;
	gxm_context = s_diff0.ctx;
	SGxmEtat *E = &s_etat_princ;
	E->cour = NULL;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	E->fp_cour = NULL;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	E->env_ub = NULL;
	E->pre_actif = false;
	s_gp_ub_ok = false;	// #32 : etat GXM change hors peau
	s_liste_ouverte = true;
	return true;
}

// Contexte GXM courant differe ? (sceGxmSetRegionClip y est interdit, #73)
bool GxmListeOuverte( void )
{
	return s_liste_ouverte;
}

void GxmListeFin( void )
{
	if( !s_liste_ouverte )
		return;
	s_liste_ouverte = false;
	gxm_context = sp_ctx_imm;
	diff_fin_executer( &s_diff0 );
}

#else
#include "p_gxm_desktop.inc"
#endif // THUG_DESKTOP

// --- Brouillard (issue #45) --------------------------------------------------
//
// Etat repris de XBox/p_nxmiscfx.cpp:1177-1275 (CFog::s_plat_*) :
//  - enable_fog : drapeau seul ;
//  - set_fog_near_distance : debut = distance, fin = debut + 600 pieds
//    (" Test code for now ", mais c'est ce qui a ete livre) ;
//  - set_fog_rgba : a = 0 coupe le brouillard, sinon l'active ; densite =
//    clamp( a / 128, 0, 1 ) ; couleur = rgb ;
//  - set_fog_exponent : sans effet (" This is no longer a valid call ").
// Densite initiale 0 (XBox/NX/nx_init.cpp:124), table lineaire (:181).

int g_vita_fog = 1;

static bool  s_fog_actif   = false;
static float s_fog_densite = 0.0f;
static float s_fog_rgb[3]  = { 0.0f, 0.0f, 0.0f };
static int   s_fog_rgba[4] = { 0, 0, 0, 0 };
static float s_fog_proche  = 0.0f;
static const float FOG_PLAGE = 600.0f * 12.0f;	// FEET_TO_INCHES( 600 )

static void fog_journal( const char *p_quoi )
{
	VLOG( "FOG", "%s : %s, couleur (%d %d %d) a=%d -> densite %.3f, debut %.0f, fin %.0f%s",
	      p_quoi, s_fog_actif ? "ACTIF" : "coupe",
	      s_fog_rgba[0], s_fog_rgba[1], s_fog_rgba[2], s_fog_rgba[3], s_fog_densite,
	      s_fog_proche, s_fog_proche + FOG_PLAGE, g_vita_fog ? "" : " (fog 0 : force a l'arret)" );
}

void BrouillardActiver( bool actif )
{
	if( actif == s_fog_actif )
		return;
	s_fog_actif = actif;
	++s_fog_gen;
	fog_journal( "activation" );
}

void BrouillardDistance( float proche )
{
	if( proche == s_fog_proche )
		return;
	s_fog_proche = proche;
	++s_fog_gen;
	fog_journal( "distance" );
}

void BrouillardCouleur( int r, int g, int b, int a )
{
	// [SOURCE] XBox/p_nxmiscfx.cpp:1257 : alpha nul = pas de brouillard.
	BrouillardActiver( a != 0 );
	const int v[4] = { r, g, b, a };
	if( !memcmp( v, s_fog_rgba, sizeof( v )))
		return;
	memcpy( s_fog_rgba, v, sizeof( v ));
	float d = (float)a / 128.0f;
	s_fog_densite = ( d > 1.0f ) ? 1.0f : (( d < 0.0f ) ? 0.0f : d );
	for( int k = 0; k < 3; ++k )
	{
		const int c = ( v[k] < 0 ) ? 0 : (( v[k] > 255 ) ? 255 : v[k] );
		s_fog_rgb[k] = (float)c / 255.0f;
	}
	++s_fog_gen;
	fog_journal( "couleur" );
}

bool BrouillardActif( void )
{
	return g_vita_fog && s_fog_actif && ( s_fog_densite > 0.0f );
}

void BrouillardUniformes( bool noir, bool ciel, float P[4], float C[4] )
{
	if( !BrouillardActif())
	{
		P[0] = P[1] = P[2] = P[3] = 0.0f;		// k = 0 : couleur intacte
		C[0] = C[1] = C[2] = C[3] = 0.0f;
		return;
	}
	P[0] = s_fog_densite;
	if( ciel )
	{
		// XBox/p_nx.cpp:329 : debut -20, fin -21 le temps du ciel.
		P[1] = 21.0f;
		P[2] = 1.0f;
	}
	else
	{
		P[1] = s_fog_proche + FOG_PLAGE;
		P[2] = 1.0f / FOG_PLAGE;
	}
	P[3] = 0.0f;
	for( int k = 0; k < 3; ++k )
		C[k] = noir ? 0.0f : s_fog_rgb[k];
	C[3] = 0.0f;
}

// Pipeline fixe : glFog de vitaGL, lineaire, applique avant le melange
// (vitaGL shaders/ffp_f.h : texColor.rgb = lerp(fogColor, texColor, vFog),
// vFog = clamp(( fin - d ) / ( fin - debut ), 0, 1), d = profondeur en vue).
// Il faut 1 - vFog = densite x ( 1 - f ) : c'est encore une rampe lineaire, de
// debut = proche a fin = proche + plage / densite. Seul ecart : au-dela de
// proche + plage, XBox plafonne a la densite, vitaGL continue de monter.
// Ciel : vFog constant = 1 - densite, par une rampe si longue (1e8) que d n'y
// change rien.
static int s_ffp_mode = 0;
static int s_ffp_noir = -1;

void BrouillardFixe( int mode )
{
	if( !BrouillardActif())
		mode = 0;
	if( mode == 0 )
	{
		if( s_ffp_mode )
			glDisable( GL_FOG );
		s_ffp_mode = 0;
		return;
	}
	if( !s_ffp_mode )
	{
		glEnable( GL_FOG );
		glFogi( GL_FOG_MODE, GL_LINEAR );
	}
	float debut, fin;
	if( mode == 2 )
	{
		const float R = 1.0e8f;
		fin   = ( 1.0f - s_fog_densite ) * R;
		debut = fin - R;
	}
	else
	{
		debut = s_fog_proche;
		fin   = s_fog_proche + FOG_PLAGE / s_fog_densite;
	}
	glFogf( GL_FOG_START, debut );
	glFogf( GL_FOG_END, fin );
	s_ffp_mode = mode;
	s_ffp_noir = -1;
	BrouillardFixeNoir( false );
}

void BrouillardFixeNoir( bool noir )
{
	if( !s_ffp_mode || ( (int)noir == s_ffp_noir ))
		return;
	s_ffp_noir = (int)noir;
	const float c[4] = { noir ? 0.0f : s_fog_rgb[0], noir ? 0.0f : s_fog_rgb[1],
	                     noir ? 0.0f : s_fog_rgb[2], 1.0f };
	glFogfv( GL_FOG_COLOR, c );
}

} // namespace NxVita
