/*****************************************************************************
**  THUG desktop -- window, OpenGL context and vitaGL replacement           **
**  desktop/src/shim_gl.cpp                                                 **
**                                                                          **
**  The game draws into an offscreen "screen" (colour + depth/stencil) that **
**  it believes is the Vita's 960x544 framebuffer. Viewport, scissor and    **
**  pixel-read coordinates given in those units are scaled to the real      **
**  size, so the render resolution is free. At swap the screen is scaled    **
**  into the window, keeping its aspect ratio.                              **
*****************************************************************************/

#define DESKTOP_GL_IMPL
#include <vitaGL.h>
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>

#include "desktop_config.h"

extern "C" void vita_log_printf( const char *sys, const char *fmt, ... );
#define DLOG( ... ) vita_log_printf( "DSK", __VA_ARGS__ )

bool cg_to_glsl( const char *p_src, bool vertex, std::string &out, std::string &err );
extern "C" void desktop_pump_events( void );

// -- Loaded entry points ----------------------------------------------------------
#define DESKTOP_GL_DEFINE( type, name ) type dgl_##name = NULL;
DESKTOP_GL_FUNCS( DESKTOP_GL_DEFINE )
#undef DESKTOP_GL_DEFINE

static SDL_Window   *s_window = NULL;
static SDL_GLContext s_context = NULL;

// Virtual screen (what the game thinks it renders to) and real target.
static const int VIRT_W = 960, VIRT_H = 544;
static int    s_rt_w = 960, s_rt_h = 544;
static GLuint s_fbo = 0, s_fbo_resolve = 0;
static GLuint s_rb_color = 0, s_rb_depth = 0, s_tex_resolve = 0;
static int    s_msaa = 0;
static GLuint s_bound_fb = 0;		// as the game sees it: 0 = screen
// vitaGL's flag: the display is bound (p_ombre picks its clip-rect y convention by it).
extern "C" { GLboolean is_rendering_display = GL_TRUE; }

extern "C" int desktop_window_width( void )  { int w = 0, h = 0; if( s_window ) SDL_GL_GetDrawableSize( s_window, &w, &h ); return w; }
extern "C" int desktop_window_height( void ) { int w = 0, h = 0; if( s_window ) SDL_GL_GetDrawableSize( s_window, &w, &h ); return h; }
extern "C" int desktop_render_width( void )  { return s_rt_w; }
extern "C" int desktop_render_height( void ) { return s_rt_h; }
extern "C" SDL_Window *desktop_sdl_window( void ) { return s_window; }

