/*****************************************************************************
**  THUG-Vita — backend VitaGL                                              **
**  Code/Gfx/Vita/p_nx_managers.cpp                                         **
**                                                                          **
**  Stubs des gestionnaires de la couche Nx : lumières, brouillard, écran   **
**  de chargement, dictionnaires de textures, sprites, imposteurs, fontes.  **
**                                                                          **
**  GÉNÉRÉ par vita/tools/gen_stubs.py. Voir p_nx.cpp pour la démarche.     **
*****************************************************************************/

#include <gfx/nx.h>
#include <gfx/NxLightMan.h>
#include <gfx/NxMiscFX.h>
#include <gfx/NxLoadScreen.h>
#include <gfx/NxTexMan.h>
#include <gfx/NxSprite.h>
#include <gfx/NxImposter.h>
#include <gfx/NxFontMan.h>

#include <gfx/nxtexture.h>
#include <gfx/NxLight.h>
#include "p_NxTexture.h"
#include "p_world_render.h"
#include "p_NxFont.h"

#include <vitaGL.h>

#include "p_NxLight.h"
#include "p_shader_decor.h"
#include "p_gamma.h"
#include "p_stub.h"
#include "vita_log.h"

namespace Nx
{

void	CLightManager::s_plat_update_engine()
{
	VITA_STUB();
}

void	CLightManager::s_plat_update_lights()
{
	VITA_STUB();
}

void	CLightManager::s_plat_update_colors()
{
	VITA_STUB();
}

bool	CLightManager::s_plat_set_light_ambient_color()
{
	// Le skater du menu doit etre une silhouette NOIRE. Si l'eclairage demande
	// est sombre, c'est bien de la que vient le noir -- et le rendre a pleine
	// couleur, comme on le fait, est le defaut.
	VLOG( "LUM", "ambiante (%d %d %d) | diffuse0 (%d %d %d) dir0 (%.2f %.2f %.2f)",
	      s_world_lights.m_light_ambient_rgba.r,
	      s_world_lights.m_light_ambient_rgba.g,
	      s_world_lights.m_light_ambient_rgba.b,
	      s_world_lights.m_light_diffuse_rgba[0].r,
	      s_world_lights.m_light_diffuse_rgba[0].g,
	      s_world_lights.m_light_diffuse_rgba[0].b,
	      s_world_lights.m_light_direction[0][0],
	      s_world_lights.m_light_direction[0][1],
	      s_world_lights.m_light_direction[0][2] );
	return true;
}

bool	CLightManager::s_plat_set_light_direction(int light_index)
{
	VITA_STUB();
	return false;
}

bool	CLightManager::s_plat_set_light_diffuse_color(int light_index)
{
	VITA_STUB();
	return false;
}

// Ces trois lecteurs RENDAIENT DU VIDE alors que le moteur venait de ranger la
// valeur juste a cote (NxLightMan.cpp:110, 135, 160 : le setter generique
// ecrit dans s_world_lights, PUIS appelle la version plateforme). Rendre
// Image::RGBA() ne disait pas « je ne sais pas », il disait « l'ambiante est
// noire et la diffuse aussi » -- a tout le moteur, pour toujours.
//
// C'est la meme erreur que les stubs qui renvoient false : un accesseur sans
// specificite plateforme doit rendre la donnee, pas un objet vide.
Image::RGBA	CLightManager::s_plat_get_light_ambient_color()
{
	return s_world_lights.m_light_ambient_rgba;
}

const Mth::Vector &	CLightManager::s_plat_get_light_direction(int light_index)
{
	Dbg_MsgAssert(( light_index >= 0 ) && ( light_index < MAX_LIGHTS ),
	              ( "indice de lumiere hors bornes" ));
	return s_world_lights.m_light_direction[light_index];
}

Image::RGBA	CLightManager::s_plat_get_light_diffuse_color(int light_index)
{
	Dbg_MsgAssert(( light_index >= 0 ) && ( light_index < MAX_LIGHTS ),
	              ( "indice de lumiere hors bornes" ));
	return s_world_lights.m_light_diffuse_rgba[light_index];
}

// La luminosite d'un modele est un facteur MODULANT, range en protected. Une
// classe derivee est le moyen prevu d'y acceder -- et il faut y acceder, car
// c'est par la que passe la silhouette noire : les cinematiques appellent
// GetModelLights()->SetBrightness( 0.0f ) (cutscenedetails.cpp:4732, commente
// « turn off skater brightness, so that it matches the other objects »).
//
// La formule (NxLight.cpp:218) est
//     brightness_ambiante = 1 + ( 2 x brightness - 1 ) x modulation
// avec 0,5 en ambiant et { 0,7 ; 1,0 } en diffus : a zero, le modele tombe a
// la moitie de l'ambiante et perd toute la deuxieme diffuse.
CModelLights *	CLightManager::s_plat_create_model_lights()
{
	return new CVitaModelLights();
}

bool	CLightManager::s_plat_free_model_lights(CModelLights *p_model_lights)
{
	VITA_STUB();
	return false;
}

// Brouillard (issue #45) : meme contrat que XBox/p_nxmiscfx.cpp:1177-1275.
// L'etat et son application sont dans p_shader_decor.cpp (Brouillard*).
void	CFog::s_plat_enable_fog(bool enable)
{
	NxVita::BrouillardActiver( enable );
}

void	CFog::s_plat_set_fog_near_distance(float distance)
{
	NxVita::BrouillardDistance( distance );
}

// XBox : " This is no longer a valid call " -- sans effet.
void	CFog::s_plat_set_fog_exponent(float exponent)
{
}

void	CFog::s_plat_set_fog_rgba(Image::RGBA rgba)
{
	NxVita::BrouillardCouleur( rgba.r, rgba.g, rgba.b, rgba.a );
}

// Vides sur XBox aussi.
void	CFog::s_plat_set_fog_color(void)
{
}

void	CFog::s_plat_fog_update(void)
{
}

// Ecran de chargement courant. Garde entre l'affichage et le masquage : le
// moteur appelle s_plat_display une fois, puis charge plusieurs secondes sans
// jamais repasser par ici.
static Nx::CVitaTexture *sp_load_screen = NULL;


// Dessine la texture courante en plein ecran. Separee de la presentation pour
// pouvoir etre rappelee pendant le chargement : sans cela, le premier
// vglSwapBuffers du moteur remplacerait l'image par un tampon vide.
static void dessine_ecran_chargement( void )
{
	if( !sp_load_screen )
		return;

	glMatrixMode( GL_PROJECTION );
	glPushMatrix();
	glLoadIdentity();
	glOrtho( 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f );
	glMatrixMode( GL_MODELVIEW );
	glPushMatrix();
	glLoadIdentity();

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_LIGHTING );
	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, sp_load_screen->GetGLTexture() );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

	// Tampons delies : si un tableau de sommets du decor est encore attache,
	// glVertexPointer lirait dedans au lieu de notre quad.
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );

	static const float pos[8] = { 0,0,  1,0,  1,1,  0,1 };

	// V INVERSE, comme pour les sprites (p_NxSprite.cpp:244) : les textures du
	// jeu sont stockees a l'envers par rapport a l'orientation d'OpenGL.
	// Retourner V a l'affichage marche pour tous les formats, y compris les
	// DXT, dont les blocs 4x4 compresses ne se retournent pas ligne a ligne.
	static const float uv[8]  = { 0,1,  1,1,  1,0,  0,0 };

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );
	glDisableClientState( GL_NORMAL_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, pos );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );

	// Etat restitue : poser un etat GL sans le rendre se paie chez le voisin,
	// lecon deja payee trois fois sur ce portage.
	glBindTexture( GL_TEXTURE_2D, 0 );
	glEnable( GL_DEPTH_TEST );
	glEnable( GL_CULL_FACE );

	glMatrixMode( GL_MODELVIEW );
	glPopMatrix();
	glMatrixMode( GL_PROJECTION );
	glPopMatrix();
	glMatrixMode( GL_MODELVIEW );
}


