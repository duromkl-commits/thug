//****************************************************************************
//* MODULE:         Gfx
//* FILENAME:       moviemanager.cpp
//* OWNER:          Gary Jesdanun
//* CREATION DATE:  02/07/2002
//****************************************************************************

/*****************************************************************************
**							  	  Includes									**
*****************************************************************************/

#ifdef __PLAT_VITA__
unsigned long long g_vita_cine_vue = 0;	// dernier passage dans une cinematique
#endif
#ifdef __PLAT_VITA__
#include "vita_log.h"
#include <psp2/kernel/processmgr.h>
// Partages entre le garde-fou (debut de Update) et la sortie normale plus bas,
// pour pouvoir dire COMBIEN DE TEMPS une cinematique a tenu.
namespace Obj { class CMovieDetails; }
static Obj::CMovieDetails *sp_vita_last_movie = 0;
static unsigned long long  s_vita_movie_debut = 0;
#endif
#include <sk/objects/moviecam.h>

#include <gel/assman/assman.h>
#include <gel/scripting/script.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/checksum.h>
#include <gel/scripting/symboltable.h>

#include <gfx/bonedanim.h>
#include <gfx/nxloadscreen.h>
#include <gfx/nxviewman.h>

#include <sk/modules/frontend/frontend.h>
#include <sk/modules/skate/skate.h>
#include <sk/modules/skate/GameMode.h>

#include <sk/objects/cutscenedetails.h>

// TODO:  Remove dependency on CSkater class...
#include <sk/objects/skater.h>
#include <sk/modules/viewer/viewer.h>

/*****************************************************************************
**								  Externals									**
*****************************************************************************/

/*****************************************************************************
**								   Defines									**
*****************************************************************************/

/*****************************************************************************
**								DBG Defines									**
*****************************************************************************/