extern "C" int desktop_gl_load( void )
{
	int missing = 0;
	// (GLX hands out a pointer for any name, so a hit proves nothing; a miss does.)
#define DESKTOP_GL_LOAD( type, name ) \
	if( !strstr( #name, "_real" )) { \
		dgl_##name = (type)SDL_GL_GetProcAddress( #name ); \
		if( !dgl_##name ) { DLOG( "!! OpenGL function missing: %s", #name ); ++missing; } }
	DESKTOP_GL_FUNCS( DESKTOP_GL_LOAD )
#undef DESKTOP_GL_LOAD
	// The *_real names are wrapped; load them under their GL names.
	dgl_glCreateShader_real    = (PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress( "glCreateShader" );
	dgl_glShaderSource_real    = (PFNGLSHADERSOURCEPROC)SDL_GL_GetProcAddress( "glShaderSource" );
	dgl_glBindFramebuffer_real = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress( "glBindFramebuffer" );
	if( !dgl_glCreateShader_real || !dgl_glShaderSource_real || !dgl_glBindFramebuffer_real )
	{
		DLOG( "!! OpenGL function missing: glCreateShader, glShaderSource or glBindFramebuffer" );
		++missing;
	}
	return missing;
}

// -- Offscreen screen ---------------------------------------------------------------
static bool screen_targets_create( void )
{
	GLint max_samples = 0;
	glGetIntegerv( GL_MAX_SAMPLES, &max_samples );
	if( s_msaa > max_samples ) s_msaa = max_samples;

	dgl_glGenFramebuffers( 1, &s_fbo );
	dgl_glGenRenderbuffers( 1, &s_rb_color );
	dgl_glGenRenderbuffers( 1, &s_rb_depth );
	dgl_glBindRenderbuffer( GL_RENDERBUFFER, s_rb_color );
	if( s_msaa > 1 )
		dgl_glRenderbufferStorageMultisample( GL_RENDERBUFFER, s_msaa, GL_RGBA8, s_rt_w, s_rt_h );
	else
		dgl_glRenderbufferStorage( GL_RENDERBUFFER, GL_RGBA8, s_rt_w, s_rt_h );
	dgl_glBindRenderbuffer( GL_RENDERBUFFER, s_rb_depth );
	if( s_msaa > 1 )
		dgl_glRenderbufferStorageMultisample( GL_RENDERBUFFER, s_msaa, GL_DEPTH24_STENCIL8, s_rt_w, s_rt_h );
	else
		dgl_glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_rt_w, s_rt_h );
	dgl_glBindRenderbuffer( GL_RENDERBUFFER, 0 );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
	dgl_glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_rb_color );
	dgl_glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_rb_depth );
	dgl_glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_rb_depth );
	GLenum st = dgl_glCheckFramebufferStatus( GL_FRAMEBUFFER );
	if( st != GL_FRAMEBUFFER_COMPLETE )
	{
		DLOG( "!! screen framebuffer incomplete (0x%x)", (unsigned)st );
		return false;
	}

	// Single-sample copy of the screen: blit source when MSAA is on, and
	// the texture post effects (SSAO, ...) will read.
	glGenTextures( 1, &s_tex_resolve );
	glBindTexture( GL_TEXTURE_2D, s_tex_resolve );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, s_rt_w, s_rt_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	glBindTexture( GL_TEXTURE_2D, 0 );
	dgl_glGenFramebuffers( 1, &s_fbo_resolve );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo_resolve );
	dgl_glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex_resolve, 0 );

	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
	s_bound_fb = 0;
	is_rendering_display = GL_TRUE;
	glViewport( 0, 0, s_rt_w, s_rt_h );
	return true;
}

// -- vgl* ------------------------------------------------------------------------
extern "C" void desktop_crash_handler_install( void );

