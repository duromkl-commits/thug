/*****************************************************************************
**  THUG-Vita — couche système                                              **
**  Code/Sys/Vita/p_sys_stubs.cpp                                           **
**                                                                          **
**  Stubs des sous-systèmes système qui n'ont PAS besoin d'être réels au    **
**  palier 1 : manette (SIO), carte mémoire (Mc), pré-chargement (Pip),     **
**  boucle principale (Mlp).                                                **
**                                                                          **
**  À distinguer de p_timer.cpp et p_filesys.cpp, qui sont de VRAIES        **
**  implémentations : un timer ou une couche fichiers stubés donneraient un **
**  moteur qui ment. La manette et la carte mémoire, elles, peuvent ne rien **
**  faire sans induire en erreur — le jeu se comportera comme si aucune     **
**  manette n'était branchée et aucune sauvegarde ne existait.              **
**                                                                          **
**  La manette deviendra réelle au palier 5, la carte mémoire plus tard.    **
**                                                                          **
**  GÉNÉRÉ par vita/tools/gen_stubs.py.                                     **
*****************************************************************************/

#include <core/defines.h>
#include <sys/siodev.h>
#include <sys/McMan.h>
#include <sys/file/pip.h>

#include "../../Gfx/Vita/p_stub.h"

// Les SIO::Device sont desormais REELS : Code/Sys/SIO/Vita/p_siodev.cpp


// Les fonctions de carte memoire vivaient ici en stubs. Elles sont
// desormais implementees pour de vrai dans
// Code/Sys/MemCard/Vita/p_McMan.cpp -- la sauvegarde ecrit sur ux0.

