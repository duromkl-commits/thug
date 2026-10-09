/*****************************************************************************
**  THUG desktop -- vitaGL shim                                             **
**  desktop/shim/vitaGL.h                                                   **
**                                                                          **
**  vitaGL exposes OpenGL (fixed function + shaders) plus a few vgl*        **
**  extras. Desktop OpenGL in a compatibility context runs the same calls  **
**  natively. Functions past OpenGL 1.1 are loaded at startup through      **
**  SDL_GL_GetProcAddress (Windows' opengl32.dll only exports 1.1), so      **
**  every gl* call past 1.1 goes through a pointer declared here.           **
**                                                                          **
**  Cg shaders (GL_CG_*_SHADER_EXT) are translated to GLSL inside           **
**  glShaderSource, see desktop/src/cg_to_glsl.cpp.                         **
*****************************************************************************/

#ifndef THUG_DESKTOP_VITAGL_H
#define THUG_DESKTOP_VITAGL_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

// SDL_opengl.h pulls GL 1.1 prototypes and SDL's copy of glext.h (types and
// PFN typedefs only, as GL_GLEXT_PROTOTYPES is left undefined).
#include <SDL_opengl.h>

#include "psp2_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

// -- vitaGL specific enums ---------------------------------------------------
#define GL_CG_VERTEX_SHADER_EXT   0x8B31A
#define GL_CG_FRAGMENT_SHADER_EXT 0x8B30A

typedef enum vglMemType {
	VGL_MEM_VRAM = 0,
	VGL_MEM_RAM = 1,
	VGL_MEM_SLOW = 2,
	VGL_MEM_PHYCONT = 2,
	VGL_MEM_BUDGET = 3,
	VGL_MEM_EXTERNAL = 4,
	VGL_MEM_ALL = 5
} vglMemType;

// -- vgl* extras ----------------------------------------------------------------
GLboolean vglInit( int legacy_pool_size );
GLboolean vglInitExtended( int legacy_pool_size, int width, int height, int ram_threshold, SceGxmMultisampleMode msaa );
void   vglSwapBuffers( GLboolean has_commondialog );
size_t vglMemFree( vglMemType type );
void   vglSetVertexBufferSize( uint32_t size );
void   vglSetFragmentBufferSize( uint32_t size );
void   vglSetVDMBufferSize( uint32_t size );
void   vglSetParamBufferSize( uint32_t size );
void   vglUseCachedMem( GLboolean use );
void  *vglGetTexDataPointer( GLenum target );
SceGxmTexture *vglGetGxmTexture( GLenum target );
void   vglGetShaderBinary( GLuint handle, GLsizei bufSize, GLsizei *length, void *binary );
void   vglLazyFree( void *addr );
void  *vglReserveVertexUniformBuffer( void *p, unsigned int size );

// eglSwapInterval (vitaGL's EGL subset): frames per vsync.
void desktop_swap_interval( int n );
#define eglSwapInterval( dpy, n ) desktop_swap_interval( n )

// Desktop window / render size (set by vglInitExtended from desktop.ini).
int  desktop_window_width( void );
int  desktop_window_height( void );
// Render target size the game draws at; 960x544 on Vita.
int  desktop_render_width( void );
int  desktop_render_height( void );
// Loads every pointer below. Called by vglInitExtended once a context exists.
int  desktop_gl_load( void );

// Compatibility-only entry point missing from SDL's glext.h.
typedef void (APIENTRYP DESKTOP_PFNGLCLIENTACTIVETEXTUREPROC)( GLenum texture );