extern "C" GLboolean vglInitExtended( int, int width, int height, int, SceGxmMultisampleMode )
{
	if( s_window )
		return GL_FALSE;
	desktop_crash_handler_install();
	const DesktopConfig &cfg = desktop_config();

	if( SDL_InitSubSystem( SDL_INIT_VIDEO ) != 0 )
	{
		DLOG( "!! SDL video: %s", SDL_GetError());
		exit( 1 );
	}
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 2 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 1 );
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 0 );
	SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 0 );

	Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if( cfg.fullscreen == 1 ) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	if( cfg.fullscreen == 2 ) flags |= SDL_WINDOW_FULLSCREEN;
	// Window size: the configured one, never larger than the screen
	// (a 1920x1080 window on a 1440x1080 desktop shows only part of the game).
	int win_w = cfg.window_w, win_h = cfg.window_h;
	SDL_Rect ecran;
	if( SDL_GetDisplayUsableBounds( 0, &ecran ) != 0 )
	{
		SDL_DisplayMode m;
		if( SDL_GetDesktopDisplayMode( 0, &m ) == 0 ) { ecran.w = m.w; ecran.h = m.h; }
		else { ecran.w = 1280; ecran.h = 720; }
	}
	if( cfg.fullscreen == 1 || win_w <= 0 || win_h <= 0 )
	{
		SDL_DisplayMode m;
		if( SDL_GetDesktopDisplayMode( 0, &m ) == 0 ) { win_w = m.w; win_h = m.h; }
		else { win_w = ecran.w; win_h = ecran.h; }
	}
	if( cfg.fullscreen == 0 )
	{
		if( win_w > ecran.w ) win_w = ecran.w;
		if( win_h > ecran.h ) win_h = ecran.h;
	}
	s_window = SDL_CreateWindow( "Tony Hawk's Underground", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                             win_w, win_h, flags );
	if( !s_window )
	{
		DLOG( "!! window: %s", SDL_GetError());
		exit( 1 );
	}
	s_context = SDL_GL_CreateContext( s_window );
	if( !s_context )
	{
		DLOG( "!! OpenGL context: %s", SDL_GetError());
		exit( 1 );
	}
	SDL_GL_SetSwapInterval( cfg.vsync ? 1 : 0 );
	DLOG( "GL_VENDOR=%s", (const char *)glGetString( GL_VENDOR ));
	DLOG( "GL_RENDERER=%s", (const char *)glGetString( GL_RENDERER ));
	DLOG( "GL_VERSION=%s", (const char *)glGetString( GL_VERSION ));
	if( desktop_gl_load() != 0 )
	{
		DLOG( "!! this GPU driver lacks OpenGL functions the game needs" );
		exit( 1 );
	}

	// Render size: explicit, or the window's size at the Vita's aspect ratio.
	if( cfg.render_w > 0 && cfg.render_h > 0 )
	{
		s_rt_w = cfg.render_w;
		s_rt_h = cfg.render_h;
	}
	else
	{
		int dw = desktop_window_width(), dh = desktop_window_height();
		if( dw <= 0 || dh <= 0 ) { dw = width; dh = height; }
		s_rt_h = dh;
		s_rt_w = ( dh * VIRT_W + VIRT_H / 2 ) / VIRT_H;
		if( s_rt_w > dw ) { s_rt_w = dw; s_rt_h = ( dw * VIRT_H + VIRT_W / 2 ) / VIRT_W; }
	}
	s_msaa = cfg.msaa;
	DLOG( "window %dx%d, render %dx%d, MSAA %d", desktop_window_width(), desktop_window_height(), s_rt_w, s_rt_h, s_msaa );
	if( !screen_targets_create())
		exit( 1 );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );
	return GL_FALSE;
}

extern "C" GLboolean vglInit( int legacy_pool_size )
{
	return vglInitExtended( legacy_pool_size, 960, 544, 0, SCE_GXM_MULTISAMPLE_NONE );
}

// Hook for post effects (SSAO etc.) run on the finished 3D image.
extern "C" void (*g_desktop_post_hook)( GLuint resolve_tex, int w, int h );
void (*g_desktop_post_hook)( GLuint, int, int ) = NULL;

// Display gamma ramp (Code/Gfx/Vita/p_gamma.cpp hands it over each frame): on
// Vita it is a full-screen pass from a 960x544 texture; here it is applied
// while scaling the finished image into the window. k = 1/gamma per channel,
// same formula as the Vita shader (the Xbox gamma table).
static float  s_gamma_k[3] = { 1.0f, 1.0f, 1.0f };
static GLuint s_gamma_prog = 0;
static GLint  s_gamma_loc_k = -1;
static bool   s_gamma_echec = false;

extern "C" void desktop_set_gamma( float kr, float kg, float kb )
{
	s_gamma_k[0] = kr; s_gamma_k[1] = kg; s_gamma_k[2] = kb;
}

static bool gamma_prog( void )
{
	if( s_gamma_prog || s_gamma_echec )
		return s_gamma_prog != 0;
	static const char *vs =
		"#version 120\n"
		"varying vec2 t;\n"
		"void main() { t = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";
	static const char *fs =
		"#version 120\n"
		"uniform sampler2D img;\n"
		"uniform vec3 k;\n"
		"varying vec2 t;\n"
		"void main() {\n"
		"	vec3 c = texture2D(img, t).rgb;\n"
		"	vec3 o = floor(256.0 * pow(c * (255.0 / 256.0), k) + 0.001);\n"
		"	gl_FragColor = vec4(min(o, 255.0) / 255.0, 1.0);\n"
		"}\n";
	GLuint sh[2] = { dgl_glCreateShader_real( GL_VERTEX_SHADER ), dgl_glCreateShader_real( GL_FRAGMENT_SHADER ) };
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	for( int i = 0; i < 2; ++i )
	{
		dgl_glShaderSource_real( sh[i], 1, &src[i], NULL );
		glCompileShader( sh[i] );
		glAttachShader( p, sh[i] );
	}
	dgl_glLinkProgram( p );
	GLint ok = 0;
	glGetProgramiv( p, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		DLOG( "!! gamma pass: program refused, gamma ignored" );
		s_gamma_echec = true;
		return false;
	}
	s_gamma_prog = p;
	s_gamma_loc_k = glGetUniformLocation( p, "k" );
	GLint avant = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &avant );
	glUseProgram( p );
	glUniform1i( glGetUniformLocation( p, "img" ), 0 );
	glUseProgram( avant );
	return true;
}

