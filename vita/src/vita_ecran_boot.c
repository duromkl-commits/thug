/*
 * Ecran de lancement : app0:boot/ecran.bin, affiche des l'entree dans main()
 * et laisse a l'ecran jusqu'au premier ecran de chargement du moteur.
 *
 * C'est la meme image que sce_sys/pic0.png, que le systeme montre pendant le
 * zoom de lancement : la transition est invisible. Sans lui, on voyait le logo
 * vitaGL puis un ecran noir pendant le demarrage.
 *
 * Deux temps :
 *   1. vita_ecran_boot_tot(), tout debut de main() : decompression dans un
 *      bloc CDRAM declare comme tampon d'affichage (sceDisplaySetFrameBuf).
 *      vglInit prend a lui seul ~2,5 s (compilateur de shaders, pools) :
 *      sans cela l'ecran restait noir jusqu'a lui.
 *   2. vita_ecran_boot(), juste apres vglInit : la meme image en texture,
 *      presentee par vitaGL dans ses deux tampons ; le bloc est alors libere.
 *
 * Format (vita/livearea/generer.py) : "THBR", u16 largeur, u16 hauteur,
 * u32 taille, puis RGBA8 brut -- lu d'un bloc, ~0,17 s. "THBT" (RGBA8 zlib,
 * 0,7 s de decompression mesures) reste accepte. Pas de decodeur PNG dans le
 * vitasdk.
 */
#include <stdlib.h>
#include <string.h>

#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <vitaGL.h>
#include <zlib.h>

#include "vita_log.h"

#define CHEMIN "app0:boot/ecran.bin"
#define L_ECRAN 960
#define H_ECRAN 544
#define TAILLE  ( L_ECRAN * H_ECRAN * 4 )
/* CDRAM : multiple de 256 Ko. */
#define TAILLE_BLOC ((( TAILLE ) + 0x3FFFF ) & ~0x3FFFF )

static SceUID         s_bloc  = -1;
static unsigned char *s_pixels = NULL;	/* RGBA8, L_ECRAN x H_ECRAN */

static int lire_image( unsigned char *dst )
{
	SceUID fd = sceIoOpen( CHEMIN, SCE_O_RDONLY, 0 );
	if( fd < 0 )
	{
		VLOG( "BOOT", "%s absent : pas d'ecran de lancement", CHEMIN );
		return 0;
	}
	unsigned char ent[12];
	int ok = ( sceIoRead( fd, ent, 12 ) == 12 );
	const int brut = ok && !memcmp( ent, "THBR", 4 );	/* RGBA brut : le plus rapide */
	ok = ok && ( brut || !memcmp( ent, "THBT", 4 ));	/* THBT : RGBA zlib */
	const unsigned w  = ent[4] | ( ent[5] << 8 );
	const unsigned h  = ent[6] | ( ent[7] << 8 );
	const unsigned nz = ent[8] | ( ent[9] << 8 ) | ( ent[10] << 16 ) | ( (unsigned)ent[11] << 24 );
	ok = ok && ( w == L_ECRAN ) && ( h == H_ECRAN ) && ( nz > 0 ) && ( nz < 8u * 1024 * 1024 );
	if( ok && brut )
	{
		/* 2 Mo lus d'un bloc (~0,17 s) : la decompression zlib en prenait 0,7. */
		ok = ( nz == TAILLE ) && ( sceIoRead( fd, dst, TAILLE ) == TAILLE );
		sceIoClose( fd );
		if( !ok )
			VLOG( "BOOT", "!! %s illisible", CHEMIN );
		return ok;
	}
	unsigned char *z = ok ? (unsigned char *)malloc( nz ) : NULL;
	if( z && ( sceIoRead( fd, z, nz ) == (int)nz ))
	{
		uLongf n = TAILLE;
		ok = ( uncompress( dst, &n, z, nz ) == Z_OK ) && ( n == TAILLE );
	}
	else
		ok = 0;
	sceIoClose( fd );
	free( z );
	if( !ok )
		VLOG( "BOOT", "!! %s illisible", CHEMIN );
	return ok;
}

