///////////////////////////////////////////////////////////////////////////////
// p_gamma.cpp -- rampe gamma d'affichage, contrat de XBox/NX/gamma.cpp
// (issues #45 et #22). Voir p_gamma.h pour le pourquoi.
//
// Methode : FBO + passe plein ecran.
//   - GammaImageDebut (debut d'image, avant l'effacement) lie un FBO dont la
//     couleur est une texture 960x544 RGBA. Tout le rendu de l'image y va :
//     vitaGL ouvre sa scene GXM sur le FBO (scene_reset), et nos chemins GXM
//     directs (p_shader_decor.cpp) dessinent dans la scene ouverte par vitaGL
//     -- ils n'ouvrent jamais de scene eux-memes et passent par les fonctions
//     d'etat de vitaGL (validate_viewport, change_cull_mode), qui tiennent
//     compte du retournement des FBO.
//   - GammaImageFin (fin d'image, apres 2D/HUD/ecran de chargement) relie
//     l'affichage, et dessine un quad plein ecran : texture -> rampe ->
//     tampon d'affichage. Les captures (glReadPixels) lisent ensuite
//     l'affichage, donc l'image corrigee, comme xemu.
//
// La rampe est CALCULEE dans le shader (pow par canal) plutot que lue dans une
// texture table 256x1 : trois lectures de texture dependantes par pixel sont
// ce que le SGX fait de plus lent, trois pow (log2/exp2) ne coutent presque
// rien. La formule est celle de la table, a l'identique (voir s_source_pixels)
// ; la table est quand meme construite en C, pour le journal et pour
// verifier.

#include <core/defines.h>
#include <gfx/gfxman.h>

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <math.h>
#include <string.h>

#include "vita_log.h"
#include "p_gamma.h"

