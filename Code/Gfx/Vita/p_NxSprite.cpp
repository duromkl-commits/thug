/*****************************************************************************
**  THUG-Vita — backend graphique                                            **
**  Code/Gfx/Vita/p_NxSprite.cpp                                            **
**                                                                          **
**  Sprites 2D : c'est tout le front-end du jeu.                            **
**                                                                          **
**  Le moteur d'origine tient une liste de dessin triee par priorite, dans   **
**  laquelle chaque sprite s'inscrit et se desinscrit (NxXbox::SDraw2D). On  **
**  reprend le principe, en plus direct : un tableau parcouru une fois par   **
**  frame, trie a la volee. Quelques dizaines d'elements au plus -- un tri   **
**  par insertion suffit largement, et surtout il se lit.                    **
**                                                                          **
**  Repere : le moteur place ses elements dans un espace de 640x448 quelle   **
**  que soit la resolution reelle (NxSprite.h). La mise a l'echelle vers     **
**  960x544 se fait ici, au dernier moment.                                  **
*****************************************************************************/

#include <float.h>
#include <core/defines.h>

#include <vitaGL.h>

#include <stdlib.h>
#include <math.h>

#include "vita_log.h"
#include "p_NxSprite.h"
#include "p_NxTexture.h"

// Espace de coordonnees du moteur pour le 2D.
// L'espace logique fait 640 x 480, PAS 640 x 448.
//
// DX9/NX/nx_init.cpp:453 (et XBox/NX/nx_init.cpp:202) :
//     screen_conv_x_multiplier = BackBufferWidth  / 640.0f
//     screen_conv_y_multiplier = BackBufferHeight / 480.0f
//     screen_conv_y_offset     = screen_conv_y_multiplier * 16.0f
//
// Autrement dit les 448 lignes utiles sont CENTREES dans un cadre de 480,
// avec 16 lignes de marge en haut et en bas -- l'overscan des televiseurs de
// l'epoque. Diviser par 448 sans decalage, comme je le faisais, etire le 2D
// de 7 % et le remonte de 18 pixels : le fond du menu debordait en bas et
// laissait une bande vide en haut.
#define ENGINE_2D_W			640.0f
#define ENGINE_2D_H			480.0f
#define ENGINE_2D_Y_MARGIN	16.0f

#define SCREEN_W	960.0f
#define SCREEN_H	544.0f

namespace NxVita
{

static Nx::CVitaSprite **sp_sprites   = NULL;
static int               s_num        = 0;
static int               s_capacity   = 0;

void SpriteRegister( Nx::CVitaSprite *p_sprite )
{
	if( s_num == s_capacity )
	{
		int cap = s_capacity ? ( s_capacity * 2 ) : 64;
		Nx::CVitaSprite **p_new =
			(Nx::CVitaSprite **)realloc( sp_sprites,
			                             cap * sizeof( Nx::CVitaSprite * ));
		if( !p_new )
			return;
		sp_sprites = p_new;
		s_capacity = cap;
	}
	sp_sprites[s_num++] = p_sprite;
}

void SpriteUnregister( Nx::CVitaSprite *p_sprite )
{
	for( int i = 0; i < s_num; ++i )
	{
		if( sp_sprites[i] == p_sprite )
		{
			sp_sprites[i] = sp_sprites[--s_num];
			return;
		}
	}
}

} // namespace NxVita


namespace Nx
{

CVitaSprite::CVitaSprite( CWindow2D *p_window )
	: CSprite( p_window ), m_seq( 0 ), m_seq_pri( 0.0f ), m_seq_hidden( true )
{
	NxVita::SpriteRegister( this );
}

CVitaSprite::~CVitaSprite()
{
	NxVita::SpriteUnregister( this );
}

// L'ordre d'inscription XBox : sSprite nait cache, priorite 0, et rejoint la
// liste quand il devient visible ou change de priorite en etant visible.
static unsigned s_seq_next = 0;

void CVitaSprite::plat_update_hidden()
{
	if( m_hidden != m_seq_hidden )
	{
		m_seq_hidden = m_hidden;
		if( !m_hidden )
			m_seq = ++s_seq_next;
	}
}

void CVitaSprite::plat_update_priority()
{
	if( m_priority != m_seq_pri )
	{
		m_seq_pri = m_priority;
		if( !m_seq_hidden )
			m_seq = ++s_seq_next;
	}
}

} // namespace Nx