// Draws s_tex_resolve into the window's (x, y, w, h) through the ramp.
static void gamma_draw( int x, int y, int w, int h )
{
	GLint prog = 0, act = 0, tex = 0, vp[4];
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog );
	glGetIntegerv( GL_ACTIVE_TEXTURE, &act );
	glGetIntegerv( GL_VIEWPORT, vp );
	glActiveTexture( GL_TEXTURE0 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex );
	glPushAttrib( GL_ENABLE_BIT );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisable( GL_BLEND );
	glDisable( GL_ALPHA_TEST );
	glDisable( GL_STENCIL_TEST );
	GLint buf = 0;
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buf );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	glViewport( x, y, w, h );
	glUseProgram( s_gamma_prog );
	glUniform3f( s_gamma_loc_k, s_gamma_k[0], s_gamma_k[1], s_gamma_k[2] );
	glBindTexture( GL_TEXTURE_2D, s_tex_resolve );
	glBegin( GL_TRIANGLE_STRIP );
	glTexCoord2f( 0, 0 ); glVertex2f( -1, -1 );
	glTexCoord2f( 1, 0 ); glVertex2f(  1, -1 );
	glTexCoord2f( 0, 1 ); glVertex2f( -1,  1 );
	glTexCoord2f( 1, 1 ); glVertex2f(  1,  1 );
	glEnd();

	glBindBuffer( GL_ARRAY_BUFFER, buf );
	glPopAttrib();
	glBindTexture( GL_TEXTURE_2D, tex );
	glActiveTexture( act );
	glUseProgram( prog );
	glViewport( vp[0], vp[1], vp[2], vp[3] );
}

extern "C" void vglSwapBuffers( GLboolean )
{
	if( !s_window )
		return;
	GLboolean scissor = glIsEnabled( GL_SCISSOR_TEST );
	if( scissor ) glDisable( GL_SCISSOR_TEST );

	int dw = desktop_window_width(), dh = desktop_window_height();
	// Fit, centred, keep the aspect ratio of the render target.
	int w = dw, h = ( dw * s_rt_h ) / s_rt_w;
	if( h > dh ) { h = dh; w = ( dh * s_rt_w ) / s_rt_h; }
	int x = ( dw - w ) / 2, y = ( dh - h ) / 2;

	dgl_glBindFramebuffer_real( GL_READ_FRAMEBUFFER, s_fbo );
	dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, 0 );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT );
	const bool gamma = ( s_gamma_k[0] != 1.0f || s_gamma_k[1] != 1.0f || s_gamma_k[2] != 1.0f ) && gamma_prog();
	if( gamma )
	{
		dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, s_fbo_resolve );
		dgl_glBlitFramebuffer( 0, 0, s_rt_w, s_rt_h, 0, 0, s_rt_w, s_rt_h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
		dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, 0 );
		gamma_draw( x, y, w, h );
	}
	else if( s_msaa > 1 || ( w != s_rt_w || h != s_rt_h ))
	{
		// MSAA needs a same-size resolve before a scaled blit.
		dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, s_fbo_resolve );
		dgl_glBlitFramebuffer( 0, 0, s_rt_w, s_rt_h, 0, 0, s_rt_w, s_rt_h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
		dgl_glBindFramebuffer_real( GL_READ_FRAMEBUFFER, s_fbo_resolve );
		dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, 0 );
		dgl_glBlitFramebuffer( 0, 0, s_rt_w, s_rt_h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR );
	}
	else
		dgl_glBlitFramebuffer( 0, 0, s_rt_w, s_rt_h, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_NEAREST );

	SDL_GL_SwapWindow( s_window );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_bound_fb ? s_bound_fb : s_fbo );
	if( scissor ) glEnable( GL_SCISSOR_TEST );
	desktop_pump_events();
}

