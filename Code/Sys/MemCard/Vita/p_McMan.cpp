/*****************************************************************************
**  THUG-Vita â€” sauvegarde                                                  **
**  Code/Sys/MemCard/Vita/p_McMan.cpp                                       **
**                                                                          **
**  Remplace le backend factice de Sys/Vita/p_mcman.cpp, qui declarait une  **
**  carte presente et vide pour que le jeu accepte de demarrer une          **
**  carriere, mais n'ecrivait jamais rien : le story mode ne conservait     **
**  aucune progression.                                                     **
**                                                                          **
**  CE QUI EXISTAIT DEJA, et qu'il ne fallait surtout pas reecrire :        **
**  Sk/Scripting/mcfuncs.cpp (4915 lignes) fait toute la serialisation des  **
**  donnees de jeu et est compile depuis le debut. Il ne lui manquait que   **
**  l'acces disque. La liste exacte du travail a ete obtenue mecaniquement, **
**  par les symboles non resolus de mcfuncs.cpp.obj â€” seize, dont quatre    **
**  etaient deja fournis.                                                   **
**                                                                          **
**  La carte memoire, ici, est un dossier : ux0:data/thug/save/. Le moteur  **
**  raisonne en repertoires de sauvegarde (un par partie), ce qui           **
**  correspond directement a des sous-dossiers.                             **
**                                                                          **
**  Les noms entrants sont de la forme Â« /foo/bar Â» (constate dans le       **
**  backend Xbox, p_McMan.cpp:730 : Â« Seems incoming filenames are of the   **
**  form /foo/bar etc. Â»). On les recolle a la racine en normalisant les    **
**  separateurs.                                                            **
*****************************************************************************/

#include <core/defines.h>
#include <sys/McMan.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>

#include <stdio.h>
#include <string.h>

#include "vita_log.h"