// Appelee a chaque frame par s_plat_post_render, AVANT la presentation.
//
// Sans cela l'image n'est peinte qu'une fois, et la premiere frame presentee
// par le moteur pendant le chargement la remplace par un tampon vide -- d'ou
// un ecran de chargement qui clignote ou disparait aussitot. Ne swappe pas :
// c'est l'appelant qui presente, sinon on presenterait deux fois par frame.
void VitaDessineEcranChargementSiActif( void )
{
	if( !sp_load_screen )
		return;
	dessine_ecran_chargement();
}


void	CLoadScreen::s_plat_display(const char *filename, bool just_freeze, bool blank)
{
	// « just_freeze » : garder a l'ecran ce qui s'y trouve deja.
	if( just_freeze )
		return;

	if( blank || !filename )
	{
		glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
		glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		vglSwapBuffers( GL_FALSE );
		return;
	}

	if( sp_load_screen )
	{
		delete sp_load_screen;
		sp_load_screen = NULL;
	}

	// On passe par CTexture::LoadTexture et NON par le chargeur directement :
	// c'est cette couche qui prefixe « images/ » et ajoute « .img.xbx ». La
	// court-circuiter donne un fichier introuvable, en silence.
	Nx::CVitaTexture *p_tex = new Nx::CVitaTexture;
	if( !p_tex->LoadTexture( filename, true, false ))
	{
		VLOG( "LOAD", "ecran de chargement '%s' introuvable", filename );
		delete p_tex;
		return;
	}

	sp_load_screen = p_tex;
	VLOG( "LOAD", "ecran de chargement '%s' (%dx%d)", filename,
	      (int)p_tex->GetWidth(), (int)p_tex->GetHeight() );

	// Rampe gamma (#45) : cette presentation-ci ne passe pas par
	// s_plat_post_render. Sans effet sur un FBO deja lie (appel en milieu
	// d'image) : la fin le referme et presente.
	NxVita::GammaImageDebut();
	dessine_ecran_chargement();
	NxVita::GammaImageFin();
	vglSwapBuffers( GL_FALSE );
}