extern "C" void desktop_swap_interval( int n )
{
	if( desktop_config().vsync )
		SDL_GL_SetSwapInterval( n );
}

extern "C" size_t vglMemFree( vglMemType ) { return 512u * 1024u * 1024u; }
extern "C" void vglSetVertexBufferSize( uint32_t ) {}
extern "C" void vglSetFragmentBufferSize( uint32_t ) {}
extern "C" void vglSetVDMBufferSize( uint32_t ) {}
extern "C" void vglSetParamBufferSize( uint32_t ) {}
extern "C" void vglUseCachedMem( GLboolean ) {}
extern "C" void vglLazyFree( void *addr ) { free( addr ); }
extern "C" void *vglReserveVertexUniformBuffer( void *, unsigned int ) { return NULL; }
extern "C" void vglGetShaderBinary( GLuint, GLsizei, GLsizei *length, void * ) { if( length ) *length = 0; }
extern "C" void glShaderBinary( GLsizei, const GLuint *, GLenum, const void *, GLsizei ) {}

// Texture data pointer: only a diagnostic command reads it ("texsum").
static std::vector<unsigned char> s_tex_readback;
extern "C" void *vglGetTexDataPointer( GLenum target )
{
	GLint w = 0, h = 0;
	glGetTexLevelParameteriv( target, 0, GL_TEXTURE_WIDTH, &w );
	glGetTexLevelParameteriv( target, 0, GL_TEXTURE_HEIGHT, &h );
	if( w <= 0 || h <= 0 )
		return NULL;
	s_tex_readback.resize( (size_t)w * h * 4 + 16384 );
	glGetTexImage( target, 0, GL_RGBA, GL_UNSIGNED_BYTE, &s_tex_readback[0] );
	return &s_tex_readback[0];
}

// "GXM descriptor" of the bound texture: a stable handle naming the GL
// texture. The desktop draw paths bind gl_name.
static std::map<GLuint, SceGxmTexture *> s_gxm_tex;
extern "C" SceGxmTexture *vglGetGxmTexture( GLenum target )
{
	GLint name = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &name );
	if( name <= 0 )
		return NULL;
	SceGxmTexture *&p = s_gxm_tex[(GLuint)name];
	if( !p )
		p = (SceGxmTexture *)calloc( 1, sizeof( SceGxmTexture ));
	GLint w = 0, h = 0, f = 0;
	glGetTexLevelParameteriv( target, 0, GL_TEXTURE_WIDTH, &w );
	glGetTexLevelParameteriv( target, 0, GL_TEXTURE_HEIGHT, &h );
	glGetTexLevelParameteriv( target, 0, GL_TEXTURE_INTERNAL_FORMAT, &f );
	p->gl_name = (GLuint)name;
	p->width = (unsigned)w;
	p->height = (unsigned)h;
	p->format = (unsigned)f;
	return p;
}

extern "C" unsigned int sceGxmTextureGetWidth( const SceGxmTexture *t )  { return t ? t->width : 0; }
extern "C" unsigned int sceGxmTextureGetHeight( const SceGxmTexture *t ) { return t ? t->height : 0; }
extern "C" SceGxmTextureFormat sceGxmTextureGetFormat( const SceGxmTexture *t )
{
	// 0x0C000000 = Vita 32-bit RGBA family, which is what texsum checks for.
	return ( t && ( t->format == GL_RGBA8 || t->format == GL_RGBA )) ? 0x0C000000 : 0;
}

