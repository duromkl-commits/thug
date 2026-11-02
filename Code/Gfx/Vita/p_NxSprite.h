///////////////////////////////////////////////////////////////////////////////
// p_NxSprite.h — sprites 2D Vita
//
// Le moteur pose ses elements d'interface dans un espace de 640x448, quelle
// que soit la resolution reelle (voir NxSprite.h : « Assumes screen size of
// 640x448 »). On met a l'echelle vers 960x544 au moment du dessin.
//
// Chaque CSprite s'inscrit dans une liste au moment de sa construction, et en
// sort a sa destruction. RenderSprites2D() la parcourt une fois par frame, du
// moins prioritaire au plus prioritaire.

#ifndef __GFX_VITA_P_NXSPRITE_H__
#define __GFX_VITA_P_NXSPRITE_H__

#include <gfx/NxSprite.h>

namespace Nx
{

class CVitaSprite : public CSprite
{
public:
	CVitaSprite( CWindow2D *p_window = NULL );
	virtual ~CVitaSprite();

	// Lus par le rendu, qui n'est pas membre de la classe.
	bool			IsHiddenNow() const		{ return m_hidden; }
	float			GetPri() const			{ return m_priority; }
	CTexture *		GetTex() const			{ return mp_texture; }
	float			PosX() const			{ return m_pos_x; }
	float			PosY() const			{ return m_pos_y; }
	float			ScaleX() const			{ return m_scale_x; }
	float			ScaleY() const			{ return m_scale_y; }
	float			AnchorX() const			{ return m_anchor_x; }
	float			AnchorY() const			{ return m_anchor_y; }
	float			Rotation() const		{ return m_rotation; }
	uint16			W() const				{ return m_width; }
	uint16			H() const				{ return m_height; }
	Image::RGBA		Color() const			{ return m_rgba; }
	unsigned		Seq() const				{ return m_seq; }

protected:
	virtual void	plat_update_hidden();
	virtual void	plat_update_priority();

private:
	unsigned		m_seq;			// rang d'inscription dans la liste XBox
	float			m_seq_pri;
	bool			m_seq_hidden;
};

} // namespace Nx


namespace NxVita
{

// Dessine tous les sprites visibles. A appeler apres le rendu du monde et
// avant la presentation.
// Sprites de priorite dans ] bas, haut ] (#54 : entrelaces avec les textes).
void RenderSprites2D( float bas, float haut );

} // namespace NxVita

#endif // __GFX_VITA_P_NXSPRITE_H__
