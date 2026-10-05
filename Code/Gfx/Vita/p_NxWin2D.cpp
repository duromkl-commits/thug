/*****************************************************************************
**  THUG-Vita — backend graphique                                            **
**  Code/Gfx/Vita/p_NxWin2D.cpp                                             **
**                                                                          **
**  Pool de fenetres 2D (rectangles de decoupe pour l'interface).           **
**                                                                          **
**  CWindow2DManager gere une liste libre pre-allouee. Le stub assembleur   **
**  muet ne remplissait rien, donc `sp_window_list` restait NULL et         **
**  sGetWindowInstance() rendait NULL — l'assert qui devait le signaler     **
**  est compile a vide chez nous, si bien que le NULL filait directement    **
**  dans p_window->SetPos(). C'est la ce qui arretait le moteur pendant     **
**  `default_system_startup`.                                              **
**                                                                          **
**  On calque le backend Xbox : une sous-classe dont plat_update_engine ne  **
**  fait rien, et dix instances chainees d'avance.                          **
*****************************************************************************/

#include <core/defines.h>
#include <gfx/NxWin2D.h>

namespace Nx
{

// Implementation Vita de CWindow2D. Rien de specifique au materiel pour
// l'instant : le decoupage sera fait au moment du rendu 2D (palier 4), pas
// via un etat global du GPU comme sur Xbox.
class CVitaWindow2D : public CWindow2D
{
public:
	CVitaWindow2D( int x = 0, int y = 0, int width = 640, int height = 448 )
		: CWindow2D( x, y, width, height ) {}
	virtual ~CVitaWindow2D() {}

private:
	virtual void plat_update_engine() {}
};


void CWindow2DManager::s_plat_alloc_window2d_pool()
{
	for( int i = 0; i < vMAX_WINDOW_INSTANCES; i++ )
	{
		CVitaWindow2D *p_window = new CVitaWindow2D;
		p_window->mp_next	= sp_window_list;
		sp_window_list		= p_window;
	}
}

} // namespace Nx
