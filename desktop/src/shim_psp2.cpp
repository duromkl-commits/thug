/*****************************************************************************
**  THUG desktop -- Vita SDK calls on SDL2 and the C library                **
**  desktop/src/shim_psp2.cpp                                               **
*****************************************************************************/

#include "psp2_shim.h"
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "desktop_config.h"

// Host file info, read before the st_*time macros are removed below.
struct HostStat { bool dir; int64_t size; time_t c, a, m; };
static bool host_stat( const char *path, HostStat *h )
{
	struct stat st;
	if( stat( path, &st ) != 0 )
		return false;
	h->dir = ( st.st_mode & S_IFMT ) == S_IFDIR;
	h->size = (int64_t)st.st_size;
	h->c = st.st_ctime;
	h->a = st.st_atime;
	h->m = st.st_mtime;
	return true;
}
#undef st_atime
#undef st_mtime
#undef st_ctime

extern "C" void vita_log_printf( const char *sys, const char *fmt, ... );
#define DLOG( ... ) vita_log_printf( "DSK", __VA_ARGS__ )

// Vita-style error codes the backend only compares against < 0.
#define ERR_NOENT  ((int)0x80010002)
#define ERR_BADF   ((int)0x80010009)
#define ERR_GENERIC ((int)0x80010005)

// ===========================================================================
// Paths
// ===========================================================================

static std::string s_root;

extern "C" const char *desktop_data_root( void )
{
	if( s_root.empty())
	{
		const DesktopConfig &cfg = desktop_config();
		if( cfg.data_root[0] )
			s_root = cfg.data_root;
		else
		{
			char *base = SDL_GetBasePath();
			s_root = base ? base : "./";
			SDL_free( base );
		}
		if( !s_root.empty() && s_root[s_root.size() - 1] != '/' && s_root[s_root.size() - 1] != '\\' )
			s_root += "/";
	}
	return s_root.c_str();
}

#ifndef _WIN32
// Windows paths are case-insensitive; the game asks for "Data/pre/..." where
// an extracted ISO may hold "data/PRE/...". Resolve each component.
static std::string resolve_case( const std::string &path )
{
	struct stat st;
	if( stat( path.c_str(), &st ) == 0 )
		return path;
	std::string out = ( !path.empty() && path[0] == '/' ) ? "/" : "";
	size_t i = out.size();
	while( i <= path.size())
	{
		size_t j = path.find( '/', i );
		if( j == std::string::npos ) j = path.size();
		std::string comp = path.substr( i, j - i );
		if( !comp.empty())
		{
			std::string cand = out + comp;
			if( stat( cand.c_str(), &st ) != 0 )
			{
				DIR *d = opendir( out.empty() ? "." : out.c_str());
				if( d )
				{
					struct dirent *e;
					while(( e = readdir( d )))
						if( strcasecmp( e->d_name, comp.c_str()) == 0 )
						{
							cand = out + e->d_name;
							break;
						}
					closedir( d );
				}
			}
			out = cand;
			if( j < path.size()) out += "/";
		}
		i = j + 1;
	}
	return out;
}
#endif

extern "C" void desktop_host_path( const char *vita, char *out, size_t out_size )
{
	std::string p( vita ? vita : "" );
	std::string rel;
	static const char *prefixes[] = { "ux0:data/thug/", "ux0:/data/thug/", "ux0:data/thug", "app0:/", "app0:", NULL };
	bool mapped = false;
	for( int i = 0; prefixes[i]; ++i )
	{
		size_t l = strlen( prefixes[i] );
		if( strncasecmp( p.c_str(), prefixes[i], l ) == 0 )
		{
			rel = p.substr( l );
			mapped = true;
			break;
		}
	}
	if( !mapped )
	{
		size_t c = p.find( ':' );
		// "ux0:data/libshacccg.suprx" etc.: device paths outside the game folder
		// land in <root>/vita/<device>/...
		if( c != std::string::npos && c < 6 )
			rel = "vita/" + p.substr( 0, c ) + "/" + p.substr( c + 1 );
		else
			rel = p;
	}
	while( !rel.empty() && ( rel[0] == '/' || rel[0] == '\\' )) rel.erase( 0, 1 );
	std::string full = std::string( desktop_data_root()) + rel;
	for( size_t i = 0; i < full.size(); ++i )
		if( full[i] == '\\' ) full[i] = '/';
#ifndef _WIN32
	full = resolve_case( full );
#endif
	snprintf( out, out_size, "%s", full.c_str());
}