// -- Functions past GL 1.1, loaded at runtime ------------------------------------
#define DESKTOP_GL_FUNCS(X) \
	X( PFNGLACTIVETEXTUREPROC,            glActiveTexture ) \
	X( DESKTOP_PFNGLCLIENTACTIVETEXTUREPROC, glClientActiveTexture ) \
	X( PFNGLBLENDEQUATIONPROC,            glBlendEquation ) \
	X( PFNGLCOMPRESSEDTEXIMAGE2DPROC,     glCompressedTexImage2D ) \
	X( PFNGLDRAWRANGEELEMENTSPROC,        glDrawRangeElements ) \
	X( PFNGLBINDBUFFERPROC,               glBindBuffer ) \
	X( PFNGLGENBUFFERSPROC,               glGenBuffers ) \
	X( PFNGLDELETEBUFFERSPROC,            glDeleteBuffers ) \
	X( PFNGLBUFFERDATAPROC,               glBufferData ) \
	X( PFNGLBUFFERSUBDATAPROC,            glBufferSubData ) \
	X( PFNGLGETBUFFERSUBDATAPROC,         glGetBufferSubData ) \
	X( PFNGLCREATESHADERPROC,             glCreateShader_real ) \
	X( PFNGLSHADERSOURCEPROC,             glShaderSource_real ) \
	X( PFNGLCOMPILESHADERPROC,            glCompileShader ) \
	X( PFNGLGETSHADERIVPROC,              glGetShaderiv ) \
	X( PFNGLGETSHADERINFOLOGPROC,         glGetShaderInfoLog ) \
	X( PFNGLDELETESHADERPROC,             glDeleteShader ) \
	X( PFNGLCREATEPROGRAMPROC,            glCreateProgram ) \
	X( PFNGLATTACHSHADERPROC,             glAttachShader ) \
	X( PFNGLBINDATTRIBLOCATIONPROC,       glBindAttribLocation ) \
	X( PFNGLGETATTRIBLOCATIONPROC,        glGetAttribLocation ) \
	X( PFNGLLINKPROGRAMPROC,              glLinkProgram ) \
	X( PFNGLGETPROGRAMIVPROC,             glGetProgramiv ) \
	X( PFNGLGETPROGRAMINFOLOGPROC,        glGetProgramInfoLog ) \
	X( PFNGLUSEPROGRAMPROC,               glUseProgram ) \
	X( PFNGLGETUNIFORMLOCATIONPROC,       glGetUniformLocation ) \
	X( PFNGLUNIFORM1FPROC,                glUniform1f ) \
	X( PFNGLUNIFORM1IPROC,                glUniform1i ) \
	X( PFNGLUNIFORM2FPROC,                glUniform2f ) \
	X( PFNGLUNIFORM3FPROC,                glUniform3f ) \
	X( PFNGLUNIFORM4FPROC,                glUniform4f ) \
	X( PFNGLUNIFORM4FVPROC,               glUniform4fv ) \
	X( PFNGLUNIFORMMATRIX4FVPROC,         glUniformMatrix4fv ) \
	X( PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer ) \
	X( PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray ) \
	X( PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray ) \
	X( PFNGLGENFRAMEBUFFERSPROC,          glGenFramebuffers ) \
	X( PFNGLDELETEFRAMEBUFFERSPROC,       glDeleteFramebuffers ) \
	X( PFNGLBINDFRAMEBUFFERPROC,          glBindFramebuffer_real ) \
	X( PFNGLFRAMEBUFFERTEXTURE2DPROC,     glFramebufferTexture2D ) \
	X( PFNGLCHECKFRAMEBUFFERSTATUSPROC,   glCheckFramebufferStatus ) \
	X( PFNGLGENRENDERBUFFERSPROC,         glGenRenderbuffers ) \
	X( PFNGLBINDRENDERBUFFERPROC,         glBindRenderbuffer ) \
	X( PFNGLRENDERBUFFERSTORAGEPROC,      glRenderbufferStorage ) \
	X( PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample ) \
	X( PFNGLFRAMEBUFFERRENDERBUFFERPROC,  glFramebufferRenderbuffer ) \
	X( PFNGLBLITFRAMEBUFFERPROC,          glBlitFramebuffer ) \
	X( PFNGLGENERATEMIPMAPPROC,           glGenerateMipmap )

#define DESKTOP_GL_DECLARE( type, name ) extern type dgl_##name;
DESKTOP_GL_FUNCS( DESKTOP_GL_DECLARE )
#undef DESKTOP_GL_DECLARE