namespace Mc
{

// Racine des sauvegardes. Volontairement distincte du dossier des assets :
// une reinstallation du jeu ne doit pas emporter les parties.
static const char *RACINE = "ux0:data/thug/save";

// Tampon de construction de chemin. Une seule operation disque a la fois sur
// ce chemin : le moteur sauvegarde depuis le thread principal.
static char	s_chemin[512];


// Recolle un nom du moteur a la racine, en normalisant les separateurs.
// Accepte Â« /foo/bar Â», Â« foo\bar Â» et Â« foo/bar Â» indiffÃ©remment.
static const char *chemin_de( const char *p_nom )
{
	int i = 0;
	const int racine_len = (int)strlen( RACINE );
	memcpy( s_chemin, RACINE, racine_len );
	i = racine_len;

	if( p_nom && *p_nom )
	{
		if(( *p_nom != '/' ) && ( *p_nom != '\\' ))
			s_chemin[i++] = '/';

		while( *p_nom && ( i < (int)sizeof( s_chemin ) - 1 ))
		{
			s_chemin[i++] = ( *p_nom == '\\' ) ? '/' : *p_nom;
			++p_nom;
		}
	}
	s_chemin[i] = '\0';
	return s_chemin;
}


// Cree la racine si elle n'existe pas. Appelee avant toute ecriture : le
// dossier n'existe pas a la premiere partie, et sceIoOpen ne le cree pas.
static void assure_racine( void )
{
	sceIoMkdir( "ux0:data/thug", 0777 );
	sceIoMkdir( RACINE, 0777 );
}


// APLATIT un nom de sauvegarde en un nom de dossier unique.
//
// Le moteur fabrique des noms qui CONTIENNENT un separateur, par exemple
// « Custom Skater-Story/Skater » (mcfuncs.cpp, s_generate_xbox_directory_name
// : sprintf « %s-Story/Skater »). Sur Xbox ce n'est pas un chemin : le
// systeme cree une sauvegarde et ConvertDirectory rend le vrai nom de
// dossier, plat.
//
// [REFUTE] « on peut prendre ce nom tel quel comme chemin ». Cela creait
// save/Custom Skater-Story/Skater/... -- deux niveaux -- et GetFileList, qui
// ne lit que le premier, ne voyait jamais la sauvegarde. Le jeu proposait
// donc de charger, sans jamais appeler LoadFromMemoryCard.
static const char *aplatit( const char *p_nom )
{
	static char plat[256];
	int i = 0;
	if( p_nom )
	{
		while( *p_nom && ( i < (int)sizeof( plat ) - 1 ))
		{
			plat[i++] = (( *p_nom == '/' ) || ( *p_nom == '\\' )) ? '_' : *p_nom;
			++p_nom;
		}
	}
	plat[i] = '\0';
	return plat;
}


// Cree TOUS les niveaux d'un chemin, pas seulement le dernier.
//
// [VERIFIE] indispensable : le moteur demande des noms a plusieurs niveaux,
// par exemple « Custom Skater-Story/Skater » (le nom vient de
// s_generate_xbox_directory_name). sceIoMkdir ne cree pas les parents et
// echouait avec 0x80010002 (ENOENT), ce qui faisait echouer toute la
// sauvegarde -- constate dans le journal avant correction.
static void cree_arborescence( const char *p_chemin )
{
	char tampon[512];
	strncpy( tampon, p_chemin, sizeof( tampon ) - 1 );
	tampon[sizeof( tampon ) - 1] = '\0';

	// On demarre apres « ux0: » pour ne pas tenter de creer le peripherique.
	char *p = strchr( tampon, ':' );
	p = p ? ( p + 1 ) : tampon;

	for( ; *p; ++p )
	{
		if( *p != '/' )
			continue;
		*p = '\0';
		if( tampon[0] )
			sceIoMkdir( tampon, 0777 );
		*p = '/';
	}
	sceIoMkdir( tampon, 0777 );
}


// --- Card -------------------------------------------------------------------

Card *Manager::GetCard( int port, int slot )
{
	// Un seul emplacement : le stockage interne de la console.
	if(( port != 0 ) || ( slot != 0 ))
		return NULL;
	return GetCardEx( 0, 0 );
}


bool Card::IsFormatted( void )
{
	return true;
}


int Card::GetNumFreeClusters( void )
{
	// THUG reclame 159 080 blocs pour l'ensemble de ses types de sauvegarde
	// -- constate a l'ecran, le jeu affichant sinon Â« not enough free blocks Â».
	// La Vita a plusieurs gigaoctets libres ; annoncer large est ici honnete.
	return 10000000;
}


bool Card::Format( void )
{
	assure_racine();
	return true;
}


bool Card::MakeDirectory( const char *dir_name )
{
	assure_racine();
	const int r = sceIoMkdir( chemin_de( aplatit( dir_name )), 0777 );

	// Â« Deja present Â» n'est pas un echec : le moteur repasse par ici pour
	// rouvrir une partie existante. On ne teste donc pas le code d'erreur â€”
	// on verifie que le dossier EST la, ce qui couvre les deux cas sans
	// dependre du nom exact de la constante d'erreur.
	if( r < 0 )
	{
		SceIoStat st;
		if(( sceIoGetstat( s_chemin, &st ) < 0 ) || !SCE_S_ISDIR( st.st_mode ))
		{
			VLOG( "MC", "creation de '%s' refusee (0x%08x)", s_chemin, r );
			return false;
		}
	}
	return true;
}


bool Card::DeleteDirectory( const char *dir_name )
{
	// Le dossier doit etre vide pour sceIoRmdir : on retire son contenu
	// d'abord, sinon la suppression d'une partie echouerait en silence.
	const char *p_dir = chemin_de( aplatit( dir_name ));
	char dossier[512];
	strncpy( dossier, p_dir, sizeof( dossier ) - 1 );
	dossier[sizeof( dossier ) - 1] = '\0';

	SceUID d = sceIoDopen( dossier );
	if( d >= 0 )
	{
		SceIoDirent e;
		while( sceIoDread( d, &e ) > 0 )
		{
			if(( strcmp( e.d_name, "." ) == 0 ) || ( strcmp( e.d_name, ".." ) == 0 ))
				continue;
			char f[600];
			snprintf( f, sizeof( f ), "%s/%s", dossier, e.d_name );
			sceIoRemove( f );
		}
		sceIoDclose( d );
	}
	return ( sceIoRmdir( dossier ) >= 0 );
}


// Sur Xbox, cette fonction demandait au systeme le vrai nom de dossier d'une
// sauvegarde (XCreateSaveGame). Ici les noms sont deja des noms de dossiers :
// on se contente de garantir que le dossier existe et de rendre le nom tel
// quel. Rendre NULL ferait echouer l'appelant.
const char *Card::ConvertDirectory( const char *dir_name )
{
	static char sortie[256];
	assure_racine();

	// Rend le nom APLATI : c'est celui que le moteur reutilisera pour
	// construire le chemin du fichier (« /<dossier>/<dossier> »), et celui
	// que GetFileList lui rendra.
	strncpy( sortie, aplatit( dir_name ), sizeof( sortie ) - 1 );
	sortie[sizeof( sortie ) - 1] = '\0';
	sceIoMkdir( chemin_de( sortie ), 0777 );
	return sortie;
}


bool Card::Delete( const char *filename )
{
	return ( sceIoRemove( chemin_de( filename )) >= 0 );
}


File *Card::Open( const char *filename, int mode, int size )
{
	m_last_error = 0;

	int flags = 0;
	if( mode & File::mMODE_WRITE )
	{
		flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;
		assure_racine();
	}
	else if( mode & File::mMODE_CREATE )
	{
		flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;
		assure_racine();
	}
	else
	{
		flags = SCE_O_RDONLY;
	}

	const char *p_chemin = chemin_de( filename );

	// En ecriture, le dossier parent peut ne pas exister encore : le moteur
	// ecrit Â« <partie>/<fichier> Â» sans avoir toujours cree Â« <partie> Â».
	if( flags & SCE_O_CREAT )
	{
		char parent[512];
		strncpy( parent, p_chemin, sizeof( parent ) - 1 );
		parent[sizeof( parent ) - 1] = '\0';
		char *p_slash = strrchr( parent, '/' );
		if( p_slash )
		{
			*p_slash = '\0';
			// Arborescence complete : le chemin du fichier compte plusieurs
			// niveaux (« <partie>/<sous-dossier>/<fichier> »).
			cree_arborescence( parent );
		}
	}

	const SceUID fd = sceIoOpen( p_chemin, flags, 0777 );
	if( fd < 0 )
	{
		// En lecture, Â« absent Â» est un cas NORMAL : le moteur teste
		// l'existence d'une sauvegarde en essayant de l'ouvrir.
		if( flags == SCE_O_RDONLY )
			return NULL;

		VLOG( "MC", "ouverture de '%s' refusee (0x%08x)", p_chemin, fd );
		m_last_error = 1;
		return NULL;
	}

	File *p_file = new File((int)fd, this );

	strncpy( p_file->m_Filename, filename ? filename : "",
	         File::vMAX_FILENAME_LEN );
	p_file->m_Filename[File::vMAX_FILENAME_LEN] = '\0';

	SceIoStat st;
	memset( &st, 0, sizeof( st ));
	p_file->m_Size = ( sceIoGetstat( p_chemin, &st ) >= 0 )
	               ? (unsigned int)st.st_size : (unsigned int)size;
	p_file->m_Attribs = 0;
	return p_file;
}


bool Card::GetFileList( const char *mask, Lst::Head< File > &file_list )
{
	assure_racine();

	SceUID d = sceIoDopen( RACINE );
	if( d < 0 )
		return true;		/* pas de sauvegarde encore : liste vide, pas une erreur */

	SceIoDirent e;
	while( sceIoDread( d, &e ) > 0 )
	{
		if(( strcmp( e.d_name, "." ) == 0 ) || ( strcmp( e.d_name, ".." ) == 0 ))
			continue;

		File *p_new = new File( 0, this );
		strncpy( p_new->m_Filename, e.d_name, File::vMAX_FILENAME_LEN );
		p_new->m_Filename[File::vMAX_FILENAME_LEN] = '\0';

#		if defined(__PLAT_XBOX__) || defined(__PLAT_WN32__)
		strncpy( p_new->m_DisplayFilename, e.d_name,
		         File::vMAX_DISPLAY_FILENAME_LEN );
		p_new->m_DisplayFilename[File::vMAX_DISPLAY_FILENAME_LEN] = '\0';
#		endif

		p_new->m_Size = (unsigned int)e.d_stat.st_size;

		// La date affichee dans le menu de chargement vient d'ici.
		p_new->m_Modified.m_Year    = e.d_stat.st_mtime.year;
		p_new->m_Modified.m_Month   = (unsigned char)e.d_stat.st_mtime.month;
		p_new->m_Modified.m_Day     = (unsigned char)e.d_stat.st_mtime.day;
		p_new->m_Modified.m_Hour    = (unsigned char)e.d_stat.st_mtime.hour;
		p_new->m_Modified.m_Minutes = (unsigned char)e.d_stat.st_mtime.minute;
		p_new->m_Modified.m_Seconds = (unsigned char)e.d_stat.st_mtime.second;
		p_new->m_Created            = p_new->m_Modified;

		p_new->m_Attribs = ( SCE_S_ISDIR( e.d_stat.st_mode ))
		                 ? File::mATTRIB_DIRECTORY : 0;

		file_list.AddToTail( p_new );
	}
	sceIoDclose( d );
	return true;
}


// --- File -------------------------------------------------------------------

File::File( int fd, Card *card ) : Lst::Node< File >( this ), m_fd( fd ), m_card( card )
{
	m_Filename[0] = '\0';
	m_Size        = 0;
	m_Attribs     = 0;
	memset( &m_Created,  0, sizeof( m_Created ));
	memset( &m_Modified, 0, sizeof( m_Modified ));
#	if defined(__PLAT_XBOX__) || defined(__PLAT_WN32__)
	m_DisplayFilename[0] = '\0';
#	endif
}


File::~File()
{
}


bool File::Close( void )
{
	if( m_fd > 0 )
	{
		sceIoClose((SceUID)m_fd );
		m_fd = 0;
	}
	return true;
}


int File::Write( void *data, int len )
{
	if( m_fd <= 0 )
		return 0;
	const int r = sceIoWrite((SceUID)m_fd, data, len );
	return ( r < 0 ) ? 0 : r;
}


int File::Read( void *data, int len )
{
	if( m_fd <= 0 )
		return 0;
	const int r = sceIoRead((SceUID)m_fd, data, len );
	return ( r < 0 ) ? 0 : r;
}


int File::Seek( int offset, FilePointerBase base )
{
	if( m_fd <= 0 )
		return 0;

	int origine = SCE_SEEK_SET;
	if( base == BASE_CURRENT )	origine = SCE_SEEK_CUR;
	else if( base == BASE_END )	origine = SCE_SEEK_END;

	const SceOff r = sceIoLseek((SceUID)m_fd, offset, origine );
	return ( r < 0 ) ? 0 : (int)r;
}


bool File::Flush( void )
{
	if( m_fd > 0 )
		sceIoSyncByFd((SceUID)m_fd, 0 );
	return true;
}

} // namespace Mc