// Note conservee, elle explique un piege reel : ce point du code a longtemps
// contenu un vglSwapBuffers de trop. Justifie au palier 2, quand l'ecran de
// chargement etait seul capable de presenter quoi que ce soit, il ajoutait
// depuis s_plat_post_render un second swap en milieu de frame -- presentant
// un tampon ne contenant que l'effacement, d'ou le � plan noir sur fond
// bleu �. Le swap ci-dessus est different : il presente une IMAGE.

void	CLoadScreen::s_plat_start_loading_bar(float seconds)
{
	VITA_STUB();
}

void	CLoadScreen::s_plat_hide()
{
	if( sp_load_screen )
	{
		delete sp_load_screen;
		sp_load_screen = NULL;
	}
}

void	CLoadScreen::s_plat_update_bar_properties()
{
	VITA_STUB();
}

void	CLoadScreen::s_plat_clear()
{
	VITA_STUB();

	// Premier vrai rendu du portage : effacer le framebuffer.
	// Bleu nuit plutôt que noir, pour distinguer « le moteur efface » de
	// « rien ne se passe » — un écran noir est ambigu.
	glClearColor( 0.0f, 0.0f, 0.25f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
}

CTexDict *	CTexDictManager::s_plat_load_texture_dictionary(const char *p_tex_dict_name, bool is_level_data, uint32 texDictOffset, bool isSkin, bool forceTexDictLookup)
{
	VITA_STUB();

	// Dictionnaire VIDE mais VALIDE, pas NULL.
	//
	// Le palier 3 veut la géométrie sans les textures : le moteur a besoin
	// d'un dictionnaire pour continuer à charger une scène, mais pas encore
	// de son contenu. CTexDict a justement un constructeur documenté
	// « loads nothing » (NxTexture.h:164) — c'est exactement ce cas.
	//
	// Rendre NULL bloquait le chargement : c'est ce qui arrêtait le moteur
	// juste après les 60 squelettes.
	// Vrai chargement depuis le .tex.xbx.
	CVitaTexDict *p_dict = new CVitaTexDict( p_tex_dict_name );
	p_dict->m_vita_liberable = isSkin;
	return p_dict;
}

CTexDict *	CTexDictManager::s_plat_load_texture_dictionary(uint32 checksum, uint32* pData, int dataSize, bool is_level_data, uint32 texDictOffset, bool isSkin, bool forceTexDictLookup)
{
	VITA_STUB();
	// Dictionnaire deja en memoire : c'est par ici que passent les sprites
	// d'interface, d'ou l'importance de ne PAS se contenter d'un objet vide.
	CVitaTexDict *p_dict = new CVitaTexDict( checksum );
	LoadVitaTextureMemory( pData, p_dict->GetTexLookup());
	p_dict->m_vita_liberable = isSkin;
	return p_dict;
}

// Dictionnaires dont le moteur s'est defait mais que le decor utilise encore.
// 32 debordait au change_level du Story Mode (NJ -> NY) : dictionnaires
// jamais detruits, VRAM perdue a chaque niveau (#47, #51).
#define MAX_DICTS_ATTENTE 256
static CVitaTexDict *sp_dicts_attente[MAX_DICTS_ATTENTE];
static int           s_nb_dicts_attente = 0;
extern bool g_vita_image_ouverte;
extern int g_vita_differer_lib;
void VitaCompterFinishEvite( void );
void VitaLibererMeshDifferes( void );
static CVitaTexDict *sp_dicts_differes[MAX_DICTS_ATTENTE];
static int           s_nb_dicts_differes = 0;
// Tete d'image (s_plat_pre_render) : maillages et dictionnaires differes (#48).
void VitaLibererDifferes( void )
{
	if( s_nb_dicts_differes )
	{
		glFinish();
		for( int i = 0; i < s_nb_dicts_differes; ++i )
			delete sp_dicts_differes[i];
		s_nb_dicts_differes = 0;
	}
	VitaLibererMeshDifferes();
}

}	// namespace Nx

