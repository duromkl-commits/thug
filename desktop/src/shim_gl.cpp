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
static void cadence_vsync( void );
static void cadence_attendre( void );
static void tampons_envoyer( void );

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
	DLOG( "GL_VENDOR=%s", (const char *)glGetString( GL_VENDOR ));
	DLOG( "GL_RENDERER=%s", (const char *)glGetString( GL_RENDERER ));
	DLOG( "GL_VERSION=%s", (const char *)glGetString( GL_VERSION ));
	cadence_vsync();
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
		// 4:3 (the game drawn like the PS2/Xbox originals, its 960x544 Vita
		// coordinates stretched to 4:3) or the Vita's own shape.
		const int aw = desktop_ecran_43() ? 4 : VIRT_W, ah = desktop_ecran_43() ? 3 : VIRT_H;
		s_rt_h = dh;
		s_rt_w = ( dh * aw + ah / 2 ) / ah;
		if( s_rt_w > dw ) { s_rt_w = dw; s_rt_h = ( dw * ah + aw / 2 ) / aw; }
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
static GLint  s_gamma_loc_k = -1, s_gamma_loc_zoom = -1, s_gamma_loc_tram = -1, s_gamma_loc_grille = -1;
static bool   s_gamma_echec = false;

static bool s_letterbox = false;

extern "C" void desktop_set_letterbox( int on )
{
	s_letterbox = on != 0;
}

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
		"uniform vec2 zoom;\n"		// overscan: the picture shrunk inside a black border
		"uniform float tram;\n"		// 1 = PS2 dither
		"uniform vec2 grille;\n"		// PS2 pixels across the picture
		"varying vec2 t;\n"
		// The GS dither matrix (DIMX, the value games load): added to the
		// 8-bit colour before it drops to 5 bits in a 16-bit frame buffer.
		"float dimx(vec2 p) {\n"
		"	int i = int(mod(p.x, 4.0)) + 4 * int(mod(p.y, 4.0));\n"
		"	float m[16];\n"
		"	m[0] = -4.0; m[1] = 0.0; m[2] = -3.0; m[3] = 1.0;\n"
		"	m[4] = 2.0; m[5] = -2.0; m[6] = 3.0; m[7] = -1.0;\n"
		"	m[8] = -3.0; m[9] = 1.0; m[10] = -4.0; m[11] = 0.0;\n"
		"	m[12] = 3.0; m[13] = -1.0; m[14] = 2.0; m[15] = -2.0;\n"
		"	float v = 0.0;\n"
		"	for (int j = 0; j < 16; ++j) if (j == i) v = m[j];\n"
		"	return v;\n"
		"}\n"
		"void main() {\n"
		"	vec2 u = 0.5 + (t - 0.5) * zoom;\n"
		"	if (u.x < 0.0 || u.x > 1.0 || u.y < 0.0 || u.y > 1.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
		"	vec3 c = texture2D(img, u).rgb;\n"
		"	vec3 o = floor(256.0 * pow(c * (255.0 / 256.0), k) + 0.001);\n"
		"	o = min(o, 255.0);\n"
		"	if (tram > 0.5) {\n"
		"		vec2 p = floor(u * grille);\n"
		"		o = floor(clamp(o + dimx(p), 0.0, 255.0) / 8.0) * 8.0;\n"
		"		o = o + o / 32.0;\n"	// 5 bits back to the full 0-255 range
		"	}\n"
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
	s_gamma_loc_zoom = glGetUniformLocation( p, "zoom" );
	s_gamma_loc_tram = glGetUniformLocation( p, "tram" );
	s_gamma_loc_grille = glGetUniformLocation( p, "grille" );
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
	{
		const DesktopConfig &cfg = desktop_config();
		float os = cfg.overscan < 0.0f ? 0.0f : cfg.overscan > 0.15f ? 0.15f : cfg.overscan;
		// Like the PS2 on a capture card (Marcus's footage): black bars of
		// about 4% of the width each side and a bit less top and bottom.
		glUniform2f( s_gamma_loc_zoom, 1.0f / ( 1.0f - 2.0f * os ), 1.0f / ( 1.0f - 1.6f * os ));
		glUniform1f( s_gamma_loc_tram, cfg.ps2_dither ? 1.0f : 0.0f );
		// 640x448 like the PS2 game in 4:3; the same pixel size across a
		// wider picture.
		glUniform2f( s_gamma_loc_grille, 448.0f * w / (float)h, 448.0f );
	}
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
	tampons_envoyer();
	GLboolean scissor = glIsEnabled( GL_SCISSOR_TEST );
	if( scissor ) glDisable( GL_SCISSOR_TEST );

	int dw = desktop_window_width(), dh = desktop_window_height();
	// Fit, centred, keep the aspect ratio of the render target.
	int w = dw, h = ( dw * s_rt_h ) / s_rt_w;
	if( h > dh ) { h = dh; w = ( dh * s_rt_w ) / s_rt_h; }
	int x = ( dw - w ) / 2, y = ( dh - h ) / 2;
	// Cutscene letterbox on a 4:3 picture: the game renders 16:9 (aspect and
	// angle set by screen_setup_letterbox) and, like the PS2, the whole frame
	// is squeezed into a 16:9 band with black bars.
	if( s_letterbox && desktop_ecran_43())
	{
		const int nh = h * 3 / 4;
		y += ( h - nh ) / 2;
		h = nh;
	}

	dgl_glBindFramebuffer_real( GL_READ_FRAMEBUFFER, s_fbo );
	dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, 0 );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT );
	const DesktopConfig &cfg = desktop_config();
	const bool gamma = ( s_gamma_k[0] != 1.0f || s_gamma_k[1] != 1.0f || s_gamma_k[2] != 1.0f
	                     || cfg.ps2_dither || cfg.overscan > 0.0f ) && gamma_prog();
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

	cadence_attendre();
	SDL_GL_SwapWindow( s_window );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_bound_fb ? s_bound_fb : s_fbo );
	if( scissor ) glEnable( GL_SCISSOR_TEST );
	desktop_pump_events();
}

