// THUG-Vita — couche de compatibilité compilateur.
//
// Le code d'origine visait MSVC (Win32/Xbox), CodeWarrior (GameCube) et ProDG
// (PS2), tous de 2003. On compile avec GCC 15 en gnu++98 + -fpermissive.
// Ce header absorbe les écarts SANS toucher au code d'origine — c'est la
// règle : garder le diff auditable.
//
// N'ajouter ici que ce qu'une erreur de compilation réelle a exigé. Pas de
// compat préventive : chaque entrée doit correspondre à un symbole qui a
// cassé, sinon on accumule du bruit invérifiable.

#ifndef __CORE_VITA_COMPAT_H
#define __CORE_VITA_COMPAT_H

#ifdef __PLAT_VITA__

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>

// -- MSVC-ismes -------------------------------------------------------------
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

#ifndef __fastcall
#define __fastcall
#endif

#ifndef __cdecl
#define __cdecl
#endif

// -- Fonctions chaînes non standard ----------------------------------------
// stricmp/strnicmp sont les noms MSVC des équivalents POSIX.
#define stricmp   strcasecmp
#define strnicmp  strncasecmp
#define _stricmp  strcasecmp
#define _strnicmp strncasecmp

// -- Allocation -------------------------------------------------------------
// _msize (MSVC) = taille utilisable d'un bloc. newlib fournit l'équivalent
// POSIX, déclaré dans <malloc.h>.
#define _msize    malloc_usable_size

// -- Types Windows résiduels ------------------------------------------------
// Le code sous __PLAT_WN32__ en utilise une poignée (4 occurrences dans
// Defines.h) hors de tout contexte Windows réel — surcharges d'operator new
// tracées par nom de fichier, essentiellement. On les définit plutôt que de
// toucher au code d'origine.
//
// À garder MINIMAL : si cette liste se met à grossir, c'est le signe qu'on
// compile un fichier qui aurait dû être remplacé ou stubé, pas qu'il faut
// réimplémenter <windows.h>.
typedef const char *LPCSTR;
typedef char       *LPSTR;
// WORD / DWORD : NE PAS les définir ici. Core/Defines_IDA.h les déclare déjà
// (typedef int16 WORD / int32 DWORD) et les redéfinir provoque un
// « conflicting declaration ».

// -- Winsock résiduel -------------------------------------------------------
// Le netplay est stubé (voir CLAUDE.md), mais quelques appels winsock traînent
// dans du code qu'on compile quand même. errno tient lieu de code d'erreur :
// les vraies sockets viennent des headers BSD du vitasdk (Gel/Net/net.h).
#define WSAGetLastError()   (errno)
#define WSAEWOULDBLOCK      EWOULDBLOCK

// -- max / min --------------------------------------------------------------
// Le code les appelle non qualifiés : sous Windows ils viennent de windows.h,
// qui les définit en MACROS. On fournit des templates plutôt que des macros —
// les macros max/min sont la raison d'être de NOMINMAX et cassent std::max,
// std::numeric_limits::max(), etc.
#ifdef __cplusplus
template<class T> inline T max(T a, T b) { return (a > b) ? a : b; }
template<class T> inline T min(T a, T b) { return (a < b) ? a : b; }
#endif

// -- Sortie de debug --------------------------------------------------------
// OutputDebugString (Win32) -> notre canal de log.
// Passe par une fonction plutôt qu'une macro vers sceClibPrintf : ce header
// est inclus par Core/Defines.h, donc par à peu près tout le moteur. Y faire
// entrer <psp2/...> contaminerait l'ensemble de la compilation.
#ifdef __cplusplus
extern "C" {
#endif
void Vita_OutputDebugString(const char *msg);
#ifdef __cplusplus
}
#endif

#define OutputDebugString Vita_OutputDebugString

#endif // __PLAT_VITA__

#endif // __CORE_VITA_COMPAT_H