// ===========================================================================
// Files
// ===========================================================================

#define MAX_FD 256
static FILE *s_files[MAX_FD];
struct DirHandle { std::vector<std::string> names; size_t pos; std::string path; };
static DirHandle *s_dirs[MAX_FD];
static SDL_mutex *s_io_mutex = NULL;

static void io_lock( void )   { if( !s_io_mutex ) s_io_mutex = SDL_CreateMutex(); SDL_LockMutex( s_io_mutex ); }
static void io_unlock( void ) { SDL_UnlockMutex( s_io_mutex ); }

static int64_t ftell64( FILE *f )
{
#ifdef _WIN32
	return _ftelli64( f );
#else
	return ftello( f );
#endif
}
static int fseek64( FILE *f, int64_t o, int w )
{
#ifdef _WIN32
	return _fseeki64( f, o, w );
#else
	return fseeko( f, (off_t)o, w );
#endif
}

static void make_dirs_for( const std::string &file )
{
	for( size_t i = 1; i < file.size(); ++i )
		if( file[i] == '/' )
		{
			std::string d = file.substr( 0, i );
#ifdef _WIN32
			_mkdir( d.c_str());
#else
			mkdir( d.c_str(), 0777 );
#endif
		}
}

extern "C" SceUID sceIoOpen( const char *file, int flags, SceMode )
{
	char host[1024];
	desktop_host_path( file, host, sizeof( host ));
	const char *mode = "rb";
	const int acc = flags & SCE_O_RDWR;
	if( acc == SCE_O_RDONLY ) mode = "rb";
	else if( flags & SCE_O_APPEND ) mode = ( acc == SCE_O_RDWR ) ? "a+b" : "ab";
	else if( flags & SCE_O_TRUNC ) mode = ( acc == SCE_O_RDWR ) ? "w+b" : "wb";
	else if( flags & SCE_O_CREAT ) mode = "r+b";
	else mode = ( acc == SCE_O_RDWR ) ? "r+b" : "r+b";
	if( acc != SCE_O_RDONLY )
		make_dirs_for( host );
	FILE *f = fopen( host, mode );
	if( !f && ( flags & SCE_O_CREAT ) && strcmp( mode, "r+b" ) == 0 )
		f = fopen( host, "w+b" );
	if( !f )
		return ERR_NOENT;
	io_lock();
	for( int i = 1; i < MAX_FD; ++i )
		if( !s_files[i] && !s_dirs[i] )
		{
			s_files[i] = f;
			io_unlock();
			return i;
		}
	io_unlock();
	fclose( f );
	return ERR_GENERIC;
}

static FILE *fd_file( SceUID fd ) { return ( fd > 0 && fd < MAX_FD ) ? s_files[fd] : NULL; }

extern "C" int sceIoClose( SceUID fd )
{
	FILE *f = fd_file( fd );
	if( !f ) return ERR_BADF;
	io_lock();
	s_files[fd] = NULL;
	io_unlock();
	fclose( f );
	return 0;
}

extern "C" int sceIoRead( SceUID fd, void *data, SceSize size )
{
	FILE *f = fd_file( fd );
	if( !f ) return ERR_BADF;
	return (int)fread( data, 1, size, f );
}

extern "C" int sceIoWrite( SceUID fd, const void *data, SceSize size )
{
	FILE *f = fd_file( fd );
	if( !f ) return ERR_BADF;
	return (int)fwrite( data, 1, size, f );
}

// The log writes line by line and flushes, so a hung game still leaves it.
extern "C" void desktop_io_flush( SceUID fd )
{
	FILE *f = fd_file( fd );
	if( f ) fflush( f );
}

extern "C" SceOff sceIoLseek( SceUID fd, SceOff offset, int whence )
{
	FILE *f = fd_file( fd );
	if( !f ) return ERR_BADF;
	const int w = ( whence == SCE_SEEK_CUR ) ? SEEK_CUR : ( whence == SCE_SEEK_END ) ? SEEK_END : SEEK_SET;
	if( fseek64( f, offset, w ) != 0 )
		return ERR_GENERIC;
	return ftell64( f );
}