// Frame pacing. The game's logic advances one step per displayed frame and
// expects the Vita's 60 Hz (or 30 with swap interval 2). On a 144 Hz monitor
// vsync alone would run it 2.4x too fast, so frames are paced by the clock;
// vsync is kept only when the refresh rate is a multiple of the target rate.
static bool   s_vsync_rythme = false;	// vsync paces the frames itself
static int    s_intervalle = 1;		// Vita vsyncs per frame (1 = 60 fps, 2 = 30)
static Uint64 s_prochaine = 0;		// perf-counter time the next frame may show

static void cadence_vsync( void )
{
	const DesktopConfig &cfg = desktop_config();
	int hz = 0;
	SDL_DisplayMode m;
	if( s_window && SDL_GetCurrentDisplayMode( SDL_GetWindowDisplayIndex( s_window ), &m ) == 0 )
		hz = m.refresh_rate;
	const int fps = 60 / s_intervalle;
	s_vsync_rythme = cfg.vsync && hz > 0 && ( hz % fps ) == 0
	                 && SDL_GL_SetSwapInterval( hz / fps ) == 0;
	if( !s_vsync_rythme )
		SDL_GL_SetSwapInterval( 0 );
	DLOG( "frame pacing: %d fps, display %d Hz, vsync %s", fps, hz,
	      s_vsync_rythme ? "on" : "off (timer)" );
}

static void cadence_attendre( void )
{
	if( s_vsync_rythme )
		return;
	const Uint64 f = SDL_GetPerformanceFrequency();
	const Uint64 periode = f * s_intervalle / 60;
	Uint64 t = SDL_GetPerformanceCounter();
	if( s_prochaine == 0 || t > s_prochaine + periode * 4 )
		s_prochaine = t;			// first frame, or far behind (loading): restart
	while( t < s_prochaine )
	{
		const Uint64 reste_ms = ( s_prochaine - t ) * 1000 / f;
		if( reste_ms > 2 )
			SDL_Delay( (Uint32)( reste_ms - 1 ));
		t = SDL_GetPerformanceCounter();
	}
	s_prochaine += periode;
}

extern "C" void desktop_swap_interval( int n )
{
	s_intervalle = ( n >= 2 ) ? 2 : 1;
	cadence_vsync();
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

// -- CPU copies of buffers (see desktop_buffer_data in vitaGL.h) -------------------
struct SCopieTampon
{
	std::vector<unsigned char> octets;
	bool sale;			// handed out since the last upload: may have been written
};
static std::map<GLuint, SCopieTampon> s_copies;
static std::vector<GLuint> s_sales;

static GLuint tampon_lie( GLenum target )
{
	GLint n = 0;
	glGetIntegerv( target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_ARRAY_BUFFER_BINDING, &n );
	return (GLuint)n;
}

extern "C" unsigned char *desktop_buffer_data( GLuint name )
{
	if( !name )
		return NULL;
	std::map<GLuint, SCopieTampon>::iterator it = s_copies.find( name );
	if( it == s_copies.end())
	{
		GLint avant = 0, taille = 0;
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &avant );
		glBindBuffer( GL_ARRAY_BUFFER, name );
		glGetBufferParameteriv( GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &taille );
		SCopieTampon &c = s_copies[name];
		c.sale = false;
		if( taille > 0 )
		{
			c.octets.resize( (size_t)taille );
			glGetBufferSubData( GL_ARRAY_BUFFER, 0, taille, &c.octets[0] );
		}
		glBindBuffer( GL_ARRAY_BUFFER, (GLuint)avant );
		it = s_copies.find( name );
	}
	if( it->second.octets.empty())
		return NULL;
	if( !it->second.sale )
	{
		it->second.sale = true;
		s_sales.push_back( name );
	}
	return &it->second.octets[0];
}

extern "C" void desktop_glBufferData( GLenum target, GLsizeiptr size, const void *data, GLenum usage )
{
	glBufferData( target, size, data, usage );
	if( s_copies.empty())
		return;
	std::map<GLuint, SCopieTampon>::iterator it = s_copies.find( tampon_lie( target ));
	if( it == s_copies.end())
		return;
	it->second.octets.assign( (size_t)size, 0 );
	if( data && size > 0 )
		memcpy( &it->second.octets[0], data, (size_t)size );
}

extern "C" void desktop_glBufferSubData( GLenum target, GLintptr offset, GLsizeiptr size, const void *data )
{
	glBufferSubData( target, offset, size, data );
	if( s_copies.empty())
		return;
	std::map<GLuint, SCopieTampon>::iterator it = s_copies.find( tampon_lie( target ));
	if( it != s_copies.end() && (size_t)( offset + size ) <= it->second.octets.size())
		memcpy( &it->second.octets[(size_t)offset], data, (size_t)size );
}

extern "C" void desktop_glDeleteBuffers( GLsizei n, const GLuint *buffers )
{
	for( GLsizei i = 0; i < n; ++i )
		s_copies.erase( buffers[i] );
	glDeleteBuffers( n, buffers );
}

