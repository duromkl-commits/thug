/*****************************************************************************
**  THUG-Vita — backend VitaGL                                              **
**  Gfx/Vita/NX/nx_init.h                                                   **
**                                                                          **
**  Équivalent minimal de Gfx/DX9/NX/nx_init.h pour la cible Vita.          **
**                                                                          **
**  Ce header n'existe que pour un besoin très étroit : Core/macros.h a     **
**  besoin des 4 champs de conversion de coordonnées écran pour définir     **
**  SCREEN_CONV_X/Y, et tire pour cela tout le header du backend — lequel   **
**  traîne D3DPRESENT_PARAMETERS, DWORD et compagnie.                       **
**                                                                          **
**  On n'expose donc QUE ces 4 champs. Le reste du contrat backend arrivera **
**  au fil des paliers, pas par recopie anticipée du header DX9.            **
*****************************************************************************/

#ifndef __GFX_VITA_NX_INIT_H
#define __GFX_VITA_NX_INIT_H

namespace NxVita
{

// Conversion des coordonnées logiques du jeu (pensé en 640x448) vers le
// framebuffer réel.
//
// Valeurs par défaut = identité : au palier 1 on ne rend rien, et une
// identité est un comportement neutre et lisible. Le vrai calcul (960x544,
// letterbox ou mise à l'échelle) se décidera au palier 4, quand il y aura une
// image à regarder pour juger — pas avant.
struct SEngineGlobals
{
	float	screen_conv_x_multiplier;
	float	screen_conv_y_multiplier;
	int		screen_conv_x_offset;
	int		screen_conv_y_offset;

	// Résolution réelle du framebuffer. Le moteur s'en sert pour ajuster le
	// placement des éléments 2D (voir Gfx/2D/Element3d.cpp).
	int		backbuffer_width;
	int		backbuffer_height;
};

extern SEngineGlobals EngineGlobals;

} // namespace NxVita

#endif // __GFX_VITA_NX_INIT_H
