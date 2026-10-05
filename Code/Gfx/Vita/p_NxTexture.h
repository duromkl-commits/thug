///////////////////////////////////////////////////////////////////////////////
// p_NxTexture.h — textures Vita
//
// Format des .tex.xbx, releve sur le backend DX9 (p_nxtexture.cpp:468,
// LoadTextureFile_Internal) :
//
//   int  version
//   int  num_textures
//   par texture :
//     uint32 checksum, largeur, hauteur, niveaux de mip,
//            profondeur texel, profondeur palette, DXT, taille palette
//     si taille palette : les octets de la palette (D3DCOLOR, donc BGRA)
//     par niveau de mip : uint32 taille, puis les octets
//
// Deux familles de donnees :
//   - DXT (1, 2 ou 5) : blocs 4x4 compresses, NON entrelaces
//   - non compresse   : ENTRELACE facon Xbox (ordre de Morton), a remettre a
//                       plat avec Unswizzle avant tout usage

#ifndef __GFX_VITA_P_NXTEXTURE_H__
#define __GFX_VITA_P_NXTEXTURE_H__

#include <gfx/NxTexture.h>
#include <vitaGL.h>

namespace Nx
{

class CVitaTexture : public CTexture
{
public:
	CVitaTexture();
	virtual ~CVitaTexture();

	// m_checksum est protege dans CTexture : on le renseigne ici, la classe
	// de base n'expose pas de setter.
	void	SetGLTexture( GLuint tex, uint16 w, uint16 h, bool transparent,
	                      uint32 checksum );

	// Variante SANS toucher au checksum. Pour les sprites : c'est
	// CTexture::LoadTexture qui l'a deja calcule A PARTIR DU NOM DE FICHIER,
	// et c'est CE checksum-la que le moteur utilisera pour retrouver la
	// texture. Ecraser avec celui de l'en-tete .img (premiere version) rendait
	// les 49 sprites introuvables.
	void	SetGLTextureKeepChecksum( GLuint tex, uint16 w, uint16 h,
	                                  bool transparent );
	GLuint	GetGLTexture() const { return m_gl_texture; }

private:
	// Ajoute « .img.xbx » et lit le fichier. C'est CTexture::LoadTexture qui
	// aura prefixe le nom par « images/ » -- couche a NE PAS court-circuiter,
	// sinon le moteur cherche « PanelSprites/x.img.xbx » au lieu de
	// « images/PanelSprites/x.img.xbx » et ne trouve jamais rien.
	virtual bool	plat_load_texture( const char *p_texture_name,
	                                   bool sprite, bool alloc_vram );

	// Remplacement de texture (CAS : peau, yeux, visage). [SOURCE]
	// XBox/p_NxTexture.cpp:78 recopie les pixels de la nouvelle texture DANS
	// l'objet existant, parce que les materiaux pointent sur lui. Chez nous
	// les maillages COPIENT l'identifiant GL : on garde donc cet identifiant
	// et on recharge l'image de remplacement dedans.
	virtual bool	plat_replace_texture( CTexture *p_texture );

	// Taille UTILE (XBox ActualWidth, texture.cpp:273) : un .img dont
	// l'image est plus petite que sa texture (completee en puissance de 2)
	// ne doit pas etre etiree avec son remplissage (#52).
	virtual uint16	plat_get_width() const			{ return m_util_w ? m_util_w : m_width; }
	virtual uint16	plat_get_height() const			{ return m_util_h ? m_util_h : m_height; }
public:
	// Fraction de la texture occupee par l'image (UV max des sprites).
	float	UtilU() const	{ return ( m_util_w && m_width ) ? (float)m_util_w / m_width : 1.0f; }
	float	UtilV() const	{ return ( m_util_h && m_height ) ? (float)m_util_h / m_height : 1.0f; }
	void	SetUtile( uint16 w, uint16 h )	{ m_util_w = w; m_util_h = h; }
private:
	virtual uint8	plat_get_num_mipmaps() const	{ return 1; }
	virtual bool	plat_is_transparent() const		{ return m_transparent; }

	GLuint	m_gl_texture;
	// Chemin (sans « .img.xbx ») si la texture vient d'un .img, NULL sinon.
	// Seule source des pixels pour plat_replace_texture : on ne garde pas
	// de copie CPU des images, et vitaGL n'a pas de glGetTexImage.
	char	*mp_img_path;
	uint16	m_width;
	uint16	m_height;
	uint16	m_util_w;
	uint16	m_util_h;
	bool	m_transparent;
};


class CVitaTexDict : public CTexDict
{
public:
	CVitaTexDict( uint32 checksum );
	CVitaTexDict( const char *p_tex_dict_name );
	virtual ~CVitaTexDict();

	// Vrai pour les pieces de personnage (isSkin) : seules celles-la sont
	// reellement detruites au dechargement. Voir
	// s_plat_unload_texture_dictionary.
	bool	m_vita_liberable;

private:
	// Sprites : le moteur demande une texture par NOM, sans extension. Le
	// backend Xbox y ajoute « .img.xbx » -- un second format, plus simple que
	// celui des dictionnaires (voir p_NxTexture.cpp).
	virtual CTexture *plat_load_texture( const char *p_texture_name,
	                                     bool sprite, bool alloc_vram );
};


// Remplit une texture depuis un .img.xbx (le nom est deja prefixe et sans
// extension). Rend false si absent ou illisible. Si reuse_tex est non nul,
// l'image est televersee dans CET identifiant GL au lieu d'un nouveau.
bool LoadVitaImgInto( CVitaTexture *p_texture, const char *p_texture_name,
                      GLuint reuse_tex = 0 );


// Charge un fichier .tex dans la table donnee. Rend le nombre de textures
// effectivement creees.
int LoadVitaTextureFile( const char *p_filename,
                         Lst::HashTable< Nx::CTexture > *p_table );

// Meme contenu, deja en memoire (dictionnaire embarque dans une archive).
int LoadVitaTextureMemory( const void *p_data,
                           Lst::HashTable< Nx::CTexture > *p_table );

} // namespace Nx

#endif // __GFX_VITA_P_NXTEXTURE_H__