// End of frame: copies handed out this frame go back to the GPU.
static void tampons_envoyer( void )
{
	if( s_sales.empty())
		return;
	GLint avant = 0;
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &avant );
	for( size_t i = 0; i < s_sales.size(); ++i )
	{
		std::map<GLuint, SCopieTampon>::iterator it = s_copies.find( s_sales[i] );
		if( it == s_copies.end())
			continue;
		it->second.sale = false;
		if( it->second.octets.empty())
			continue;
		glBindBuffer( GL_ARRAY_BUFFER, it->first );
		glBufferSubData( GL_ARRAY_BUFFER, 0, (GLsizeiptr)it->second.octets.size(), &it->second.octets[0] );
	}
	s_sales.clear();
	glBindBuffer( GL_ARRAY_BUFFER, (GLuint)avant );
}

// ---------------------------------------------------------------------------
// SSAO: ambient occlusion from the depth buffer, multiplied onto the 3D image
// after the world's opaque pass (before translucents and the 2D). The world
// projection comes from the engine (desktop_ssao_projection); AO is computed
// at half size and blurred with depth-aware weights when it is applied.
// ---------------------------------------------------------------------------

static float  s_proj[4] = { 0, 0, 4.0f, 100000.0f };	// tanH, tanV, near, far
static bool   s_proj_ok = false;
static GLuint s_ao_dep_tex = 0, s_ao_dep_fbo = 0, s_ao_tex = 0, s_ao_fbo = 0;
static GLuint s_ao_prog = 0, s_ao_mix = 0;
static GLint  s_ao_l_proj = -1, s_ao_l_par = -1, s_ao_l_tx = -1;
static GLint  s_mix_l_proj = -1, s_mix_l_hx = -1;
static int    s_ao_etat = 0;		// 0 not tried, 1 ready, -1 off

extern "C" void desktop_ssao_projection( float f, float aspect, float znear, float zfar )
{
	if( f <= 0.0f || aspect <= 0.0f )
		return;
	s_proj[0] = aspect / f;
	s_proj[1] = 1.0f / f;
	s_proj[2] = znear;
	s_proj[3] = zfar;
	s_proj_ok = true;
}

#define AO_GLSL_COMMUN \
	"#version 120\n" \
	"uniform sampler2D dep;\n" \
	"uniform vec4 proj;\n" \
	"varying vec2 t;\n" \
	"float lin(float d) { float z = d * 2.0 - 1.0;\n" \
	"	return 2.0 * proj.z * proj.w / ((proj.w + proj.z) - z * (proj.w - proj.z)); }\n" \
	"vec3 pos(vec2 uv) { float z = lin(texture2D(dep, uv).r);\n" \
	"	return vec3((uv * 2.0 - 1.0) * proj.xy * z, -z); }\n"

static const char *s_ao_vs =
	"#version 120\n"
	"varying vec2 t;\n"
	"void main() { t = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";

static const char *s_ao_fs =
	AO_GLSL_COMMUN
	"uniform vec3 par;\n"		// radius, strength, fade distance
	"uniform vec2 tx;\n"		// one full-size texel
	"void main() {\n"
	"	float d = texture2D(dep, t).r;\n"
	"	vec3 P = pos(t);\n"
	"	float z = -P.z;\n"
	"	if (d >= 0.99999 || z > par.z) { gl_FragColor = vec4(1.0); return; }\n"
	// Normal from the neighbours on the nearer side (no smearing across edges),
	// 3 texels away: one texel apart, depth steps on ground seen at a low
	// angle tilt the normal row by row -- horizontal bands of false occlusion.
	"	vec2 e = 3.0 * tx;\n"
	"	vec3 pr = pos(t + vec2(e.x, 0.0)) - P, pl = P - pos(t - vec2(e.x, 0.0));\n"
	"	vec3 pu = pos(t + vec2(0.0, e.y)) - P, pd = P - pos(t - vec2(0.0, e.y));\n"
	"	vec3 dx = abs(pr.z) < abs(pl.z) ? pr : pl;\n"
	"	vec3 dy = abs(pu.z) < abs(pd.z) ? pu : pd;\n"
	"	vec3 N = normalize(cross(dx, dy));\n"
	"	if (dot(N, P) > 0.0) N = -N;\n"
	"	vec2 r = min(vec2(0.15), 0.5 * par.x / (z * proj.xy));\n"
	"	float a = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));\n"
	"	float occ = 0.0;\n"
	"	for (int i = 0; i < 12; ++i) {\n"
	"		float k = (float(i) + 0.5) / 12.0;\n"
	"		float g = a + float(i) * 2.3999632;\n"
	"		vec3 v = pos(t + vec2(cos(g), sin(g)) * r * sqrt(k)) - P;\n"
	"		float l = length(v) + 0.001;\n"
	"		occ += max(0.0, (dot(v, N) - 0.001 * z) / l - 0.1) * (1.0 - smoothstep(0.6 * par.x, par.x, l));\n"
	"	}\n"
	"	float ao = 1.0 - par.y * min(1.0, occ / 3.0);\n"
	"	ao = mix(ao, 1.0, smoothstep(0.6 * par.z, par.z, z));\n"
	"	gl_FragColor = vec4(vec3(clamp(ao, 0.0, 1.0)), 1.0);\n"
	"}\n";

static const char *s_mix_fs =
	AO_GLSL_COMMUN
	"uniform sampler2D ao;\n"
	"uniform vec2 hx;\n"		// one half-size texel
	"void main() {\n"
	"	float zc = lin(texture2D(dep, t).r);\n"
	"	float s = 0.0, w = 0.0;\n"
	"	for (int j = -2; j <= 2; ++j)\n"
	"		for (int i = -2; i <= 2; ++i) {\n"
	"			vec2 o = vec2(float(i), float(j)) * hx;\n"
	"			float zs = lin(texture2D(dep, t + o).r);\n"
	"			float k = exp(-abs(zs - zc) / (0.03 * zc));\n"
	"			s += texture2D(ao, t + o).r * k;\n"
	"			w += k;\n"
	"		}\n"
	"	float a = s / max(w, 0.0001);\n"
	"	gl_FragColor = vec4(a, a, a, 1.0);\n"
	"}\n";

