/*****************************************************************************
**  THUG-Vita — couche système                                              **
**  Code/Sys/Vita/p_timer.cpp                                               **
**                                                                          **
**  VRAIE implémentation, pas un stub : le timer cadence toute la boucle de **
**  jeu. Un timer stubé donnerait un moteur qui « tourne » avec un delta    **
**  nul — la physique n'avancerait pas et on chercherait le bug ailleurs.   **
**                                                                          **
**  Base : sceKernelGetProcessTimeWide(), en microsecondes depuis le        **
**  démarrage du process.                                                    **
*****************************************************************************/

#include <core/defines.h>
#include <sys/timer.h>
#include "vita_log.h"

#include <psp2/kernel/processmgr.h>

#ifdef THUG_DESKTOP
extern "C" double desktop_frame_step( void );	// desktop/src/shim_gl.cpp
#endif

namespace Tmr
{

// Le moteur travaille en millisecondes (Time = uint32, cf. sys/timer.h).
static SceUInt64	s_start_us			= 0;
static uint64		s_vblanks			= 0;

// Durée de la frame précédente, en secondes — c'est l'unité qu'attend le
// moteur (FrameLength() est multiplié par des vitesses en unités/seconde).
static float		s_frame_length		= 1.0f / 60.0f;
static double		s_uncapped_length	= 1.0 / 60.0;
static SceUInt64	s_last_frame_us		= 0;

static float		s_slomo				= 1.0f;

// StoreTimerInfo/RecallTimerInfo : le moteur sauve l'état du temps avant une
// opération longue (chargement) pour ne pas se retrouver avec un delta énorme
// à la reprise, qui ferait exploser la physique.
static SceUInt64	s_stored_last_frame	= 0;
static float		s_stored_frame_len	= 1.0f / 60.0f;

static inline SceUInt64 now_us( void )
{
	return sceKernelGetProcessTimeWide();
}


void	Init( void )
{
	s_start_us		= now_us();
	s_last_frame_us	= s_start_us;
	s_vblanks		= 0;
	s_slomo			= 1.0f;
}


void	DeInit( void )
{
}


Time	GetTime( void )
{
	// Millisecondes depuis Init(). Le type Time est un uint32 : il reboucle
	// au bout de 49 jours, comportement identique à celui d'origine.
	return (Time)(( now_us() - s_start_us ) / 1000 );
}


uint64	GetVblanks()
{
	// Sur PS2 et Xbox, ce compteur etait alimente par une interruption
	// materielle a chaque retour de balayage. Ici PERSONNE ne l'incrementait :
	// il rendait 0 pour toujours, et tout ce qui se date en vblanks restait
	// fige. C'est ce qui bloquait les cinematiques -- leur horloge est
	// exactement ( GetVblanks() - debut ) / Config::FPS()
	// (cutscenedetails.cpp:4383) : la premiere image s'affichait, puis plus
	// rien, et IsAnimComplete() n'etait jamais vrai.
	//
	// On le DERIVE de l'horloge systeme plutot que de l'incrementer une fois
	// par image, pour deux raisons :
	//
	//  - gfxman.cpp:124 ATTEND dans une boucle que ce compteur progresse. Une
	//    valeur qui ne bouge qu'entre deux images y tournerait indefiniment.
	//  - a 30 images/s reelles, compter une image pour un vblank ferait jouer
	//    les cinematiques a moitie vitesse. Le temps, lui, ne ment pas.
	//
	// s_vblanks reste ajoute : les boucles d'attente de chargement appellent
	// VSync() explicitement.
	return s_vblanks + (( now_us() - s_start_us ) * 60ULL ) / 1000000ULL;
}


float	FrameLength()
{
	return s_frame_length * s_slomo;
}


double	UncappedFrameLength()
{
	return s_uncapped_length;
}


// [SOURCE] XBox/p_timer.cpp:264 et le long commentaire de :360. Le moteur
// d'origine ne mesure pas la duree d'une image en microsecondes : il compte
// les VSYNC entre deux presentations, puis fait la moyenne des 4 dernieres
// (vSMOOTH_N). Raison ecrite dans le source : si les images alternent entre 1
// et 2 vsync, un pas pris sur la seule image precedente fait avancer le
// skater a 0,5x puis 2x sa vitesse -- « incredibly jerky ».
//
// Notre premiere version prenait le temps brut de l'image precedente : a
// 22 ms de moyenne (mesure, New Jersey en mouvement), les images affichees
// durent 16,7 ou 33,3 ms mais le pas suivait le temps de calcul, pas
// l'affichage. D'ou la saccade signalee par l'humain.
//
// La presentation etant calee sur la vsync (vglSwapBuffers), l'intervalle
// entre deux appels, arrondi au nombre de vsync, est ce qui a ete affiche.
#define vSMOOTH_N	4
static float	s_render_buffer[vSMOOTH_N] = { 1.0f, 1.0f, 1.0f, 1.0f };
static int		s_render_index = 0;

// CORRECTION du 2026-09-27 : la premiere version arrondissait chaque duree
// au nombre ENTIER de vsync le plus proche. Or notre presentation ne bloque
// pas toujours sur la vsync : une image de 22 ms etait comptee 1 vsync
// (16,7 ms) -- le jeu tournait a ~75 % de sa vitesse, ralenti signale par
// l'humain (« les animations tournent au ralenti »). On garde le lissage sur
// 4 images de la Xbox, mais en vsync FRACTIONNAIRES : la somme des pas suit
// exactement le temps ecoule.
void	OncePerRender()
{
	SceUInt64 t	= now_us();
	SceUInt64 dt = t - s_last_frame_us;
	s_last_frame_us = t;
#ifdef THUG_DESKTOP
	// The desktop knows how long the last frame was actually on screen
	// (shim_gl.cpp, cadence_mesure): whole refreshes, without the CPU
	// timing noise. Long frames (loads) keep the measured time.
	{
		if( dt < 200000 )
			dt = (SceUInt64)( desktop_frame_step() * 1000000.0 + 0.5 );
	}
#endif

	float diff = (float)dt / 16666.667f;
	// Issue #17 : toute image longue est tracee (hors chargements, > 1 s).
	if(( dt > 50000 ) && ( dt < 1000000 ))
		VLOG( "PERF", "image longue : %.1f ms", (float)dt / 1000.0f );
	s_render_buffer[s_render_index] = diff;
	if( ++s_render_index == vSMOOTH_N )
		s_render_index = 0;

	float total = 0.0f, uncapped_total = 0.0f;
	for( int i = 0; i < vSMOOTH_N; ++i )
	{
		float d = s_render_buffer[i];
		uncapped_total += d;
		if(( d > 10.0f ) || ( d < 0.0f ))	// valeurs aberrantes (chargement)
			d = 1.0f;
		if( d > 4.0f )
			d = 4.0f;
		total += d;
	}
	float render_length = total / (float)vSMOOTH_N;
	if( render_length < 1.0f )
		render_length = 1.0f;

	s_frame_length    = render_length / 60.0f;
	s_uncapped_length = ((double)uncapped_total / (double)vSMOOTH_N ) / 60.0;
}


void	VSync()
{
	// La synchronisation verticale réelle est faite par vglSwapBuffers() dans
	// la boucle de rendu. Ici on ne fait que compter.
	++s_vblanks;
}


void	Vblank( void )
{
	++s_vblanks;
}


void	IncrementVblankCounters( void )
{
	++s_vblanks;
}


void	PreUpdateTimerInfo()
{
}


float	GetSlomo()
{
	return s_slomo;
}


void	SetSlomo( float slomo )
{
	s_slomo = slomo;
}


void	RestartClock( void )
{
	s_start_us		= now_us();
	s_last_frame_us	= s_start_us;
}


void	StoreTimerInfo( void )
{
	s_stored_last_frame	= s_last_frame_us;
	s_stored_frame_len	= s_frame_length;
}


void	RecallTimerInfo( void )
{
	s_last_frame_us	= s_stored_last_frame;
	s_frame_length	= s_stored_frame_len;
}

} // namespace Tmr
