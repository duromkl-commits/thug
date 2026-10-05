///////////////////////////////////////////////////////////////////////////////
// p_occlusion.h -- occlusion par volumes d'ombre, cote Vita
//
// Le moteur d'origine ne se contente pas du tronc de vision : il rejette aussi
// ce qui est CACHE DERRIERE un batiment. Sans ce mecanisme, regarder au loin
// dans une rue fait dessiner tout ce qui s'aligne derriere les facades.
//
// [SOURCE] XBox/NX/occlude.cpp. Le chemin est deja portable jusqu'a nous : des
// secteurs sont marques occludeurs (Gfx/NxSector.cpp:534), leur collision
// produit des quads (Gel/Collision/CollTriData.cpp, ProcessOcclusion), et
// CEngine::sAddOcclusionPoly les transmet au backend. Cote Vita, ce contrat
// etait un stub -- c'est ce fichier qui le remplit.

#ifndef __GFX_VITA_P_OCCLUSION_H__
#define __GFX_VITA_P_OCCLUSION_H__

#include <core/defines.h>

namespace NxVita
{

// Un quad occludeur, en coordonnees monde. p_verts pointe sur 4 sommets de
// 3 flottants. Le checksum permet aux scripts de l'allumer ou de l'eteindre.
void OcclusionAjouter( const float *p_verts, unsigned int checksum );
void OcclusionActiver( unsigned int checksum, bool actif );
void OcclusionVider( void );

// A appeler UNE fois par image, avant tout test : choisit les occludeurs les
// plus rentables depuis ce point de vue et construit leurs volumes d'ombre.
void OcclusionConstruire( const float *p_cam );

// Vrai si la sphere est entierement dans l'ombre d'un occludeur. Conservateur :
// il vaut mieux dessiner un objet cache que d'en effacer un visible.
bool OcclusionTesteSphere( float x, float y, float z, float rayon );

// Nombre d'occludeurs retenus cette image, et nombre de quads connus.
int  OcclusionNbActifs( void );
int  OcclusionNbConnus( void );

// Debrayable a chaud (commande « occ 0/1 »).
extern bool g_vita_occlusion;

} // namespace NxVita

#endif // __GFX_VITA_P_OCCLUSION_H__
