/*****************************************************************************
**																			**
**			              Neversoft Entertainment			                **
**																		   	**
**				   Copyright (C) 1999 - All Rights Reserved				   	**
**																			**
******************************************************************************
**																			**
**	Project:		GEL (Game Engine Library)								**
**																			**
**	Module:			GEL					 									**
**																			**
**	File name:		movies.cpp												**
**																			**
**	Created:		5/14/1	-	mjd											**
**																			**
**	Description:	streaming movies										**
**																			**
*****************************************************************************/


/*****************************************************************************
**							  	  Includes									**
*****************************************************************************/

#include <core/defines.h>
#include <core/macros.h>
#include <core/singleton.h>

#ifdef __PLAT_NGPS__
#include <gel/movies/ngps/p_movies.h>
#elif defined( __PLAT_XBOX__ )
#include <gel/movies/xbox/p_movies.h>
#elif defined( __PLAT_NGC__ )
#include <gel/movies/ngc/p_movies.h>
#endif

#include <sys/profiler.h>

#ifdef THUG_DESKTOP
#include <gel/music/music.h>
#include <gel/soundfx/soundfx.h>
extern "C" int desktop_play_movie( const char *p_name, float volume );	// desktop/src/movie_player.cpp
#endif


/*****************************************************************************
**								DBG Information								**
*****************************************************************************/



namespace Flx
{



void PlayMovie( const char *pMovieName )
{
#ifdef THUG_DESKTOP
	// As the Xbox (Xbox/p_movies.cpp): music and streams stop, the movie
	// plays at the louder of the music and sound volumes.
	Spt::SingletonPtr< Sfx::CSfxManager > sfx_manager;
	const float music_vol = Pcm::GetVolume();
	const float sfx_vol   = sfx_manager->GetMainVolume();
	const float vol       = music_vol > sfx_vol ? music_vol : sfx_vol;
	Pcm::StopMusic();
	Pcm::StopStreams();
	desktop_play_movie( pMovieName, vol * 0.01f );
#else
	return; // lwss: stubbed for now..
	//PMovies_PlayMovie( pMovieName );
#endif
}

}  // namespace Flx