namespace NxVita
{

// Garde DESARMEE depuis que la vraie cause est corrigee.
//
// Elle sautait les rideaux noirs opaques plein ecran. Le rideau qu'elle
// masquait etait le cache d'une cinematique que rien ne terminait faute de
// lecteur video (voir CMovieManager::Update) : la cause etant traitee, plus
// aucun rideau n'est cree, et cette garde effacerait desormais des fondus
// legitimes. On la laisse, desarmee, comme point d'observation.
//
// On la garde active pour pouvoir continuer a travailler sur le rendu du
// niveau. Elle effacerait aussi les fondus legitimes -- a retirer des que la
// levee du fondu est comprise.
bool g_vita_skip_black_curtain = false;

// Commande � spr � : journalise TOUS les sprites de la prochaine image, avec
// leur sort. La salve periodique ci-dessous ne montre que les 40 premiers
// dessines : un sprite absent de cette salve n'est pas forcement absent de
// l'ecran (piege rencontre en enquetant sur le fond du menu).
bool g_vita_sprite_dump = false;

static void dump_sprite( int i, Nx::CVitaSprite *p, const char *p_sort, unsigned gl_tex )
{
	VLOG( "SPR", "#%d pri=%d tex=%u pos=(%.0f,%.0f) taille=%dx%d x(%.2f,%.2f) rgb=%d,%d,%d a=%d : %s",
	      i, (int)p->GetPri(), gl_tex, p->PosX(), p->PosY(), (int)p->W(), (int)p->H(),
	      p->ScaleX(), p->ScaleY(), (int)p->Color().r, (int)p->Color().g, (int)p->Color().b, (int)p->Color().a, p_sort );
}

// "mx2 0/1" : modulation x2 des sprites et textes comme XBox (#52).
int g_vita_mx2 = 1;

// "snt 0/1" : sprites SANS texture dessines comme XBox (#65). [SOURCE]
// XBox/NX/sprite.cpp:275 -- sans texture, sSprite::BeginDraw prend
// PixelShader5 ("mov r0, v0" : la couleur de sommet seule, 0..255, sans
// doublage) et le quad est dessine quand meme. On les sautait : la jauge de
// memoire de l'editeur de parc (percent_bar, ParkEdMenu.q:485, deux
// SpriteElement sans texture) n'apparaissait pas.
int g_vita_sprites_sans_tex = 1;

void RenderSprites2D( float bas, float haut )
{
	if( s_num == 0 )
		return;

	// Tri par priorite croissante : le plus prioritaire est dessine en
	// dernier, donc par-dessus. Tri par insertion, sur quelques dizaines
	// d'elements deja presque tries d'une frame a l'autre.
	for( int i = 1; i < s_num; ++i )
	{
		Nx::CVitaSprite *p = sp_sprites[i];
		int j = i - 1;
		// A priorite egale, la liste XBox (NX/sprite.cpp:177) insere un
		// sprite DEVANT ses egaux : le dernier inscrit est dessine en premier,
		// donc dessous. Le coche des cases (cree avant la case, meme
		// z_priority) passait sinon sous la case.
		while(( j >= 0 ) && (( sp_sprites[j]->GetPri() > p->GetPri())
		                     || (( sp_sprites[j]->GetPri() == p->GetPri())
		                         && ( sp_sprites[j]->Seq() < p->Seq()))))
		{
			sp_sprites[j + 1] = sp_sprites[j];
			--j;
		}
		sp_sprites[j + 1] = p;
	}

	// Projection orthographique en coordonnees ecran, origine en haut a
	// gauche -- c'est la convention du moteur pour le 2D.
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0.0f, SCREEN_W, SCREEN_H, 0.0f, -1.0f, 1.0f );

	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	// XBox PixelShader4 (sprites ET textes) : r0 = mul_x2( v0, t0 ) -- couleur
	// de sommet sur 0..255 (moteur 0..128 = 1.0), multipliee par la texture PUIS
	// doublee, sature. Un rgba moteur de 255 donne donc 2x la texture : rouge du
	// theme des menus (#52). On reproduit par GL_COMBINE + GL_RGB/ALPHA_SCALE 2.
	if( g_vita_mx2 )
	{
		glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE );
		glTexEnvi( GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE );
		glTexEnvi( GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE );
		glTexEnvi( GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR );
		glTexEnvi( GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_TEXTURE );
		glTexEnvi( GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_PRIMARY_COLOR );
		{ const GLfloat deux = 2.0f;
		  glTexEnvfv( GL_TEXTURE_ENV, GL_RGB_SCALE, &deux );
		  glTexEnvfv( GL_TEXTURE_ENV, GL_ALPHA_SCALE, &deux ); }
	}

	// Tableaux cote client : aucun VBO ne doit rester lie.
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );

	const float sx = SCREEN_W / ENGINE_2D_W;
	const float sy = SCREEN_H / ENGINE_2D_H;

	int drawn = 0;

	const bool dump = g_vita_sprite_dump;
	if( haut >= FLT_MAX )
		g_vita_sprite_dump = false;	// derniere tranche de l'image
	if( dump && ( bas <= -FLT_MAX ))
		VLOG( "SPR", "=== %d sprites enregistres ===", s_num );

	for( int i = 0; i < s_num; ++i )
	{
		Nx::CVitaSprite *p = sp_sprites[i];
		if(( p->GetPri() <= bas ) || ( p->GetPri() > haut ))
			continue;

		if( p->IsHiddenNow())
		{
			if( dump ) dump_sprite( i, p, "CACHE", 0 );
			continue;
		}

		Nx::CTexture *p_tex = p->GetTex();
		if( !p_tex && !g_vita_sprites_sans_tex )
		{
			if( dump ) dump_sprite( i, p, "SANS TEXTURE", 0 );
			continue;
		}

		GLuint gl_tex = p_tex ? ((Nx::CVitaTexture *)p_tex )->GetGLTexture() : 0;
		if( p_tex && !gl_tex )
		{
			if( dump ) dump_sprite( i, p, "TEXTURE GL NULLE", 0 );
			continue;
		}
		if( dump )
			dump_sprite( i, p, p_tex ? "dessine" : "dessine SANS TEXTURE (#65)", gl_tex );

		float w = (float)p->W() * p->ScaleX();
		float h = (float)p->H() * p->ScaleY();

		// L'ancre va de -1 (bord gauche/haut) a +1 (bord droit/bas).
		float ox = (( p->AnchorX() + 1.0f ) * 0.5f ) * w;
		float oy = (( p->AnchorY() + 1.0f ) * 0.5f ) * h;

		float x0 = ( p->PosX() - ox ) * sx;
		float y0 = ( p->PosY() - oy + ENGINE_2D_Y_MARGIN ) * sy;
		float x1 = x0 + w * sx;
		float y1 = y0 + h * sy;

		Image::RGBA c = p->Color();

		// Les couleurs du moteur sont sur 0..128 (heritage PS2), pas 0..255 :
		// prendre 255 comme plein assombrirait tout de moitie.
		// Sans texture : PixelShader5, couleur de sommet seule sur 0..255 (#65).
		const float kc = ( g_vita_mx2 || !p_tex ) ? 255.0f : 128.0f;	// mx2 : echelle XBox 0..255 + x2
		float cr = (float)c.r / kc;
		float cg = (float)c.g / kc;
		float cb = (float)c.b / kc;
		float ca = (float)c.a / kc;
		if( cr > 1.0f ) cr = 1.0f;
		if( cg > 1.0f ) cg = 1.0f;
		if( cb > 1.0f ) cb = 1.0f;
		if( ca > 1.0f ) ca = 1.0f;

		if( !p_tex )
		{
			// Sprite sans texture (#65) : aplat de la couleur de sommet. Le
			// facteur x2 de GL_RGB_SCALE ne s'applique qu'a l'etage de
			// texture, eteint ici.
			glDisable( GL_TEXTURE_2D );
			glDisableClientState( GL_TEXTURE_COORD_ARRAY );
		}
		else
		{
		glEnableClientState( GL_TEXTURE_COORD_ARRAY );
		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, gl_tex );
		// Bords FIGES, comme la Xbox : SDraw2D::DrawAll pose
		// RS_UVADDRESSMODE0 = 0x00010001 (XBox/NX/sprite.cpp:128), traduit en
		// D3DTADDRESS_CLAMP en U et V (render.cpp:1598). Laisses en GL_REPEAT
		// (defaut GL), les sprites agrandis x1,5 et filtres lineairement
		// echantillonnaient le bord OPPOSE de leur texture : liseres clairs
		// et joints visibles dans les cadres de menu (#20).
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		}
		glColor4f( cr, cg, cb, ca );

		float rot = p->Rotation();
		glPushMatrix();

		// PAS de glBegin/glEnd : le mode immediat de VitaGL puise dans un
		// « legacy pool » dont la taille est le PREMIER argument de
		// vglInitExtended -- et nous y passons 0. glVertex3f ecrivait donc
		// dans un tampon nul (Data abort constate sur psp2core).
		// Les tableaux de sommets n'ont pas cette dependance.
		float verts[12] = { x0, y0, 0.0f,
		                    x1, y0, 0.0f,
		                    x1, y1, 0.0f,
		                    x0, y1, 0.0f };
		// ROTATION comme XBox (NX/sprite.cpp:338) : les coins, relatifs a la
		// position du sprite, tournent dans l'espace MOTEUR 640x480 (pixels
		// carres), PUIS passent a l'ecran. Avant : rotation apres la mise a
		// l'echelle 960x544 (non uniforme) et pivot sans la marge verticale --
		// l'icone de position (switch/nollie, rot_angle 90) tombait sur la barre
		// SPECIAL (#61).
		if( rot != 0.0f )
		{
			const float c = cosf( rot ), sn = sinf( rot );
			const float lx[4] = { -ox, w - ox, w - ox, -ox };
			const float ly[4] = { -oy, -oy, h - oy, h - oy };
			for( int k = 0; k < 4; ++k )
			{
				const float rx = lx[k] * c - ly[k] * sn;
				const float ry = lx[k] * sn + ly[k] * c;
				verts[k * 3 + 0] = ( p->PosX() + rx ) * sx;
				verts[k * 3 + 1] = ( p->PosY() + ry + ENGINE_2D_Y_MARGIN ) * sy;
			}
		}
		// V inverse. Raison : Direct3D place la ligne 0 en HAUT de l'image,
		// OpenGL en BAS. Les textures de THUG sont ecrites pour D3D/Xbox, donc
		// televersees telles quelles elles sortent a l'envers.
		//
		// [RESOLU] Je decrivais le defaut comme un « miroir horizontal » -- il
		// etait VERTICAL. Deux hypotheses ont ete refutees avant d'y arriver :
		// l'axe U (inverser U deplace le fond sans redresser les lettres) et le
		// desentrelacement (un .img decode hors ligne donne une image
		// parfaitement coherente). Le pas manquant etait simplement de tester
		// l'autre axe.
		//
		// Pourquoi ici et pas au televersement : les textures DXT sont des blocs
		// 4x4 compresses, qu'on ne peut pas retourner ligne par ligne sans les
		// decompresser. Retourner V a l'affichage marche pour tous les formats.
		// XBox sprite.cpp:303 : u1 = Actual/Base, v1 = 1 - Actual/Base -- seule
		// la partie utile de la texture est lue (#52).
		const float uu = p_tex ? ((Nx::CVitaTexture *)p_tex )->UtilU() : 1.0f;
		const float vv = p_tex ? 1.0f - ((Nx::CVitaTexture *)p_tex )->UtilV() : 0.0f;
		const float uvs[8]    = { 0.0f, 1.0f,
		                          uu,   1.0f,
		                          uu,   vv,
		                          0.0f, vv };

		// Un rideau plein ecran, opaque et sombre, expliquerait a lui seul un
		// niveau « charge mais noir ». On ne journalise que ces quads-la, et
		// avec un battement : ceux qui couvrent au moins 80 % de l'ecran.
		if((( x1 - x0 ) >= 0.8f * SCREEN_W ) && (( y1 - y0 ) >= 0.8f * SCREEN_H ))
		{
			static int s_big = 0;
			if(( ++s_big % 120 ) == 1 )
				VLOG( "2D", "PLEIN ECRAN : (%.0f %.0f)-(%.0f %.0f) "
				            "couleur rgba %.2f %.2f %.2f %.2f tex=%u",
				      x0, y0, x1, y1, cr, cg, cb, ca, (unsigned)gl_tex );

			// OUTIL DE DIAGNOSTIC, PAS UN CORRECTIF.
			//
			// Un rideau noir opaque recouvre le niveau, et son alpha ne bouge
			// pas d'un relevé a l'autre. Le sauter repond a UNE question :
			// est-il le seul obstacle, ou le decor est-il invisible pour une
			// autre raison ? Ce que l'on voit alors est l'etat reel du jeu.
			//
			// A RETIRER des que la cause du fondu bloque est trouvee : en
			// l'etat, ce test effacerait aussi les vrais fondus du jeu.
			if( g_vita_skip_black_curtain
			    && ( cr < 0.02f ) && ( cg < 0.02f ) && ( cb < 0.02f )
			    && ( ca > 0.98f ))
			{
				// glPushMatrix a deja eu lieu plus haut : sortir sans depiler
				// desequilibrerait la pile de matrices frame apres frame.
				glPopMatrix();
				continue;
			}
		}

		// Le rectangle blanc du menu resiste a tous les correctifs 3D : il
		// est peut-etre 2D. Salve periodique listant chaque sprite dessine --
		// position, taille, rotation, texture -- pour l'identifier.
		{
			static int s_dump = 0;
			if(( s_dump++ % 3600 ) < 40 )
				VLOG( "2D", "sprite tex=%u (%.0f,%.0f)-(%.0f,%.0f) rot=%.2f "
				            "rgba %.2f %.2f %.2f %.2f",
				      (unsigned)gl_tex, x0, y0, x1, y1, rot, cr, cg, cb, ca );

			// Les GRANDS sprites (fonds) en detail : le resultat ecran ne dit
			// pas lequel des trois termes -- taille de texture, echelle,
			// position -- s'ecarte. 512 x 1,25 = 640 pile : si l'echelle X
			// vaut 1,25 le fond touche le bord droit, sinon non.
			if(( p->W() >= 256 ) && ( p->H() >= 128 ))
			{
				static int s_big = 0;
				if(( s_big++ % 1200 ) < 6 )
					VLOG( "2D", "FOND tex=%u dims %dx%d scale (%.4f %.4f) "
					            "pos (%.1f %.1f) ancre (%.1f %.1f)",
					      (unsigned)gl_tex, (int)p->W(), (int)p->H(),
					      p->ScaleX(), p->ScaleY(), p->PosX(), p->PosY(),
					      p->AnchorX(), p->AnchorY() );
			}
		}

		glVertexPointer( 3, GL_FLOAT, 0, verts );
		glTexCoordPointer( 2, GL_FLOAT, 0, uvs );
		glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

		glPopMatrix();
		++drawn;
	}

	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );
	if( g_vita_mx2 )
	{
	{ const GLfloat un = 1.0f;
	  glTexEnvfv( GL_TEXTURE_ENV, GL_RGB_SCALE, &un );
	  glTexEnvfv( GL_TEXTURE_ENV, GL_ALPHA_SCALE, &un ); }
	glTexEnvi( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	}
	glDisable( GL_BLEND );
	glDisable( GL_TEXTURE_2D );

	// Battement : dire POURQUOI rien ne sort est aussi utile que dire que
	// quelque chose sort. On compte separement les causes de rejet.
	{
		static int s_frames = 0;
		if(( haut >= FLT_MAX ) && (( ++s_frames % 120 ) == 1 ))
		{
			int hidden = 0, no_tex = 0;
			for( int i = 0; i < s_num; ++i )
			{
				if( sp_sprites[i]->IsHiddenNow())	++hidden;
				else if( !sp_sprites[i]->GetTex())	++no_tex;
			}
			VLOG( "2D", "sprites : %d inscrits, %d caches, %d sans texture, "
			            "%d dessines", s_num, hidden, no_tex, drawn );
		}
	}
}

} // namespace NxVita