void vita_ecran_boot_tot( void )
{
	s_bloc = sceKernelAllocMemBlock( "thug_ecran_boot", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
	                                 TAILLE_BLOC, NULL );
	if( s_bloc < 0 )
	{
		VLOG( "BOOT", "!! bloc d'affichage refuse (0x%08x)", (unsigned)s_bloc );
		return;
	}
	void *base = NULL;
	sceKernelGetMemBlockBase( s_bloc, &base );
	if( !base || !lire_image( (unsigned char *)base ))
	{
		sceKernelFreeMemBlock( s_bloc );
		s_bloc = -1;
		return;
	}
	s_pixels = (unsigned char *)base;

	/* A8B8G8R8 : en memoire R, G, B, A -- l'ordre de nos octets RGBA. */
	SceDisplayFrameBuf fb;
	memset( &fb, 0, sizeof( fb ));
	fb.size        = sizeof( fb );
	fb.base        = base;
	fb.pitch       = L_ECRAN;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	fb.width       = L_ECRAN;
	fb.height      = H_ECRAN;
	const int r = sceDisplaySetFrameBuf( &fb, SCE_DISPLAY_SETBUF_NEXTFRAME );
	VLOG( "BOOT", "ecran de lancement affiche avant vglInit (sceDisplaySetFrameBuf -> 0x%08x)", (unsigned)r );
}

static void quad_plein_ecran( GLuint tex )
{
	static const float pos[] = { 0, 0,  L_ECRAN, 0,  0, H_ECRAN,  L_ECRAN, H_ECRAN };
	static const float uv[]  = { 0, 0,  1, 0,        0, 1,        1, 1 };

	glViewport( 0, 0, L_ECRAN, H_ECRAN );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisable( GL_BLEND );
	glDisable( GL_ALPHA_TEST );
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0, L_ECRAN, H_ECRAN, 0, -1, 1 );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
	glColor4f( 1, 1, 1, 1 );
	glEnable( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, tex );
	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, pos );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_VERTEX_ARRAY );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_TEXTURE_2D );
}

/* Apres vglInit. Rend 1 si l'image a ete affichee, 0 sinon (l'appelant garde
 * son effacement). */
int vita_ecran_boot( void )
{
	unsigned char *rgba = s_pixels;
	unsigned char *a_liberer = NULL;
	if( !rgba )
	{
		/* Pas d'affichage anticipe (bloc refuse) : lecture ici. */
		a_liberer = (unsigned char *)malloc( TAILLE );
		if( !a_liberer || !lire_image( a_liberer ))
		{
			free( a_liberer );
			return 0;
		}
		rgba = a_liberer;
	}

	GLuint tex = 0;
	glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, L_ECRAN, H_ECRAN, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba );
	free( a_liberer );

	/* Les deux tampons d'affichage portent l'image : elle reste quel que
	 * soit celui que le moteur presentera en dernier avant son chargement. */
	for( int i = 0; i < 2; ++i )
	{
		quad_plein_ecran( tex );
		vglSwapBuffers( GL_FALSE );
	}
	glDeleteTextures( 1, &tex );
	/* Etat rendu au moteur tel qu'il l'attend apres vglInit. */
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();

	/* Le tampon anticipe n'est plus balaye une fois les images de vitaGL a
	 * l'ecran : deux synchronisations de marge avant de le rendre. */
	if( s_bloc >= 0 )
	{
		sceDisplayWaitVblankStart();
		sceDisplayWaitVblankStart();
		sceKernelFreeMemBlock( s_bloc );
		s_bloc   = -1;
		s_pixels = NULL;
	}
	VLOG( "BOOT", "ecran de lancement repris par vitaGL" );
	return 1;
}
