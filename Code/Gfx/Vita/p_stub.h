/*****************************************************************************
**  THUG-Vita — instrumentation des stubs de backend                        **
**  Code/Gfx/Vita/p_stub.h                                                  **
**                                                                          **
**  Le palier 1 veut un backend qui LINKE, pas qui rende. Chaque fonction   **
**  plateforme est donc un stub qui signale son premier appel puis rend une **
**  valeur neutre.                                                          **
**                                                                          **
**  « Premier appel seulement » est le point important : ces fonctions sont **
**  appelées par frame. Logger à chaque passage noierait le log et ferait   **
**  chuter le framerate au point de fausser tout diagnostic. Ce qui nous    **
**  intéresse au palier 2, c'est QUELLES fonctions le moteur atteint et     **
**  dans quel ordre — pas combien de fois.                                  **
*****************************************************************************/

#ifndef __GFX_VITA_P_STUB_H
#define __GFX_VITA_P_STUB_H

#ifdef __cplusplus
extern "C" {
#endif

// Émet « [GFX] stub: <nom> » une seule fois par site d'appel.
void Vita_StubHit(const char *name, int *p_already_seen);

#ifdef __cplusplus
}
#endif

// À placer en première ligne du corps d'un stub.
//
//   bool CVitaModel::plat_set_color(uint8 r, uint8 g, uint8 b, uint8 a)
//   {
//       VITA_STUB();
//       return true;
//   }
//
// __FUNCTION__ plutôt que __PRETTY_FUNCTION__ : ce dernier produit, pour des
// méthodes C++ avec namespaces et paramètres, des chaînes très longues qui
// ressortaient tronquées à « ? » dans le log. __FUNCTION__ donne le nom simple
// de la fonction — suffisant pour savoir laquelle le moteur appelle, et
// toujours impossible à désynchroniser d'un renommage.
#define VITA_STUB()                                       \
	do {                                                  \
		static int s_seen = 0;                            \
		Vita_StubHit(__FUNCTION__, &s_seen);              \
	} while (0)

#endif // __GFX_VITA_P_STUB_H
