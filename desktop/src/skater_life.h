/*****************************************************************************
**  THUG desktop -- Skater Life mode (skater_life.cpp)                      **
*****************************************************************************/

#ifndef THUG_SKATER_LIFE_H
#define THUG_SKATER_LIFE_H

#ifdef __cplusplus
extern "C" {
#endif

enum { LIFE_EVENT_MESSAGE = 0, LIFE_EVENT_BLACKOUT = 1 };

int  life_on( void );
void life_start( void );			/* turns the mode on, loading the save */
void life_stop( void );				/* saves and turns it off */
void life_tick( float seconds );	/* real seconds of unpaused play */
int  life_landed( int score );		/* a landed combo; returns the cash earned */
void life_bailed( void );

/* An action from the Skater Life menu (snack, meal, coffee, shower, toilet,
   sleep, radio, newlife). Returns 0 when it can't be done (not enough cash).
   'sound' gets the Sims sound to play, 'fade' the seconds of black screen. */
int  life_action( const char *action, char *sound, int n_sound, float *fade );

/* Messages to show, held while an action's fade runs. */
void life_hold( int on );
int  life_pop_event( char *text, int n_text, char *sound, int n_sound, int *kind );

int         life_num_needs( void );
float       life_need( int i );		/* 0..100 */
const char *life_need_name( int i );
int         life_cash( void );
float       life_mood( void );
void        life_clock_text( char *out, int n );
int         life_gain( float *age );	/* last cash gain and its age in seconds */

const char *life_station_name( void );
int  life_radio_active( void );		/* a Sims station replaces the soundtrack */
int  life_radio_open( void );		/* opens a song of it in the music decoder */

#ifdef __cplusplus
}
#endif

#endif