// -- Region clip (shadow pass) -> scissor --------------------------------------
SceGxmContext *gxm_context = NULL;
extern "C" int sceGxmSetRegionClip( SceGxmContext *, int mode, unsigned int x0, unsigned int y0,
                                    unsigned int x1, unsigned int y1 )
{
	if( mode == SCE_GXM_REGION_CLIP_OUTSIDE )
	{
		// GXM: inclusive pixel rectangle, y down from the top on the display.
		// vitaGL FBOs are stored flipped, so there the caller already passes GL y.
		glEnable( GL_SCISSOR_TEST );
		const GLint gy = is_rendering_display ? (GLint)( VIRT_H - 1 - y1 ) : (GLint)y0;
		desktop_glScissor( (GLint)x0, gy, (GLsizei)( x1 - x0 + 1 ), (GLsizei)( y1 - y0 + 1 ));
	}
	else
		glDisable( GL_SCISSOR_TEST );
	return 0;
}

// -- Coordinate-scaling wrappers ------------------------------------------------------
static inline bool on_screen( void ) { return s_bound_fb == 0; }
static inline int sx( int v ) { return (int)(( (long long)v * s_rt_w + VIRT_W / 2 ) / VIRT_W ); }
static inline int sy( int v ) { return (int)(( (long long)v * s_rt_h + VIRT_H / 2 ) / VIRT_H ); }

// Viewport and scissor as the game set them (screen = Vita 960x544
// coordinates). Like vitaGL, they are state that follows the game across
// render-target switches: p_ombre.cpp restores the screen viewport while its
// shadow map is still bound, then binds the screen. So the real GL values are
// recomputed for whichever target is bound.
static GLint s_vp[4] = { 0, 0, VIRT_W, VIRT_H };
static GLint s_sc[4] = { 0, 0, VIRT_W, VIRT_H };

static void appliquer_vp( void )
{
	const GLint x = s_vp[0], y = s_vp[1], w = s_vp[2], h = s_vp[3];
	if( on_screen())
		glViewport( sx( x ), sy( y ), sx( x + w ) - sx( x ), sy( y + h ) - sy( y ));
	else
		glViewport( x, y, w, h );
}

static void appliquer_sc( void )
{
	const GLint x = s_sc[0], y = s_sc[1], w = s_sc[2], h = s_sc[3];
	if( on_screen())
		glScissor( sx( x ), sy( y ), sx( x + w ) - sx( x ), sy( y + h ) - sy( y ));
	else
		glScissor( x, y, w, h );
}

extern "C" void desktop_glViewport( GLint x, GLint y, GLsizei w, GLsizei h )
{
	s_vp[0] = x; s_vp[1] = y; s_vp[2] = w; s_vp[3] = h;
	appliquer_vp();
}

extern "C" void desktop_glScissor( GLint x, GLint y, GLsizei w, GLsizei h )
{
	s_sc[0] = x; s_sc[1] = y; s_sc[2] = w; s_sc[3] = h;
	appliquer_sc();
}

extern "C" void desktop_glGetIntegerv( GLenum pname, GLint *v )
{
	if( pname == GL_FRAMEBUFFER_BINDING )
	{
		*v = (GLint)s_bound_fb;
		return;
	}
	if( pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX )
	{
		const GLint *src = ( pname == GL_VIEWPORT ) ? s_vp : s_sc;
		for( int i = 0; i < 4; ++i ) v[i] = src[i];
		return;
	}
	glGetIntegerv( pname, v );
}

extern "C" void desktop_glReadPixels( GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels )
{
	if( !on_screen())
	{
		glReadPixels( x, y, w, h, format, type, pixels );
		return;
	}
	// The game samples the screen at Vita coordinates (probes, screenshots).
	// MSAA buffers can't be read directly: resolve first.
	GLuint src = s_fbo;
	if( s_msaa > 1 )
	{
		dgl_glBindFramebuffer_real( GL_READ_FRAMEBUFFER, s_fbo );
		dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, s_fbo_resolve );
		dgl_glBlitFramebuffer( 0, 0, s_rt_w, s_rt_h, 0, 0, s_rt_w, s_rt_h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
		src = s_fbo_resolve;
	}
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, src );
	if( w == 1 && h == 1 )
		glReadPixels( sx( x ), sy( y ), 1, 1, format, type, pixels );
	else
	{
		// Nearest sampling of the full-size image back to w x h.
		std::vector<unsigned char> full( (size_t)s_rt_w * s_rt_h * 4 );
		glReadPixels( 0, 0, s_rt_w, s_rt_h, GL_RGBA, GL_UNSIGNED_BYTE, &full[0] );
		unsigned char *o = (unsigned char *)pixels;
		for( int j = 0; j < h; ++j )
			for( int i = 0; i < w; ++i )
			{
				const int fx = sx( x + i ) < s_rt_w ? sx( x + i ) : s_rt_w - 1;
				const int fy = sy( y + j ) < s_rt_h ? sy( y + j ) : s_rt_h - 1;
				memcpy( o + ((size_t)j * w + i ) * 4, &full[((size_t)fy * s_rt_w + fx ) * 4], 4 );
			}
	}
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
}