static GLuint ao_lier( const char *vs, const char *fs, const char *nom )
{
	GLuint sh[2] = { dgl_glCreateShader_real( GL_VERTEX_SHADER ), dgl_glCreateShader_real( GL_FRAGMENT_SHADER ) };
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	for( int i = 0; i < 2; ++i )
	{
		dgl_glShaderSource_real( sh[i], 1, &src[i], NULL );
		glCompileShader( sh[i] );
		GLint ok = 0;
		glGetShaderiv( sh[i], GL_COMPILE_STATUS, &ok );
		if( !ok )
		{
			char log[1024] = "";
			glGetShaderInfoLog( sh[i], sizeof( log ), NULL, log );
			DLOG( "!! SSAO %s shader: %s", nom, log );
			return 0;
		}
		glAttachShader( p, sh[i] );
	}
	dgl_glLinkProgram( p );
	GLint ok = 0;
	glGetProgramiv( p, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		DLOG( "!! SSAO %s program refused", nom );
		return 0;
	}
	return p;
}

// Single-sample copy of the scene depth, shared by SSAO and the sun shadows.
static int s_dep_etat = 0;		// 0 not tried, 1 ready, -1 failed

static bool dep_pret( void )
{
	if( s_dep_etat )
		return s_dep_etat > 0;
	s_dep_etat = -1;
	GLint tex = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex );
	glGenTextures( 1, &s_ao_dep_tex );
	glBindTexture( GL_TEXTURE_2D, s_ao_dep_tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, s_rt_w, s_rt_h, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL );
	glBindTexture( GL_TEXTURE_2D, tex );
	dgl_glGenFramebuffers( 1, &s_ao_dep_fbo );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_ao_dep_fbo );
	dgl_glFramebufferTexture2D( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_ao_dep_tex, 0 );
	glDrawBuffer( GL_NONE );
	glReadBuffer( GL_NONE );
	const GLenum st = dgl_glCheckFramebufferStatus( GL_FRAMEBUFFER );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
	if( st != GL_FRAMEBUFFER_COMPLETE )
	{
		DLOG( "!! depth copy target incomplete (0x%x)", (unsigned)st );
		return false;
	}
	s_dep_etat = 1;
	return true;
}

// Depth copy (resolves MSAA: one sample per pixel). Leaves s_fbo bound.
static void copier_profondeur( void )
{
	dgl_glBindFramebuffer_real( GL_READ_FRAMEBUFFER, s_fbo );
	dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, s_ao_dep_fbo );
	dgl_glBlitFramebuffer( 0, 0, s_rt_w, s_rt_h, 0, 0, s_rt_w, s_rt_h, GL_DEPTH_BUFFER_BIT, GL_NEAREST );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
}

static bool ao_pret( void )
{
	if( s_ao_etat )
		return s_ao_etat > 0;
	s_ao_etat = -1;
	const DesktopConfig &cfg = desktop_config();
	if( !cfg.ssao )
		return false;

	s_ao_prog = ao_lier( s_ao_vs, s_ao_fs, "occlusion" );
	s_ao_mix  = ao_lier( s_ao_vs, s_mix_fs, "blur" );
	if( !s_ao_prog || !s_ao_mix )
		return false;
	GLint avant = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &avant );
	glUseProgram( s_ao_prog );
	glUniform1i( glGetUniformLocation( s_ao_prog, "dep" ), 0 );
	s_ao_l_proj = glGetUniformLocation( s_ao_prog, "proj" );
	s_ao_l_par  = glGetUniformLocation( s_ao_prog, "par" );
	s_ao_l_tx   = glGetUniformLocation( s_ao_prog, "tx" );
	glUseProgram( s_ao_mix );
	glUniform1i( glGetUniformLocation( s_ao_mix, "dep" ), 0 );
	glUniform1i( glGetUniformLocation( s_ao_mix, "ao" ), 1 );
	s_mix_l_proj = glGetUniformLocation( s_ao_mix, "proj" );
	s_mix_l_hx   = glGetUniformLocation( s_ao_mix, "hx" );
	glUseProgram( avant );

	if( !dep_pret())
		return false;
	GLint tex = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex );
	glGenTextures( 1, &s_ao_tex );
	glBindTexture( GL_TEXTURE_2D, s_ao_tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, ( s_rt_w + 1 ) / 2, ( s_rt_h + 1 ) / 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	glBindTexture( GL_TEXTURE_2D, tex );

	const GLenum st1 = GL_FRAMEBUFFER_COMPLETE;
	dgl_glGenFramebuffers( 1, &s_ao_fbo );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_ao_fbo );
	dgl_glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_ao_tex, 0 );
	const GLenum st2 = dgl_glCheckFramebufferStatus( GL_FRAMEBUFFER );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
	if( st1 != GL_FRAMEBUFFER_COMPLETE || st2 != GL_FRAMEBUFFER_COMPLETE )
	{
		DLOG( "!! SSAO targets incomplete (0x%x, 0x%x), SSAO off", (unsigned)st1, (unsigned)st2 );
		return false;
	}
	DLOG( "SSAO on: strength %.2f, radius %.0f", cfg.ssao_strength, cfg.ssao_radius );
	s_ao_etat = 1;
	return true;
}

static void ao_quad( void )
{
	glBegin( GL_TRIANGLE_STRIP );
	glTexCoord2f( 0, 0 ); glVertex2f( -1, -1 );
	glTexCoord2f( 1, 0 ); glVertex2f(  1, -1 );
	glTexCoord2f( 0, 1 ); glVertex2f( -1,  1 );
	glTexCoord2f( 1, 1 ); glVertex2f(  1,  1 );
	glEnd();
}