extern "C" int sceIoLseek32( SceUID fd, int offset, int whence )
{
	return (int)sceIoLseek( fd, offset, whence );
}

extern "C" int sceIoSyncByFd( SceUID fd, int )
{
	FILE *f = fd_file( fd );
	if( !f ) return ERR_BADF;
	fflush( f );
	return 0;
}

extern "C" int sceIoRemove( const char *file )
{
	char host[1024];
	desktop_host_path( file, host, sizeof( host ));
	return remove( host ) == 0 ? 0 : ERR_NOENT;
}

extern "C" int sceIoMkdir( const char *dir, SceMode )
{
	char host[1024];
	desktop_host_path( dir, host, sizeof( host ));
	std::string h( host );
	make_dirs_for( h + "/" );
	struct stat st;
	return ( stat( host, &st ) == 0 ) ? 0 : ERR_GENERIC;
}

extern "C" int sceIoRmdir( const char *path )
{
	char host[1024];
	desktop_host_path( path, host, sizeof( host ));
#ifdef _WIN32
	return _rmdir( host ) == 0 ? 0 : ERR_GENERIC;
#else
	return rmdir( host ) == 0 ? 0 : ERR_GENERIC;
#endif
}

extern "C" int sceIoRename( const char *a, const char *b )
{
	char ha[1024], hb[1024];
	desktop_host_path( a, ha, sizeof( ha ));
	desktop_host_path( b, hb, sizeof( hb ));
	remove( hb );
	return rename( ha, hb ) == 0 ? 0 : ERR_GENERIC;
}

static void to_datetime( time_t t, SceDateTime *d )
{
	struct tm *tm = localtime( &t );
	memset( d, 0, sizeof( *d ));
	if( !tm ) return;
	d->year = (unsigned short)( tm->tm_year + 1900 );
	d->month = (unsigned short)( tm->tm_mon + 1 );
	d->day = (unsigned short)tm->tm_mday;
	d->hour = (unsigned short)tm->tm_hour;
	d->minute = (unsigned short)tm->tm_min;
	d->second = (unsigned short)tm->tm_sec;
}

static int stat_host( const char *host, SceIoStat *out )
{
	HostStat h;
	if( !host_stat( host, &h ))
		return ERR_NOENT;
	memset( out, 0, sizeof( *out ));
	out->st_mode = ( h.dir ? SCE_S_IFDIR : SCE_S_IFREG ) | 0777;
	out->st_attr = h.dir ? SCE_SO_IFDIR : SCE_SO_IFREG;
	out->st_size = (SceOff)h.size;
	to_datetime( h.c, &out->st_ctime );
	to_datetime( h.a, &out->st_atime );
	to_datetime( h.m, &out->st_mtime );
	return 0;
}

extern "C" int sceIoGetstat( const char *file, SceIoStat *out )
{
	char host[1024];
	desktop_host_path( file, host, sizeof( host ));
	// stat() refuses a trailing slash on Windows
	size_t l = strlen( host );
	while( l > 1 && host[l - 1] == '/' ) host[--l] = 0;
	return stat_host( host, out );
}

extern "C" int sceIoGetstatByFd( SceUID fd, SceIoStat *out )
{
	FILE *f = fd_file( fd );
	if( !f ) return ERR_BADF;
	memset( out, 0, sizeof( *out ));
	int64_t cur = ftell64( f );
	fseek64( f, 0, SEEK_END );
	out->st_size = ftell64( f );
	fseek64( f, cur, SEEK_SET );
	out->st_mode = SCE_S_IFREG | 0777;
	return 0;
}

extern "C" SceUID sceIoDopen( const char *dirname )
{
	char host[1024];
	desktop_host_path( dirname, host, sizeof( host ));
	DirHandle *d = new DirHandle;
	d->pos = 0;
	d->path = host;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(( d->path + "/*" ).c_str(), &fd );
	if( h == INVALID_HANDLE_VALUE ) { delete d; return ERR_NOENT; }
	do
	{
		if( strcmp( fd.cFileName, "." ) && strcmp( fd.cFileName, ".." ))
			d->names.push_back( fd.cFileName );
	} while( FindNextFileA( h, &fd ));
	FindClose( h );
#else
	DIR *dd = opendir( host );
	if( !dd ) { delete d; return ERR_NOENT; }
	struct dirent *e;
	while(( e = readdir( dd )))
		if( strcmp( e->d_name, "." ) && strcmp( e->d_name, ".." ))
			d->names.push_back( e->d_name );
	closedir( dd );
#endif
	io_lock();
	for( int i = 1; i < MAX_FD; ++i )
		if( !s_files[i] && !s_dirs[i] )
		{
			s_dirs[i] = d;
			io_unlock();
			return i;
		}
	io_unlock();
	delete d;
	return ERR_GENERIC;
}