extern "C" void desktop_glBindFramebuffer( GLenum target, GLuint fb )
{
	const bool changement = ( fb != s_bound_fb );
	s_bound_fb = fb;
	is_rendering_display = fb ? GL_FALSE : GL_TRUE;
	dgl_glBindFramebuffer_real( target, fb ? fb : s_fbo );
	if( changement )
	{
		appliquer_vp();
		appliquer_sc();
	}
}

extern "C" GLuint desktop_screen_fbo( void ) { return s_fbo; }
extern "C" GLuint desktop_screen_resolve( int *w, int *h )
{
	dgl_glBindFramebuffer_real( GL_READ_FRAMEBUFFER, s_fbo );
	dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, s_fbo_resolve );
	dgl_glBlitFramebuffer( 0, 0, s_rt_w, s_rt_h, 0, 0, s_rt_w, s_rt_h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_bound_fb ? s_bound_fb : s_fbo );
	if( w ) *w = s_rt_w;
	if( h ) *h = s_rt_h;
	return s_tex_resolve;
}

// -- Shaders: Cg stages translated to GLSL ---------------------------------------------
static std::map<GLuint, bool> s_cg_vertex;		// shader -> is a Cg vertex shader

extern "C" GLuint desktop_glCreateShader( GLenum type )
{
	if( type == GL_CG_VERTEX_SHADER_EXT || type == GL_CG_FRAGMENT_SHADER_EXT )
	{
		const bool vs = ( type == GL_CG_VERTEX_SHADER_EXT );
		GLuint sh = dgl_glCreateShader_real( vs ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER );
		s_cg_vertex[sh] = vs;
		return sh;
	}
	return dgl_glCreateShader_real( type );
}

extern "C" void desktop_glShaderSource( GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length )
{
	std::map<GLuint, bool>::iterator it = s_cg_vertex.find( shader );
	if( it == s_cg_vertex.end())
	{
		dgl_glShaderSource_real( shader, count, string, length );
		return;
	}
	std::string cg;
	for( GLsizei i = 0; i < count; ++i )
		cg += ( length && length[i] >= 0 ) ? std::string( string[i], length[i] ) : std::string( string[i] );
	std::string glsl, err;
	if( !cg_to_glsl( cg.c_str(), it->second, glsl, err ))
	{
		DLOG( "!! Cg translation failed (%s): %s", err.c_str(), cg.c_str());
		glsl = it->second ? "#version 120\nvoid main(){ gl_Position = vec4(2.0,2.0,2.0,1.0); }\n"
		                  : "#version 120\nvoid main(){ discard; }\n";
	}
	const GLchar *p = glsl.c_str();
	dgl_glShaderSource_real( shader, 1, &p, NULL );
	if( desktop_config().dump_shaders )
	{
		static int n = 0;
		DLOG( "shader %d (%s):\n%s", n++, it->second ? "vertex" : "fragment", p );
	}
}

extern "C" void desktop_glLinkProgram( GLuint program )
{
	dgl_glLinkProgram( program );
	GLint ok = 0;
	dgl_glGetProgramiv( program, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		char log[2048];
		GLsizei n = 0;
		dgl_glGetProgramInfoLog( program, sizeof( log ) - 1, &n, log );
		log[( n > 0 && n < (GLsizei)sizeof( log )) ? n : 0] = 0;
		DLOG( "!! program link failed: %s", log );
	}
}
