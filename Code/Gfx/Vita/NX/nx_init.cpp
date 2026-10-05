/*****************************************************************************
**  THUG-Vita — backend VitaGL                                              **
**  Gfx/Vita/NX/nx_init.cpp                                                 **
*****************************************************************************/

#include <gfx/Vita/NX/nx_init.h>

namespace NxVita
{

// Conversion à l'identité : neutre tant que rien n'est rendu (voir le header).
// Framebuffer : 960x544, la résolution native de la Vita.
SEngineGlobals EngineGlobals = { 1.0f, 1.0f, 0, 0, 960, 544 };

} // namespace NxVita