namespace NxVita
{

// p_shader_decor.cpp : compilation CG avec le cache disque ux0:data/thug/shd.
GLuint CompilerShaderCache( GLenum type, const char *p_source, const char *p_nom );

/*****************************************************************************
**  Contrat de XBox/NX/gamma.cpp                                            **
*****************************************************************************/

// Memes defines que l'original.
#define GAMMA_DEFAUT	1.0f
#define GAMMA_DOMAINE	256
#define GAMMA_ETENDUE	256.0f
#define MAX_GAMMA		2.0f

static float         s_gamma[3] = { GAMMA_DEFAUT, GAMMA_DEFAUT, GAMMA_DEFAUT };
static unsigned char s_table[3][GAMMA_DOMAINE];

int g_vita_gamma = 1;

// gamma.cpp:93, gammaInitTable : out[i] = (BYTE)(256 * (i/256)^(1/g)).
static void table_init( void )
{
	for( int c = 0; c < 3; ++c )
		for( int i = 0; i < GAMMA_DOMAINE; ++i )
			s_table[c][i] = (unsigned char)( GAMMA_ETENDUE *
				powf( (float)i / (float)GAMMA_DOMAINE, 1.0f / s_gamma[c] ));
}

// gamma.cpp:113, gammaSetValue. Le D3DDevice_SetGammaRamp est remplace par
// la passe de GammaImageFin, qui lit s_gamma a chaque image.
static void gamma_poser( float gr, float gg, float gb )
{
	s_gamma[0] = gr;
	s_gamma[1] = gg;
	s_gamma[2] = gb;
	table_init();
	VLOG( "GAM", "rampe : g = %.3f %.3f %.3f | table[64] = %d %d %d, "
	             "table[128] = %d %d %d, table[255] = %d %d %d",
	      gr, gg, gb,
	      s_table[0][64],  s_table[1][64],  s_table[2][64],
	      s_table[0][128], s_table[1][128], s_table[2][128],
	      s_table[0][255], s_table[1][255], s_table[2][255] );
}

// gamma.cpp:135 : borne a 0..1, x MAX_GAMMA, + 0.5 -> g de 0.5 a 2.5.
void SetGammaNormalized( float fr, float fg, float fb )
{
	fr = ( fr < 0.0f ) ? 0.0f : (( fr > 1.0f ) ? 1.0f : fr );
	fg = ( fg < 0.0f ) ? 0.0f : (( fg > 1.0f ) ? 1.0f : fg );
	fb = ( fb < 0.0f ) ? 0.0f : (( fb > 1.0f ) ? 1.0f : fb );

	fr *= MAX_GAMMA;
	fg *= MAX_GAMMA;
	fb *= MAX_GAMMA;

	fr += 0.5f;
	fg += 0.5f;
	fb += 0.5f;

	gamma_poser( fr, fg, fb );
}

// gamma.cpp:165 : l'operation inverse.
void GetGammaNormalized( float *fr, float *fg, float *fb )
{
	*fr = ( s_gamma[0] - 0.5f ) / MAX_GAMMA;
	*fg = ( s_gamma[1] - 0.5f ) / MAX_GAMMA;
	*fb = ( s_gamma[2] - 0.5f ) / MAX_GAMMA;
}


/*****************************************************************************
**  Passe plein ecran                                                       **
*****************************************************************************/

#define GAM_L	960
#define GAM_H	544

enum { GAM_ATTR_POS = 0, GAM_ATTR_UV = 1 };

static const char *s_source_sommets =
	"void main(\n"
	"	float2 aPosition,\n"
	"	float2 aTexcoord,\n"
	"	float4 out vPosition : POSITION,\n"
	"	float2 out vTexcoord : TEXCOORD0)\n"
	"{\n"
	"	vPosition = float4(aPosition, 0.f, 1.f);\n"
	"	vTexcoord = aTexcoord;\n"
	"}\n";

// c = i/255 (lecture U8 normalisee). Table Xbox : out = floor(256 (i/256)^k),
// k = 1/g, ecrite en out/255 -- l'ecriture U8 la rend exacte. Le +0.001
// absorbe l'erreur d'arrondi de c et de pow (sinon k = 1 rendrait i-1).
static const char *s_source_pixels =
	"float4 main(\n"
	"	float2 vTexcoord : TEXCOORD0,\n"
	"	uniform sampler2D uImage,\n"
	"	uniform float3 uK)\n"
	"{\n"
	"	float3 c = tex2D(uImage, vTexcoord).rgb;\n"
	"	float3 o = floor(256.f * pow(c * (255.f / 256.f), uK) + 0.001f);\n"
	"	return float4(min(o, 255.f) / 255.f, 1.f);\n"
	"}\n";

static int    s_etat  = 0;		// 0 jamais tente, 1 pret, -1 echec (rampe abandonnee)
static GLuint s_fbo   = 0;
static GLuint s_tex   = 0;
static GLuint s_vbo   = 0;
static GLuint s_prog  = 0;
static GLint  s_loc_k = -1;
static bool   s_lie   = false;	// le FBO est la cible de l'image en cours
static bool   s_inverse = false;

static bool pret( void )
{
	if( s_etat != 0 )
		return ( s_etat > 0 );
	s_etat = -1;

	GLuint vs = CompilerShaderCache( GL_CG_VERTEX_SHADER_EXT, s_source_sommets, "gamma sommets" );
	GLuint fs = CompilerShaderCache( GL_CG_FRAGMENT_SHADER_EXT, s_source_pixels, "gamma pixels" );
	if( !vs || !fs )
	{
		VLOG( "GAM", "!! shaders refuses : pas de rampe gamma" );
		return false;
	}
	s_prog = glCreateProgram();
	glAttachShader( s_prog, vs );
	glAttachShader( s_prog, fs );
	glBindAttribLocation( s_prog, GAM_ATTR_POS, "aPosition" );
	glBindAttribLocation( s_prog, GAM_ATTR_UV,  "aTexcoord" );
	glLinkProgram( s_prog );
	GLint ok = 0;
	glGetProgramiv( s_prog, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		VLOG( "GAM", "!! edition de liens refusee : pas de rampe gamma" );
		return false;
	}
	s_loc_k = glGetUniformLocation( s_prog, "uK" );
	GLint prog_avant = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog_avant );
	glUseProgram( s_prog );
	glUniform1i( glGetUniformLocation( s_prog, "uImage" ), 0 );
	glUseProgram( prog_avant );

