/*****************************************************************************
**  THUG desktop -- the raw pad as the engine saw it                        **
*****************************************************************************/

#include "desktop_pad.h"

namespace
{
static unsigned s_b = 0;
static unsigned char s_axes[4] = { 128, 128, 128, 128 };
}

extern "C" void desktop_pad_record( unsigned buttons, unsigned char lx, unsigned char ly, unsigned char rx, unsigned char ry )
{
	s_b = buttons;
	s_axes[0] = lx; s_axes[1] = ly; s_axes[2] = rx; s_axes[3] = ry;
}

extern "C" unsigned desktop_pad_raw( float *lx, float *ly, float *rx, float *ry )
{
	float *o[4] = { lx, ly, rx, ry };
	for( int i = 0; i < 4; ++i )
	{
		float v = ( s_axes[i] - 128.0f ) / 127.0f;
		if( v > 1.0f ) v = 1.0f;
		if( v < -1.0f ) v = -1.0f;
		const float zm = 0.18f;
		if( v > -zm && v < zm ) v = 0.0f;
		else v = ( v > 0 ? v - zm : v + zm ) / ( 1.0f - zm );
		if( o[i] ) *o[i] = v;
	}
	return s_b;
}
