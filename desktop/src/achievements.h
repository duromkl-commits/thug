/*****************************************************************************
**  THUG desktop -- achievements (achievements.cpp)                         **
*****************************************************************************/

#ifndef THUG_ACHIEVEMENTS_H
#define THUG_ACHIEVEMENTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* What the game reports. MAX keeps the best value, the others add up. */
enum
{
	ACH_COMBO_BEST, ACH_COMBO_TRICKS, ACH_COMBO_MULT, ACH_COMBO_TIME,
	ACH_AIR_TIME, ACH_GRIND_TIME, ACH_MANUAL_TIME,
	ACH_BAILS, ACH_GOALS, ACH_STORY, ACH_ZERO_GOALS, ACH_ZERO_STORY,
	ACH_CL_RUNS, ACH_CL_SKATE, ACH_CL_COMBO, ACH_CL_TAPE, ACH_CL_SICK, ACH_CL_ALL,
	ACH_GA_GOALS, ACH_GA_PB, ACH_PHOTOS, ACH_LIFE_CASH, ACH_LIFE_BLACKOUT,
	ACH_CHAOS, ACH_FP_COMBO, ACH_SLOWMO_COMBOS, ACH_POINTS, ACH_PLAYTIME,
	ACH_NB_STATS
};

void ach_add( int stat, float v );		/* adds v */
void ach_max( int stat, float v );		/* keeps the larger */

int         ach_count( void );
int         ach_unlocked_count( void );
/* One line for the list: "* Name - what to do" or "Name - what to do (3/25)". */
void        ach_line( int i, char *out, int n );
/* A new unlock to announce: name and description. 0 when none. */
int         ach_pop( char *name, int n_name, char *desc, int n_desc );
/* Saves if anything changed (cheap to call every few seconds). */
void        ach_flush( void );

#ifdef __cplusplus
}
#endif

#endif
