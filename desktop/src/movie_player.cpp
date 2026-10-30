/*****************************************************************************
**  THUG desktop port -- Bink movies                                        **
**                                                                          **
**  The Xbox played data\movies\bik\<name>.bik with RAD's Bink SDK          **
**  (Gel/Movies/Xbox/p_movies.cpp). Here FFmpeg's Bink decoders (LGPL,      **
**  third_party/ffmpeg-bink, only the Bink parts built) do the decoding.    **
**  Like the Xbox, playback is blocking: the game waits until the movie     **
**  ends or Start / X skips it. Frames go through the normal present        **
**  (scaling, PS2-look pass); the sound goes out its own audio port.        **
*****************************************************************************/

#ifdef THUG_BINK

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#include "psp2_shim.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>

extern "C" void vita_log_printf( const char *sys, const char *fmt, ... );
#define DLOG( ... ) vita_log_printf( "MOV", __VA_ARGS__ )

extern "C" void desktop_host_path( const char *vita, char *out, size_t out_size );
extern "C" void desktop_movie_frame( const unsigned char *rgba, int w, int h );
extern "C" void desktop_pump_events( void );
extern "C" bool desktop_quit_requested( void );

namespace
{

const int GRAIN = 1024;		// frames per audio port write

// Decoded sound, 48 kHz stereo, between the decoder and the port thread.
struct Son
{
	SDL_mutex *mx;
	std::vector<short> fifo;
	size_t lu;
	volatile bool fini;		// no more samples coming
	volatile bool stop;		// leave now
	int port;
	float volume;
};

int fil_son( void *p_arg )
{
	Son &S = *(Son *)p_arg;
	short grain[GRAIN * 2];
	while( !S.stop )
	{
		int n = 0;
		SDL_LockMutex( S.mx );
		const size_t dispo = ( S.fifo.size() - S.lu ) / 2;
		n = dispo < (size_t)GRAIN ? (int)dispo : GRAIN;
		for( int i = 0; i < n * 2; ++i )
		{
			int v = (int)( S.fifo[S.lu + i] * S.volume );
			grain[i] = (short)( v < -32768 ? -32768 : v > 32767 ? 32767 : v );
		}
		S.lu += n * 2;
		if( S.lu > 1 << 20 )
		{
			S.fifo.erase( S.fifo.begin(), S.fifo.begin() + S.lu );
			S.lu = 0;
		}
		const bool fini = S.fini;
		SDL_UnlockMutex( S.mx );
		if( n == 0 && fini )
			break;
		if( n < GRAIN )
			memset( grain + n * 2, 0, ( GRAIN - n ) * 2 * sizeof( short ));
		sceAudioOutOutput( S.port, grain );
	}
	return 0;
}

inline unsigned char sat( int v ) { return (unsigned char)( v < 0 ? 0 : v > 255 ? 255 : v ); }

// BT.601, video range (what Bink writes).
void vers_rgba( const AVFrame *f, std::vector<unsigned char> &o )
{
	const int w = f->width, h = f->height;
	o.resize((size_t)w * h * 4 );
	for( int y = 0; y < h; ++y )
	{
		const unsigned char *py = f->data[0] + y * f->linesize[0];
		const unsigned char *pu = f->data[1] + ( y >> 1 ) * f->linesize[1];
		const unsigned char *pv = f->data[2] + ( y >> 1 ) * f->linesize[2];
		unsigned char *d = &o[(size_t)y * w * 4];
		for( int x = 0; x < w; ++x )
		{
			const int c = 298 * ( py[x] - 16 );
			const int u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
			d[0] = sat(( c + 409 * v + 128 ) >> 8 );
			d[1] = sat(( c - 100 * u - 208 * v + 128 ) >> 8 );
			d[2] = sat(( c + 516 * u + 128 ) >> 8 );
			d[3] = 255;
			d += 4;
		}
	}
}

unsigned boutons( void )
{
	SceCtrlData c;
	sceCtrlPeekBufferPositive( 0, &c, 1 );
	unsigned b = c.buttons & ( SCE_CTRL_START | SCE_CTRL_CROSS );
	const Uint8 *k = SDL_GetKeyboardState( NULL );
	if( k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_SPACE] )
		b |= 0x80000000u;
	return b;
}

