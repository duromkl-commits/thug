/*****************************************************************************
**  THUG-Vita — backend graphique                                            **
**  Code/Gfx/Vita/p_nx_pools.cpp                                            **
**                                                                          **
**  Fabriques et reservoirs d'objets que chaque backend doit fournir.       **
**                                                                          **
**  Toutes etaient des stubs ASSEMBLEUR muets, donc « rend 0 » sans un mot. **
**  Le motif de panne est toujours le meme et il coute cher a diagnostiquer :**
**                                                                          **
**    reservoir vide  ->  sGetXxxInstance() rend NULL                       **
**                    ->  le Dbg_MsgAssert cense l'attraper est compile     **
**                        A VIDE dans cette configuration                   **
**                    ->  le NULL file jusqu'au premier dereferencement     **
**                    ->  Data abort, loin de la cause.                     **
**                                                                          **
**  Deja paye deux fois : CWindow2D (p_NxWin2D.cpp) puis CText, chacun      **
**  coutant un aller-retour console complet. On traite donc ici d'un bloc   **
**  toutes les fabriques encore muettes que le moteur appelle au demarrage. **
*****************************************************************************/

#include <core/defines.h>

#include <gfx/nx.h>
#include <gfx/NxFont.h>
#include <gfx/NxScene.h>
#include "p_NxScene.h"
#include "p_NxFont.h"
#include <gfx/NxImposter.h>

#include "p_stub.h"

namespace Nx
{

// ---------------------------------------------------------------------------
// Reservoir de CText.
//
// [VERIFIE par psp2core] Sans lui : Data abort dans CText::SetHidden avec
// this=0, appele depuis CTextMan::sFreeTextInstance -- le moteur rendait au
// reservoir un objet que sGetTextInstance n'avait jamais pu lui donner.
//
// 512 instances, comme les autres backends (NxFont.h : vMAX_TEXT_INSTANCES).
// ---------------------------------------------------------------------------
void CTextMan::s_plat_alloc_text_pool()
{
	for( int i = 0; i < vMAX_TEXT_INSTANCES; ++i )
	{
		CText *p_text			= new CVitaText;
		p_text->mp_next			= sp_dynamic_text_list;
		sp_dynamic_text_list	= p_text;
	}
}


// ---------------------------------------------------------------------------
// Scene. Vide mais valide : au palier 3 elle ne contient aucune geometrie,
// elle sert seulement a ce que le chargement de niveau aboutisse. Le vrai
// travail (lire le format de maillage Xbox, remplir des vertex/index buffers
// VitaGL) est la suite immediate.
// ---------------------------------------------------------------------------
CScene *CEngine::s_plat_create_scene( const char *p_name, CTexDict *p_tex_dict,
                                      bool add_super_sectors )
{
	VITA_STUB();

	CScene *p_scene = new CVitaScene;
	p_scene->SetInSuperSectors( add_super_sectors );
	p_scene->SetIsSky( false );
	return p_scene;
}


// ---------------------------------------------------------------------------
// Groupe d'imposteurs (rendu lointain simplifie). Objet vide : rien ne sera
// dessine, mais le moteur ne meurt pas sur un NULL.
// ---------------------------------------------------------------------------
CImposterGroup *CImposterManager::plat_create_imposter_group( void )
{
	VITA_STUB();
	return new CImposterGroup;
}


// ---------------------------------------------------------------------------
// Particules (anciennes, CParticle) : la fabrique plat_create_particle est
// dans p_NxParticle.cpp, avec le rendu.
// ---------------------------------------------------------------------------

} // namespace Nx