extern "C" void desktop_ssao( void )
{
	// Full-screen 3D only (split screen draws the world once per viewport).
	static unsigned s_n_ok = 0, s_n_saut[4] = { 0, 0, 0, 0 };
	const int saut = !s_window || !on_screen() ? 1 : !s_proj_ok ? 2
	               : ( s_vp[0] != 0 || s_vp[1] != 0 || s_vp[2] < VIRT_W || s_vp[3] < VIRT_H ) ? 3 : 0;
	if(( s_n_ok + s_n_saut[1] + s_n_saut[2] + s_n_saut[3] ) % 1800 == 1799 )
		DLOG( "SSAO: %u frames applied, skipped %u offscreen / %u no projection / %u viewport (last %d,%d %dx%d)",
		      s_n_ok, s_n_saut[1], s_n_saut[2], s_n_saut[3], s_vp[0], s_vp[1], s_vp[2], s_vp[3] );
	if( saut )
	{
		++s_n_saut[saut];
		return;
	}
	if( !ao_pret())
		return;
	++s_n_ok;
	const DesktopConfig &cfg = desktop_config();
	const int hw = ( s_rt_w + 1 ) / 2, hh = ( s_rt_h + 1 ) / 2;

	// Back on the screen target BEFORE glPushAttrib (copier_profondeur does
	// it): GL_COLOR_BUFFER_BIT saves the bound framebuffer's draw buffer, and
	// the depth copy's is GL_NONE -- popping that onto the screen target
	// stopped all drawing to the screen.
	copier_profondeur();

	GLint prog = 0, act = 0, tex0 = 0, tex1 = 0, buf = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog );
	glGetIntegerv( GL_ACTIVE_TEXTURE, &act );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buf );
	glActiveTexture( GL_TEXTURE1 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex1 );
	glActiveTexture( GL_TEXTURE0 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex0 );
	glPushAttrib( GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_VIEWPORT_BIT | GL_SCISSOR_BIT );
	glDisable( GL_DEPTH_TEST );
	glDepthMask( GL_FALSE );
	glDisable( GL_CULL_FACE );
	glDisable( GL_ALPHA_TEST );
	glDisable( GL_STENCIL_TEST );
	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_BLEND );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );

	// Occlusion, half size.
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_ao_fbo );
	glViewport( 0, 0, hw, hh );
	glUseProgram( s_ao_prog );
	glUniform4f( s_ao_l_proj, s_proj[0], s_proj[1], s_proj[2], s_proj[3] );
	glUniform3f( s_ao_l_par, cfg.ssao_radius * 1.75f, cfg.ssao_strength, 6000.0f );
	glUniform2f( s_ao_l_tx, 1.0f / s_rt_w, 1.0f / s_rt_h );
	glBindTexture( GL_TEXTURE_2D, s_ao_dep_tex );
	ao_quad();

	// Blur and multiply onto the image.
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
	glViewport( 0, 0, s_rt_w, s_rt_h );
	// ssao=2 in the ini: show the occlusion itself (debug view).
	if( cfg.ssao != 2 )
	{
		glEnable( GL_BLEND );
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ZERO, GL_SRC_COLOR );
	}
	glUseProgram( s_ao_mix );
	glUniform4f( s_mix_l_proj, s_proj[0], s_proj[1], s_proj[2], s_proj[3] );
	glUniform2f( s_mix_l_hx, 1.0f / hw, 1.0f / hh );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, s_ao_tex );
	glActiveTexture( GL_TEXTURE0 );
	ao_quad();

	glPopAttrib();
	glBindBuffer( GL_ARRAY_BUFFER, buf );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, tex1 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, tex0 );
	glActiveTexture( act );
	glUseProgram( prog );
	appliquer_vp();
	appliquer_sc();
}

// ---------------------------------------------------------------------------
// Distance haze and bloom, on the 3D image before the 2D (called right after
// desktop_ssao). The haze takes the sky's colour: the average of the pixels
// with no geometry, smoothed over time and kept when no sky is in view.
// ---------------------------------------------------------------------------

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
static const int PP_NIV = 5;			// bloom levels: 1/2 ... 1/32
static GLuint s_pp_tex[PP_NIV], s_pp_fbo[PP_NIV];
static int    s_pp_w[PP_NIV], s_pp_h[PP_NIV];
static GLuint s_pp_ciel_tex = 0, s_pp_ciel_fbo = 0, s_pp_moy_tex[2] = { 0, 0 }, s_pp_moy_fbo[2] = { 0, 0 };
static int    s_pp_moy = 0;
static GLuint s_pp_p_ciel = 0, s_pp_p_moy = 0, s_pp_p_fog = 0, s_pp_p_pre = 0, s_pp_p_down = 0, s_pp_p_up = 0;
static int    s_pp_etat = 0;

static const char *s_pp_ciel_fs =
	"#version 120\n"
	"uniform sampler2D img;\n"
	"uniform sampler2D dep;\n"
	"varying vec2 t;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(dep, t).r >= 0.99999 ? vec4(texture2D(img, t).rgb, 1.0) : vec4(0.0);\n"
	"}\n";

static const char *s_pp_moy_fs =
	"#version 130\n"
	"uniform sampler2D ciel;\n"
	"uniform sampler2D prec;\n"
	"in vec2 t;\n"
	"void main() {\n"
	"	vec4 s = textureLod(ciel, vec2(0.5), 6.0);\n"
	"	vec4 p = texture(prec, vec2(0.5));\n"
	"	if (s.a < 0.02) { gl_FragColor = p; return; }\n"
	"	vec3 c = s.rgb / s.a;\n"
	"	gl_FragColor = vec4(p.a < 0.5 ? c : mix(p.rgb, c, 0.05), 1.0);\n"
	"}\n";