void NxVita::LibererDictsEnAttente( void )
{
	if( Nx::s_nb_dicts_attente )
		VLOG( "TEX", "%d dictionnaires de niveau detruits", Nx::s_nb_dicts_attente );
	for( int i = 0; i < Nx::s_nb_dicts_attente; ++i )
		delete Nx::sp_dicts_attente[i];
	Nx::s_nb_dicts_attente = 0;
}

namespace Nx
{

CTexDict *	CTexDictManager::s_plat_create_texture_dictionary(uint32 checksum)
{
	VITA_STUB();
	return new CVitaTexDict( checksum );
}

bool	CTexDictManager::s_plat_unload_texture_dictionary(CTexDict *p_tex_dict)
{
	// [SOURCE] XBox/p_nxtexman.cpp:59 : delete p_tex_dict. Le stub ne
	// liberait rien : l'appelant (NxTexMan.cpp) retirait deja le dictionnaire
	// de sa table, donc chaque reconstruction du skater (un changement de
	// visage) RECHARGEAIT ses 9 dictionnaires sans jamais rendre les anciens.
	// Quelques changements plus tard, le pool de textures de vitaGL etait
	// plein et les nouvelles textures sortaient NOIRES -- issue #11.
	//
	// Pieces de personnage : tout de suite. Les autres (niveau, ciel) : le
	// decor garde les identifiants GL de leurs textures tant que le monde
	// n'est pas vide -- les detruire avant ferait dessiner des textures
	// mortes. Ils attendent donc MondeSceneRetiree (issue #32 : jusque-la ils
	// n'etaient JAMAIS detruits, ~24 Mo de VRAM perdus par niveau).
	CVitaTexDict *p_vita = static_cast< CVitaTexDict * >( p_tex_dict );
	if( !p_vita )
		return false;
	if( p_vita->m_vita_liberable || NxVita::MondeVide())
	{
		// #48 : en pleine image, pas de glFinish (il coupe la scene) : file.
		if( g_vita_image_ouverte && g_vita_differer_lib && ( s_nb_dicts_differes < MAX_DICTS_ATTENTE ))
		{
			sp_dicts_differes[s_nb_dicts_differes++] = p_vita;
			VitaCompterFinishEvite();
			return true;
		}
		glFinish();		// textures peut-etre encore lues : voir s_plat_finish_rendering
		delete p_vita;
		return true;
	}
	if( s_nb_dicts_attente < MAX_DICTS_ATTENTE )
		sp_dicts_attente[s_nb_dicts_attente++] = p_vita;
	else
		VLOG( "TEX", "!! file des dictionnaires a liberer pleine : %08x perdu",
		      (unsigned)p_vita->GetChecksum());
	return true;
}

void	CSprite::plat_enable_constant_z_value(bool enable)
{
	VITA_STUB();
}

void	CSprite::plat_set_constant_z_value(Nx::ZBufferValue z)
{
	VITA_STUB();
}

Nx::ZBufferValue	CSprite::plat_get_constant_z_value()
{
	VITA_STUB();
	return Nx::ZBufferValue();
}

Nx::CFont *	CFontManager::s_plat_load_font(const char *pName)
{
	VITA_STUB();
	// Rendre NULL arretait le moteur net : l'appelant (NxFontMan.cpp:37)
	// enchaine sans verifier sur mp_font->SetSpacings(). On rend donc un
	// objet vide mais valide -- CFont est concrete, toutes ses virtuelles
	// plat_* ont une implementation neutre. Le rendu du texte viendra au
	// palier 4 ; ici on veut seulement laisser le moteur avancer.
	Nx::CVitaFont *p_font = new Nx::CVitaFont;
	p_font->Load( pName );
	return p_font;
}

void	CFontManager::s_plat_unload_font(Nx::CFont *pFont)
{
	VITA_STUB();
	if( pFont )
		pFont->Unload();
}

} // namespace Nx
