///////////////////////////////////////////////////////////////////////////////
// p_debug_hud.h — affichage de debogage a l'ecran
//
// Repondre a « combien de fps ? » ou « le culling est-il actif ? » demandait
// jusqu'ici un aller-retour FTP et la lecture d'un journal de plusieurs
// milliers de lignes. Ces questions se posent a CHAQUE essai : elles doivent
// se lire a l'ecran.

#ifndef __GFX_VITA_P_DEBUG_HUD_H__
#define __GFX_VITA_P_DEBUG_HUD_H__

namespace NxVita
{

// Affichage actif ? Bascule par la commande « hud 0 » / « hud 1 ».
extern bool g_vita_hud;

// A appeler en fin de frame, apres le 2D du jeu. Ne fait rien si l'affichage
// est coupe.
void DrawDebugHud( float ms_frame, int meshes_drawn, int meshes_total );

// Etat du decor de la derniere frame, renseigne par RenderWorld : l'affichage
// n'a pas a aller le chercher lui-meme.
void SetHudWorldStats( int drawn, int total );

} // namespace NxVita

#endif // __GFX_VITA_P_DEBUG_HUD_H__