	// Quad plein ecran en bande : 4 sommets V normal, puis 4 V retourne
	// ("gam 2"). x, y (espace de decoupe), u, v.
	static const float quad[2][16] = {
		{ -1.f, -1.f, 0.f, 0.f,   1.f, -1.f, 1.f, 0.f,
		  -1.f,  1.f, 0.f, 1.f,   1.f,  1.f, 1.f, 1.f },
		{ -1.f, -1.f, 0.f, 1.f,   1.f, -1.f, 1.f, 1.f,
		  -1.f,  1.f, 0.f, 0.f,   1.f,  1.f, 1.f, 0.f } };
	GLint buf_avant = 0;
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buf_avant );
	glGenBuffers( 1, &s_vbo );
	glBindBuffer( GL_ARRAY_BUFFER, s_vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( quad ), quad, GL_STATIC_DRAW );
	glBindBuffer( GL_ARRAY_BUFFER, buf_avant );

	// Texture intermediaire : 1 texel par pixel, lue en NEAREST.
	const unsigned int vram_avant = (unsigned int)vglMemFree( VGL_MEM_VRAM );
	GLint act_avant = 0, tex_avant = 0;
	glGetIntegerv( GL_ACTIVE_TEXTURE, &act_avant );
	glActiveTexture( GL_TEXTURE0 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex_avant );
	glGenTextures( 1, &s_tex );
	glBindTexture( GL_TEXTURE_2D, s_tex );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, GAM_L, GAM_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, tex_avant );
	glActiveTexture( act_avant );

	// FBO. vitaGL lui fabrique a la premiere scene un tampon de profondeur
	// (sans stencil -- le backend n'en utilise pas) et une cible de rendu.
	glGenFramebuffers( 1, &s_fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, s_fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0 );
	const GLenum statut = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	if( statut != GL_FRAMEBUFFER_COMPLETE )
	{
		VLOG( "GAM", "!! FBO incomplet (0x%x) : pas de rampe gamma", (unsigned)statut );
		return false;
	}
	VLOG( "GAM", "passe gamma prete : FBO %dx%d, VRAM libre %u Ko -> %u Ko",
	      GAM_L, GAM_H, vram_avant >> 10, (unsigned)( vglMemFree( VGL_MEM_VRAM ) >> 10 ));
	s_etat = 1;
	return true;
}

#ifdef THUG_DESKTOP
// Desktop: no intermediate 960x544 target (the screen is rendered at the
// window's resolution); the ramp is applied by the final scaling pass.
extern "C" void desktop_set_gamma( float kr, float kg, float kb );
void GammaImageDebut( void ) {}
void GammaImageFin( void )
{
	if( g_vita_gamma )
		desktop_set_gamma( 1.0f / s_gamma[0], 1.0f / s_gamma[1], 1.0f / s_gamma[2] );
	else
		desktop_set_gamma( 1.0f, 1.0f, 1.0f );
}
#else
void GammaImageDebut( void )
{
	if( s_lie || !g_vita_gamma || !pret() )
		return;
	s_inverse = ( g_vita_gamma == 2 );
	glBindFramebuffer( GL_FRAMEBUFFER, s_fbo );
	s_lie = true;
}