extern "C" int sceIoDread( SceUID fd, SceIoDirent *out )
{
	DirHandle *d = ( fd > 0 && fd < MAX_FD ) ? s_dirs[fd] : NULL;
	if( !d ) return ERR_BADF;
	if( d->pos >= d->names.size())
		return 0;
	memset( out, 0, sizeof( *out ));
	const std::string &n = d->names[d->pos++];
	snprintf( out->d_name, sizeof( out->d_name ), "%s", n.c_str());
	stat_host(( d->path + "/" + n ).c_str(), &out->d_stat );
	return 1;
}

extern "C" int sceIoDclose( SceUID fd )
{
	DirHandle *d = ( fd > 0 && fd < MAX_FD ) ? s_dirs[fd] : NULL;
	if( !d ) return ERR_BADF;
	io_lock();
	s_dirs[fd] = NULL;
	io_unlock();
	delete d;
	return 0;
}

extern "C" int sceIoDevctl( const char *, unsigned int, void *, int, void *outdata, int outlen )
{
	// Only used to ask for free space on ux0: report plenty.
	if( outdata && outlen >= 16 )
	{
		memset( outdata, 0, outlen );
		uint64_t *o = (uint64_t *)outdata;
		o[0] = 64ull << 30;		// max
		o[1] = 32ull << 30;		// free
	}
	return 0;
}

// ===========================================================================
// Time, threads, synchronisation
// ===========================================================================

static Uint64 s_t0 = 0;
extern "C" SceUInt64 sceKernelGetProcessTimeWide( void )
{
	const Uint64 f = SDL_GetPerformanceFrequency();
	const Uint64 c = SDL_GetPerformanceCounter();
	if( !s_t0 ) s_t0 = c;
	const Uint64 d = c - s_t0;
	return (SceUInt64)(( d / f ) * 1000000ull + (( d % f ) * 1000000ull ) / f );
}
extern "C" SceUInt32 sceKernelGetProcessTimeLow( void ) { return (SceUInt32)sceKernelGetProcessTimeWide(); }
extern "C" int sceKernelGetProcessTime( SceKernelSysClock *c ) { if( c ) *c = sceKernelGetProcessTimeWide(); return 0; }

extern "C" int sceKernelDelayThread( SceUInt us )
{
	if( us >= 1000 )
		SDL_Delay( us / 1000 );
	else
		SDL_Delay( 0 );
	return 0;
}
extern "C" int sceKernelDelayThreadCB( SceUInt us ) { return sceKernelDelayThread( us ); }

struct ThreadRec
{
	SceKernelThreadEntry entry;
	SDL_Thread *thread;
	std::string name;
	SceSize arglen;
	void *argp;
	int result;
};
static std::map<int, ThreadRec *> s_threads;
static int s_next_uid = 0x100;
static SDL_mutex *s_k_mutex = NULL;
static void k_lock( void )   { if( !s_k_mutex ) s_k_mutex = SDL_CreateMutex(); SDL_LockMutex( s_k_mutex ); }
static void k_unlock( void ) { SDL_UnlockMutex( s_k_mutex ); }

static int thread_trampoline( void *p )
{
	ThreadRec *t = (ThreadRec *)p;
	t->result = t->entry( t->arglen, t->argp );
	return t->result;
}

extern "C" SceUID sceKernelCreateThread( const char *name, SceKernelThreadEntry entry, int, SceSize,
                                         SceUInt, int, const SceKernelThreadOptParam * )
{
	ThreadRec *t = new ThreadRec;
	t->entry = entry;
	t->thread = NULL;
	t->name = name ? name : "thread";
	t->arglen = 0;
	t->argp = NULL;
	t->result = 0;
	k_lock();
	const int id = s_next_uid++;
	s_threads[id] = t;
	k_unlock();
	return id;
}

