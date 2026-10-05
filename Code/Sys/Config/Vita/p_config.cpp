/*****************************************************************************
**  THUG-Vita — configuration de plateforme                                 **
**  Code/Sys/Config/Vita/p_config.cpp                                       **
**                                                                          **
**  Ce fichier manquait, et son absence coutait plus cher qu'il n'y parait. **
**                                                                          **
**  gHardware restait a HARDWARE_UNDEFINED (config.cpp:8), sa valeur par    **
**  defaut, faute d'un Plat_Init pour la poser. Or le moteur aiguille sur   **
**  Config::GetHardware() a de nombreux endroits, souvent avec un           **
**  « default: » qui echoue -- neuf switch de ce genre dans le seul         **
**  Sk/Scripting/mcfuncs.cpp.                                               **
**                                                                          **
**  Symptome constate : la sauvegarde echouait AVANT d'ouvrir le moindre    **
**  fichier (mcfuncs.cpp:3303, « default: goto ERRORGOTO »). Le dossier de  **
**  sauvegarde restait vide et aucune trace n'apparaissait, puisque le      **
**  backend disque n'etait jamais atteint.                                  **
**                                                                          **
**  ON SE DECLARE XBOX, et c'est le choix coherent : les assets utilises    **
**  SONT ceux de l'ISO Xbox -- textures .img.xbx, audio Xbox ADPCM,         **
**  systeme de fichiers repris du backend XBox. Le chemin de sauvegarde     **
**  Xbox (s_make_xbox_dir_and_icons) n'appelle d'ailleurs que des methodes  **
**  portables : MakeDirectory, ConvertDirectory, puis File::Open sur les    **
**  icones, qui existent bien dans images/miscellaneous/.                   **
**                                                                          **
**  A SURVEILLER : ce choix change le comportement partout ou le moteur     **
**  teste HARDWARE_XBOX, pas seulement dans la sauvegarde. Exemple deja     **
**  repere : mcfuncs.cpp:3074 ne met PAS la musique en pause pendant une    **
**  sauvegarde sur Xbox. C'est benin, mais si un comportement inattendu     **
**  apparait ailleurs, c'est la premiere piste a examiner.                  **
*****************************************************************************/

#include <sys/config/config.h>

#include "vita_log.h"

namespace Config
{

void Plat_Init( sint argc, char **argv )
{
	gHardware = HARDWARE_XBOX;

	// Version NTSC americaine : l'ISO de reference est « Tony Hawk's
	// Underground (USA) », et data/streams/wma/ y est vide, ce qui confirme
	// une construction NTSC americaine (les versions PAL embarquent les WMA).
	gLanguage  = LANGUAGE_ENGLISH;
	gTerritory = TERRITORY_US;

	// Le jeu ne tourne pas depuis un CD/DVD mais depuis ux0. Le declarer
	// evite que le moteur active ses chemins « lecteur optique », qui
	// supposent un systeme de fichiers en lecture seule et des temps
	// d'acces tres differents.
	gCD = false;

	VLOG( "CFG", "plateforme declaree : materiel=%d langue=%d territoire=%d",
	      (int)gHardware, (int)gLanguage, (int)gTerritory );
}

} // namespace Config
