/*****************************************************************************
**																			**
**			              Neversoft Entertainment							**
**																		   	**
**				   Copyright (C) 1999 - All Rights Reserved				   	**
**																			**
******************************************************************************
**																			**
**	Project:		Core Library											**
**																			**
**	Module:			Task Manager (TSK)										**
**																			**
**	File name:		tlist.cpp												**
**																			**
**	Created by:		05/27/99	-	mjb										**
**																			**
**	Description:	Task List Support										**
**																			**
*****************************************************************************/

/*****************************************************************************
**							  	  Includes									**
*****************************************************************************/

#ifdef __PLAT_VITA__
#include <psp2/kernel/processmgr.h>
#include "vita_log.h"
#endif
#include <core/defines.h>
#include <core/task.h>

#include <sys/profiler.h>			// Including for debugging
#include <sys/timer.h>				// Including for debugging

/*****************************************************************************
**							  DBG Information								**
*****************************************************************************/

namespace Tsk
{



/*****************************************************************************
**								   Externals								**
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


/*****************************************************************************
**								 Public Data								**
*****************************************************************************/

List*	TSK_BkGndTasks = NULL;

/*****************************************************************************
**							  Private Prototypes							**
*****************************************************************************/


/*****************************************************************************
**							   Private Functions							**
*****************************************************************************/



/*****************************************************************************
**							   Public Functions								**
*****************************************************************************/

List::List( void )
: stamp( 0 ), list_changed( false )
{
	
	
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

List::~List ( void )
{
	
	
	Dbg_MsgAssert( list.IsEmpty(),( "Task List Not Empty" ));
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void		List::Process( bool time, uint mask )
{
	

	if ( !HaveAccess ())
	{
		return;
	}

	stamp++;
	
	do
	{
		Lst::Search<BaseTask>	iterator;
		BaseTask*				next = iterator.FirstItem( list );

		list_changed = false; 
	
		while ( next )
		{
			BaseTask*	task = next;

			Dbg_AssertType( task, BaseTask );
			Dbg_AssertPtr( task );
			Dbg_AssertPtr( task->node );
			Dbg_MsgAssert( task->node->InList(),( "Task was removed from list by previous task" ));
			
			next = iterator.NextItem();		// get next task before we execute the current 	
											// one as the task might remove itself
			
			Dbg_MsgAssert(!list_changed,( "list changed, you fucker"));

			if ( task->HaveAccess() )
			{
				if ( task->stamp != stamp )
				{
					task->stamp = stamp;
					
#ifdef		__USE_PROFILER__			
					Tmr::MicroSeconds start_time=0;
#ifdef __PLAT_NGPS__
					Tmr::MicroSeconds end_time=0;
#endif
					if (time)
					{
						start_time =  Tmr::GetTimeInUSeconds();
					}
#endif
					// only call the task if it is not masked off
					if (! (task->GetMask() & mask))
					{
#ifdef __PLAT_VITA__
						// game_logic() prend 80 % de la frame et se resume a
						// cette boucle de taches. On chronometre CHAQUE tache
						// et on retient la plus chere du lot : son adresse de
						// code se resout hors ligne avec nm, comme pour les
						// c-functions QB.
						//
						// Cumul sur 60 frames : une tache isolee ne dit rien
						// sur une machine dont la cadence varie.
						const SceUInt64 t_task = sceKernelGetProcessTimeWide();
						task->vCall();
						const SceUInt64 dt = sceKernelGetProcessTimeWide() - t_task;

						// Classement par TACHE (adresse de code) : la plus
						// chere en cumul, pas le pire appel isole. Toutes les
						// 2000 executions de tache (~10 s), les 8 premieres
						// sont journalisees ; vita/tools/prof.py les nomme.
						struct STacheProf { void *code; SceUInt64 us; int n; };
						static STacheProf s_tab[64];
						static int        s_ntab  = 0;
						static int        s_calls = 0;
						static SceUInt64  s_sum   = 0;
						void *code = (void *)task->GetCode();
						int k = 0;
						while(( k < s_ntab ) && ( s_tab[k].code != code ))
							++k;
						if(( k == s_ntab ) && ( s_ntab < 64 ))
						{
							s_tab[k].code = code; s_tab[k].us = 0; s_tab[k].n = 0;
							++s_ntab;
						}
						if( k < s_ntab )
						{
							s_tab[k].us += dt;
							++s_tab[k].n;
						}
						s_sum += dt;
						if( ++s_calls >= 20000 )
						{
							VLOG( "PROF", "taches : %d appels, %.1f ms cumulees, %d taches distinctes, repere vita_log_printf=%p",
							      s_calls, s_sum / 1000.0, s_ntab, (void *)&vita_log_printf );
							for( int r = 0; r < 8; ++r )
							{
								int best = -1;
								for( int j = 0; j < s_ntab; ++j )
									if( s_tab[j].n >= 0 && ( best < 0 || s_tab[j].us > s_tab[best].us ))
										best = j;
								if(( best < 0 ) || ( s_tab[best].n < 0 ))
									break;
								VLOG( "PROF", "  tache %p : %.1f ms (%.1f%%), %d appels",
								      s_tab[best].code, s_tab[best].us / 1000.0,
								      s_sum ? 100.0 * s_tab[best].us / s_sum : 0.0,
								      s_tab[best].n );
								s_tab[best].n = -1;		// deja sortie
							}
							s_ntab = 0; s_calls = 0; s_sum = 0;
						}
#else
						task->vCall();
#endif
					}
#ifdef		__USE_PROFILER__			
#ifdef __PLAT_NGPS__
					if (time)
					{
						end_time = Tmr::GetTimeInUSeconds();
						
						int length = (int)(end_time - start_time);
						int	size;
						printf ("%6d %s\n",length,MemView_GetFunctionName((int)task->GetCode(), &size));
					}
#endif
#endif				
				}
			}

			if ( list_changed )
			{
				break;
			}
		}

	} while ( list_changed );

}


/******************************************************************/
// Mick Debugging: Dump out the contents of the task list
// 
/******************************************************************/

void		List::Dump( void )
{
	

	Lst::Search<BaseTask>	iterator;
	BaseTask*				next = iterator.FirstItem( list );

	list_changed = false; 

	while ( next )
	{
		BaseTask*	task = next;

		Dbg_AssertType( task, BaseTask );
		Dbg_AssertPtr( task );
		Dbg_AssertPtr( task->node );
		
		next = iterator.NextItem();		// get next task before we execute the current 	
										// one as the task might remove itself
#ifdef __PLAT_NGPS__
		if ( task->HaveAccess() )
		{
//				char *MemView_GetFunctionName(int pc, int *p_size);				
//				task->vCall();
				int size;
				printf ("Task %p %s\n",task->GetCode(),
				MemView_GetFunctionName((int)task->GetCode(), &size));
		}
#elif defined( __PLAT_NGC__ )
		if ( task->HaveAccess() )
		{
//				char *MemView_GetFunctionName(int pc, int *p_size);				
//				task->vCall();
				Dbg_Printf ("Task %p\n",task->GetCode() );
//				MemView_GetFunctionName((int)task->GetCode(), &size));
		}
#endif
	}
}


/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void		List::AddTask( BaseTask& task )
{
	
	
	Dbg_AssertType( &task, BaseTask );
	Dbg_MsgAssert( !task.node->InList(),( "Task is already in a list" ));

	Forbid ();			// stop any processing while list is modified
	
	list.AddNode( task.node );
	task.tlist = this;
	task.stamp = stamp - 1;
	list_changed = true;

	Permit();
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void		List::RemoveAllTasks( void )
{
	

	Lst::Search<BaseTask>	iterator;
	BaseTask*				next;

	Forbid();
	
	while (( next = iterator.FirstItem( list ) ))
	{
		next->Remove();
		list_changed = true;
	}
		
	Permit();

}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void		List::SignalListChange( void )
{
	list_changed = true;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
} // namespace Tsk