#define glActiveTexture            dgl_glActiveTexture
#define glClientActiveTexture      dgl_glClientActiveTexture
#define glBlendEquation            dgl_glBlendEquation
#define glCompressedTexImage2D     dgl_glCompressedTexImage2D
#define glDrawRangeElements        dgl_glDrawRangeElements
#define glBindBuffer               dgl_glBindBuffer
#define glGenBuffers               dgl_glGenBuffers
#define glDeleteBuffers            dgl_glDeleteBuffers
#define glBufferData               dgl_glBufferData
#define glBufferSubData            dgl_glBufferSubData
#define glGetBufferSubData         dgl_glGetBufferSubData
#define glCompileShader            dgl_glCompileShader
#define glGetShaderiv              dgl_glGetShaderiv
#define glGetShaderInfoLog         dgl_glGetShaderInfoLog
#define glDeleteShader             dgl_glDeleteShader
#define glCreateProgram            dgl_glCreateProgram
#define glAttachShader             dgl_glAttachShader
#define glBindAttribLocation       dgl_glBindAttribLocation
#define glGetAttribLocation        dgl_glGetAttribLocation
#ifndef DESKTOP_GL_IMPL
#define glLinkProgram              desktop_glLinkProgram
#endif
#define glGetProgramiv             dgl_glGetProgramiv
#define glGetProgramInfoLog        dgl_glGetProgramInfoLog
#define glUseProgram               dgl_glUseProgram
#define glGetUniformLocation       dgl_glGetUniformLocation
#define glUniform1f                dgl_glUniform1f
#define glUniform1i                dgl_glUniform1i
#define glUniform2f                dgl_glUniform2f
#define glUniform3f                dgl_glUniform3f
#define glUniform4f                dgl_glUniform4f
#define glUniform4fv               dgl_glUniform4fv
#define glUniformMatrix4fv         dgl_glUniformMatrix4fv
#define glVertexAttribPointer      dgl_glVertexAttribPointer
#define glEnableVertexAttribArray  dgl_glEnableVertexAttribArray
#define glDisableVertexAttribArray dgl_glDisableVertexAttribArray
#define glGenFramebuffers          dgl_glGenFramebuffers
#define glDeleteFramebuffers       dgl_glDeleteFramebuffers
#define glFramebufferTexture2D     dgl_glFramebufferTexture2D
#define glCheckFramebufferStatus   dgl_glCheckFramebufferStatus
#define glGenRenderbuffers         dgl_glGenRenderbuffers
#define glBindRenderbuffer         dgl_glBindRenderbuffer
#define glRenderbufferStorage      dgl_glRenderbufferStorage
#define glRenderbufferStorageMultisample dgl_glRenderbufferStorageMultisample
#define glFramebufferRenderbuffer  dgl_glFramebufferRenderbuffer
#define glBlitFramebuffer          dgl_glBlitFramebuffer
#define glGenerateMipmap           dgl_glGenerateMipmap

// Wrapped calls (desktop/src/shim_gl.cpp):
//  - shaders remember their Cg stage so glShaderSource can translate;
//  - framebuffer 0 means "the game's render target", which on desktop is an
//    offscreen buffer scaled to the window at swap;
//  - glDepthRangef is GLES / GL 4.1, desktop GL 2.x has the double version.
GLuint desktop_glCreateShader( GLenum type );
void   desktop_glShaderSource( GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length );
void   desktop_glLinkProgram( GLuint program );
void   desktop_glBindFramebuffer( GLenum target, GLuint fb );
void   glShaderBinary( GLsizei count, const GLuint *shaders, GLenum format, const void *binary, GLsizei length );
// The game addresses its screen as 960x544 (viewports, scissors, pixel
// reads). On desktop the screen is an offscreen target of any size; these
// wrappers scale those coordinates while it is bound, so the engine keeps
// its Vita numbers and renders at full resolution.
void   desktop_glViewport( GLint x, GLint y, GLsizei w, GLsizei h );
void   desktop_glScissor( GLint x, GLint y, GLsizei w, GLsizei h );
void   desktop_glGetIntegerv( GLenum pname, GLint *data );
void   desktop_glReadPixels( GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *pixels );
#ifndef DESKTOP_GL_IMPL
#define glCreateShader             desktop_glCreateShader
#define glShaderSource             desktop_glShaderSource
#define glBindFramebuffer          desktop_glBindFramebuffer
#define glViewport                 desktop_glViewport
#define glScissor                  desktop_glScissor
#define glGetIntegerv              desktop_glGetIntegerv
#define glReadPixels               desktop_glReadPixels
#endif
#define glDepthRangef( n, f )      glDepthRange( (GLdouble)( n ), (GLdouble)( f ))

#ifdef __cplusplus
}
#endif

#endif // THUG_DESKTOP_VITAGL_H