void GammaImageFin( void )
{
	if( !s_lie )
		return;
	s_lie = false;

	const SceUInt64 t0 = sceKernelGetProcessTimeWide();

	// L'etat est SAUVE puis RENDU : le backend garde des caches d'etat GL
	// (cull_poser, zbias_poser...) qui deviendraient faux.
	GLint vp[4], prog_avant = 0, act_avant = 0, tex_avant = 0, buf_avant = 0;
	glGetIntegerv( GL_VIEWPORT, vp );
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog_avant );
	glGetIntegerv( GL_ACTIVE_TEXTURE, &act_avant );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buf_avant );
	glActiveTexture( GL_TEXTURE0 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex_avant );
	const GLboolean dt = glIsEnabled( GL_DEPTH_TEST );
	const GLboolean cf = glIsEnabled( GL_CULL_FACE );
	const GLboolean bl = glIsEnabled( GL_BLEND );
	const GLboolean sc = glIsEnabled( GL_SCISSOR_TEST );
	const GLboolean st = glIsEnabled( GL_STENCIL_TEST );

	// Retour a l'affichage. La scene du FBO est fermee par vitaGL au premier
	// dessin qui suit (scene_reset), qui reapplique aussi le viewport.
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	glViewport( 0, 0, GAM_L, GAM_H );
	if( dt ) glDisable( GL_DEPTH_TEST );
	if( cf ) glDisable( GL_CULL_FACE );
	if( bl ) glDisable( GL_BLEND );
	if( sc ) glDisable( GL_SCISSOR_TEST );
	if( st ) glDisable( GL_STENCIL_TEST );

	glUseProgram( s_prog );
	glUniform3f( s_loc_k, 1.0f / s_gamma[0], 1.0f / s_gamma[1], 1.0f / s_gamma[2] );
	glBindTexture( GL_TEXTURE_2D, s_tex );
	glBindBuffer( GL_ARRAY_BUFFER, s_vbo );
	glEnableVertexAttribArray( GAM_ATTR_POS );
	glEnableVertexAttribArray( GAM_ATTR_UV );
	glVertexAttribPointer( GAM_ATTR_POS, 2, GL_FLOAT, GL_FALSE, 4 * sizeof( float ), (void *)0 );
	glVertexAttribPointer( GAM_ATTR_UV,  2, GL_FLOAT, GL_FALSE, 4 * sizeof( float ),
	                       (void *)( 2 * sizeof( float )));
	glDrawArrays( GL_TRIANGLE_STRIP, s_inverse ? 4 : 0, 4 );
	glDisableVertexAttribArray( GAM_ATTR_POS );
	glDisableVertexAttribArray( GAM_ATTR_UV );

	// Etat rendu.
	glBindBuffer( GL_ARRAY_BUFFER, buf_avant );
	glBindTexture( GL_TEXTURE_2D, tex_avant );
	glActiveTexture( act_avant );
	glUseProgram( prog_avant );
	if( dt ) glEnable( GL_DEPTH_TEST );
	if( cf ) glEnable( GL_CULL_FACE );
	if( bl ) glEnable( GL_BLEND );
	if( sc ) glEnable( GL_SCISSOR_TEST );
	if( st ) glEnable( GL_STENCIL_TEST );
	glViewport( vp[0], vp[1], vp[2], vp[3] );

	// Cout PROCESSEUR de la passe (soumission). Le cout GPU ne se voit que
	// dans le temps d'image : A/B "gam 0" / "gam 1" sur [PERF].
	static SceUInt64 s_us = 0;
	static int       s_n  = 0;
	s_us += sceKernelGetProcessTimeWide() - t0;
	if(( ++s_n % 600 ) == 0 )
	{
		VLOG( "GAM", "passe gamma : %.3f ms processeur par image (moyenne sur 600)",
		      (float)s_us / 600000.0f );
		s_us = 0;
	}
}
#endif // THUG_DESKTOP

} // namespace NxVita


/*****************************************************************************
**  Gfx::Manager -- ce que XBox/p_gfxman.cpp:78-92 transmet a NxXbox        **
*****************************************************************************/

namespace Gfx
{

void Manager::SetGammaNormalized( float fr, float fg, float fb )
{
	NxVita::SetGammaNormalized( fr, fg, fb );
}

void Manager::GetGammaNormalized( float *fr, float *fg, float *fb )
{
	NxVita::GetGammaNormalized( fr, fg, fb );
}

} // namespace Gfx