extern "C" int sceKernelStartThread( SceUID thid, SceSize arglen, void *argp )
{
	k_lock();
	std::map<int, ThreadRec *>::iterator it = s_threads.find( thid );
	ThreadRec *t = ( it == s_threads.end()) ? NULL : it->second;
	k_unlock();
	if( !t ) return ERR_GENERIC;
	// The Vita copies the argument block onto the new thread's stack.
	if( arglen && argp )
	{
		t->argp = malloc( arglen );
		memcpy( t->argp, argp, arglen );
	}
	t->arglen = arglen;
	t->thread = SDL_CreateThreadWithStackSize( thread_trampoline, t->name.c_str(), 1024 * 1024, t );
	return t->thread ? 0 : ERR_GENERIC;
}

extern "C" int sceKernelWaitThreadEnd( SceUID thid, int *stat, SceUInt * )
{
	k_lock();
	std::map<int, ThreadRec *>::iterator it = s_threads.find( thid );
	ThreadRec *t = ( it == s_threads.end()) ? NULL : it->second;
	k_unlock();
	if( !t || !t->thread ) return ERR_GENERIC;
	int r = 0;
	SDL_WaitThread( t->thread, &r );
	t->thread = NULL;
	if( stat ) *stat = r;
	return 0;
}

extern "C" int sceKernelDeleteThread( SceUID thid )
{
	k_lock();
	std::map<int, ThreadRec *>::iterator it = s_threads.find( thid );
	if( it != s_threads.end())
	{
		if( it->second->thread ) SDL_DetachThread( it->second->thread );
		s_threads.erase( it );
	}
	k_unlock();
	return 0;
}

extern "C" int sceKernelExitDeleteThread( int ) { return 0; }
extern "C" int sceKernelGetThreadId( void ) { return (int)SDL_ThreadID(); }
extern "C" int sceKernelChangeThreadPriority( SceUID, int ) { return 0; }
extern "C" int sceKernelGetThreadCpuRegisters( SceUID, SceThreadCpuRegisters *r ) { if( r ) memset( r, 0, sizeof( *r )); return ERR_GENERIC; }

static std::map<int, SDL_sem *> s_semas;
static std::map<int, SDL_mutex *> s_mutexes;

extern "C" SceUID sceKernelCreateSema( const char *, SceUInt, int initVal, int, SceKernelSemaOptParam * )
{
	SDL_sem *s = SDL_CreateSemaphore( (Uint32)( initVal < 0 ? 0 : initVal ));
	if( !s ) return ERR_GENERIC;
	k_lock();
	const int id = s_next_uid++;
	s_semas[id] = s;
	k_unlock();
	return id;
}
static SDL_sem *sema( SceUID id )
{
	k_lock();
	std::map<int, SDL_sem *>::iterator it = s_semas.find( id );
	SDL_sem *s = ( it == s_semas.end()) ? NULL : it->second;
	k_unlock();
	return s;
}
extern "C" int sceKernelDeleteSema( SceUID id )
{
	SDL_sem *s = sema( id );
	if( !s ) return ERR_GENERIC;
	k_lock();
	s_semas.erase( id );
	k_unlock();
	SDL_DestroySemaphore( s );
	return 0;
}
extern "C" int sceKernelSignalSema( SceUID id, int n )
{
	SDL_sem *s = sema( id );
	if( !s ) return ERR_GENERIC;
	for( int i = 0; i < n; ++i ) SDL_SemPost( s );
	return 0;
}
extern "C" int sceKernelWaitSema( SceUID id, int n, SceUInt *timeout )
{
	SDL_sem *s = sema( id );
	if( !s ) return ERR_GENERIC;
	for( int i = 0; i < n; ++i )
	{
		if( timeout )
		{
			if( SDL_SemWaitTimeout( s, *timeout / 1000 ) != 0 )
				return (int)0x80028148;		// SCE_KERNEL_ERROR_WAIT_TIMEOUT
		}
		else
			SDL_SemWait( s );
	}
	return 0;
}
extern "C" int sceKernelPollSema( SceUID id, int n )
{
	SDL_sem *s = sema( id );
	if( !s ) return ERR_GENERIC;
	if( (int)SDL_SemValue( s ) < n ) return (int)0x80028045;
	for( int i = 0; i < n; ++i ) SDL_SemTryWait( s );
	return 0;
}

