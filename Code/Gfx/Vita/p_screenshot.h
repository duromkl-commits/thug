///////////////////////////////////////////////////////////////////////////////
// p_screenshot.h — capture d'ecran de verification
//
// Outil de debug : le critere de sortie du palier 3 est VISUEL, et l'humain
// n'est pas toujours devant la console. Une capture recuperee par FTP est une
// preuve mecanique, contrairement a « ca a l'air de marcher ».

#ifndef __GFX_VITA_P_SCREENSHOT_H__
#define __GFX_VITA_P_SCREENSHOT_H__

namespace NxVita
{

// A appeler juste avant la presentation. Capture les frames listees dans
// s_wanted_frames, une seule fois chacune, puis ne fait plus rien.
void MaybeGrabScreenshot( void );

// Demande une capture immediate, ecrite dans shotnow.bmp. Appelee depuis
// l'injection d'entrees a distance : viser des numeros de frame precis ne
// marche pas quand la cadence varie d'un facteur dix entre chargement et jeu.
void RequestScreenshot( void );

} // namespace NxVita

#endif
