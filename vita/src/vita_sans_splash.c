/*
 * Pas de splashscreen vitaGL au demarrage.
 *
 * La vitaGL installee dans le vitasdk joue son logo anime au vglInit
 * (splashscreen.o : invoke_splashscreen, depuis vgl.o) et ne le coupe qu'au
 * premier dessin, apres un fondu d'au moins une seconde (splashscreen.c :
 * SPLASH_FADE_SEC). Le jeu montre a la place son propre ecran de lancement
 * (vita_ecran_boot.c).
 *
 * Plutot que de recompiler vitaGL -- le clone disponible n'est pas la version
 * installee (preprocesseur GLSL different) --, on definit ici les quatre
 * symboles que le reste de vitaGL attend de splashscreen.o (gxm.o, vgl.o).
 * Nos objets passant avant libvitaGL.a a l'edition de liens, l'archive n'a
 * plus de raison d'extraire splashscreen.o : il n'est pas lie du tout.
 */
#include <psp2/types.h>

typedef unsigned char GLboolean_vgl;	/* GLboolean de vitaGL */

GLboolean_vgl is_splashscreen_active = 0;	/* jamais actif : gxm.c n'attend rien */
SceUID        splash_mutex[2];

void invoke_splashscreen( void ) {}
void clear_splashscreen( void ) {}