extern "C" SceUID sceKernelCreateMutex( const char *, SceUInt, int initCount, SceKernelMutexOptParam * )
{
	SDL_mutex *m = SDL_CreateMutex();
	if( !m ) return ERR_GENERIC;
	for( int i = 0; i < initCount; ++i ) SDL_LockMutex( m );
	k_lock();
	const int id = s_next_uid++;
	s_mutexes[id] = m;
	k_unlock();
	return id;
}
static SDL_mutex *mutex( SceUID id )
{
	k_lock();
	std::map<int, SDL_mutex *>::iterator it = s_mutexes.find( id );
	SDL_mutex *m = ( it == s_mutexes.end()) ? NULL : it->second;
	k_unlock();
	return m;
}
extern "C" int sceKernelDeleteMutex( SceUID id )
{
	SDL_mutex *m = mutex( id );
	if( !m ) return ERR_GENERIC;
	k_lock();
	s_mutexes.erase( id );
	k_unlock();
	SDL_DestroyMutex( m );
	return 0;
}
extern "C" int sceKernelLockMutex( SceUID id, int n, unsigned int * )
{
	SDL_mutex *m = mutex( id );
	if( !m ) return ERR_GENERIC;
	for( int i = 0; i < n; ++i ) SDL_LockMutex( m );
	return 0;
}
extern "C" int sceKernelTryLockMutex( SceUID id, int n )
{
	SDL_mutex *m = mutex( id );
	if( !m ) return ERR_GENERIC;
	if( SDL_TryLockMutex( m ) != 0 ) return (int)0x80028140;
	for( int i = 1; i < n; ++i ) SDL_LockMutex( m );
	return 0;
}
extern "C" int sceKernelUnlockMutex( SceUID id, int n )
{
	SDL_mutex *m = mutex( id );
	if( !m ) return ERR_GENERIC;
	for( int i = 0; i < n; ++i ) SDL_UnlockMutex( m );
	return 0;
}

extern "C" int sceKernelCreateLwMutex( SceKernelLwMutexWork *w, const char *, unsigned int, int initCount, const SceKernelLwMutexOptParam * )
{
	SDL_mutex *m = SDL_CreateMutex();
	for( int i = 0; i < initCount; ++i ) SDL_LockMutex( m );
	memset( w, 0, sizeof( *w ));
	memcpy( w->data, &m, sizeof( m ));
	return 0;
}
static SDL_mutex *lw( SceKernelLwMutexWork *w ) { SDL_mutex *m; memcpy( &m, w->data, sizeof( m )); return m; }
extern "C" int sceKernelDeleteLwMutex( SceKernelLwMutexWork *w ) { SDL_DestroyMutex( lw( w )); return 0; }
extern "C" int sceKernelLockLwMutex( SceKernelLwMutexWork *w, int n, unsigned int * ) { for( int i = 0; i < n; ++i ) SDL_LockMutex( lw( w )); return 0; }
extern "C" int sceKernelTryLockLwMutex( SceKernelLwMutexWork *w, int ) { return SDL_TryLockMutex( lw( w )) == 0 ? 0 : (int)0x80028140; }
extern "C" int sceKernelUnlockLwMutex( SceKernelLwMutexWork *w, int n ) { for( int i = 0; i < n; ++i ) SDL_UnlockMutex( lw( w )); return 0; }

extern "C" int sceKernelPowerTick( int ) { return 0; }
extern "C" int sceKernelExitProcess( int res )
{
	fflush( NULL );
	SDL_Quit();
	exit( res );
	return 0;
}
extern "C" int sceKernelGetFreeMemorySize( SceKernelFreeMemorySizeInfo *info )
{
	if( !info ) return ERR_GENERIC;
	info->size_user = 256 * 1024 * 1024;
	info->size_cdram = 128 * 1024 * 1024;
	info->size_phycont = 26 * 1024 * 1024;
	return 0;
}

