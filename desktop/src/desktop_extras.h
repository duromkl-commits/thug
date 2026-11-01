/*****************************************************************************
**  THUG desktop -- pause-menu Extras toggles (desktop_extras.cpp)          **
*****************************************************************************/

#ifndef THUG_DESKTOP_EXTRAS_H
#define THUG_DESKTOP_EXTRAS_H

#ifdef __cplusplus
extern "C" {
#endif

enum { EXTRA_INPUT_DISPLAY = 0, EXTRA_COMBO_STATS = 1, EXTRA_CHAOS = 2,
       EXTRA_GOAL_ATTACK = 3, EXTRA_SLOWMO = 4, EXTRA_PERFECT = 5, EXTRA_READOUT = 6, NB_EXTRAS = 7 };

int         extras_get( int i );			/* 0/1, loaded from thug_extras.txt */
void        extras_toggle( int i );			/* flips it and saves */
const char *extras_name( int i );

/* Records (best classic scores, goal attack splits): key=value lines in a
   text file next to the exe. */
float extras_record_get( const char *file, const char *key, float def );
void  extras_record_set( const char *file, const char *key, float value );

#ifdef __cplusplus
}
#endif

#endif