static const char *s_pp_fog_fs =
	AO_GLSL_COMMUN
	"uniform sampler2D moy;\n"
	"uniform vec2 par;\n"			// strength, half distance
	"void main() {\n"
	"	float d = texture2D(dep, t).r;\n"
	"	if (d >= 0.99999) discard;\n"
	"	vec4 m = texture2D(moy, vec2(0.5));\n"
	"	float f = par.x * (1.0 - exp2(-length(pos(t)) / par.y));\n"
	"	gl_FragColor = vec4(m.rgb, f * m.a);\n"
	"}\n";

static const char *s_pp_pre_fs =
	"#version 120\n"
	"uniform sampler2D src;\n"
	"uniform vec2 hx;\n"			// one source texel
	"uniform float seuil;\n"
	"varying vec2 t;\n"
	"void main() {\n"
	"	vec3 c = 0.25 * (texture2D(src, t + vec2(-hx.x, -hx.y)).rgb + texture2D(src, t + vec2(hx.x, -hx.y)).rgb\n"
	"	               + texture2D(src, t + vec2(-hx.x, hx.y)).rgb + texture2D(src, t + vec2(hx.x, hx.y)).rgb);\n"
	"	float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
	"	gl_FragColor = vec4(c * smoothstep(seuil, seuil + 0.25, l), 1.0);\n"
	"}\n";

static const char *s_pp_down_fs =
	"#version 120\n"
	"uniform sampler2D src;\n"
	"uniform vec2 hx;\n"
	"varying vec2 t;\n"
	"void main() {\n"
	"	vec3 c = texture2D(src, t).rgb * 4.0\n"
	"	       + texture2D(src, t + vec2(-hx.x, -hx.y)).rgb + texture2D(src, t + vec2(hx.x, -hx.y)).rgb\n"
	"	       + texture2D(src, t + vec2(-hx.x, hx.y)).rgb + texture2D(src, t + vec2(hx.x, hx.y)).rgb;\n"
	"	gl_FragColor = vec4(c / 8.0, 1.0);\n"
	"}\n";

static const char *s_pp_up_fs =
	"#version 120\n"
	"uniform sampler2D src;\n"
	"uniform vec2 hx;\n"
	"uniform float k;\n"
	"varying vec2 t;\n"
	"void main() {\n"
	"	vec3 c = texture2D(src, t + vec2(-2.0 * hx.x, 0.0)).rgb + texture2D(src, t + vec2(2.0 * hx.x, 0.0)).rgb\n"
	"	       + texture2D(src, t + vec2(0.0, -2.0 * hx.y)).rgb + texture2D(src, t + vec2(0.0, 2.0 * hx.y)).rgb\n"
	"	       + 2.0 * (texture2D(src, t + vec2(-hx.x, hx.y)).rgb + texture2D(src, t + vec2(hx.x, hx.y)).rgb\n"
	"	              + texture2D(src, t + vec2(hx.x, -hx.y)).rgb + texture2D(src, t + vec2(-hx.x, -hx.y)).rgb);\n"
	"	gl_FragColor = vec4(c / 12.0 * k, 1.0);\n"
	"}\n";

static GLuint s_pp_p_dof = 0;
static const char *s_pp_dof_fs =
	AO_GLSL_COMMUN
	"uniform sampler2D flou;\n"
	"uniform vec2 hx;\n"			// one quarter-size texel
	"uniform float force;\n"
	"void main() {\n"
	// Focus: the nearest of 9 points around the middle of the screen, where
	// the camera keeps the skater.
	"	float f = 1e9;\n"
	"	for (int j = 0; j < 3; ++j)\n"
	"		for (int i = 0; i < 3; ++i)\n"
	"			f = min(f, lin(texture2D(dep, vec2(0.44 + 0.06 * float(i), 0.38 + 0.08 * float(j))).r));\n"
	"	f = clamp(f, 120.0, 3000.0);\n"
	"	float z = lin(texture2D(dep, t).r);\n"
	"	float c = force * smoothstep(f * 2.5, f * 8.0, z);\n"
	"	if (c < 0.004) discard;\n"
	"	vec3 b = texture2D(flou, t + vec2(-2.0 * hx.x, 0.0)).rgb + texture2D(flou, t + vec2(2.0 * hx.x, 0.0)).rgb\n"
	"	       + texture2D(flou, t + vec2(0.0, -2.0 * hx.y)).rgb + texture2D(flou, t + vec2(0.0, 2.0 * hx.y)).rgb\n"
	"	       + 2.0 * (texture2D(flou, t + vec2(-hx.x, hx.y)).rgb + texture2D(flou, t + vec2(hx.x, hx.y)).rgb\n"
	"	              + texture2D(flou, t + vec2(hx.x, -hx.y)).rgb + texture2D(flou, t + vec2(-hx.x, -hx.y)).rgb);\n"
	"	gl_FragColor = vec4(b / 12.0, c);\n"
	"}\n";

static GLuint pp_cible( GLuint *fbo, int w, int h, bool mips )
{
	GLuint tex = 0;
	glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL );
	if( mips )
		dgl_glGenerateMipmap( GL_TEXTURE_2D );
	dgl_glGenFramebuffers( 1, fbo );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, *fbo );
	dgl_glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0 );
	if( dgl_glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		return 0;
	glClearColor( 0, 0, 0, 0 );
	glClear( GL_COLOR_BUFFER_BIT );
	return tex;
}

