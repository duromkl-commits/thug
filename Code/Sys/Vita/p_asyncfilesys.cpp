/*****************************************************************************
**  THUG-Vita — couche système                                              **
**  Code/Sys/Vita/p_asyncfilesys.cpp                                        **
**                                                                          **
**  Chargeur de fichiers asynchrone.                                        **
**                                                                          **
**  Ces six fonctions étaient des stubs ASSEMBLEUR muets (p_link_stubs.S),  **
**  rendant 0 sans rien signaler. Deux d'entre elles étaient franchement    **
**  nocives :                                                               **
**                                                                          **
**    - s_plat_exist()  répondait « ce fichier n'existe pas » à TOUTE       **
**      question, y compris sur des fichiers bien présents dans les         **
**      archives ;                                                          **
**    - s_plat_update() ne signalait jamais qu'un chargement était fini.    **
**                                                                          **
**  C'est le suspect principal du point d'arrêt non déterministe observé    **
**  au palier 3 : un « chargement terminé » qui n'arrive jamais, ou qui     **
**  dépend de l'ordre dans lequel les requêtes ont été empilées.            **
**                                                                          **
**  Choix : PAS d'asynchrone sur Vita pour l'instant. `s_plat_async_supported`
**  rend false, donc le moteur emprunte son propre chemin synchrone, qui    **
**  passe par File::Open/Read et le repli PRE — code déjà éprouvé. Rien à   **
**  synchroniser, rien à attendre.                                          **
**                                                                          **
**  L'asynchrone réel (sceIo async ou thread dédié) n'aura d'intérêt qu'au  **
**  palier 5, quand les temps de chargement seront mesurés plutôt que       **
**  supposés.                                                               **
*****************************************************************************/

#include <core/defines.h>
#include <sys/file/filesys.h>
#include <sys/file/AsyncFilesys.h>

#include "../../Gfx/Vita/p_stub.h"

namespace File
{

void CAsyncFileLoader::s_plat_init()
{
	VITA_STUB();
	// Rien à monter : le chemin synchrone est déjà opérationnel.
}


void CAsyncFileLoader::s_plat_cleanup()
{
	VITA_STUB();
}


bool CAsyncFileLoader::s_plat_async_supported()
{
	VITA_STUB();
	// false, et c'est un choix délibéré : le moteur bascule alors sur son
	// chemin synchrone. Répondre true nous obligerait à honorer tout le
	// protocole de callbacks, qu'aucune de ces fonctions n'implémente.
	return false;
}


bool CAsyncFileLoader::s_plat_exist( const char *filename )
{
	VITA_STUB();
	// Vraie réponse, via la couche fichiers (qui consulte les archives PRE).
	// Le stub muet rendait toujours false — le moteur croyait donc qu'aucun
	// fichier n'existait.
	return File::Exist( filename );
}


void CAsyncFileLoader::s_plat_swap_callback_list()
{
	VITA_STUB();
	// Sans asynchrone, il n'y a pas de liste de callbacks à permuter.
}


void CAsyncFileLoader::s_plat_update()
{
	VITA_STUB();
	// Sans asynchrone, rien n'est en vol : aucune requête à faire progresser.
}

} // namespace File
