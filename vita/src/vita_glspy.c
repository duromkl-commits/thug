/* Espion GL (#48, clignotement des personnages).
 *
 * Lie avec -Wl,--wrap=... : chaque appel du JEU aux fonctions vitaGL capables
 * de couper ou d'effacer la scene GXM en cours (glClear, glBindFramebuffer,
 * glFinish, glFlush, glReadPixels, glGetQueryObject*) passe ici. Pendant une
 * image ouverte (entre s_plat_pre_render et vglSwapBuffers), on compte les
 * appels par fonction et par appelant, et on les journalise toutes les 5 s.
 * Les appels internes a vitaGL ne sont pas interceptes (--wrap ne touche que
 * les references non resolues des autres objets), et c'est voulu.
 */
#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include "vita_log.h"

int vita_glspy_ouverte = 0;     /* pose par p_nx.cpp */
int vita_glspy_phase   = 0;     /* 1 = logique, 2 = rendu du monde, 3 = 2D/fin */

typedef struct { const char *f; void *appelant; int phase; int n; } SAppel;
static SAppel s_t[48];
static int    s_n = 0;

static void noter(const char *f, void *ra)
{
	int i;
	if (!vita_glspy_ouverte)
		return;
	for (i = 0; i < s_n; ++i)
		if (s_t[i].f == f && s_t[i].appelant == ra && s_t[i].phase == vita_glspy_phase) {
			++s_t[i].n;
			return;
		}
	if (s_n < 48) {
		s_t[s_n].f = f; s_t[s_n].appelant = ra; s_t[s_n].phase = vita_glspy_phase; s_t[s_n].n = 1;
		++s_n;
	}
}

void vita_glspy_bilan(void)
{
	static SceUInt64 s_t0 = 0;
	int i;
	SceUInt64 t = sceKernelGetProcessTimeWide();
	if (t - s_t0 < 5000000)
		return;
	s_t0 = t;
	for (i = 0; i < s_n; ++i)
		VLOG("GLSPY", "%s phase %d appelant %p : %d fois (5 s)",
		     s_t[i].f, s_t[i].phase, s_t[i].appelant, s_t[i].n);
	if (s_n)
		VLOG("GLSPY", "--- %d sites en cours d'image ---", s_n);
	s_n = 0;
}

void __real_glClear(GLbitfield m);
void __wrap_glClear(GLbitfield m)
{ noter("glClear", __builtin_return_address(0)); __real_glClear(m); }

void __real_glBindFramebuffer(GLenum t, GLuint f);
void __wrap_glBindFramebuffer(GLenum t, GLuint f)
{ noter("glBindFramebuffer", __builtin_return_address(0)); __real_glBindFramebuffer(t, f); }

void __real_glFinish(void);
void __wrap_glFinish(void)
{ noter("glFinish", __builtin_return_address(0)); __real_glFinish(); }

void __real_glFlush(void);
void __wrap_glFlush(void)
{ noter("glFlush", __builtin_return_address(0)); __real_glFlush(); }

void __real_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, void *d);
void __wrap_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, void *d)
{ noter("glReadPixels", __builtin_return_address(0)); __real_glReadPixels(x, y, w, h, f, ty, d); }

void __real_glGetQueryObjectuiv(GLuint id, GLenum p, GLuint *v);
void __wrap_glGetQueryObjectuiv(GLuint id, GLenum p, GLuint *v)
{ noter("glGetQueryObjectuiv", __builtin_return_address(0)); __real_glGetQueryObjectuiv(id, p, v); }

/* #69 : pics de 3-10 ms au milieu d'un dessin de modele. vitaGL compile-t-il
 * des shaders (pipeline fixe) a la volee ? Chaque compilation et chaque
 * creation de programme patche est chronometree ; au-dela de 1 ms, on journalise. */
#include <psp2/kernel/processmgr.h>
const void *__real_sceShaccCgCompileProgram(const void *o, const void *c, int u);
const void *__wrap_sceShaccCgCompileProgram(const void *o, const void *c, int u)
{
	SceUInt64 t0 = sceKernelGetProcessTimeWide();
	const void *r = __real_sceShaccCgCompileProgram(o, c, u);
	SceUInt64 d = sceKernelGetProcessTimeWide() - t0;
	static int n = 0;
	++n;
	VLOG("GLSPY", "compilation Cg n %d : %.2f ms (phase %d, image ouverte %d)", n, d / 1000.0,
	     vita_glspy_phase, vita_glspy_ouverte);
	return r;
}
int __real_sceGxmShaderPatcherCreateVertexProgram(void *a, void *b, const void *c, unsigned d, const void *e, unsigned f, void **g);
int __wrap_sceGxmShaderPatcherCreateVertexProgram(void *a, void *b, const void *c, unsigned d, const void *e, unsigned f, void **g)
{
	SceUInt64 t0 = sceKernelGetProcessTimeWide();
	int r = __real_sceGxmShaderPatcherCreateVertexProgram(a, b, c, d, e, f, g);
	SceUInt64 dd = sceKernelGetProcessTimeWide() - t0;
	if (dd > 1000)
		VLOG("GLSPY", "patch programme de sommets : %.2f ms", dd / 1000.0);
	return r;
}