static void pp_echantillons( GLuint p, const char *a, int ua, const char *b, int ub )
{
	GLint avant = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &avant );
	glUseProgram( p );
	if( a ) glUniform1i( glGetUniformLocation( p, a ), ua );
	if( b ) glUniform1i( glGetUniformLocation( p, b ), ub );
	glUseProgram( avant );
}

static bool pp_pret( void )
{
	if( s_pp_etat )
		return s_pp_etat > 0;
	s_pp_etat = -1;
	if( !dep_pret())
		return false;
	s_pp_p_ciel = ao_lier( s_ao_vs, s_pp_ciel_fs, "sky average" );
	s_pp_p_moy  = ao_lier( s_ao_vs, s_pp_moy_fs, "sky smoothing" );
	s_pp_p_fog  = ao_lier( s_ao_vs, s_pp_fog_fs, "haze" );
	s_pp_p_pre  = ao_lier( s_ao_vs, s_pp_pre_fs, "bloom bright pass" );
	s_pp_p_down = ao_lier( s_ao_vs, s_pp_down_fs, "bloom down" );
	s_pp_p_up   = ao_lier( s_ao_vs, s_pp_up_fs, "bloom up" );
	s_pp_p_dof  = ao_lier( s_ao_vs, s_pp_dof_fs, "depth of field" );
	if( !s_pp_p_ciel || !s_pp_p_moy || !s_pp_p_fog || !s_pp_p_pre || !s_pp_p_down || !s_pp_p_up || !s_pp_p_dof )
		return false;
	pp_echantillons( s_pp_p_dof, "dep", 0, "flou", 1 );
	pp_echantillons( s_pp_p_ciel, "img", 1, "dep", 0 );
	pp_echantillons( s_pp_p_moy, "ciel", 0, "prec", 1 );
	pp_echantillons( s_pp_p_fog, "dep", 0, "moy", 1 );
	pp_echantillons( s_pp_p_pre, "src", 0, NULL, 0 );
	pp_echantillons( s_pp_p_down, "src", 0, NULL, 0 );
	pp_echantillons( s_pp_p_up, "src", 0, NULL, 0 );

	GLint tex = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex );
	bool ok = true;
	for( int i = 0; i < PP_NIV; ++i )
	{
		s_pp_w[i] = s_rt_w >> ( i + 1 ); if( s_pp_w[i] < 1 ) s_pp_w[i] = 1;
		s_pp_h[i] = s_rt_h >> ( i + 1 ); if( s_pp_h[i] < 1 ) s_pp_h[i] = 1;
		ok = ok && ( s_pp_tex[i] = pp_cible( &s_pp_fbo[i], s_pp_w[i], s_pp_h[i], false ));
	}
	ok = ok && ( s_pp_ciel_tex = pp_cible( &s_pp_ciel_fbo, 64, 64, true ));
	ok = ok && ( s_pp_moy_tex[0] = pp_cible( &s_pp_moy_fbo[0], 1, 1, false ));
	ok = ok && ( s_pp_moy_tex[1] = pp_cible( &s_pp_moy_fbo[1], 1, 1, false ));
	glBindTexture( GL_TEXTURE_2D, tex );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_bound_fb ? s_bound_fb : s_fbo );
	if( !ok )
	{
		DLOG( "!! bloom/haze targets incomplete, both off" );
		return false;
	}
	DLOG( "post effects on: bloom %d, haze %d, depth of field %d", desktop_config().bloom, desktop_config().fog,
	      desktop_config().dof );
	s_pp_etat = 1;
	return true;
}

static void pp_vers( GLuint fbo, int w, int h )
{
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, fbo );
	glViewport( 0, 0, w, h );
}

static void pp_resoudre( void )
{
	dgl_glBindFramebuffer_real( GL_READ_FRAMEBUFFER, s_fbo );
	dgl_glBindFramebuffer_real( GL_DRAW_FRAMEBUFFER, s_fbo_resolve );
	dgl_glBlitFramebuffer( 0, 0, s_rt_w, s_rt_h, 0, 0, s_rt_w, s_rt_h, GL_COLOR_BUFFER_BIT, GL_NEAREST );
	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
}

