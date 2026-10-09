/*****************************************************************************
**  THUG desktop -- controller, keyboard and audio output                   **
**  desktop/src/shim_input_audio.cpp                                        **
**                                                                          **
**  sceCtrl: an SDL game controller (Xbox layout) or the keyboard fills the **
**  Vita pad. p_siodev.cpp builds the PS2 DualShock2 packet the engine      **
**  reads from it. On desktop the four shoulders are real buttons:          **
**     LB = L1, RB = R1 (spins), LT = L2, RT = R2 (nollie / revert).        **
**                                                                          **
**  sceAudioOut: every port is a ring the SDL audio callback mixes. A       **
**  blocking sceAudioOutOutput waits until its ring has room, which keeps  **
**  the mixer threads paced the way the Vita's ports did.                   **
*****************************************************************************/

#include "psp2_shim.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>

#include "desktop_config.h"

extern "C" void vita_log_printf( const char *sys, const char *fmt, ... );
#define DLOG( ... ) vita_log_printf( "DSK", __VA_ARGS__ )

// Desktop-only pad bits for the real L1/R1 (outside the Vita's sceCtrl set).
#define DESKTOP_PAD_L1 SCE_CTRL_L1
#define DESKTOP_PAD_R1 SCE_CTRL_R1

// ===========================================================================
// Input
// ===========================================================================

static SDL_GameController *s_pad = NULL;
static bool s_quit_requested = false;

extern "C" bool desktop_quit_requested( void ) { return s_quit_requested; }

static void open_pad( void )
{
	if( s_pad )
		return;
	for( int i = 0; i < SDL_NumJoysticks(); ++i )
		if( SDL_IsGameController( i ))
		{
			s_pad = SDL_GameControllerOpen( i );
			if( s_pad )
			{
				DLOG( "controller: %s", SDL_GameControllerName( s_pad ));
				return;
			}
		}
}

extern "C" SDL_Window *desktop_sdl_window( void );

extern "C" void desktop_pump_events( void )
{
	SDL_Event e;
	while( SDL_PollEvent( &e ))
	{
		switch( e.type )
		{
			case SDL_QUIT:
				s_quit_requested = true;
				sceKernelExitProcess( 0 );
				break;
			case SDL_CONTROLLERDEVICEADDED:
				open_pad();
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				if( s_pad && !SDL_GameControllerGetAttached( s_pad ))
				{
					SDL_GameControllerClose( s_pad );
					s_pad = NULL;
					open_pad();
				}
				break;
			case SDL_KEYDOWN:
				// Alt+Enter: toggle borderless fullscreen.
				if( e.key.keysym.sym == SDLK_RETURN && ( e.key.keysym.mod & KMOD_ALT ) && desktop_sdl_window())
				{
					SDL_Window *w = desktop_sdl_window();
					const bool fs = ( SDL_GetWindowFlags( w ) & SDL_WINDOW_FULLSCREEN_DESKTOP ) != 0;
					SDL_SetWindowFullscreen( w, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP );
				}
				break;
			default:
				break;
		}
	}
}

extern "C" int sceCtrlSetSamplingMode( SceCtrlPadInputMode )
{
	static bool s_init = false;
	if( !s_init )
	{
		s_init = true;
		SDL_InitSubSystem( SDL_INIT_GAMECONTROLLER );
		open_pad();
	}
	return 0;
}

static unsigned char axis_to_byte( Sint16 v )
{
	int b = ( (int)v + 32768 ) >> 8;
	return (unsigned char)( b < 0 ? 0 : b > 255 ? 255 : b );
}