std::string nom_de_base( const char *p )
{
	const char *b = p;
	for( const char *q = p; *q; ++q )
		if( *q == '\\' || *q == '/' )
			b = q + 1;
	std::string s( b );
	if( s.size() > 4 && !strcasecmp( s.c_str() + s.size() - 4, ".bik" ))
		s.resize( s.size() - 4 );
	return s;
}

}	// namespace

// Returns 0 if the movie could not be played (missing file, bad data).
extern "C" int desktop_play_movie( const char *p_name, float volume )
{
	if( !p_name || !*p_name )
		return 0;
	const std::string base = nom_de_base( p_name );
	const char *essais[] = { "ux0:data/thug/Data/movies/bik/%s.bik", "ux0:data/thug/Data/movies/%s.bik", NULL };
	char vita[512], hote[1024];
	AVFormatContext *fmt = NULL;
	for( int i = 0; essais[i] && !fmt; ++i )
	{
		snprintf( vita, sizeof( vita ), essais[i], base.c_str());
		desktop_host_path( vita, hote, sizeof( hote ));
		if( avformat_open_input( &fmt, hote, NULL, NULL ) != 0 )
			fmt = NULL;
	}
	if( !fmt )
	{
		DLOG( "movie '%s': no movies/bik/%s.bik", p_name, base.c_str());
		return 0;
	}
	if( avformat_find_stream_info( fmt, NULL ) < 0 )
	{
		avformat_close_input( &fmt );
		return 0;
	}
	const int iv = av_find_best_stream( fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0 );
	const int ia = av_find_best_stream( fmt, AVMEDIA_TYPE_AUDIO, -1, iv, NULL, 0 );
	if( iv < 0 )
	{
		avformat_close_input( &fmt );
		return 0;
	}

	AVCodecContext *cv = NULL, *ca = NULL;
	{
		const AVCodecParameters *par = fmt->streams[iv]->codecpar;
		const AVCodec *c = avcodec_find_decoder( par->codec_id );
		cv = c ? avcodec_alloc_context3( c ) : NULL;
		if( !cv || avcodec_parameters_to_context( cv, par ) < 0 || avcodec_open2( cv, c, NULL ) < 0 )
		{
			DLOG( "movie '%s': video decoder refused", base.c_str());
			avcodec_free_context( &cv );
			avformat_close_input( &fmt );
			return 0;
		}
	}
	SwrContext *swr = NULL;
	if( ia >= 0 )
	{
		const AVCodecParameters *par = fmt->streams[ia]->codecpar;
		const AVCodec *c = avcodec_find_decoder( par->codec_id );
		ca = c ? avcodec_alloc_context3( c ) : NULL;
		if( !ca || avcodec_parameters_to_context( ca, par ) < 0 || avcodec_open2( ca, c, NULL ) < 0 )
			avcodec_free_context( &ca );
		if( ca )
		{
			AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
			if( swr_alloc_set_opts2( &swr, &stereo, AV_SAMPLE_FMT_S16, 48000,
			                         &ca->ch_layout, ca->sample_fmt, ca->sample_rate, 0, NULL ) < 0
			    || swr_init( swr ) < 0 )
			{
				swr_free( &swr );
				avcodec_free_context( &ca );
			}
		}
	}
	const AVRational tb = fmt->streams[iv]->time_base;
	DLOG( "movie '%s': %dx%d, %.2f fps, sound %s", base.c_str(), cv->width, cv->height,
	      av_q2d( fmt->streams[iv]->avg_frame_rate ), ca ? "yes" : "no" );

	Son S;
	S.mx = SDL_CreateMutex();
	S.lu = 0;
	S.fini = false;
	S.stop = false;
	S.volume = volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume;
	S.port = ca ? sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, 48000, SCE_AUDIO_OUT_MODE_STEREO ) : -1;
	SDL_Thread *th = ( S.port > 0 ) ? SDL_CreateThread( fil_son, "movie sound", &S ) : NULL;

	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	std::vector<unsigned char> rgba;
	const Uint64 freq = SDL_GetPerformanceFrequency();
	Uint64 debut = 0;
	unsigned tenus = boutons();		// held when the movie starts: not a skip
	bool saute = false, fin = false;
	int images = 0;

	auto son = [&]( AVFrame *a )
	{
		const int max = swr_get_out_samples( swr, a ? a->nb_samples : 0 );
		if( max <= 0 ) return;
		std::vector<short> tmp((size_t)max * 2 );
		uint8_t *out = (uint8_t *)&tmp[0];
		const int n = swr_convert( swr, &out, max, a ? (const uint8_t **)a->extended_data : NULL, a ? a->nb_samples : 0 );
		if( n <= 0 ) return;
		SDL_LockMutex( S.mx );
		S.fifo.insert( S.fifo.end(), tmp.begin(), tmp.begin() + n * 2 );
		SDL_UnlockMutex( S.mx );
	};
	auto attendre = [&]( double t ) -> bool	// false: skipped / quit
	{
		for( ;; )
		{
			desktop_pump_events();
			const unsigned b = boutons();
			if( b & ~tenus ) { saute = true; return false; }
			tenus &= b;
			if( desktop_quit_requested()) { saute = true; return false; }
			const double now = (double)( SDL_GetPerformanceCounter() - debut ) / (double)freq;
			if( now >= t ) return true;
			const double reste = t - now;
			if( reste > 0.003 ) SDL_Delay((Uint32)( reste * 1000.0 ) - 1 );
		}
	};
	auto image = [&]( AVFrame *v ) -> bool
	{
		if( !debut ) debut = SDL_GetPerformanceCounter();
		const int64_t pts = v->best_effort_timestamp != AV_NOPTS_VALUE ? v->best_effort_timestamp : v->pts;
		const double t = pts != AV_NOPTS_VALUE ? pts * av_q2d( tb ) : 0.0;
		if( !attendre( t )) return false;
		vers_rgba( v, rgba );
		desktop_movie_frame( &rgba[0], v->width, v->height );
		++images;
		return true;
	};

	while( !saute && !fin )
	{
		const int r = av_read_frame( fmt, pkt );
		if( r < 0 )
		{
			fin = true;
			avcodec_send_packet( cv, NULL );
			if( ca ) avcodec_send_packet( ca, NULL );
		}
		else if( pkt->stream_index == ia && ca )
			avcodec_send_packet( ca, pkt );
		else if( pkt->stream_index == iv )
			avcodec_send_packet( cv, pkt );
		if( r >= 0 ) av_packet_unref( pkt );

		if( ca )
			while( avcodec_receive_frame( ca, fr ) == 0 )
			{
				son( fr );
				av_frame_unref( fr );
			}
		while( !saute && avcodec_receive_frame( cv, fr ) == 0 )
		{
			image( fr );
			av_frame_unref( fr );
		}
	}
	if( swr && !saute ) son( NULL );

	// Let the sound run out unless skipped.
	SDL_LockMutex( S.mx );
	S.fini = true;
	if( saute ) S.stop = true;
	SDL_UnlockMutex( S.mx );
	if( th ) SDL_WaitThread( th, NULL );
	if( S.port > 0 ) sceAudioOutReleasePort( S.port );
	SDL_DestroyMutex( S.mx );

	DLOG( "movie '%s': %d frames shown%s", base.c_str(), images, saute ? ", skipped" : "" );
	av_frame_free( &fr );
	av_packet_free( &pkt );
	swr_free( &swr );
	avcodec_free_context( &ca );
	avcodec_free_context( &cv );
	avformat_close_input( &fmt );
	return 1;
}

#else

extern "C" int desktop_play_movie( const char *, float ) { return 0; }

#endif
