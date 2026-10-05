/*****************************************************************************
**  THUG-Vita — instrumentation des stubs de backend                        **
**  Code/Gfx/Vita/p_stub.cpp                                                **
*****************************************************************************/

#include "p_stub.h"

// VLOG et non sceClibPrintf : ce dernier part vers Cat-A-Log, muet sur cette
// console. Des stubs qui loggent dans le vide ne servent a rien — c'est
// precisement l'instrumentation dont on a besoin au palier 2.
#include "vita_log.h"

// Compteur global : donne d'un coup d'œil l'étendue du backend réellement
// atteinte par le moteur. Utile au palier 2 pour savoir si on a effleuré le
// rendu ou s'il s'y engouffre vraiment.
static int s_distinct_stubs = 0;

extern "C" void Vita_StubHit(const char *name, int *p_already_seen)
{
	if (*p_already_seen)
		return;
	*p_already_seen = 1;
	++s_distinct_stubs;

	// Garde sur name : __PRETTY_FUNCTION__ ne devrait jamais être nul, mais un
	// log qui affiche « ? » sans dire lequel ne sert à rien.
	VLOG( "GFX", "stub #%d: %s", s_distinct_stubs, name ? name : "(nom absent)" );
}
