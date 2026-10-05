/*****************************************************************************
**  THUG-Vita — paramètres de runtime                                       **
**  vita/src/vita_runtime.cpp                                               **
**                                                                          **
**  Ces variables étaient dans main_vita.cpp, le point d'entrée du palier 0. **
**  Ce main-là est retiré du build depuis que le moteur apporte le sien     **
**  (Sk/Main.cpp) — mais les réglages mémoire, eux, restent nécessaires.    **
**                                                                          **
**  Valeurs calquées sur un port qui boote réellement sur cette console :    **
**  sceLibcHeapSize à 128 Mio (première tentative) affamait vitaGL au point **
**  qu'il crashait en allouant la texture de son splashscreen.              **
*****************************************************************************/

// Réserve SceLibc — SÉPARÉE du heap newlib. 4 Mio suffisent : on ne fait
// presque rien passer par SceLibc.
int sceLibcHeapSize			= 4 * 1024 * 1024;

// Heap principal (malloc/new).
int _newlib_heap_size_user	= 192 * 1024 * 1024;

// Le moteur fait de grosses allocations sur pile (parsing des scripts QB).
int sceUserMainThreadStackSize = 4 * 1024 * 1024;

// ---------------------------------------------------------------------------
// Globale du moteur definie, sur PC, dans Sys/Win32/WinMain.cpp -- fichier
// exclu du build parce qu'il est specifique a Windows. Le moteur la lit dans
// Mainloop.cpp et l'ecrit dans GameNet.cpp, il faut donc bien qu'elle existe
// quelque part.
// ---------------------------------------------------------------------------
bool g_hasJustEnteredNetworkGame = false;
