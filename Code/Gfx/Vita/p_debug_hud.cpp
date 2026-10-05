/*****************************************************************************
**  THUG-Vita — affichage de debogage                                        **
**  Code/Gfx/Vita/p_debug_hud.cpp                                            **
**                                                                          **
**  Pourquoi : jusqu'ici, repondre a « combien de fps ? » ou « le culling    **
**  est-il actif ? » demandait un aller-retour FTP et la lecture d'un        **
**  journal de plusieurs milliers de lignes. Ces questions se posent a       **
**  chaque essai ; elles doivent se lire A L'ECRAN.                          **
**                                                                          **
**  Police EMBARQUEE, 5x7 pixels, dessinee au quad : celles du jeu           **
**  dependent d'un dictionnaire charge, donc indisponibles pendant un        **
**  chargement -- precisement quand on veut voir ce qui se passe.            **
*****************************************************************************/

#include <core/defines.h>
#include <vitaGL.h>
#include <stdio.h>
#include <string.h>

#include "p_debug_hud.h"
#include "p_world_render.h"
#include "p_NxModel.h"

namespace NxVita
{

bool g_vita_hud = false;

// Etat du decor de la derniere frame. RenderWorld le depose ici plutot que
// l'affichage n'aille le chercher : c'est lui qui le connait.
static int s_meshes_drawn = 0;
static int s_meshes_total = 0;

void SetHudWorldStats( int drawn, int total )
{
	s_meshes_drawn = drawn;
	s_meshes_total = total;
}

// --- police 5x7 -------------------------------------------------------------
// Un bit par pixel, 7 octets par glyphe (5 bits utiles). Couvre chiffres,
// majuscules et quelques symboles -- assez pour des compteurs et des noms de
// reglages, pas pour des phrases.
static const unsigned char s_font[][7] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // espace
	{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
	{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
	{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
	{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
	{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
	{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
	{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
	{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
	{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
	{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
	{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, // .
	{0x00,0x00,0x0C,0x00,0x0C,0x00,0x00}, // :
	{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, // -
	{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, // +
	{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
	{0x1E,0x11,0x1E,0x11,0x11,0x11,0x1E}, // B
	{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
	{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, // D
	{0x1F,0x10,0x1E,0x10,0x10,0x10,0x1F}, // E
	{0x1F,0x10,0x1E,0x10,0x10,0x10,0x10}, // F
	{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, // G
	{0x11,0x11,0x1F,0x11,0x11,0x11,0x11}, // H
	{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
	{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, // J
	{0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
	{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
	{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
	{0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
	{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
	{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, // P
	{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, // Q
	{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, // R
	{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, // S
	{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
	{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, // U
	{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, // V
	{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, // W
	{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
	{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, // Y
	{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, // Z
};

static int glyph_index( char c )
{
	if( c == ' ' ) return 0;
	if(( c >= '0' ) && ( c <= '9' )) return 1 + ( c - '0' );
	if( c == '.' ) return 11;
	if( c == ':' ) return 12;
	if( c == '-' ) return 13;
	if( c == '+' ) return 14;
	if(( c >= 'A' ) && ( c <= 'Z' )) return 15 + ( c - 'A' );
	if(( c >= 'a' ) && ( c <= 'z' )) return 15 + ( c - 'a' );
	return 0;
}

#define HUD_PX	2		/* taille d'un pixel de police, a l'ecran */

static void draw_text( float x, float y, const char *p_txt,
                       float r, float g, float b )
{
	glColor4f( r, g, b, 1.0f );

	float quads[6 * 2 * 64];		/* 64 pixels allumes par lot */
	int n = 0;

	for( const char *p = p_txt; *p; ++p )
	{
		const unsigned char *gl = s_font[glyph_index( *p )];
		for( int ligne = 0; ligne < 7; ++ligne )
		{
			for( int col = 0; col < 5; ++col )
			{
				if( !( gl[ligne] & ( 1 << ( 4 - col ))))
					continue;
				const float px = x + col * HUD_PX;
				const float py = y + ligne * HUD_PX;
				const float q[12] = {
					px, py,  px + HUD_PX, py,  px + HUD_PX, py + HUD_PX,
					px, py,  px + HUD_PX, py + HUD_PX,  px, py + HUD_PX };
				memcpy( &quads[n * 12], q, sizeof( q ));
				if( ++n == 64 )
				{
					glVertexPointer( 2, GL_FLOAT, 0, quads );
					glDrawArrays( GL_TRIANGLES, 0, n * 6 );
					n = 0;
				}
			}
		}
		x += 6 * HUD_PX;
	}

	if( n > 0 )
	{
		glVertexPointer( 2, GL_FLOAT, 0, quads );
		glDrawArrays( GL_TRIANGLES, 0, n * 6 );
	}
}


void DrawDebugHud( float ms_frame, int meshes_drawn, int meshes_total )
{
	if( !g_vita_hud )
		return;

	glMatrixMode( GL_PROJECTION );
	glPushMatrix();
	glLoadIdentity();
	glOrtho( 0.0f, 960.0f, 544.0f, 0.0f, -1.0f, 1.0f );
	glMatrixMode( GL_MODELVIEW );
	glPushMatrix();
	glLoadIdentity();

	glDisable( GL_DEPTH_TEST );
	glDisable( GL_TEXTURE_2D );
	glDisable( GL_LIGHTING );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	glEnableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );
	glDisableClientState( GL_NORMAL_ARRAY );

	char ligne[64];
	const float fps = ( ms_frame > 0.01f ) ? ( 1000.0f / ms_frame ) : 0.0f;

	snprintf( ligne, sizeof( ligne ), "%d FPS  %d.%d MS",
	          (int)( fps + 0.5f ), (int)ms_frame,
	          (int)(( ms_frame - (int)ms_frame ) * 10.0f ));
	draw_text( 8.0f, 8.0f, ligne, 1.0f, 1.0f, 0.4f );

	if(( meshes_drawn | meshes_total ) == 0 )
	{
		meshes_drawn = s_meshes_drawn;
		meshes_total = s_meshes_total;
	}
	snprintf( ligne, sizeof( ligne ), "DECOR %d-%d", meshes_drawn, meshes_total );
	draw_text( 8.0f, 26.0f, ligne, 0.7f, 0.9f, 1.0f );

	// Etat des reglages : c'est ce qu'on oublie le plus souvent en testant.
	snprintf( ligne, sizeof( ligne ), "LUM %s  CIEL %s  CULL %s",
	          Nx::g_vita_lighting ? "ON" : "OFF",
	          g_vita_sky          ? "ON" : "OFF",
	          g_vita_cull         ? "ON" : "OFF" );
	draw_text( 8.0f, 44.0f, ligne, 0.8f, 0.8f, 0.8f );

	snprintf( ligne, sizeof( ligne ), "ZOOM %d.%02d",
	          (int)g_vita_world_zoom,
	          (int)(( g_vita_world_zoom - (int)g_vita_world_zoom ) * 100.0f ));
	draw_text( 8.0f, 62.0f, ligne, 0.8f, 0.8f, 0.8f );

	// Aide-memoire : on oublie les combinaisons entre deux sessions.
	draw_text( 8.0f, 500.0f, "GAUCHE+ SELECT HUD  CARRE LUM  TRI CIEL",
	           0.5f, 0.5f, 0.5f );
	draw_text( 8.0f, 518.0f, "ROND CULL  START FIN  CROIX SHOT",
	           0.5f, 0.5f, 0.5f );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisable( GL_BLEND );
	glEnable( GL_DEPTH_TEST );
	glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );

	glMatrixMode( GL_MODELVIEW );
	glPopMatrix();
	glMatrixMode( GL_PROJECTION );
	glPopMatrix();
	glMatrixMode( GL_MODELVIEW );
}

} // namespace NxVita
