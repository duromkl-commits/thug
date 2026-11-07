/*****************************************************************************
**  THUG desktop -- custom soundtrack                                       **
**  desktop/src/custom_music.h                                              **
*****************************************************************************/

#ifndef THUG_CUSTOM_MUSIC_H
#define THUG_CUSTOM_MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Decoder (custom_music_decode.c): one song at a time, 48 kHz s16 stereo. */
#ifdef _WIN32
#include <wchar.h>
int  cm_open( const wchar_t *path );
#else
int  cm_open( const char *path );
#endif
int  cm_read( short *out, int frames );		/* frames decoded, 0 at the end */
void cm_close( void );

/* Soundtrack (custom_music.cpp). */

/* Index of a custom song from the engine's track name
   ("MUSIC\VAG\SONGS\THUGCUSTOM007" -> 7), or -1. */
int  custom_music_index( const char *track_name );
/* Opens custom song i in the decoder. */
int  custom_music_open( int i );

/* Called on every .qb before the engine parses it. Returns a patched copy
   (malloc'd, the caller frees it) or NULL to parse the original. */
unsigned char *custom_music_patch_qb( const char *file_name, const unsigned char *qb );

/* Playlist on/off by song title ("band: title"), kept in thug_playlist.txt
   (music.cpp asks these instead of trusting the save's bits by position). */
int  custom_music_playlist_known( void );	/* the file exists */
int  custom_music_playlist_off( const char *title );
void custom_music_playlist_set( const char *title, int off );

#ifdef __cplusplus
}
#endif

#endif
