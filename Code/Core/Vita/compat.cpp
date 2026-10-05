/*****************************************************************************
**  THUG-Vita — implémentations de la couche de compatibilité               **
**  Code/Core/Vita/compat.cpp                                               **
**                                                                          **
**  Seul endroit où compat peut dépendre du SDK Vita. Le header, lui, reste **
**  propre : il est inclus par Core/Defines.h, donc par tout le moteur.     **
*****************************************************************************/

#include <psp2/kernel/clib.h>

extern "C" void Vita_OutputDebugString(const char *msg)
{
	if (!msg)
		return;
	// Le moteur passe des messages déjà formatés, souvent sans '\n'.
	// On ne rajoute rien : le préfixe de sous-système et le retour à la ligne
	// sont la responsabilité de l'appelant, comme pour VLOG.
	sceClibPrintf("%s", msg);
}
