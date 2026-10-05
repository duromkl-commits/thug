/*****************************************************************************
**  THUG-Vita — couche système                                              **
**  Code/Sys/Vita/p_filesys.cpp                                             **
**                                                                          **
**  VRAIE implémentation, comme le timer : c'est cette couche qui devra     **
**  ouvrir les 183 archives .prx au palier 2. Un stub ferait échouer tous   **
**  les chargements, et le moteur signalerait des assets « corrompus »      **
**  alors que le problème serait ici.                                        **
**                                                                          **
**  Deux traductions à faire sur chaque chemin :                            **
**    - séparateurs : le jeu écrit « data\levels\foo.prx » (Windows/Xbox)   **
**    - racine : « data/... » -> « ux0:data/thug/Data/... »                 **
*****************************************************************************/

#include <core/defines.h>
#include <sys/file/filesys.h>
#include <sys/file/PRE.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

// Le verdict sur la racine des assets doit aller dans le log FICHIER : le
// canal sceClibPrintf part vers Cat-A-Log, qui est muet sur cette console.
#include "vita_log.h"

namespace File
{

// Racine des assets sur la console.
//
// ATTENTION au niveau : le moteur demande « scripts\qdir.txt », pas
// « data\scripts\qdir.txt ». Les fichiers de l'ISO ont été poussés dans
// ux0:data/thug/Data/, donc la racine à préfixer inclut « /Data ».
// Une première version pointait sur ux0:data/thug et produisait des chemins
// amputés d'un niveau — aucun fichier trouvé, sans le moindre message.
static const char *VITA_DATA_ROOT = "ux0:data/thug/Data";

// Fichier témoin pour vérifier que la racine est la bonne. Vérifier seulement
// l'existence du DOSSIER ne prouve rien : ux0:data/thug existait bel et bien
// alors que les chemins construits étaient faux, d'où un « racine OK »
// trompeur pendant tout un cycle de debug.
static const char *VITA_DATA_PROBE = "ux0:data/thug/Data/scripts/qdir.txt";

// Le moteur passe des chemins relatifs de la forme « data\levels\foo.prx » ou
// « Data/levels/foo.prx ». On normalise vers un chemin absolu de la console.
//
// Pas de malloc ici : cette fonction est appelée à chaque ouverture de
// fichier, et le brief rappelle que l'I/O est LE goulot de la Vita. Un buffer
// statique suffit — la couche fichiers n'est pas réentrante de toute façon.
static const char *translate_path( const char *filename )
{
	static char s_path[512];

	if( !filename )
		return NULL;

	// Chemin déjà absolu pour la console (ux0:, ur0:, app0:...) : ne pas y
	// toucher.
	if( strstr( filename, "0:" ) != NULL )
		return filename;

	const char *rel = filename;

	// Sauter un « ./ » ou « .\ » de tête.
	if( rel[0] == '.' && ( rel[1] == '/' || rel[1] == '\\' ))
		rel += 2;

	snprintf( s_path, sizeof(s_path), "%s/%s", VITA_DATA_ROOT, rel );

	// Backslashes -> slashes. À faire APRÈS la concaténation pour attraper
	// aussi ceux qui viendraient de la racine.
	for( char *p = s_path; *p; ++p )
	{
		if( *p == '\\' )
			*p = '/';
	}

	// « .pre » -> « .prx ».
	//
	// Le moteur demande ses archives en « .pre » (nom générique), mais sur
	// Xbox elles sont livrées en « .prx » — et nos assets viennent d'une ISO
	// Xbox. Le backend Xbox fait exactement la même substitution
	// (Sys/File/XBox/p_filesys.cpp:162).
	//
	// Note pour une éventuelle version PAL : ce backend choisit .prf/.prg/.prx
	// selon la langue. Notre dump est USA, donc .prx sans condition.
	size_t len = strlen( s_path );
	if(( len >= 4 ) && ( strcmp( s_path + len - 4, ".pre" ) == 0 ))
		s_path[len - 1] = 'x';

	return s_path;
}


void InstallFileSystem( void )
{
	// Rien à monter : ux0 est déjà disponible. On vérifie en revanche que la
	// racine des assets existe, parce que « aucun fichier trouvé » est un
	// symptôme qu'on veut voir tout de suite et pas au bout de 200 échecs de
	// chargement silencieux.
	SceIoStat st;
	if( sceIoGetstat( VITA_DATA_PROBE, &st ) < 0 )
	{
		VLOG( "SYS", "ATTENTION: temoin %s introuvable", VITA_DATA_PROBE );
		VLOG( "SYS", "  -> la racine '%s' est probablement fausse", VITA_DATA_ROOT );
	}
	else
	{
		VLOG( "SYS", "racine des assets OK: %s (temoin qdir.txt trouve)",
			  VITA_DATA_ROOT );
	}
}


void UninstallFileSystem( void )
{
}


bool Exist( const char *filename )
{
	if( PreMgr::sPreEnabled())
	{
		bool in_pre = PreMgr::pre_fexist( filename );
		if( PreMgr::sPreExecuteSuccess() && in_pre )
			return true;
	}

	const char *p = translate_path( filename );
	if( !p )
		return false;

	SceIoStat st;
	return ( sceIoGetstat( p, &st ) >= 0 );
}


// ---------------------------------------------------------------------------
// Handles sceIo plutot que FILE*.
//
// [VERIFIE] La version stdio mettait ~3 min 30 pour lire les 2,8 Mo de
// unloadableanims.prx, soit de l'ordre de 13 Ko/s. Ce n'etait pas un blocage :
// l'horodatage du log vient du temps PROCESSUS, qui n'avance pas pendant une
// attente d'E/S -- d'ou un log qui parait fige alors que la lecture progresse.
//
// newlib bufferise par 1 Ko et chaque _read traverse une couche POSIX ; a
// l'echelle des 292 Mo d'assets du jeu, c'est intenable. sceIoRead va droit au
// pilote et accepte des blocs de plusieurs Mo.
//
// C'est exactement l'avertissement du brief : sur Vita, suspecter l'I/O avant
// le CPU ou le GPU.
// ---------------------------------------------------------------------------
#define VITA_FILE_MAGIC 0x56544631u	// 'VTF1'

struct SVitaFile
{
	uint32	magic;
	SceUID	fd;
	long	size;
	// #68 : cout des fichiers nus (hors PRE), trace a la fermeture.
	SceUInt64	us_open, us_read;
	unsigned	n_read;
	char		nom[64];
	// #68 : fichier lu EN UNE FOIS a l'ouverture (lecture seule, <= 2 Mo).
	// Chaque sceIoRead coute ~0,3 ms quelle que soit sa taille : un .skin.xbx
	// de 12 Ko lu en 68 morceaux par le chargeur prenait 21 ms, un pieton
	// complet (tete, torse, jambes : .tex + .skin) 100 a 150 ms. En memoire,
	// le descripteur est ferme aussitot ; buf == NULL : chemin sceIo d'avant.
	char		*buf;
	long		pos;
};
#define VITA_FILE_TAMPON_MAX	( 2 * 1024 * 1024 )

static int mode_flags( const char *access )
{
	if( !access )
		return SCE_O_RDONLY;

	bool plus = ( strchr( access, '+' ) != NULL );

	if( strchr( access, 'w' ))
		return plus ? ( SCE_O_RDWR | SCE_O_CREAT | SCE_O_TRUNC )
		            : ( SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC );
	if( strchr( access, 'a' ))
		return plus ? ( SCE_O_RDWR | SCE_O_CREAT | SCE_O_APPEND )
		            : ( SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND );

	return plus ? SCE_O_RDWR : SCE_O_RDONLY;
}


void *Open( const char *filename, const char *access )
{
	const char *p = translate_path( filename );
	if( !p )
		return NULL;

	// stdio plutôt que sceIo : le contrat de cette couche rend un void* qui
	// est trimballé partout, et FILE* s'y prête directement. Si l'I/O devient
	// le goulot mesuré (et non supposé), c'est ici qu'on passera à sceIo.
	// Repli sur les archives PRE — c'est ce qui manquait, et ca bloquait tout
	// le chargement des squelettes/animations : ces donnees n'existent PAS en
	// fichiers nus dans l'ISO, elles vivent dans anims.prx / skeletons.prx.
	//
	// Le backend Xbox procede pareil (Sys/File/XBox/p_filesys.cpp:438) :
	// on tente l'archive, et sPreExecuteSuccess() dit si l'operation a abouti.
	// Sinon seulement on ouvre un vrai fichier.
	if( PreMgr::sPreEnabled())
	{
		void *pre = (void *)PreMgr::pre_fopen( filename, access );
		if( PreMgr::sPreExecuteSuccess())
		{
			static int s_pre_traced = 0;
			if( s_pre_traced < 12 )
			{
				++s_pre_traced;
				VLOG( "FILE", "PRE '%s' -> %s", filename, pre ? "ok" : "absent" );
			}
			return pre;
		}
	}

	const SceUInt64 t0 = sceKernelGetProcessTimeWide();
	SceUID fd = sceIoOpen( p, mode_flags( access ), 0777 );

	// Trace des N premières ouvertures : au palier 2 on cherche à savoir si le
	// moteur tente seulement de lire ses archives. Bornée, sinon le log
	// exploserait dès que le chargement démarre pour de bon.
	static int s_traced = 0;
	if( s_traced < 24 )
	{
		++s_traced;
		VLOG( "FILE", "%s '%s' -> %s", access, p, ( fd >= 0 ) ? "ok" : "ECHEC" );
	}

	if( fd < 0 )
		return NULL;

	SVitaFile *h = (SVitaFile *)malloc( sizeof( SVitaFile ));
	if( !h )
	{
		sceIoClose( fd );
		return NULL;
	}
	h->magic = VITA_FILE_MAGIC;
	h->fd    = fd;
	// Taille retenue a l'ouverture : GetFileSize est appele en plein milieu
	// d'une lecture par le moteur, et un aller-retour de position casserait
	// la lecture en cours.
	h->size  = (long)sceIoLseek( fd, 0, SCE_SEEK_END );
	sceIoLseek( fd, 0, SCE_SEEK_SET );
	h->buf = NULL;
	h->pos = 0;
	if(( mode_flags( access ) == SCE_O_RDONLY ) && ( h->size > 0 ) && ( h->size <= VITA_FILE_TAMPON_MAX ))
	{
		char *b = (char *)malloc( (size_t)h->size );
		long done = 0;
		while( b && ( done < h->size ))
		{
			int got = sceIoRead( fd, b + done, (SceSize)( h->size - done ));
			if( got <= 0 )
				break;
			done += got;
		}
		if( b && ( done == h->size ))
		{
			h->buf = b;
			sceIoClose( fd );
			h->fd = -1;
		}
		else
		{
			// Lecture incomplete : on revient au chemin sceIo, depuis le debut.
			free( b );
			sceIoLseek( fd, 0, SCE_SEEK_SET );
		}
	}
	h->us_open = sceKernelGetProcessTimeWide() - t0;
	h->us_read = 0;
	h->n_read  = 0;
	{
		const char *b = strrchr( p, '/' );
		strncpy( h->nom, b ? b + 1 : p, sizeof( h->nom ) - 1 );
		h->nom[sizeof( h->nom ) - 1] = 0;
	}

	return (void *)h;
}


int Close( void *pFP )
{
	if( !pFP )
		return -1;

	if( PreMgr::sPreEnabled())
	{
		int r = PreMgr::pre_fclose( (PreFile::FileHandle *)pFP );
		if( PreMgr::sPreExecuteSuccess())
			return r;
	}
	SVitaFile *h = (SVitaFile *)pFP;
	if( h->magic != VITA_FILE_MAGIC )
		return -1;

	const SceUInt64 t0 = sceKernelGetProcessTimeWide();
	int r = 0;
	if( h->buf )
		free( h->buf );
	else
		r = sceIoClose( h->fd );
	const SceUInt64 us_close = sceKernelGetProcessTimeWide() - t0;
	if( h->us_open + h->us_read + us_close > 5000 )
		VLOG( "LENT", "fichier '%s' (%ld o) : ouverture %.1f ms, %u lectures %.1f ms, fermeture %.1f ms",
		      h->nom, h->size, (float)h->us_open / 1000.0f, h->n_read,
		      (float)h->us_read / 1000.0f, (float)us_close / 1000.0f );
	h->magic = 0;
	free( h );
	return ( r < 0 ) ? -1 : 0;
}


size_t Read( void *addr, size_t size, size_t count, void *pFP )
{
	if( !pFP || !addr )
		return 0;

	if( PreMgr::sPreEnabled())
	{
		size_t r = PreMgr::pre_fread( addr, size, count, (PreFile::FileHandle *)pFP );
		if( PreMgr::sPreExecuteSuccess())
			return r;
	}
	SVitaFile *h = (SVitaFile *)pFP;
	if( h->magic != VITA_FILE_MAGIC )
		return 0;

	size_t want = size * count;
	size_t done = 0;
	const SceUInt64 t0 = sceKernelGetProcessTimeWide();
	++h->n_read;
	if( h->buf )
	{
		const long reste = h->size - h->pos;
		const size_t n = ( reste <= 0 ) ? 0 : (( want < (size_t)reste ) ? want : (size_t)reste );
		memcpy( addr, h->buf + h->pos, n );
		h->pos += (long)n;
		h->us_read += sceKernelGetProcessTimeWide() - t0;
		return n;
	}
	// sceIoRead a le droit de rendre moins que demande : on boucle.
	while( done < want )
	{
		int got = sceIoRead( h->fd, (char *)addr + done, (SceSize)( want - done ));
		if( got <= 0 )
			break;
		done += (size_t)got;
	}
	// ATTENTION : malgre sa signature a la fread, cette fonction rend un
	// nombre d'OCTETS, pas d'elements. C'est le contrat du moteur -- le
	// backend Xbox (Sys/File/XBox/p_filesys.cpp:509) rend bytes_read, et
	// pip.cpp compare le retour a une taille de fichier. Diviser par `size`
	// (premiere version) cassait silencieusement tous les appelants.
	h->us_read += sceKernelGetProcessTimeWide() - t0;
	return done;
}


size_t Write( const void *addr, size_t size, size_t count, void *pFP )
{
	if( !pFP || !addr )
		return 0;

	if( PreMgr::sPreEnabled())
	{
		size_t r = PreMgr::pre_fwrite( addr, size, count, (PreFile::FileHandle *)pFP );
		if( PreMgr::sPreExecuteSuccess())
			return r;
	}
	SVitaFile *h = (SVitaFile *)pFP;
	if( h->magic != VITA_FILE_MAGIC )
		return 0;

	if( h->buf )
		return 0;		// lecture seule
	int put = sceIoWrite( h->fd, addr, (SceSize)( size * count ));
	if( put <= 0 )
		return 0;
	return (size_t)put;		// des octets, comme Read
}


long GetFileSize( void *pFP )
{
	if( !pFP )
		return 0;

	// Repli PRE — indispensable : sans lui, un handle d'archive était traité
	// comme un FILE*, donc ftell/fseek dessus, avec un résultat indéfini.
	// C'est ce qui bloquait le parsing du premier squelette alors même que le
	// fichier s'ouvrait correctement.
	if( PreMgr::sPreEnabled())
	{
		int r = PreMgr::pre_get_file_size( (PreFile::FileHandle *)pFP );
		if( PreMgr::sPreExecuteSuccess())
			return (long)r;
	}

	// Taille relevee a l'ouverture : le moteur appelle parfois cette fonction
	// au milieu d'une lecture, et il ne s'attend pas a ce que la position
	// courante bouge.
	SVitaFile *h = (SVitaFile *)pFP;
	if( h->magic != VITA_FILE_MAGIC )
		return 0;
	return h->size;
}


// LoadAlloc est déjà fourni par Sys/File/pip.cpp — le définir ici aussi donne
// un « multiple definition » au link. On garde la version d'origine, qui sait
// gérer les archives PRE ; la nôtre ne lisait que des fichiers nus.
#if 0
void *LoadAlloc( const char *p_fileName, int *p_filesize, void *p_dest, int maxSize )
{
	void *fp = Open( p_fileName, "rb" );
	if( !fp )
		return NULL;

	int size = GetFileSize( fp );
	if(( maxSize > 0 ) && ( size > maxSize ))
	{
		Close( fp );
		return NULL;
	}

	void *dest = p_dest ? p_dest : malloc( size );
	if( !dest )
	{
		Close( fp );
		return NULL;
	}

	size_t got = Read( dest, 1, (size_t)size, fp );
	Close( fp );

	if( (int)got != size )
	{
		if( !p_dest )
			free( dest );
		return NULL;
	}

	if( p_filesize )
		*p_filesize = size;

	return dest;
}
#endif // LoadAlloc — fourni par Sys/File/pip.cpp


uint32 CanFileBeLoadedQuickly( const char *filename )
{
	// « Quickly » désigne, sur les plateformes d'origine, un fichier déjà
	// présent en mémoire ou sur un support rapide. Rien de tel ici : on
	// répond non, le moteur passera par le chemin de chargement normal.
	return 0;
}


bool LoadFileQuicklyPlease( const char *filename, uint8 *addr )
{
	return ( LoadAlloc( filename, NULL, addr, 0 ) != NULL );
}


int Seek( void *pFP, long offset, int origin )
{
	if( !pFP )
		return -1;

	// Les archives PRE gerent leur propre position.
	if( PreMgr::sPreEnabled())
	{
		int r = PreMgr::pre_fseek( (PreFile::FileHandle *)pFP, offset, origin );
		if( PreMgr::sPreExecuteSuccess())
			return r;
	}

	SVitaFile *h = (SVitaFile *)pFP;
	if( h->magic != VITA_FILE_MAGIC )
		return -1;

	int whence = SCE_SEEK_SET;
	if( origin == SEEK_CUR )
		whence = SCE_SEEK_CUR;
	else if( origin == SEEK_END )
		whence = SCE_SEEK_END;

	if( h->buf )
	{
		long base = ( whence == SCE_SEEK_SET ) ? 0 : ( whence == SCE_SEEK_CUR ) ? h->pos : h->size;
		long np = base + offset;
		if( np < 0 )
			return -1;
		h->pos = ( np > h->size ) ? h->size : np;
		return 0;
	}
	return ( sceIoLseek( h->fd, offset, whence ) < 0 ) ? -1 : 0;
}


int Tell( void *pFP )
{
	if( !pFP )
		return -1;

	if( PreMgr::sPreEnabled())
	{
		int r = PreMgr::pre_get_file_position( (PreFile::FileHandle *)pFP );
		if( PreMgr::sPreExecuteSuccess())
			return r;
	}

	SVitaFile *h = (SVitaFile *)pFP;
	if( h->magic != VITA_FILE_MAGIC )
		return -1;

	if( h->buf )
		return (int)h->pos;
	return (int)sceIoLseek( h->fd, 0, SCE_SEEK_CUR );
}


long GetFilePosition( void *pFP )
{
	return (long)Tell( pFP );
}


void StopStreaming( void )
{
}

} // namespace File
