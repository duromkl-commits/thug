/*****************************************************************************
**  THUG desktop -- pause-menu Extras toggles (desktop_extras.cpp)          **
*****************************************************************************/

#ifndef THUG_DESKTOP_EXTRAS_H
#define THUG_DESKTOP_EXTRAS_H

#ifdef __cplusplus
extern "C" {
#endif

enum { EXTRA_INPUT_DISPLAY = 0, EXTRA_COMBO_STATS = 1, EXTRA_CHAOS = 2, NB_EXTRAS = 3 };

int         extras_get( int i );			/* 0/1, loaded from thug_extras.txt */
void        extras_toggle( int i );			/* flips it and saves */
const char *extras_name( int i );

#ifdef __cplusplus
}
#endif

#endif
