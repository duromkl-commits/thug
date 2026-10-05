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
**	Module:			Main Loop (ML) 											**
**																			**
**	File name:		mainloop.cpp											**
**																			**
**	Created:		05/27/99	-	mjb										**
**																			**
**	Description:	Main loop and support code								**
**																			**
*****************************************************************************/


/*****************************************************************************
**							  	  Includes									**
*****************************************************************************/

#ifdef __PLAT_VITA__
#ifdef __PLAT_VITA__
#ifdef __PLAT_VITA__
extern "C" void VitaServiceFrontendRequest( void );
#endif
#include <psp2/kernel/processmgr.h>
#endif
#include "vita_log.h"
#endif

#include <gel/mainloop.h>

#include <core/defines.h>
#include <core/singleton.h>
#include <core/task.h>

#include <gfx/nx.h>

// Vita prend la branche console : Defines_IDA.h tire <type_traits> et
// std::enable_if, donc du C++11, alors qu'on compile en gnu++98.
#if !defined(__PLAT_WN32__) || defined(__PLAT_VITA__)
    #include <sys/profiler.h>
#else
	#include <Core/Defines_IDA.h>
#endif

#include <gel/scripting/symboltable.h>

#ifdef	__PLAT_NGPS__
    #include <gel/collision/batchtricoll.h>
    #include <gel/soundfx/ngps/p_sfx.h>

    namespace NxPs2
    {
        void	WaitForRendering();
		void 	StuffAfterGSFinished();
    }
#endif

/*****************************************************************************
**								DBG Information								**
*****************************************************************************/

Dbg_DefineProject( GEL, "GEL Library" )

// lwss add
#ifdef __PLAT_WN32__
void Win32_ApplyInputs();
void Win32_ProcessMsgPump();
void Win32_GameNetUpdate();

void D3D_ResetTextures();
void D3D_ResetBuffers();
void D3D_ReleaseRenderSurfaces();

void D3D_ReCreateTextures();
void D3D_ReCreateBuffers();
void D3D_RecreateRenderSurfaces();
#endif
// lwss end

namespace Mdl
{
	void	Rail_DebugRender();			// for debugging
}

