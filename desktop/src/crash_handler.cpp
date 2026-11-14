// Crash report for the desktop build: on a crash, writes the exception, the
// faulting address and the code addresses found on the stack to thug.log
// (as offsets into thug.exe, which addr2line maps back to source lines with
// the unstripped build), then flushes the log, which is buffered.
#include <stdio.h>
#include <stdint.h>

extern "C" void vita_log_flush( void );
extern "C" void vita_log_printf( const char *sys, const char *fmt, ... );

#ifdef _WIN32
#include <windows.h>

static uintptr_t s_base = 0, s_fin = 0;

static bool dans_exe( uintptr_t a ) { return a >= s_base && a < s_fin; }

static HANDLE s_principal = NULL;
static volatile LONG s_battement = 0;

static void echantillon( int k )
{
	// Nothing that can lock (logging, the heap) while the main thread is
	// suspended: it may hold the lock. Copy first, log after resuming.
	uintptr_t pc = 0, pile[30]; int n = 0;
	if( SuspendThread( s_principal ) == (DWORD)-1 ) return;
	CONTEXT c; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if( GetThreadContext( s_principal, &c ))
	{
#ifndef _WIN64
		pc = c.Eip; const uintptr_t *sp = (const uintptr_t *)c.Esp;
#else
		pc = c.Rip; const uintptr_t *sp = (const uintptr_t *)c.Rsp;
#endif
		MEMORY_BASIC_INFORMATION mbi;
		if( VirtualQuery( sp, &mbi, sizeof( mbi )))
		{
			const uintptr_t *fin = (const uintptr_t *)( (uintptr_t)mbi.BaseAddress + mbi.RegionSize );
			for( const uintptr_t *p = sp; p < fin && n < 30; ++p )
				if( dans_exe( *p )) pile[n++] = *p;
		}
	}
	ResumeThread( s_principal );
	vita_log_printf( "WDOG", "hang sample %d at %s+0x%lx", k, dans_exe( pc ) ? "thug.exe" : "?",
	          (unsigned long)( dans_exe( pc ) ? pc - s_base : pc ));
	for( int i = 0; i < n; ++i )
		vita_log_printf( "WDOG", "  stack thug.exe+0x%lx", (unsigned long)( pile[i] - s_base ));
	vita_log_flush();
}

static DWORD WINAPI veille( void * )
{
	LONG vu = -1; int immobile = 0, attente = 20;
	for( ;; )
	{
		Sleep( 1000 );
		const LONG b = s_battement;
		if( b != vu ) { vu = b; immobile = 0; attente = 20; continue; }
		if( b == 0 || ++immobile < attente ) continue;	// not started drawing yet
		vita_log_printf( "WDOG", "no frame for %d s", immobile );
		for( int k = 0; k < 3; ++k ) { echantillon( k ); Sleep( 1000 ); }
		immobile = 0; attente = 60;
	}
	return 0;
}

static LONG WINAPI rapport( EXCEPTION_POINTERS *ep )
{
	const EXCEPTION_RECORD *er = ep->ExceptionRecord;
	const uintptr_t pc = (uintptr_t)er->ExceptionAddress;
	vita_log_printf( "CRASH", "exception 0x%08lx at %s+0x%lx (base 0x%lx)",
	          (unsigned long)er->ExceptionCode, dans_exe( pc ) ? "thug.exe" : "?",
	          (unsigned long)( dans_exe( pc ) ? pc - s_base : pc ), (unsigned long)s_base );
	if( er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 )
		vita_log_printf( "CRASH", "  %s address 0x%08lx", er->ExceptionInformation[0] ? "writing" : "reading",
		          (unsigned long)er->ExceptionInformation[1] );
#ifndef _WIN64
	const uintptr_t *sp = (const uintptr_t *)ep->ContextRecord->Esp;
#else
	const uintptr_t *sp = (const uintptr_t *)ep->ContextRecord->Rsp;
#endif
	// Return addresses on the stack (a scan, not an exact unwind: some are stale).
	int n = 0;
	MEMORY_BASIC_INFORMATION mbi;
	if( VirtualQuery( sp, &mbi, sizeof( mbi )))
	{
		const uintptr_t *fin = (const uintptr_t *)( (uintptr_t)mbi.BaseAddress + mbi.RegionSize );
		for( const uintptr_t *p = sp; p < fin && n < 40; ++p )
			if( dans_exe( *p ))
			{
				vita_log_printf( "CRASH", "  stack thug.exe+0x%lx", (unsigned long)( *p - s_base ));
				++n;
			}
	}
	vita_log_flush();
	fflush( NULL );
	// End here: left to Windows, the window could sit frozen until the
	// process was killed from Task Manager.
	TerminateProcess( GetCurrentProcess(), er->ExceptionCode );
	return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void desktop_crash_handler_install( void )
{
	HMODULE m = GetModuleHandleA( NULL );
	s_base = (uintptr_t)m;
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)m;
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)( (const char *)m + dos->e_lfanew );
	s_fin = s_base + nt->OptionalHeader.SizeOfImage;
	SetUnhandledExceptionFilter( rapport );
	DuplicateHandle( GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_principal,
	                 THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0 );
	CreateThread( NULL, 0, veille, NULL, 0, NULL );
}

// Hang report: no frame shown for 20 s (the game stuck in a loop, Windows'
// "not responding") -> the main thread's position and stack go to thug.log,
// three samples a second apart, then again if it is still stuck 60 s on.
extern "C" void desktop_watchdog_beat( void ) { InterlockedIncrement( &s_battement ); }

#else
#include <signal.h>
#include <string.h>
#include <execinfo.h>
#include <unistd.h>

static void rapport( int sig )
{
	vita_log_printf( "CRASH", "signal %d", sig );
	void *pile[40];
	const int n = backtrace( pile, 40 );
	for( int i = 0; i < n; ++i )
		vita_log_printf( "CRASH", "  stack %p", pile[i] );
	vita_log_flush();
	fflush( NULL );
	signal( sig, SIG_DFL );
	raise( sig );
}

extern "C" void desktop_crash_handler_install( void )
{
	signal( SIGSEGV, rapport );
	signal( SIGILL, rapport );
	signal( SIGFPE, rapport );
	signal( SIGABRT, rapport );
}
extern "C" void desktop_watchdog_beat( void ) {}
#endif