static std::map<int, void *> s_blocks;
extern "C" SceUID sceKernelAllocMemBlock( const char *, SceKernelMemBlockType, SceSize size, SceKernelAllocMemBlockOpt * )
{
	void *p = calloc( 1, size );
	if( !p ) return ERR_GENERIC;
	k_lock();
	const int id = s_next_uid++;
	s_blocks[id] = p;
	k_unlock();
	return id;
}
extern "C" int sceKernelFreeMemBlock( SceUID uid )
{
	k_lock();
	std::map<int, void *>::iterator it = s_blocks.find( uid );
	if( it != s_blocks.end()) { free( it->second ); s_blocks.erase( it ); }
	k_unlock();
	return 0;
}
extern "C" int sceKernelGetMemBlockBase( SceUID uid, void **basep )
{
	k_lock();
	std::map<int, void *>::iterator it = s_blocks.find( uid );
	*basep = ( it == s_blocks.end()) ? NULL : it->second;
	k_unlock();
	return *basep ? 0 : ERR_GENERIC;
}

extern "C" int sceClibPrintf( const char *fmt, ... )
{
	va_list a;
	va_start( a, fmt );
	int n = vfprintf( stdout, fmt, a );
	va_end( a );
	return n;
}
extern "C" int sceClibSnprintf( char *dst, SceSize len, const char *fmt, ... )
{
	va_list a;
	va_start( a, fmt );
	int n = vsnprintf( dst, len, fmt, a );
	va_end( a );
	return n;
}

// ===========================================================================
// Power, display, rtc, modules, network: nothing to do on a PC
// ===========================================================================

extern "C" int scePowerSetArmClockFrequency( int ) { return 0; }
extern "C" int scePowerSetBusClockFrequency( int ) { return 0; }
extern "C" int scePowerSetGpuClockFrequency( int ) { return 0; }
extern "C" int scePowerSetGpuXbarClockFrequency( int ) { return 0; }
extern "C" int scePowerGetArmClockFrequency( void ) { return 444; }
extern "C" int scePowerGetBusClockFrequency( void ) { return 222; }
extern "C" int scePowerGetGpuClockFrequency( void ) { return 222; }
extern "C" int scePowerGetGpuXbarClockFrequency( void ) { return 166; }
extern "C" int sceDisplayWaitVblankStart( void ) { return 0; }
extern "C" int sceDisplayWaitVblankStartMulti( unsigned int ) { return 0; }
extern "C" int sceRtcGetCurrentClockLocalTime( SceDateTime *t ) { to_datetime( time( NULL ), t ); return 0; }
extern "C" int sceSysmoduleLoadModule( SceUInt16 ) { return 0; }
extern "C" int sceNetInit( void * ) { return ERR_GENERIC; }
extern "C" int sceNetCtlInit( void ) { return ERR_GENERIC; }
extern "C" int sceNetSocket( const char *, int, int, int ) { return ERR_GENERIC; }
extern "C" int sceNetSocketClose( int ) { return 0; }
extern "C" int sceNetBind( int, const void *, unsigned int ) { return ERR_GENERIC; }
extern "C" int sceNetListen( int, int ) { return ERR_GENERIC; }
extern "C" int sceNetAccept( int, void *, unsigned int * ) { return ERR_GENERIC; }
extern "C" int sceNetSend( int, const void *, unsigned int, int ) { return ERR_GENERIC; }
extern "C" int sceNetRecv( int, void *, unsigned int, int ) { return ERR_GENERIC; }
extern "C" int sceNetSetsockopt( int, int, int, const void *, unsigned int ) { return ERR_GENERIC; }
extern "C" unsigned short sceNetHtons( unsigned short n ) { return (unsigned short)(( n >> 8 ) | ( n << 8 )); }

// ===========================================================================
// Touch: no touch panels on a PC
// ===========================================================================

extern "C" int sceTouchSetSamplingState( SceUInt32, SceTouchSamplingState ) { return 0; }
extern "C" int sceTouchPeek( SceUInt32, SceTouchData *d, SceUInt32 )
{
	if( d ) memset( d, 0, sizeof( *d ));
	return 1;
}
extern "C" int sceTouchGetPanelInfo( SceUInt32 port, SceTouchPanelInfo *p )
{
	memset( p, 0, sizeof( *p ));
	p->maxAaX = p->maxDispX = 1919;
	p->maxAaY = p->maxDispY = ( port == SCE_TOUCH_PORT_FRONT ) ? 1087 : 889;
	p->maxForce = 128;
	return 0;
}
