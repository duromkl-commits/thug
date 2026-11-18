/*****************************************************************************
**  THUG desktop -- the raw pad as the engine saw it (desktop_pad.cpp)      **
*****************************************************************************/

#ifndef THUG_DESKTOP_PAD_H
#define THUG_DESKTOP_PAD_H

#ifdef __cplusplus
extern "C" {
#endif

/* shim_input_audio.cpp: the pad state of this read (Vita bits, 0..255 sticks). */
void     desktop_pad_record( unsigned buttons, unsigned char lx, unsigned char ly,
                             unsigned char rx, unsigned char ry );
/* The last pad state: returns the buttons, sticks -1..1 with the dead zone
   removed (any pointer may be NULL). Used by the input display, first
   person and photo mode. */
unsigned desktop_pad_raw( float *lx, float *ly, float *rx, float *ry );

#ifdef __cplusplus
}
#endif

#endif