extern "C" void desktop_post( void )
{
	const DesktopConfig &cfg = desktop_config();
	const bool brume = cfg.fog && cfg.fog_strength > 0.0f && cfg.fog_distance > 1.0f;
	const bool halo = cfg.bloom && cfg.bloom_strength > 0.0f;
	const bool flou = cfg.dof && cfg.dof_strength > 0.0f;
	if(( !brume && !halo && !flou ) || !s_window || !on_screen() || !s_proj_ok ||
	    s_vp[0] != 0 || s_vp[1] != 0 || s_vp[2] < VIRT_W || s_vp[3] < VIRT_H || !pp_pret())
		return;

	GLint prog = 0, act = 0, tex0 = 0, tex1 = 0, buf = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &prog );
	glGetIntegerv( GL_ACTIVE_TEXTURE, &act );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buf );
	glActiveTexture( GL_TEXTURE1 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex1 );
	glActiveTexture( GL_TEXTURE0 );
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &tex0 );
	glPushAttrib( GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_VIEWPORT_BIT | GL_SCISSOR_BIT );
	glDisable( GL_DEPTH_TEST );
	glDepthMask( GL_FALSE );
	glDisable( GL_CULL_FACE );
	glDisable( GL_ALPHA_TEST );
	glDisable( GL_STENCIL_TEST );
	glDisable( GL_SCISSOR_TEST );
	glDisable( GL_BLEND );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBlendEquation( GL_FUNC_ADD );

	if( brume )
	{
		copier_profondeur();
		pp_resoudre();
		// Sky colour of this image, averaged down to one texel.
		pp_vers( s_pp_ciel_fbo, 64, 64 );
		glUseProgram( s_pp_p_ciel );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, s_tex_resolve );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, s_ao_dep_tex );
		ao_quad();
		glBindTexture( GL_TEXTURE_2D, s_pp_ciel_tex );
		dgl_glGenerateMipmap( GL_TEXTURE_2D );
		// Smoothed over time.
		const int src = s_pp_moy, dst = 1 - s_pp_moy;
		pp_vers( s_pp_moy_fbo[dst], 1, 1 );
		glUseProgram( s_pp_p_moy );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, s_pp_moy_tex[src] );
		glActiveTexture( GL_TEXTURE0 );
		ao_quad();
		s_pp_moy = dst;
		// Haze over the geometry.
		pp_vers( s_fbo, s_rt_w, s_rt_h );
		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glUseProgram( s_pp_p_fog );
		glUniform4f( glGetUniformLocation( s_pp_p_fog, "proj" ), s_proj[0], s_proj[1], s_proj[2], s_proj[3] );
		glUniform2f( glGetUniformLocation( s_pp_p_fog, "par" ),
		             cfg.fog_strength > 1.0f ? 1.0f : cfg.fog_strength, cfg.fog_distance );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, s_pp_moy_tex[s_pp_moy] );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, s_ao_dep_tex );
		ao_quad();
		glDisable( GL_BLEND );
	}

	if( flou )
	{
		// Depth of field: the image at a quarter size, blended back where it
		// lies well behind what's in the middle of the screen (the skater).
		if( !brume )
			copier_profondeur();
		pp_resoudre();
		glUseProgram( s_pp_p_down );
		const GLint l_dhx = glGetUniformLocation( s_pp_p_down, "hx" );
		pp_vers( s_pp_fbo[0], s_pp_w[0], s_pp_h[0] );
		glUniform2f( l_dhx, 1.0f / s_rt_w, 1.0f / s_rt_h );
		glBindTexture( GL_TEXTURE_2D, s_tex_resolve );
		ao_quad();
		pp_vers( s_pp_fbo[1], s_pp_w[1], s_pp_h[1] );
		glUniform2f( l_dhx, 1.0f / s_pp_w[0], 1.0f / s_pp_h[0] );
		glBindTexture( GL_TEXTURE_2D, s_pp_tex[0] );
		ao_quad();
		pp_vers( s_fbo, s_rt_w, s_rt_h );
		glEnable( GL_BLEND );
		glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		glUseProgram( s_pp_p_dof );
		glUniform4f( glGetUniformLocation( s_pp_p_dof, "proj" ), s_proj[0], s_proj[1], s_proj[2], s_proj[3] );
		glUniform2f( glGetUniformLocation( s_pp_p_dof, "hx" ), 1.0f / s_pp_w[1], 1.0f / s_pp_h[1] );
		glUniform1f( glGetUniformLocation( s_pp_p_dof, "force" ), cfg.dof_strength > 1.0f ? 1.0f : cfg.dof_strength );
		glActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, s_pp_tex[1] );
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, s_ao_dep_tex );
		ao_quad();
		glDisable( GL_BLEND );
	}

	if( halo )
	{
		pp_resoudre();
		// Bright parts, half size, then down to 1/32.
		pp_vers( s_pp_fbo[0], s_pp_w[0], s_pp_h[0] );
		glUseProgram( s_pp_p_pre );
		glUniform2f( glGetUniformLocation( s_pp_p_pre, "hx" ), 1.0f / s_rt_w, 1.0f / s_rt_h );
		glUniform1f( glGetUniformLocation( s_pp_p_pre, "seuil" ), cfg.bloom_threshold );
		glBindTexture( GL_TEXTURE_2D, s_tex_resolve );
		ao_quad();
		glUseProgram( s_pp_p_down );
		const GLint l_dhx = glGetUniformLocation( s_pp_p_down, "hx" );
		for( int i = 1; i < PP_NIV; ++i )
		{
			pp_vers( s_pp_fbo[i], s_pp_w[i], s_pp_h[i] );
			glUniform2f( l_dhx, 1.0f / s_pp_w[i - 1], 1.0f / s_pp_h[i - 1] );
			glBindTexture( GL_TEXTURE_2D, s_pp_tex[i - 1] );
			ao_quad();
		}
		// Back up, each level added onto the one above, the last onto the image.
		glUseProgram( s_pp_p_up );
		const GLint l_uhx = glGetUniformLocation( s_pp_p_up, "hx" ), l_k = glGetUniformLocation( s_pp_p_up, "k" );
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE );
		for( int i = PP_NIV - 1; i >= 0; --i )
		{
			if( i > 0 ) pp_vers( s_pp_fbo[i - 1], s_pp_w[i - 1], s_pp_h[i - 1] );
			else        pp_vers( s_fbo, s_rt_w, s_rt_h );
			glUniform2f( l_uhx, 1.0f / s_pp_w[i], 1.0f / s_pp_h[i] );
			glUniform1f( l_k, i > 0 ? 1.0f : ( cfg.bloom_strength > 1.0f ? 1.0f : cfg.bloom_strength ) / (float)PP_NIV );
			glBindTexture( GL_TEXTURE_2D, s_pp_tex[i] );
			ao_quad();
		}
		glDisable( GL_BLEND );
	}

	dgl_glBindFramebuffer_real( GL_FRAMEBUFFER, s_fbo );
	glPopAttrib();
	glBindBuffer( GL_ARRAY_BUFFER, buf );
	glActiveTexture( GL_TEXTURE1 );
	glBindTexture( GL_TEXTURE_2D, tex1 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, tex0 );
	glActiveTexture( act );
	glUseProgram( prog );
	appliquer_vp();
	appliquer_sc();
}