extern "C" int sceCtrlPeekBufferPositive( int, SceCtrlData *d, int )
{
	sceCtrlSetSamplingMode( SCE_CTRL_MODE_ANALOG_WIDE );
	memset( d, 0, sizeof( *d ));
	d->lx = d->ly = d->rx = d->ry = 128;
	d->timeStamp = sceKernelGetProcessTimeWide();
	unsigned int b = 0;

	if( s_pad )
	{
		SDL_GameController *p = s_pad;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_A )) b |= SCE_CTRL_CROSS;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_B )) b |= SCE_CTRL_CIRCLE;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_X )) b |= SCE_CTRL_SQUARE;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_Y )) b |= SCE_CTRL_TRIANGLE;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_START )) b |= SCE_CTRL_START;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_BACK )) b |= SCE_CTRL_SELECT;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_DPAD_UP )) b |= SCE_CTRL_UP;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_DPAD_DOWN )) b |= SCE_CTRL_DOWN;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_DPAD_LEFT )) b |= SCE_CTRL_LEFT;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_DPAD_RIGHT )) b |= SCE_CTRL_RIGHT;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_LEFTSHOULDER )) b |= DESKTOP_PAD_L1;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER )) b |= DESKTOP_PAD_R1;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_LEFTSTICK )) b |= SCE_CTRL_L3;
		if( SDL_GameControllerGetButton( p, SDL_CONTROLLER_BUTTON_RIGHTSTICK )) b |= SCE_CTRL_R3;
		if( SDL_GameControllerGetAxis( p, SDL_CONTROLLER_AXIS_TRIGGERLEFT ) > 12000 ) b |= SCE_CTRL_LTRIGGER;
		if( SDL_GameControllerGetAxis( p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT ) > 12000 ) b |= SCE_CTRL_RTRIGGER;
		d->lx = axis_to_byte( SDL_GameControllerGetAxis( p, SDL_CONTROLLER_AXIS_LEFTX ));
		d->ly = axis_to_byte( SDL_GameControllerGetAxis( p, SDL_CONTROLLER_AXIS_LEFTY ));
		d->rx = axis_to_byte( SDL_GameControllerGetAxis( p, SDL_CONTROLLER_AXIS_RIGHTX ));
		d->ry = axis_to_byte( SDL_GameControllerGetAxis( p, SDL_CONTROLLER_AXIS_RIGHTY ));
	}

	// Keyboard: arrows = d-pad, WASD = left stick, IJKL = right stick.
	const Uint8 *k = SDL_GetKeyboardState( NULL );
	if( k )
	{
		if( k[SDL_SCANCODE_UP] )        b |= SCE_CTRL_UP;
		if( k[SDL_SCANCODE_DOWN] )      b |= SCE_CTRL_DOWN;
		if( k[SDL_SCANCODE_LEFT] )      b |= SCE_CTRL_LEFT;
		if( k[SDL_SCANCODE_RIGHT] )     b |= SCE_CTRL_RIGHT;
		if( k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_KP_2] ) b |= SCE_CTRL_CROSS;		// ollie / select
		if( k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_KP_4] ) b |= SCE_CTRL_SQUARE;	// flip
		if( k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_KP_6] || k[SDL_SCANCODE_BACKSPACE] ) b |= SCE_CTRL_CIRCLE;	// grab / back
		if( k[SDL_SCANCODE_F] || k[SDL_SCANCODE_KP_8] ) b |= SCE_CTRL_TRIANGLE;		// grind
		if( k[SDL_SCANCODE_Q] )         b |= DESKTOP_PAD_L1;
		if( k[SDL_SCANCODE_E] )         b |= DESKTOP_PAD_R1;
		if( k[SDL_SCANCODE_Z] )         b |= SCE_CTRL_LTRIGGER;
		if( k[SDL_SCANCODE_C] )         b |= SCE_CTRL_RTRIGGER;
		if( k[SDL_SCANCODE_RETURN] && !( k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT] )) b |= SCE_CTRL_START;
		if( k[SDL_SCANCODE_ESCAPE] )    b |= SCE_CTRL_START;
		if( k[SDL_SCANCODE_TAB] )       b |= SCE_CTRL_SELECT;
		int x = 0, y = 0;
		if( k[SDL_SCANCODE_A] ) x -= 127;
		if( k[SDL_SCANCODE_D] ) x += 127;
		if( k[SDL_SCANCODE_W] ) y -= 127;
		if( k[SDL_SCANCODE_S] ) y += 127;
		if( x || y ) { d->lx = (unsigned char)( 128 + x ); d->ly = (unsigned char)( 128 + y ); }
		x = y = 0;
		if( k[SDL_SCANCODE_J] ) x -= 127;
		if( k[SDL_SCANCODE_L] ) x += 127;
		if( k[SDL_SCANCODE_I] ) y -= 127;
		if( k[SDL_SCANCODE_K] ) y += 127;
		if( x || y ) { d->rx = (unsigned char)( 128 + x ); d->ry = (unsigned char)( 128 + y ); }
	}
	d->buttons = b;
	return 1;
}

extern "C" int sceCtrlReadBufferPositive( int port, SceCtrlData *d, int count )
{
	return sceCtrlPeekBufferPositive( port, d, count );
}

// ===========================================================================
// Audio
// ===========================================================================

#define MAX_PORTS 4
struct Port
{
	bool  used;
	int   grain;			// samples per output call
	int   channels;
	int   vol_l, vol_r;		// Q15
	short *ring;			// interleaved stereo
	int   ring_frames;
	volatile int rd, wr;		// frame indices, wr - rd = queued
	SDL_sem *space;
};
static Port s_ports[MAX_PORTS];
static SDL_AudioDeviceID s_dev = 0;
static SDL_mutex *s_amx = NULL;

