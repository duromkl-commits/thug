/*****************************************************************************
**  THUG-Vita — profileur par instrumentation                                **
**                                                                          **
**  Pourquoi : trois fois de suite, j'ai accuse le mauvais coupable en       **
**  raisonnant au lieu de mesurer (le transfert des sommets, le nombre       **
**  d'appels de dessin, les VBO). Un compteur pose a la main ne trouve que   **
**  ce qu'on soupconne deja.                                                 **
**                                                                          **
**  Comment : GCC, avec -finstrument-functions, appelle les deux fonctions   **
**  ci-dessous a l'entree et a la sortie de CHAQUE fonction compilee avec ce **
**  drapeau. On accumule le temps par adresse ; les noms sont resolus a      **
**  froid, cote PC, avec nm (vita/tools/prof.py).                            **
**                                                                          **
**  L'echantillonnage du PC aurait ete plus leger, mais                      **
**  sceKernelGetThreadCpuRegisters n'existe qu'en kernel : hors de portee    **
**  d'un homebrew userland.                                                  **
**                                                                          **
**  DEFAUT CONNU, a corriger avant reutilisation : la pile d'appels          **
**  ci-dessous est GLOBALE, alors que le jeu est multithread. Des que deux   **
**  threads s'entremelent, les paires entree/sortie se desynchronisent et    **
**  les temps atterrissent sur des fonctions arbitraires -- releve : 269 s   **
**  cumulees pour 15 s de jeu, et des symboles jamais instrumentes.          **
**                                                                          **
**  Correctif : une pile PAR THREAD (sceKernelGetThreadId comme cle), et     **
**  ignorer tout thread autre que la boucle principale.                      **
**                                                                          **
**  CE FICHIER ne doit PAS etre instrumente lui-meme (recursion infinie) :   **
**  voir -fno-instrument-functions dans CMakeLists.txt.                      **
*****************************************************************************/

#include <psp2/kernel/processmgr.h>
#include <string.h>

#define PROF_SLOTS	4096		/* puissance de 2 : masque au lieu de modulo */
#define PROF_DEPTH	256

typedef struct
{
	unsigned int		addr;		/* adresse de la fonction, 0 = libre */
	unsigned long long	total;		/* microsecondes cumulees, appels inclus */
	unsigned int		calls;
} SProfSlot;

static SProfSlot		s_slots[PROF_SLOTS];
static unsigned long long	s_stack[PROF_DEPTH];
static unsigned int		s_stack_fn[PROF_DEPTH];
static int			s_depth   = 0;
static int			s_enabled = 0;
/* Thread qui a arme le profileur (la boucle principale : les commandes sont
   traitees par la lecture des manettes). Tous les autres sont ignores --
   correctif du defaut decrit en tete. Lu dans TPIDRURO, sans appel systeme :
   ces fonctions tournent a chaque entree et sortie. */
static unsigned int		s_thread  = 0;

static inline unsigned int thread_courant( void )
{
	unsigned int tp;
	__asm__ volatile( "mrc p15, 0, %0, c13, c0, 3" : "=r"( tp ));
	return tp;
}

void vita_prof_enable( int on )
{
	if( on && !s_enabled )
	{
		memset( s_slots, 0, sizeof( s_slots ));
		s_depth = 0;
		s_thread = thread_courant();
	}
	s_enabled = on;
}

int vita_prof_dump( unsigned int *p_addr, unsigned long long *p_total,
                    unsigned int *p_calls, int max )
{
	int n = 0;
	int i;
	for( i = 0; ( i < PROF_SLOTS ) && ( n < max ); ++i )
	{
		if( s_slots[i].addr && s_slots[i].calls )
		{
			p_addr[n]  = s_slots[i].addr;
			p_total[n] = s_slots[i].total;
			p_calls[n] = s_slots[i].calls;
			++n;
		}
	}
	return n;
}

static SProfSlot *slot_for( unsigned int addr )
{
	/* Sondage lineaire : la table est large et le nombre de fonctions
	   reellement chaudes est petit, les collisions restent rares. */
	unsigned int h = ( addr >> 2 ) * 2654435761u;
	int i;
	for( i = 0; i < 64; ++i )
	{
		SProfSlot *p = &s_slots[( h + i ) & ( PROF_SLOTS - 1 )];
		if( p->addr == addr )
			return p;
		if( p->addr == 0 )
		{
			p->addr = addr;
			return p;
		}
	}
	return 0;					/* table saturee : on laisse tomber cette fonction */
}

void __cyg_profile_func_enter( void *this_fn, void *call_site )
{
	(void)call_site;
	if( !s_enabled || ( thread_courant() != s_thread ))
		return;
	if( s_depth >= PROF_DEPTH )
	{
		if( s_enabled )
			++s_depth;			/* garder la pile coherente */
		return;
	}
	s_stack_fn[s_depth] = (unsigned int)this_fn;
	s_stack[s_depth]    = sceKernelGetProcessTimeWide();
	++s_depth;
}

void __cyg_profile_func_exit( void *this_fn, void *call_site )
{
	(void)this_fn;
	(void)call_site;
	if( !s_enabled || ( s_depth <= 0 ) || ( thread_courant() != s_thread ))
		return;
	--s_depth;
	if( s_depth < PROF_DEPTH )
	{
		SProfSlot *p = slot_for( s_stack_fn[s_depth] );
		if( p )
		{
			p->total += sceKernelGetProcessTimeWide() - s_stack[s_depth];
			++p->calls;
		}
	}
}
