/* Traceur d'allocations (#47 : fuite de 13-17 Mo par changement de niveau).
 *
 * Lie avec -Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,
 * --wrap=memalign : chaque bloc vivant est note dans une table (adresse ->
 * taille, appelant, generation). La generation avance a chaque vidage du monde
 * (vita_memspy_niveau). A chaque vidage, on regroupe par appelant les blocs
 * alloues depuis le vidage precedent et encore vivants : ce que le niveau
 * quitte n'a pas rendu. Les allocations internes a la libc (_malloc_r) ne passent pas ici.
 */
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include "vita_log.h"

void *__real_malloc(size_t n);
void  __real_free(void *p);
void *__real_realloc(void *p, size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_memalign(size_t al, size_t n);

#define TAILLE (1 << 19)            /* 524288 entrees, ~8 Mo hors tas newlib */
typedef struct { void *p; unsigned taille; void *appelant; unsigned gen; } SBloc;
static SBloc *s_t = NULL;
static unsigned s_gen = 1;
static int s_actif = 0;
static int s_init = 0;
static SceKernelLwMutexWork s_mutex;

static void init(void)
{
	if (s_init)
		return;
	s_init = 1;
	sceKernelCreateLwMutex(&s_mutex, "memspy", 0, 0, NULL);
	/* Table hors du tas newlib : memoire noyau, pour ne pas fausser la mesure. */
	SceUID b = sceKernelAllocMemBlock("memspy", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
	                                  (TAILLE * sizeof(SBloc) + 0xFFFFF) & ~0xFFFFF, NULL);
	if (b >= 0 && sceKernelGetMemBlockBase(b, (void **)&s_t) >= 0) {
		memset(s_t, 0, TAILLE * sizeof(SBloc));
		s_actif = 1;
	}
}

static unsigned h(void *p) { return ((unsigned)p >> 3) * 2654435761u >> 13; }

static void noter(void *p, size_t n, void *ra)
{
	unsigned i, k;
	if (!p || !s_actif)
		return;
	sceKernelLockLwMutex(&s_mutex, 1, NULL);
	i = h(p) & (TAILLE - 1);
	for (k = 0; k < 64; ++k, i = (i + 1) & (TAILLE - 1))
		if (!s_t[i].p || s_t[i].p == (void *)1) {
			s_t[i].p = p; s_t[i].taille = n; s_t[i].appelant = ra; s_t[i].gen = s_gen;
			break;
		}
	sceKernelUnlockLwMutex(&s_mutex, 1);
}

static void oublier(void *p)
{
	unsigned i, k;
	if (!p || !s_actif)
		return;
	sceKernelLockLwMutex(&s_mutex, 1, NULL);
	i = h(p) & (TAILLE - 1);
	for (k = 0; k < 64; ++k, i = (i + 1) & (TAILLE - 1)) {
		if (!s_t[i].p)
			break;
		if (s_t[i].p == p) {
			s_t[i].p = (void *)1;           /* pierre tombale */
			break;
		}
	}
	sceKernelUnlockLwMutex(&s_mutex, 1);
}

void *__wrap_malloc(size_t n)
{ void *p; init(); p = __real_malloc(n); noter(p, n, __builtin_return_address(0)); return p; }
void __wrap_free(void *p)
{ oublier(p); __real_free(p); }
void *__wrap_calloc(size_t a, size_t b)
{ void *p; init(); p = __real_calloc(a, b); noter(p, a * b, __builtin_return_address(0)); return p; }
void *__wrap_memalign(size_t al, size_t n)
{ void *p; init(); p = __real_memalign(al, n); noter(p, n, __builtin_return_address(0)); return p; }
void *__wrap_realloc(void *p, size_t n)
{
	void *q;
	init();
	oublier(p);
	q = __real_realloc(p, n);
	noter(q, n, __builtin_return_address(0));
	return q;
}

/* Vidage du monde : bilan de la generation precedente, puis on avance. */
void vita_memspy_niveau(void)
{
	typedef struct { void *appelant; unsigned octets, n; } SAgg;
	static SAgg agg[256];
	int na = 0, i, j;
	unsigned total = 0, k;
	if (!s_actif)
		return;
	sceKernelLockLwMutex(&s_mutex, 1, NULL);
	for (k = 0; k < TAILLE; ++k) {
		SBloc *b = &s_t[k];
		if (!b->p || b->p == (void *)1 || b->gen < 2 || b->gen + 1 >= s_gen)
			continue;
		total += b->taille;
		for (i = 0; i < na && agg[i].appelant != b->appelant; ++i) ;
		if (i == na) {
			if (na == 256) continue;
			agg[na].appelant = b->appelant; agg[na].octets = 0; agg[na].n = 0; ++na;
		}
		agg[i].octets += b->taille; ++agg[i].n;
	}
	++s_gen;
	sceKernelUnlockLwMutex(&s_mutex, 1);
	/* tri decroissant sommaire, 15 premiers */
	for (i = 0; i < na; ++i)
		for (j = i + 1; j < na; ++j)
			if (agg[j].octets > agg[i].octets) { SAgg t = agg[i]; agg[i] = agg[j]; agg[j] = t; }
	VLOG("FUITE", "vidage %u : %u Ko alloues il y a 2 niveaux ou plus (hors demarrage) et toujours vivants", s_gen - 1, total >> 10);
	for (i = 0; i < na && i < 15; ++i)
		VLOG("FUITE", "  %p : %u Ko en %u blocs", agg[i].appelant, agg[i].octets >> 10, agg[i].n);
}