static void audio_cb( void *, Uint8 *stream, int len )
{
	short *out = (short *)stream;
	const int frames = len / 4;
	static int *acc = NULL;
	static int acc_n = 0;
	if( acc_n < frames * 2 ) { free( acc ); acc = (int *)malloc( sizeof( int ) * frames * 2 ); acc_n = frames * 2; }
	memset( acc, 0, sizeof( int ) * frames * 2 );
	SDL_LockMutex( s_amx );
	for( int p = 0; p < MAX_PORTS; ++p )
	{
		Port &P = s_ports[p];
		if( !P.used ) continue;
		int avail = P.wr - P.rd;
		int n = avail < frames ? avail : frames;
		for( int i = 0; i < n; ++i )
		{
			const int f = ( P.rd + i ) % P.ring_frames;
			acc[i * 2]     += ( P.ring[f * 2] * P.vol_l ) >> 15;
			acc[i * 2 + 1] += ( P.ring[f * 2 + 1] * P.vol_r ) >> 15;
		}
		P.rd += n;
		if( n > 0 )
			SDL_SemPost( P.space );
	}
	SDL_UnlockMutex( s_amx );
	for( int i = 0; i < frames * 2; ++i )
	{
		int v = acc[i];
		out[i] = (short)( v > 32767 ? 32767 : v < -32768 ? -32768 : v );
	}
}

static bool audio_open( void )
{
	if( s_dev )
		return true;
	if( SDL_InitSubSystem( SDL_INIT_AUDIO ) != 0 )
	{
		DLOG( "!! SDL audio: %s", SDL_GetError());
		return false;
	}
	s_amx = SDL_CreateMutex();
	SDL_AudioSpec want, have;
	SDL_zero( want );
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	want.callback = audio_cb;
	s_dev = SDL_OpenAudioDevice( NULL, 0, &want, &have, 0 );
	if( !s_dev )
	{
		DLOG( "!! audio device: %s", SDL_GetError());
		return false;
	}
	DLOG( "audio: %d Hz, %d channels, %d-frame buffer", have.freq, have.channels, have.samples );
	SDL_PauseAudioDevice( s_dev, 0 );
	return true;
}

extern "C" int sceAudioOutOpenPort( SceAudioOutPortType, int len, int freq, SceAudioOutMode mode )
{
	if( freq != 48000 || !audio_open())
		return (int)0x80260005;
	SDL_LockMutex( s_amx );
	for( int i = 0; i < MAX_PORTS; ++i )
		if( !s_ports[i].used )
		{
			Port &P = s_ports[i];
			P.grain = len;
			P.channels = ( mode == SCE_AUDIO_OUT_MODE_STEREO ) ? 2 : 1;
			P.vol_l = P.vol_r = SCE_AUDIO_OUT_MAX_VOL;
			P.ring_frames = len * 4;
			P.ring = (short *)calloc( P.ring_frames * 2, sizeof( short ));
			P.rd = P.wr = 0;
			P.space = SDL_CreateSemaphore( 0 );
			P.used = true;
			SDL_UnlockMutex( s_amx );
			return i + 1;
		}
	SDL_UnlockMutex( s_amx );
	return (int)0x80260005;
}

extern "C" int sceAudioOutReleasePort( int port )
{
	if( port < 1 || port > MAX_PORTS ) return -1;
	SDL_LockMutex( s_amx );
	Port &P = s_ports[port - 1];
	P.used = false;
	free( P.ring );
	P.ring = NULL;
	SDL_UnlockMutex( s_amx );
	return 0;
}

extern "C" int sceAudioOutOutput( int port, const void *buf )
{
	if( port < 1 || port > MAX_PORTS || !s_ports[port - 1].used ) return -1;
	Port &P = s_ports[port - 1];
	// Keep at most ~2 grains queued: the caller sleeps here, like on Vita.
	while( P.wr - P.rd > P.ring_frames - P.grain || P.wr - P.rd >= 2 * P.grain )
		SDL_SemWaitTimeout( P.space, 20 );
	if( !buf )
		return 0;
	const short *s = (const short *)buf;
	SDL_LockMutex( s_amx );
	for( int i = 0; i < P.grain; ++i )
	{
		const int f = ( P.wr + i ) % P.ring_frames;
		if( P.channels == 2 )
		{
			P.ring[f * 2] = s[i * 2];
			P.ring[f * 2 + 1] = s[i * 2 + 1];
		}
		else
			P.ring[f * 2] = P.ring[f * 2 + 1] = s[i];
	}
	P.wr += P.grain;
	// keep indices bounded
	if( P.rd > ( 1 << 28 )) { P.rd -= ( 1 << 27 ) / P.ring_frames * P.ring_frames; P.wr -= ( 1 << 27 ) / P.ring_frames * P.ring_frames; }
	SDL_UnlockMutex( s_amx );
	return 0;
}

extern "C" int sceAudioOutSetVolume( int port, SceAudioOutChannelFlag ch, int *vol )
{
	if( port < 1 || port > MAX_PORTS ) return -1;
	Port &P = s_ports[port - 1];
	if( ch & SCE_AUDIO_VOLUME_FLAG_L_CH ) P.vol_l = vol[0];
	if( ch & SCE_AUDIO_VOLUME_FLAG_R_CH ) P.vol_r = vol[1];
	return 0;
}

extern "C" int sceAudioOutGetRestSample( int port )
{
	if( port < 1 || port > MAX_PORTS ) return 0;
	return s_ports[port - 1].wr - s_ports[port - 1].rd;
}
