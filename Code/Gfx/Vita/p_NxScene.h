///////////////////////////////////////////////////////////////////////////////
// p_NxScene.h — scene Vita
//
// CScene est concrete, mais ses virtuelles plat_* de base ne font que
// printf-er et rendre 0. plat_create_sector() en particulier rend NULL, et
// CScene::CreateSector ecrit dedans sans verifier -- Data abort immediat
// (constate sur psp2core).
//
// On calque le backend Xbox (Gfx/XBox/p_NxScene.cpp:128) : une sous-classe qui
// fabrique de vrais secteurs.

#ifndef __GFX_VITA_P_NXSCENE_H__
#define __GFX_VITA_P_NXSCENE_H__

#include <gfx/NxScene.h>
#include <gfx/NxSector.h>

namespace Nx
{

// Secteur Vita : seul le CLONAGE differe (issue #29). L'editeur de parc
// construit les parcs en clonant des pieces ; la version de base rend NULL
// et CClonedPiece::SetDesiredPos ecrivait dedans. Comme sur Xbox
// (XBox/p_nxsector.cpp:748, corps commente), le secteur clone est vide : la
// geometrie suit par mp_geom->Clone et la collision par mp_coll_sector->Clone
// (NxSector.cpp:695).
class CVitaSector : public CSector
{
public:
	CVitaSector() {}
	virtual ~CVitaSector() {}
private:
	virtual CSector *plat_clone( bool instance, CScene *p_dest_scene )
	{
		(void)instance; (void)p_dest_scene;
		return new CVitaSector;
	}
};

class CVitaScene : public CScene
{
public:
	CVitaScene( int sector_table_size = 10 ) : CScene( sector_table_size ) {}
	virtual ~CVitaScene() {}

private:
	// Secteur VIDE mais valide : aucune geometrie tant que le decodage du
	// format de scene Xbox n'est pas ecrit. Rendre NULL tuait le moteur.
	virtual CSector *plat_create_sector() { return new CVitaSector; }
};

} // namespace Nx

#endif // __GFX_VITA_P_NXSCENE_H__
