///////////////////////////////////////////////////////////////////////////////
// p_gamma.h -- rampe gamma d'affichage (issues #45 et #22)
//
// [SOURCE] La Xbox applique a TOUTE l'image affichee une rampe gamma
// materielle : XBox/NX/nx_init.cpp:354 pose SetGammaNormalized( 0.14f, 0.13f,
// 0.12f ) au demarrage, XBox/NX/gamma.cpp en fait une table 256 entrees par
// canal (g = 0.5 + 2 f, out[i] = 256 * (i/256)^(1/g)) passee a
// D3DDevice_SetGammaRamp. xemu l'emule. La Vita n'a pas de rampe d'affichage
// (sceDisplay n'en expose pas, SceGxmColorSurfaceGammaMode n'est qu'un sRGB
// tout ou rien) : on rend l'image dans une texture, puis une passe plein ecran
// applique la meme formule en la recopiant dans le tampon d'affichage.

#ifndef __GFX_VITA_P_GAMMA_H__
#define __GFX_VITA_P_GAMMA_H__

namespace NxVita
{

// Contrat de XBox/NX/gamma.cpp : valeurs normalisees 0..1 par canal.
void SetGammaNormalized( float fr, float fg, float fb );
void GetGammaNormalized( float *fr, float *fg, float *fb );

// "gam 0/1/2" : 0 = pas de rampe (rendu direct dans l'affichage),
// 1 = rampe Xbox (defaut), 2 = rampe avec la texture lue a l'envers en V
// (diagnostic, si l'image sortait retournee).
extern int g_vita_gamma;

// Debut d'image : redirige le rendu vers la texture intermediaire. A appeler
// AVANT l'effacement de debut d'image.
void GammaImageDebut( void );

// Fin d'image : passe plein ecran texture -> affichage avec la rampe, puis
// l'affichage redevient la cible. A appeler APRES tout le rendu (2D, HUD,
// ecran de chargement) et AVANT les relectures (captures) et le swap.
// Sans effet si GammaImageDebut n'a pas redirige le rendu.
void GammaImageFin( void );

} // namespace NxVita

#endif // __GFX_VITA_P_GAMMA_H__
