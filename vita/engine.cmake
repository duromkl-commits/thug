# Sources du moteur pour la cible Vita.
#
# Principe : on prend TOUT Code/**/*.cpp, puis on retire ce qui appartient à
# une autre plateforme. C'est plus robuste qu'une liste blanche — un fichier
# ajouté en amont par kisak entre automatiquement dans le build, et on le voit
# à la compilation plutôt que six mois plus tard.
#
# Backends exclus, et pourquoi :
#   Wn32/Win32  code Windows (WinMain, timer Win32)
#   XBox        XDK Microsoft (xtl.h)
#   ngc         GameCube (dolphin/)
#   NGPS/Ngps   PS2 (eekernel.h, sifdev.h)
#   DX9         Direct3D
#   SDL         backend audio SDL2/miniaudio (exige C++11, SDL2 absent)
#   GameSpy     netplay propriétaire — stubé en bloc (voir CLAUDE.md)
#
# Ce qui reste : la couche portable + nos backends Code/*/Vita/.

file(GLOB_RECURSE THUG_SOURCES CONFIGURE_DEPENDS "${THUG_ROOT}/Code/*.cpp")

set(THUG_EXCLUDE_PATTERNS
  "/Wn32/" "/Win32/" "/wn32/" "/win32/"
  "/XBox/" "/Xbox/" "/xbox/"
  "/NGC/"  "/ngc/"  "/Ngc/"
  "/NGPS/" "/ngps/" "/Ngps/"
  "/DX9/"  "/dx9/"
  "/SDL/"  "/sdl/"
  "/GameSpy/" "/gamespy/"
  # GameSpy uniquement : son SDK refuse explicitement toute plateforme qu'il
  # ne connaît pas (« #error The GameSpy SDKs do not support... »).
  #
  # ATTENTION, l'exclusion portait d'abord sur TOUT /GameNet/ — c'était trop
  # large et ça coupait le chemin vers un skater jouable :
  # GameNet::Manager::ClientAddNewPlayer (GameNet.cpp:3402) appelle add_skater
  # MEME HORS LIGNE. Sans ces fichiers, aucun skater n'est jamais cree.
  # Verification faite fichier par fichier : seul Lobby.cpp inclut GameSpy.
  "/GameSpy/"
  "GameNet/Lobby.cpp"
  # Fichiers Windows hors dossier de plateforme : le nom est le seul indice.
  "win32functions"
  # Sys/SIO : siodev.cpp/sioman.cpp sont du code PS2 (scePadRead, scePadInfoAct)
  # posé hors d'un dossier de plateforme. Nos SIO::Device / SIO::Manager
  # viennent de Sys/Vita/p_sys_stubs.cpp.
  "/SIO/"
  # standard.cpp et template.cpp sont des documents de convention de codage,
  # pas du code : ils contiennent des exemples volontairement incomplets
  # (« ... some code », #ifdef sans #endif pour illustrer une règle).
  "Code/standard.cpp"
  "Code/template.cpp"
  # Les variantes « Gunslinger* » redéfinissent à l'identique des méthodes des
  # composants standards (WalkComponent, CameraLookAroundComponent…) et
  # produisent 33 « multiple definition » au link. Doublons laissés en amont ;
  # on garde les versions non préfixées.
  "Gunslinger"
)

foreach(pattern ${THUG_EXCLUDE_PATTERNS})
  list(FILTER THUG_SOURCES EXCLUDE REGEX "${pattern}")
endforeach()

# Nos backends vivent parfois DANS un dossier exclu (Code/Sk/GameNet/Vita/ est
# sous /GameNet/). On les remet après coup : les motifs d'exclusion visent le
# code d'origine, pas le nôtre.
# Glob large puis filtre sur /Vita/ : « Code/*/Vita/*.cpp » ne descend que
# d'un niveau et raterait Code/Sk/GameNet/Vita/.
file(GLOB_RECURSE THUG_ALL_CPP CONFIGURE_DEPENDS "${THUG_ROOT}/Code/*.cpp")
set(THUG_VITA_BACKENDS ${THUG_ALL_CPP})
list(FILTER THUG_VITA_BACKENDS INCLUDE REGEX "/Vita/")
list(APPEND THUG_SOURCES ${THUG_VITA_BACKENDS})

# Dernier filet du palier 1 : symboles définis par leur nom manglé, en
# assembleur. Voir vita/tools/gen_link_stubs.py — ces stubs NE LOGGENT PAS.
if(EXISTS "${THUG_ROOT}/Code/Sys/Vita/p_link_stubs.S")
  list(APPEND THUG_SOURCES "${THUG_ROOT}/Code/Sys/Vita/p_link_stubs.S")
endif()

list(REMOVE_DUPLICATES THUG_SOURCES)

list(LENGTH THUG_SOURCES THUG_SOURCE_COUNT)
message(STATUS "moteur THUG : ${THUG_SOURCE_COUNT} sources")
