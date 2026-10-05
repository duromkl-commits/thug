/*****************************************************************************
**  THUG-Vita — stubs de link, dernier lot                                  **
**  Code/Sys/Vita/p_link_stubs.cpp                                          **
**                                                                          **
**  Reliquat de symboles que le moteur référence sans qu'aucun backend ne   **
**  les fournisse : son, carte mémoire, manette, imposteurs, gestion VRAM.  **
**                                                                          **
**  GÉNÉRÉ par gen_stubs.py, filtré sur les symboles réellement non résolus **
**  au link.                                                                **
*****************************************************************************/

#include <core/defines.h>
#include <gel/soundfx/soundfx.h>
#include <sys/sioman.h>
#include <sys/McMan.h>
#include <gfx/gfxman.h>
#include <gfx/nx.h>
#include <gfx/NxImposter.h>

#include "../../Gfx/Vita/p_stub.h"

namespace Sfx {
}

// SIO::Manager::Pause / UnPause : desormais dans
// Code/Sys/SIO/Vita/p_sioman.cpp, avec le reste du vrai gestionnaire.

// Les fonctions de fichier de carte memoire vivaient ici en stubs, et
// rendaient toutes « rien fait / echec ». Elles sont desormais implementees
// pour de vrai dans Code/Sys/MemCard/Vita/p_McMan.cpp : la sauvegarde ecrit
// dans ux0:data/thug/save.

namespace Gfx {
void	Manager::DumpVRAMUsage(void)
{
	VITA_STUB();
}

void	Manager::ScreenShot(const char *fileroot)
{
	VITA_STUB();
}

}

namespace Nx {
void	CImposterManager::plat_pre_render_imposters(void)
{
	VITA_STUB();
}

void	CImposterManager::plat_post_render_imposters(void)
{
	VITA_STUB();
}

}
