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
}

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
#endif
