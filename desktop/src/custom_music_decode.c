/*****************************************************************************
**  THUG desktop -- decoder for the custom soundtrack                       **
**  desktop/src/custom_music_decode.c                                       **
**                                                                          **
**  MP3, FLAC and WAV through miniaudio, OGG Vorbis through stb_vorbis,    **
**  all handed out as 48 kHz 16-bit stereo, the music port's format.       **
**  One decoder at a time: the music channel plays one song.               **
*****************************************************************************/

#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_RUNTIME_LINKING
#include "../../Code/Gel/Music/SDL/miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"

#include "custom_music.h"

static ma_decoder s_dec;
static int        s_ouvert = 0;

static ma_decoder_config config( void )
{
	return ma_decoder_config_init( ma_format_s16, 2, 48000 );
}

#ifdef _WIN32
int cm_open( const wchar_t *path )
{
	cm_close();
	ma_decoder_config c = config();
	s_ouvert = ( ma_decoder_init_file_w( path, &c, &s_dec ) == MA_SUCCESS );
	return s_ouvert;
}
#else
int cm_open( const char *path )
{
	cm_close();
	ma_decoder_config c = config();
	s_ouvert = ( ma_decoder_init_file( path, &c, &s_dec ) == MA_SUCCESS );
	return s_ouvert;
}
#endif

int cm_read( short *out, int frames )
{
	if( !s_ouvert )
		return 0;
	ma_uint64 lu = 0;
	if( ma_decoder_read_pcm_frames( &s_dec, out, (ma_uint64)frames, &lu ) != MA_SUCCESS && lu == 0 )
		return 0;
	return (int)lu;
}

void cm_close( void )
{
	if( s_ouvert )
		ma_decoder_uninit( &s_dec );
	s_ouvert = 0;
}