namespace Mlp
{




/*****************************************************************************
**								  Externals									**
*****************************************************************************/


/*****************************************************************************
**								   Defines									**
*****************************************************************************/


/*****************************************************************************
**								Private Types								**
*****************************************************************************/


/*****************************************************************************
**								 Private Data								**
*****************************************************************************/

DefineSingletonClass( Manager, "Main Loop Manager" );

/*****************************************************************************
**								 Public Data								**
*****************************************************************************/


/*****************************************************************************
**							  Private Prototypes							**
*****************************************************************************/


/*****************************************************************************
**							   Private Functions							**
*****************************************************************************/

Manager::Manager( void )
{
	
	
	start_render_hook = NULL;
	end_render_hook = NULL;
	done = false;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

Manager::~Manager( void )
{
	
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

inline void		Manager::service_system( void )
{
#ifdef __PLAT_VITA__
	{
		static int s_c = 0;
		if( ++s_c <= 3 )
			VLOG( "MLP", ">> service_system (appel %d)", s_c );
	}
#endif
	

#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PushContext( 255, 0, 0 );
#	endif // __USE_PROFILER__
#endif

	system_task_stack.Process (currently_profiling);

#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PopContext();
#	endif // __USE_PROFILER__
#endif
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

inline void		Manager::game_logic( void )
{
#ifdef __PLAT_VITA__
	{
		static int s_c = 0;
		if( ++s_c <= 3 )
			VLOG( "MLP", ">> game_logic (appel %d)", s_c );
	}
#endif
	
	
#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PushContext( 0, 255, 0 );
#	endif // __USE_PROFILER__
#endif

//	printf ("\nTiming Logic\n"); 
	logic_task_stack.Process(currently_profiling);

//	printf ("\nDumping Task List\n\n");	
//	logic_task_stack.Dump();

#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PopContext();
#	endif // __USE_PROFILER__
#endif
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

inline	void		Manager::render_frame( void )
{
#ifdef __PLAT_VITA__
	{
		static int s_c = 0;
		if( ++s_c <= 3 )
			VLOG( "MLP", ">> render_frame (appel %d)", s_c );
	}
#endif
	
	
//	printf ("############################## render_frame #############################\n");	
	

#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PushContext( 0, 0, 255 );
#	endif // __USE_PROFILER__
#endif

#	ifdef __PLAT_NGC__
	if( start_render_hook )
	{
		start_render_hook->Call();
	}
	if( !display_tasks_paused )
	{
		display_task_stack.Process( currently_profiling );
	}
#	else
	if (!display_tasks_paused)
	{
		// If paused don't call the start_render_hook
		// as this clears the none-visible frame buffer
		// which will result in a flash when display is unpaused
		if ( start_render_hook )			// set up for rendering
		{
			start_render_hook->Call();
		}

//		printf ("\nTiming render\n"); 
	
		display_task_stack.Process(currently_profiling);		// service rendering routines
	}
#	endif // __PLAT_NGC__
	
#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PopContext();
	Sys::CPUProfiler->PushContext( 255, 255, 0 );
#	endif // __USE_PROFILER__
#endif

	if ( end_render_hook )				// end rendering
	{
		end_render_hook->Call();
	}
#ifndef __PLAT_WN32__
#	ifdef __USE_PROFILER__
	Sys::CPUProfiler->PopContext();
#	endif // __USE_PROFILER__
#endif
}

/*****************************************************************************
**							   Public Functions								**
*****************************************************************************/

void		Manager::MainLoop( void )
{
	bool old_flag = done;	// push the current done flag

	done = false;

// lwss add
// Vita écartée : ce bloc lit le délai d'écran de veille Windows et cale la
// boucle sur QueryPerformanceFrequency. La cadence est déjà gérée par
// Tmr::OncePerRender + le vsync de vglSwapBuffers.
#if defined(__PLAT_WN32__) && !defined(__PLAT_VITA__)
	void* timeout;
	SystemParametersInfoA(SPI_GETSCREENSAVETIMEOUT, 0, &timeout, 0);
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);

	int v3 = (frequency.QuadPart / 60) >> 32;
	DWORD v4 = (frequency.QuadPart / 60);

	LARGE_INTEGER perf;
	QueryPerformanceCounter(&perf);

	int v12 = 0;
	while (true)
	{
		// g_debugMeshSubmitCounter = 0; // optional

		if (timeout)
		{
			if (v12++ >= 200)
			{
				v12 = 0;
				SystemParametersInfoA(SPI_SETSCREENSAVETIMEOUT, (UINT)timeout, 0, 0); // KISAKTODO: check logic here...
			}
		}
		NxXbox::g_disableRendering = false;
		Win32_ApplyInputs();
		Win32_ProcessMsgPump();
		Win32_GameNetUpdate();

		if (g_isWindowInFocus)
		{
			break;
		}

		if (g_hasJustEnteredNetworkGame)
		{
			NxXbox::g_disableRendering = true;
			break;
		}

		do
		{
			Sleep(100u);
			Win32_ProcessMsgPump();
		} while (!g_isWindowInFocus);

		if (done)
		{
			done = old_flag;
			return;
		}
	}

#endif

#ifdef __PLAT_VITA__
	// Critere du palier 2 : la mainloop tourne. Un battement par seconde
	// suffit — logger chaque frame noierait tout et fausserait le framerate.
	VLOG( "MLP", "entree dans la boucle principale" );
	{
		static int s_frames = 0;
		(void)s_frames;
	}
#endif
	while ( !done )
	{
#ifdef __PLAT_VITA__
		// Chronometre par PHASE.
		//
		// Le tri de visibilite rejette 75 % des maillages et la cadence n'a pas
		// bouge (9,5 -> 10,1 fps) : le goulot n'est donc PAS le nombre d'appels
		// de dessin. Troisieme hypothese de performance refutee sur ce projet ;
		// on arrete de deviner et on mesure ou passe la frame.
		//
		// Cumul sur 60 frames puis moyenne : une frame isolee ne dit rien sur
		// une machine dont la cadence varie d'un facteur dix.
		static SceUInt64 s_acc_sys = 0, s_acc_logic = 0;
		static SceUInt64 s_acc_world = 0, s_acc_post = 0, s_acc_total = 0; static SceUInt64 s_max_image = 0; static int s_n_lentes = 0;
		static int       s_prof_n = 0;
		const SceUInt64  t_frame0 = sceKernelGetProcessTimeWide();
		SceUInt64        t0;
		{
			static int s_frame = 0;
			if(( ++s_frame % 60 ) == 1 )
				VLOG( "MLP", "frame %d", s_frame );
		}
#endif
// lwss Add
// Vita écartée : ce bloc est un limiteur de frame par attente active
// (QueryPerformanceCounter + Sleep(0)) plus une gestion du focus fenêtre.
// Ni l'un ni l'autre n'a de sens ici — la cadence vient du vsync de
// vglSwapBuffers, et il n'y a pas de fenêtre à perdre de vue.
#if defined(__PLAT_WN32__) && !defined(__PLAT_VITA__)
		//if (g_windowJustWentOutOfFocus)
		//{
		//	if (D3DDevice_TestCooperativeLevel() != D3DERR_DEVICELOST)
		//	{
		//		D3D_ResetTextures();
		//		D3D_ResetBuffers();
		//		D3D_ReleaseRenderSurfaces();
		//		if (D3DDevice_Reset(&NxXbox::EngineGlobals.params) >= 0)
		//		{
		//			D3D_ReCreateTextures();
		//			D3D_ReCreateBuffers();
		//			D3D_RecreateRenderSurfaces();
		//			g_windowJustWentOutOfFocus = false;
		//		}
		//	}
		//	//NxXbox::g_disableRendering = true;
		//}

		static int s_mainLoopCounter = 0;

		LARGE_INTEGER perfcount;
		QueryPerformanceCounter(&perfcount);
		LONG itr;
		for (itr = perfcount.HighPart; perfcount.QuadPart < __PAIR64__(v3, v4) + perf.QuadPart; itr = perfcount.HighPart)
		{
			Sleep(0);
			QueryPerformanceCounter(&perfcount);
		}
		DWORD v8 = (__PAIR64__(itr, perfcount.LowPart) - perf.QuadPart) >> 32;
		DWORD v7 = perfcount.LowPart - perf.LowPart;
		if (__PAIR64__(itr, perfcount.LowPart) - perf.QuadPart > 20 * __PAIR64__(v3, v4))
		{
			perf.QuadPart = __PAIR64__(itr, perfcount.LowPart) - __PAIR64__(v3, v4);
			v7 = v4;
			v8 = v3;
		}
		if (s_mainLoopCounter >= 4 && g_isWindowInFocus)
		{
			s_mainLoopCounter = 0;
		}
		else
		{
			if (__PAIR64__(v8, v7) < __PAIR64__(v3, v4) + (__PAIR64__(v3, v4) >> 1) && g_isWindowInFocus)
			{
				s_mainLoopCounter = 0;
			}
			else
			{
				//++dword_69B6F0;
				++s_mainLoopCounter;
				//NxXbox::g_disableRendering = true;
			}
		}
		perf.QuadPart += __PAIR64__(v3, v4);

		Tmr::Vblank();
		Tmr::PreUpdateTimerInfo();
	
#endif
// lwss end
		if (trigger_profiling)
		{
			printf ("\nProfiling.....Start of main loop\n\n");
			trigger_profiling--;
			currently_profiling = 1;
		}
		

//#ifndef __PLAT_WN32__

#ifdef	__PLAT_NGPS__		
// for profiling on the PS2, we do a bit of the code from sPreRender here
// so we can time the final waiting for GS		
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PushContext( 0,0,255 );		// blue = wait for VU/DMA/GPU to finish
#	endif		
		NxPs2::WaitForRendering();		// PS2 Specific, wait for prior frame's DMA to finish	
		NxPs2::StuffAfterGSFinished();		// PS2 Specific, DMA altering stuff run after previous frame's DMA finished
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PopContext(  );
		Sys::CPUProfiler->PushContext( 128,0,0 );		// red = wait for vblank
#	endif		
		int framerate = Script::GetInteger(0x3214c818/*"lock_framerate"*/);
		if (framerate)	// normal vsync
		{
			static uint64 next_vblanks= 0;
			// Still call VSync, as it updates some functions
			Tmr::VSync();
#if defined(__PLAT_VITA__)
			// Vita : PAS d'attente active sur le compteur de vblanks.
			//
			// Sur PS2/Xbox, une interruption materielle incremente ce
			// compteur pendant l'attente. Chez nous rien ne l'incremente a
			// l'interieur de la boucle : des la 2e frame (next_vblanks vaut 0
			// au premier passage) c'est un DEADLOCK. C'est ce qui faisait que
			// la mainloop ne realisait qu'une seule iteration.
			//
			// La cadence est de toute facon assuree par le vsync de
			// vglSwapBuffers, appele dans s_plat_display.
			(void)next_vblanks;
#else
			while (Tmr::GetVblanks()<next_vblanks)
			{
				// just hanging until vblanks gets big enough
			}
#endif
			next_vblanks = Tmr::GetVblanks() + framerate;
		}
		else
		{
			Tmr::VSync1();
		}
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PopContext(  );
#	endif		
#endif				
#ifndef	__PLAT_NGC__		
#	ifdef __USE_PROFILER__
		Sys::Profiler::sStartFrame();		  		
#	endif		
#endif		
// Vita écartée : g_disableRendering est un drapeau du chemin DX9 (perte du
// device Direct3D, fenêtre hors focus). Sans la condition, le bloc suivant
// s'exécute toujours — c'est bien ce qu'on veut, on rend à chaque frame.
#if defined(__PLAT_WN32__) && !defined(__PLAT_VITA__)
		if (!NxXbox::g_disableRendering)
#endif
		{
			Nx::CEngine::sPreRender();			 			// start rendering previous frame's DMA list
		}

#ifdef	__PLAT_NGPS__		
		Sfx::CSpuManager::sUpdateStatus();				// Garrett: This should go into some system task, but I'll put it here for now
#endif

//		Sys::Profiler::sStartFrame();		  		
//#endif

#ifdef	__PLAT_NGC__		
#	ifdef __USE_PROFILER__
		Sys::Profiler::sStartFrame();		  		
#	endif		
#	endif		
					 
#ifdef __PLAT_VITA__
		t0 = sceKernelGetProcessTimeWide();
#endif
#ifdef __PLAT_VITA__
		// Demandes differees venant des raccourcis de debogage : elles doivent
		// s'executer ICI, entre deux images, et non depuis la lecture manette.
		VitaServiceFrontendRequest();
#endif
		service_system();
#ifdef __PLAT_VITA__
		s_acc_sys += sceKernelGetProcessTimeWide() - t0;
		t0 = sceKernelGetProcessTimeWide();
#endif

#if	defined(__PLAT_NGPS__) && defined(BATCH_TRI_COLLISION)
		// Enable VU0 collision
		bool got_vu0 = Nx::CBatchTriCollMan::sUseVU0Micro();
		Dbg_Assert(got_vu0);
#endif

 #ifdef	__PLAT_NGPS__		
//		snProfSetRange( -1, (void*)0, (void*)-1);
//		snProfSetFlagValue(0x01);
 #endif

		game_logic();
#ifdef __PLAT_VITA__
		s_acc_logic += sceKernelGetProcessTimeWide() - t0;
#endif		

 #ifdef	__PLAT_NGPS__		
//		snProfSetRange( 4, (void*)NULL, (void*)-1);
 #endif		


#if	defined(__PLAT_NGPS__) && defined(BATCH_TRI_COLLISION)
		// Disable VU0 collision
		if (got_vu0)
		{
			Nx::CBatchTriCollMan::sDisableVU0Micro();
		}
#endif

//#ifndef __PLAT_WN32__
		// Display the memory contents, (if memview is active)
		//MemView_Display();
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PushContext( 255, 255, 0 );  // yellow = render world
#	endif		

 #ifdef	__PLAT_NGPS__		
//		snProfSetRange( -1, (void*)0, (void*)-1);
//		snProfSetFlagValue(0x01);
 #endif
		
#ifdef __PLAT_VITA__
		t0 = sceKernelGetProcessTimeWide();
#endif
		Nx::CEngine::sRenderWorld();
#ifdef __PLAT_VITA__
		s_acc_world += sceKernelGetProcessTimeWide() - t0;
		t0 = sceKernelGetProcessTimeWide();
#endif		
#ifdef	__PLAT_NGC__		
#ifdef		__USE_PROFILER__
		Sys::Render_Profiler();		
#endif
#endif
		// Mick: bit of a patch here, we need some better debug hooks
		//Mdl::Rail_DebugRender();
#	ifdef __USE_PROFILER__
			Sys::CPUProfiler->PushContext( 0, 0, 0 );	 	// Black (Under Yellow) = sPostRender
#	endif // __USE_PROFILER__
// Vita écartée : g_disableRendering est un drapeau du chemin DX9 (perte du
// device Direct3D, fenêtre hors focus). Sans la condition, le bloc suivant
// s'exécute toujours — c'est bien ce qu'on veut, on rend à chaque frame.
#if defined(__PLAT_WN32__) && !defined(__PLAT_VITA__)
		if (!NxXbox::g_disableRendering)
#endif		
		{
			Nx::CEngine::sPostRender();		  // Previous frames profiler is rendered here
		}
#ifdef __PLAT_VITA__
		s_acc_post  += sceKernelGetProcessTimeWide() - t0;
		s_acc_total += sceKernelGetProcessTimeWide() - t_frame0;
		// A-coups : une moyenne sur 60 images les dilue (une image de 90 ms
		// ne fait que +1,5 ms de moyenne). Image la plus longue de la
		// fenetre, et nombre d'images de plus de 25 ms.
		{
			const SceUInt64 duree = sceKernelGetProcessTimeWide() - t_frame0;
			if( duree > s_max_image ) s_max_image = duree;
			if( duree > 25000 ) ++s_n_lentes;
		}
		if( ++s_prof_n >= 60 )
		{
			// Millisecondes moyennes par frame, phase par phase. � reste �
			// couvre tout ce qui n'est dans aucune des quatre.
			const double n = (double)s_prof_n;
			VLOG( "PROF", "ms/frame : total=%.1f | systeme=%.1f logique=%.1f "
			              "monde=%.1f presentation=%.1f reste=%.1f | pire %.1f, %d > 25 ms",
			      s_acc_total / 1000.0 / n, s_acc_sys / 1000.0 / n,
			      s_acc_logic / 1000.0 / n, s_acc_world / 1000.0 / n,
			      s_acc_post / 1000.0 / n,
			      ( s_acc_total - s_acc_sys - s_acc_logic
			        - s_acc_world - s_acc_post ) / 1000.0 / n,
			      s_max_image / 1000.0, s_n_lentes );
			s_max_image = 0;
			s_n_lentes  = 0;
			s_acc_sys = s_acc_logic = s_acc_world = s_acc_post = s_acc_total = 0;
			s_prof_n  = 0;
		}
#endif
#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PopContext(  );
#	endif		


	

#	ifdef __USE_PROFILER__
		Sys::CPUProfiler->PopContext(  );
#	endif		
		Tmr::OncePerRender();
 #ifdef	__PLAT_NGPS__		
//		snProfSetRange( 4, (void*)NULL, (void*)-1);
 #endif		
//#endif		
		
		currently_profiling = false;

	}

	done = old_flag;
}

void Manager::ProfileTasks(int n)
{
	trigger_profiling = n;
	currently_profiling = 0;
}


/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void		Manager::QuitLoop( void )
{
	

	Dbg_Notify( "Exiting..." );

	done = true;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void		Manager::DoGameLogic( void )
{
	game_logic();
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

} // namespace Mlp

