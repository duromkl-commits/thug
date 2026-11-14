/*****************************************************************************
**  THUG desktop -- files next to the exe (desktop_extras.cpp)          **
*****************************************************************************/

#ifndef THUG_DESKTOP_EXTRAS_H
#define THUG_DESKTOP_EXTRAS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Whole binary files next to the exe (the Pro Skater 4 career save).      */
/* read: malloc'd, NULL when missing; write: 1 when written.               */
void *extras_file_read( const char *file, unsigned *size );
int   extras_file_write( const char *file, const void *data, unsigned size );

#ifdef __cplusplus
}
#endif

#endif
