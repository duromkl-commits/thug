/*****************************************************************************
**  THUG-Vita â€” outil de verification                                        **
**  Code/Gfx/Vita/p_screenshot.cpp                                          **
**                                                                          **
**  Capture du framebuffer en BMP dans ux0:data/thug/, recuperable par FTP.  **
**                                                                          **
**  Raison d'etre : le critere de sortie du palier 3 est visuel. Sans        **
**  capture, la seule verification possible est Â« demander a l'humain de     **
**  regarder l'ecran Â» -- ce qui n'est pas toujours faisable, et ce qui ne   **
**  laisse aucune trace. Une image sur le disque est une preuve qu'on peut   **
**  relire, dater et comparer.                                              **
**                                                                          **
**  BMP 24 bits sans compression : le format le plus simple a ecrire a la    **
**  main, et lisible partout. Les lignes sont ecrites de bas en haut, ce qui **
**  tombe bien -- c'est aussi l'ordre de glReadPixels.                       **
*****************************************************************************/

#include <vitaGL.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <psp2/io/fcntl.h>

#include "vita_log.h"
#include "p_screenshot.h"

namespace NxVita
{

#define SHOT_W 960
#define SHOT_H 544

// Plusieurs instants plutot qu'un seul : la premiere frame rendue montre
// rarement l'etat interessant.
// Frames choisies parmi celles REELLEMENT atteintes : a ~3 fps, viser 700
// donnait une capture qui n'arrivait jamais -- et on comparait alors deux
// builds sur un fichier perime.
// [CORRIGE] #18 : 300, 3000, 6000 et 12000 figeaient le jeu ~2,7 s en pleine
// partie (BMP ecrit sur la carte). Les captures se prennent par vctl.py.
static const int s_wanted_frames[] = { 2 };
#define NUM_WANTED ( (int)( sizeof( s_wanted_frames ) / sizeof( s_wanted_frames[0] )))


static void write_bmp( const char *p_path, const unsigned char *p_rgba )
{
	const int row_bytes = SHOT_W * 3;
	const int pad       = ( 4 - ( row_bytes % 4 )) % 4;
	const int img_size  = ( row_bytes + pad ) * SHOT_H;
	const int file_size = 54 + img_size;

	unsigned char hdr[54];
	memset( hdr, 0, sizeof( hdr ));
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2] = (unsigned char)( file_size       );
	hdr[3] = (unsigned char)( file_size >>  8 );
	hdr[4] = (unsigned char)( file_size >> 16 );
	hdr[5] = (unsigned char)( file_size >> 24 );
	hdr[10] = 54;						// offset des donnees
	hdr[14] = 40;						// taille de l'en-tete d'info
	hdr[18] = (unsigned char)( SHOT_W       );
	hdr[19] = (unsigned char)( SHOT_W >>  8 );
	hdr[22] = (unsigned char)( SHOT_H       );
	hdr[23] = (unsigned char)( SHOT_H >>  8 );
	hdr[26] = 1;						// plans
	hdr[28] = 24;						// bits par pixel

	SceUID fd = sceIoOpen( p_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777 );
	if( fd < 0 )
	{
		VLOG( "SHOT", "!! ouverture impossible : %s", p_path );
		return;
	}
	sceIoWrite( fd, hdr, sizeof( hdr ));

	unsigned char *p_row = (unsigned char *)malloc( row_bytes + pad );
	if( p_row )
	{
		memset( p_row, 0, row_bytes + pad );
		// glReadPixels rend deja les lignes de bas en haut, comme le BMP.
		for( int y = 0; y < SHOT_H; ++y )
		{
			const unsigned char *p_src = p_rgba + ( y * SHOT_W * 4 );
			for( int x = 0; x < SHOT_W; ++x )
			{
				p_row[x * 3 + 0] = p_src[x * 4 + 2];	// B
				p_row[x * 3 + 1] = p_src[x * 4 + 1];	// G
				p_row[x * 3 + 2] = p_src[x * 4 + 0];	// R
			}
			sceIoWrite( fd, p_row, row_bytes + pad );
		}
		free( p_row );
	}
	sceIoClose( fd );
}


static bool s_requested = false;

void RequestScreenshot( void )
{
	s_requested = true;
}


void MaybeGrabScreenshot( void )
{
	static int s_frame = 0;
	static int s_taken = 0;

	++s_frame;

	bool on_demand = s_requested;
	if( on_demand )
	{
		s_requested = false;
	}
	else
	{
		if( s_taken >= NUM_WANTED )
			return;
		if( s_frame != s_wanted_frames[s_taken] )
			return;
		++s_taken;
	}

	unsigned char *p_pixels = (unsigned char *)malloc( SHOT_W * SHOT_H * 4 );
	if( !p_pixels )
	{
		VLOG( "SHOT", "!! pas de memoire pour la capture" );
		return;
	}

	glReadPixels( 0, 0, SHOT_W, SHOT_H, GL_RGBA, GL_UNSIGNED_BYTE, p_pixels );

	char path[64];
	if( on_demand )
	{
		// NUMEROTEES. La premiere version ecrivait toujours « shotnow.bmp » :
		// l'humain a pris six captures d'affilee en pensant documenter six
		// vues, et n'en a rapporte qu'une -- chacune ayant efface la
		// precedente. Une capture coute un appui et 1,5 Mo ; la perdre coute
		// une manche de jeu a refaire.
		static int s_on_demand = 0;
		snprintf( path, sizeof( path ), "ux0:data/thug/shotnow%03d.bmp",
		          s_on_demand++ );
	}
	else
		snprintf( path, sizeof( path ), "ux0:data/thug/shot%03d.bmp", s_frame );
	write_bmp( path, p_pixels );
	free( p_pixels );

	VLOG( "SHOT", "capture frame %d -> %s", s_frame, path );
}

} // namespace NxVita