namespace Obj
{

/*****************************************************************************
**								 Private Data								**
*****************************************************************************/

/*****************************************************************************
**								 Public Data								**
*****************************************************************************/

/*****************************************************************************
**							  Private Prototypes							**
*****************************************************************************/

/*****************************************************************************
**							   Private Functions							**
*****************************************************************************/

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

int CMovieManager::get_movie_count()
{
	return m_movieDetailsList.CountItems();
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

CMovieDetails* CMovieManager::get_movie_details( int index )
{
	if ( get_movie_count() == 0 )
	{
		return NULL;
	}

	return (CMovieDetails*)m_movieDetailsList.GetItem( index );
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

CMovieDetails* CMovieManager::get_movie_details_by_name( uint32 name )
{
	for ( int i = 0; i < get_movie_count(); i++ )
	{
		CMovieDetails* pDetails = get_movie_details( i );
		Dbg_Assert( pDetails );
		if ( pDetails->GetName() == name )
		{
			return pDetails;
		}
	}

	return NULL;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void CMovieManager::setup_last_movie_camera( CMovieDetails* pDetails )
{
	// Check if this is an overridding camera
	if ( !( pDetails && pDetails->OverridesCamera() ) )
	{
		return;
	}

	// Don't set up if the game is paused or in the skateshop
	if ( pDetails->NeedsCameraTransition() &&
		 !Mdl::FrontEnd::Instance()->GamePaused() &&
		 (Mdl::Skate::Instance()->m_cur_level != CRCD(0x9f2bafb7,"Load_Skateshop") ) )
	{
		m_last_movie_camera = *(pDetails->GetCamera());
		m_last_movie_frame = Tmr::GetRenderFrame() + 1;		// End it one frame later so there aren't any glitches
		m_use_last_movie_camera = true;
	}
	else
	{
//		Script::RunScript( CRCD( 0x15674315, "Restore_skater_camera" ) );  // Temp patch to get the skater camera back at end of cutscenes
	}
}

/*****************************************************************************
**								Public Functions							**
*****************************************************************************/

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

CMovieManager::CMovieManager()
{
	m_use_last_movie_camera = false;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

CMovieManager::~CMovieManager()
{
	ClearMovieQueue();
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

Gfx::Camera* CMovieManager::GetActiveCamera()
{
	CMovieDetails* pDetails = get_movie_details( 0 );
	if ( pDetails && pDetails->OverridesCamera() )
	{
		return pDetails->GetCamera();
	}
	else if ( m_use_last_movie_camera )
	{
		return &m_last_movie_camera;
	}
	else
	{
		return NULL;
	}
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void CMovieManager::ClearMovieQueue( void )
{
	if ( !m_movieDetailsList.IsEmpty() )
	{
		setup_last_movie_camera( get_movie_details( 0 ) );
		//Script::RunScript( CRCD( 0x15674315, "Restore_skater_camera" ) );  // Temp patch to get the skater camera back at end of cutscenes
	}
	m_movieDetailsList.DestroyAllNodes();
	//Dbg_Message("Removing all movies\n");
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void CMovieManager::CleanupMovie( CMovieDetails* pDetails )
{
	Dbg_MsgAssert( pDetails, ( "No details?" ) );

	// remember the exit script, for use
	// after the movie has been removed
	// from the list
	uint32 exitScriptName = pDetails->GetExitScriptName();
	Script::CStruct* pTempExitParams = new Script::CStruct;
	pTempExitParams->AppendStructure( pDetails->GetExitParams() );
										  
	// Check if last movie
	if (get_movie_count() == 1)
	{
		setup_last_movie_camera(pDetails);
	}

	pDetails->Cleanup();
	pDetails->Remove();
	delete pDetails;
	printf( "Details have been removed %ld\n", Tmr::GetRenderFrame() );
	
	// GJ:  run the exit script AFTER we delete it
	// need to do it after the details are deleted
	// in case the exit script affects the camera
	// somehow (would cause a glitch otherwise)
	if ( exitScriptName != 0 )
	{
		Script::RunScript( exitScriptName, pTempExitParams, NULL );
	}

	delete pTempExitParams;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::AbortCurrentMovie( bool only_if_skippable )
{
	bool aborted = false;
	
	if ( !only_if_skippable )
	{
		CMovieDetails* pDetails = get_movie_details( 0 );
		if ( pDetails )
		{
			CleanupMovie( pDetails );
			
			aborted = true;
		}
	}
	else
	{
		// delete as many skippable movies as there are...
		CMovieDetails* pDetails = get_movie_details( 0 );
		while ( pDetails && pDetails->IsSkippable() )
		{
			CleanupMovie( pDetails );
			
			pDetails = get_movie_details( 0 );
			aborted = true;
		}
	}

	// now that a new movie is about to begin,
	// reset its custom keys
	CMovieDetails* pDetails = get_movie_details( 0 );
	if ( pDetails )
	{
		pDetails->ResetCustomKeys();
	}

	// restore skater cam
	// Nx::CViewportManager::sSetCamera( 0, Mdl::Skate::Instance()->GetSkater(0)->GetCamera() );

	// if there are no more movies, then restore the camera
//	if (aborted)  <-- causes one frame glitch with queued up cameras
	if ( !aborted && (get_movie_count() == 0) )
	{
//		Script::RunScript(CRCD(0x15674315, "Restore_skater_camera"));		// Temp pathc to get the skater camera back at end of cutscenes
	}

	return true;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::SetMovieSkippable( uint32 name, bool skippable )
{
	if ( m_movieDetailsList.CountItems() == 0 )
	{
		// no movies in the queue
		return false;
	}

	if ( name == 0 )
	{
		// no name specified, so set the current movie
		CMovieDetails* pDetails = get_movie_details( 0 );
		Dbg_Assert( pDetails );
		pDetails->SetSkippable( skippable );
	}
	else
	{
		// a name was specified, so only do this movie
		CMovieDetails* pDetails = get_movie_details_by_name( name );
		if ( pDetails )
		{
			pDetails->SetSkippable( skippable );
		}
		else
		{
			Dbg_MsgAssert( 0, ( "Movie to skip (%s) not found\n", Script::FindChecksumName(name) ) );
		}
	}

	return true;
}


/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::SetMoviePauseMode( uint32 name, bool pause_mode )
{
	if ( m_movieDetailsList.CountItems() == 0 )
	{
		// no movies in the queue
		return false;
	}

	if ( name == 0 )
	{
		// no name specified, so set the current movie
		CMovieDetails* pDetails = get_movie_details( 0 );
		Dbg_Assert( pDetails );
		pDetails->SetPauseMode( pause_mode );
	}
	else
	{
		// a name was specified, so only do this movie
		CMovieDetails* pDetails = get_movie_details_by_name( name );
		if ( pDetails )
		{
			pDetails->SetPauseMode( pause_mode );
		}
		else
		{
			Dbg_MsgAssert( 0, ( "Movie to set pause mode (%s) not found\n", Script::FindChecksumName(name) ) );
		}
	}
	return true;
}


/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::IsMovieComplete( uint32 name )
{
	if ( get_movie_count() == 0 )
	{
		return true;
	}

	if ( name != 0 )
	{
		if ( get_movie_details_by_name( name ) )
		{
			// the movie still exists in the queue,
			// so it's obviously not done yet
			return false;
		}
		return true;
	}

	return false;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::IsMovieHeld( uint32 name )
{
	if ( m_movieDetailsList.CountItems() == 0 )
	{
		return false;
	}

	if ( name != 0 )
	{
		CMovieDetails* pDetails = get_movie_details_by_name( name );
		
		if ( pDetails )
		{
			return pDetails->IsHeld();
		}
		else
		{
			// movie not found in list!
			return false;
		}
	}
	else
	{
		CMovieDetails* pDetails = get_movie_details( 0 );
		Dbg_Assert( pDetails );
		return pDetails->IsHeld();
	}
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::RemoveMovieFromQueue( uint32 name )
{
	CMovieDetails* pDetails = get_movie_details_by_name( name );

	if ( pDetails )
	{
		if ( pDetails == get_movie_details( 0 ) )
		{
			// first movie?  then just abort current
			AbortCurrentMovie( false );
			return true;
		}
		else
		{
			Dbg_Message( "Removing %s from movie queue", Script::FindChecksumName(name) );
			pDetails->Remove();
			delete pDetails;
			return true;
		}
	}
	
	//Dbg_Message( "RemoveMovieFromQueue:  Movie %s not found.", Script::FindChecksumName(name) );
	return false;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::AddMovieToQueue( uint32 name, Script::CStruct* pParams, uint32 movieType )
{
	CMovieDetails* pDetails = NULL;

    bool old_last_movie_camera = m_use_last_movie_camera;

	// GJ:  a new camera has been queued up, so don't need
	// to do anything special to fix the 1-frame glitch
	// (otherwise, some of the cutscenes will have the
	// wrong camera for the first frame...)  must be done before
	// the cutscenedetails constructor, which updates the model component,
	// which contains a check for "IsRolling"
	m_use_last_movie_camera = false;

	switch ( movieType )
	{
		case 0xf5012f52: // cutscene
			
			// check for multiple movies in the queue
			if ( get_movie_count() != 0 )
			{
				for ( int i = 0; i < get_movie_count(); i++ )
				{
					CMovieDetails* pDetails = get_movie_details( i );
					if ( pDetails->GetName() == CRCD(0xf5012f52,"cutscene") )
					{
						// GJ:  theoretically, we should be able to just clear the movie queue
						// to get rid of the extra cutscene, but i suspect that there will be
						// problems with cleanup, and that it would be too risky to do
						// that so close to the end of the project...
						Dbg_MsgAssert( 0, ( "Cutscenes should not be queued (only 1 cutscene at a time, please)" ) );
					}
				}
				// if we've currently got skatercamanims in the queue,
				// then trash them...  (due to the convoluted level
				// loading process, it's possible for a skatercamanim
				// to be launched on the same frame as a cutscene;
				// in this case, we want the cutscene to take
				// precedence)
				ClearMovieQueue();
								
				// we should set this to false, since
				// we know we're about to add a new movie
				// in a moment anyway.  (otherwise, modelcomponent
				// will assert because it doesn't expect the
				// cutscene camera to be active...
				m_use_last_movie_camera = false;
			}

			{
				// fix 1-frame glitch with a loading screen here...
				char* pFileName = NULL;
				bool freeze = true;
				bool blank = false;
				Nx::CLoadScreen::sDisplay( pFileName, freeze, blank );
			}										  

			pDetails = new CCutsceneDetails;
			Dbg_Message( "Trying to create new cutscene" );
			break;
		case 0x12743edb: // objectanim
			pDetails = new CObjectAnimDetails;
			Dbg_Message( "Trying to create new object anim" );
			break;
		case 0x66b7dd11: // skatercamanim
			pDetails = new CSkaterCamDetails;
			Dbg_Message( "Trying to create new skater cam anim" );
			break;
		default:
			Dbg_MsgAssert( 0, ( "Unrecognized movie type %s", Script::FindChecksumName(movieType) ) );
			break;
	}

	Dbg_Assert( pDetails );

	pDetails->SetName( name );

	if ( !pDetails->InitFromStructure( pParams ) )
	{
		// failed
		delete pDetails;

		// restore the old "last movie camera" flag
		m_use_last_movie_camera = old_last_movie_camera;
		
		return false;
	}

	m_movieDetailsList.AddToTail( pDetails );
	//Dbg_Message("Adding a movie on frame %ld.  Count now %d\n", Tmr::GetRenderFrame(), get_movie_count());

	// GJ:  if it's the first movie in the queue,
	// then set the correct camera (to prevent
	// one frame glitch between movies)
	if ( get_movie_count() == 1 )
	{
		Nx::CViewportManager::sSetCamera( 0, this->GetActiveCamera() );
	}

	// TODO:  Support the following parameters
//	pDetails->SetTarget( Obj::MovingObject* pObject );	
//	pDetails->SetFocusSkater( pParams->ContainsFlag("focus_skater") );
//	pDetails->SetFocusTarget( pParams->ContainsFlag("focus_target") );

	return true;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void CMovieManager::ApplyLastCamera()
{
   	Nx::CViewportManager::sSetCamera( 0, &m_last_movie_camera );
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::Update()
{
	// Clear out old camera if we advanced a frame
	if (m_use_last_movie_camera && (m_last_movie_frame <= Tmr::GetRenderFrame()))
	{
		//Dbg_Message("Turning off camera on frame %d", Tmr::GetRenderFrame());
		m_use_last_movie_camera = false;

		if ( get_movie_count() == 0 )
		{
//			Script::RunScript(CRCD(0x15674315, "Restore_skater_camera"));		// Temp pathc to get the skater camera back at end of cutscenes
		}
	}

	CMovieDetails* pDetails = get_movie_details( 0 );

	// there are no movies in the queue,
	// so return "no movies" code
	// (the camera manager should then immediately
	// pass control back to the skater cam)
	if ( !pDetails )
	{
		return false;
	}

#ifdef __PLAT_VITA__
	// [CORRIGE] La version precedente abandonnait TOUTES les cinematiques, au
	// motif qu'il n'y a pas de lecteur video. La premisse etait fausse : les
	// cinematiques de THUG sont jouees par le MOTEUR, et IsComplete() ne
	// regarde que LoadFinished() et IsAnimComplete()
	// (cutscenedetails.cpp:5326) -- aucun codec la-dedans. Les sauter figeait
	// le skater du menu principal, laissant la camera de jeu cadrer le decor
	// n'importe comment (skater centre, trop proche, bord vide a droite).
	//
	// On les laisse donc se jouer. Le garde-fou d'origine reste utile mais
	// devient conditionnel : si une cinematique ne se termine JAMAIS, le
	// script d'entree de niveau attend derriere un rideau noir plein ecran.
	// Passe un delai large, on emprunte sa sortie normale.
	{
		const unsigned long long now = sceKernelGetProcessTimeWide();
		g_vita_cine_vue = now;	// raccourcis de test bloques pendant une cinematique (#51)

		if( pDetails != sp_vita_last_movie )
		{
			sp_vita_last_movie = pDetails;
			s_vita_movie_debut = now;
			{
				Game::CGameMode *p_mode = Mdl::Skate::Instance()->GetGameMode();
				VLOG( "MOV", "cinematique DEMARRE (mode %s, is_frontend %d)",
				      p_mode ? Script::FindChecksumName( p_mode->GetNameChecksum()) : "?",
				      p_mode ? (int)p_mode->IsFrontEnd() : -1 );
			}
		}

		// [CORRIGE] Le critere Â« ca dure trop longtemps Â» etait faux : la
		// cinematique du menu principal tourne EN BOUCLE et ne se termine
		// jamais -- c'est sa raison d'etre. Mesure : trois demarrages, aucun
		// Â« TERMINEE Â», puis ce garde-fou la tuait pile 30 s apres, d'ou le
		// retour a la camera de skate.
		//
		// Le seul cas vraiment pathologique est celui d'origine : une
		// cinematique qui ne DEMARRE jamais, parce que son asset n'arrive
		// pas. On teste donc HasMovieStarted(), pas la duree. Une cinematique
		// demarree a le droit de jouer aussi longtemps qu'elle veut.
		// DEUX situations opposees, qu'il a fallu payer pour distinguer :
		//
		//  - Au MENU PRINCIPAL, la cinematique tourne en boucle et ne se
		//    termine jamais : c'est sa raison d'etre. Un garde-fou de duree la
		//    tuait pile 30 s apres son debut, et la vue retombait sur la
		//    camera de skate.
		//
		//  - En JEU (story mode), la cinematique d'introduction doit, elle,
		//    s'achever. Sans lecteur video elle ne le fait jamais, et le
		//    joueur reste devant un ecran noir, camera figee.
		//
		// On ne borne donc la duree qu'en jeu.
		//
		// [REFUTE] « le mode de jeu les separe, IsFrontEnd() est vrai au
		// menu » : mesure sur console, le mode au menu est « career »,
		// is_frontend 0. C'est le comportement d'origine -- startup.q lance le
		// menu par « autolaunch level=load_skateshop game=career ». Le
		// garde-fou tuait donc la cinematique du menu 90 s apres son debut ;
		// la camera retombait sur SkaterCam0 et le decor 3D du menu
		// (mainmenu_bg) sortait du cadre : grand aplat violet, skater coupe.
		//
		// Le critere est le NIVEAU, teste comme le fait le source d'origine
		// quelques lignes plus haut (ligne ~135) et dans GameHandler.cpp.
		const bool au_menu =
			( Mdl::Skate::Instance()->m_cur_level == CRCD( 0x9f2bafb7, "Load_Skateshop" ))
			|| ( Mdl::Skate::Instance()->GetGameMode()
			     && Mdl::Skate::Instance()->GetGameMode()->IsFrontEnd());

		const bool jamais_demarree =
			!pDetails->HasMovieStarted()
			&& (( now - s_vita_movie_debut ) > 15000000ULL );

		// 90 s : simple filet. La cause reelle du blocage est corrigee -- le
		// compteur de vblank, dont derive l'horloge des cinematiques, n'etait
		// alimente par personne sur Vita (voir Sys/Vita/p_timer.cpp,
		// GetVblanks). Une cinematique progresse donc maintenant et se termine
		// d'elle-meme ; ce garde-fou ne doit plus servir qu'en cas d'imprevu,
		// d'ou un seuil largement au-dessus de la plus longue scene du jeu.
		// [CORRIGE] #64 : 90 s coupait la sequence de fin du Story Mode, plus
		// longue que ca. Seuil porte a 10 min.
		const bool trop_longue =
			!au_menu && (( now - s_vita_movie_debut ) > 600000000ULL );

		if( jamais_demarree || trop_longue )
		{
			{
				Game::CGameMode *p_mode = Mdl::Skate::Instance()->GetGameMode();
				VLOG( "MOV", "cinematique abandonnee (%s) -- mode %s, is_frontend %d",
				      jamais_demarree ? "jamais demarree" : "trop longue, hors menu",
				      p_mode ? Script::FindChecksumName( p_mode->GetNameChecksum()) : "?",
				      p_mode ? (int)p_mode->IsFrontEnd() : -1 );
			}
			sp_vita_last_movie = NULL;
			AbortCurrentMovie( false );
			return ( get_movie_count() != 0 );
		}
	}
#endif

#if 0
	printf( "Movie manager update %d:\n", get_movie_count() );
	for ( int i = 0; i < get_movie_count(); i++ )
	{
		printf( "\tmovie name = %s\n", Script::FindChecksumName(get_movie_details(i)->GetName()) );
	}
#endif
	
	if ( Mdl::FrontEnd::Instance()->GamePaused() && ( Mdl::CViewer::sGetViewMode() == 0 ) )
	{
		// printf("game is paused in normal view mode\n");
		if ( !pDetails->ShouldPauseMovie() )
		{
			pDetails->Update();
		}
	}
	else
	{
		// printf("view mode is not normal\n");
		pDetails->Update();
	}
		
	// if the animation is done,
	// the remove it from the list
	if ( pDetails->IsComplete() )
	{
		// TODO:  run callback function if necessary?

#ifdef __PLAT_VITA__
		// Le menu retombe sur la camera de skate au bout de quelques
		// secondes : c'est ici que ca se decide. Reste a savoir si la
		// cinematique s'acheve normalement (et que le script devrait en
		// relancer une) ou si elle se termine trop tot.
		VLOG( "MOV", "cinematique TERMINEE apres %.1f s -- la main revient a "
		             "la camera de skate",
		      (double)( sceKernelGetProcessTimeWide() - s_vita_movie_debut )
		      / 1000000.0 );
		sp_vita_last_movie = NULL;
#endif

		AbortCurrentMovie( false );
	}

	return ( get_movie_count() != 0 );
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

Script::CStruct* CMovieManager::GetMovieParams( uint32 name )
{
	if ( m_movieDetailsList.CountItems() == 0 )
	{
		// no movies in the queue
		return NULL;
	}

	if ( name == 0 )
	{
		// no name specified, so set the current movie
		CMovieDetails* pDetails = get_movie_details( 0 );
		Dbg_Assert( pDetails );
		if ( pDetails )
			return pDetails->GetParams();
	}
	else
	{
		// a name was specified, so only do this movie
		CMovieDetails* pDetails = get_movie_details_by_name( name );
		if ( pDetails )
			return pDetails->GetParams();
	}
	return NULL;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::CallMemberFunction( uint32 checksum, Script::CStruct* pParams, Script::CScript* pScript )
{
	CMovieDetails* pDetails = get_movie_details( 0 );
	Dbg_MsgAssert( pDetails, ("No current movie?") );
	bool success = pDetails->CallMemberFunction( checksum, pParams, pScript );
	if ( success )
	{
		return success;
	}

	return Obj::CObject::CallMemberFunction( checksum, pParams, pScript );
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::OverridesCamera()
{
	CMovieDetails* pDetails = get_movie_details( 0 );

	if ( pDetails )
	{
		return pDetails->OverridesCamera();
	}

	return false;
}


/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

uint32 CMovieManager::GetCurrentMovieName()
{
	CMovieDetails* pDetails = get_movie_details( 0 );

	if ( pDetails )
	{
		return pDetails->GetName();
	}

	return 0;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::HasMovieStarted()
{
	CMovieDetails* pDetails = get_movie_details( 0 );

	if ( pDetails )
	{
		return pDetails->HasMovieStarted();
	}

	Dbg_MsgAssert( 0, ( "Shouldn't call this without knowing there's a movie queued %d", get_movie_count() ) );
	return true;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool CMovieManager::IsMovieQueued()
{
	return ( get_movie_count() != 0 );
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

} // namespace Obj
