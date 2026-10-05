/*****************************************************************************
**  THUG-Vita — couche système                                              **
**  Code/Sys/Vita/p_singletons.cpp                                          **
**                                                                          **
**  Singletons dont l'implementation vit dans des .cpp exclus du build :    **
**    - GameNet::Manager  (netplay : hors perimetre v1, GameSpy refuse par  **
**                         un #error toute plateforme inconnue)             **
**    - Mc::Manager       (carte memoire PS2)                               **
**    - SIO::Manager      (peripheriques d'entree PS2)                      **
**                                                                          **
**  Leur sSgltnInstance() etait un stub assembleur muet rendant NULL. Le    **
**  moteur ne verifie pas : GameNet::Manager::ReadyToPlay() lit un booleen  **
**  a l'offset 0x120 de `this`, d'ou un Data abort immediat.                **
**  [VERIFIE par psp2core]                                                  **
**                                                                          **
**  On rend donc un BLOC STATIQUE MIS A ZERO, pas un objet construit : les  **
**  constructeurs ne sont pas compiles. Chaque champ lu vaut 0 -- « pas de  **
**  partie reseau », « pas de carte », « pas de peripherique » -- ce qui    **
**  est l'etat reel de la console.                                          **
**                                                                          **
**  LIMITE CONNUE : le pointeur de vtable est nul lui aussi. Tout appel     **
**  VIRTUEL sur ces objets plantera. Si cela arrive, le dump le dira, et il **
**  faudra alors une vraie classe minimale plutot que ce bloc.              **
*****************************************************************************/

#include <core/defines.h>

#include <sys/mcman.h>

#include <string.h>

#include "vita_log.h"

// Fabrique le trio impose par DeclareSingletonClass (Core/singleton.h:277).
//
// ATTENTION : Instance() est un inline qui lit sp_sgltn_instance DIRECTEMENT,
// sans passer par sSgltnInstance(). Se contenter de faire rendre un bloc a
// sSgltnInstance ne changeait donc rien -- premiere version, meme plantage au
// meme endroit. Il faut que le POINTEUR STATIQUE lui-meme designe le bloc des
// l'initialisation statique.
#define VITA_NEUTRAL_SINGLETON( NS, CLS )                                     \
	static union { char bytes[sizeof( NS::CLS )]; double align; }              \
		s_neutral_##NS##_##CLS = { { 0 } };                                   \
	                                                                          \
	namespace NS                                                              \
	{                                                                         \
	CLS *CLS::sp_sgltn_instance = (CLS *)s_neutral_##NS##_##CLS.bytes;        \
	uint CLS::s_sgltn_count     = 1;                                          \
	                                                                          \
	CLS *CLS::sSgltnInstance( bool create )                                   \
	{                                                                         \
		return sp_sgltn_instance;                                             \
	}                                                                         \
	                                                                          \
	void CLS::sSgltnDelete( void )                                            \
	{                                                                         \
		/* Rien a detruire : le bloc est statique. */                         \
	}                                                                         \
	}

// GameNet::Manager : plus de bloc neutre, le vrai singleton vient de
// Code/Sk/GameNet/GameNet.cpp, desormais compile.
VITA_NEUTRAL_SINGLETON( Mc,      Manager )
// SIO::Manager : plus de bloc neutre, vrai gestionnaire dans
// Code/Sys/SIO/Vita/p_sioman.cpp.
