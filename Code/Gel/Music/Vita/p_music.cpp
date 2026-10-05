/*****************************************************************************
**  THUG-Vita — backend musique / PCM                                       **
**  Code/Gel/Music/Vita/p_music.cpp                                         **
**                                                                          **
**  LA MUSIQUE JOUE. Ce fichier n'est plus une rangée de stubs.             **
**                                                                          **
**  Tout le format a été établi mécaniquement AVANT d'écrire une ligne de   **
**  C++, avec vita/tools/audio_check.py (même méthode que cas_check.py) :   **
**                                                                          **
**  [VÉRIFIÉ] data/streams/pcm/music_pcm.dat est un index :                  **
**            uint32 nombre, puis nombre x { checksum, offset, taille }.     **
**            Preuve : 4 + 167*12 == 2008 == taille exacte du fichier,       **
**            aucun chevauchement, tout tient dans le .wad.                  **
**                                                                          **
**  [VÉRIFIÉ] Les entrées sont triées par checksum croissant, pour une       **
**            recherche binaire. Confirmé par le backend d'origine :         **
**            Gel/Music/Xbox/p_music.cpp:575.                                **
**                                                                          **
**  [VÉRIFIÉ] Le .wad concatène des RIFF/WAVE dont le formatTag vaut         **
**            0x0069 = XBOX ADPCM — et NON du PCM, malgré le nom du          **
**            dossier. 48000 Hz, stéréo, blockAlign 72, 4 bits.              **
**            Contrôle : 48000/64 échantillons par bloc * 72 octets = 54000, **
**            exactement l'avgBytesPerSec de l'en-tête.                      **
**                                                                          **
**  [VÉRIFIÉ] data/streams/wma/ est VIDE sur cette ISO (0 octet). Aucun      **
**            décodeur WMA n'est donc nécessaire — c'est ce qui rend ce      **
**            chantier faisable.                                             **
**                                                                          **
**  Xbox ADPCM est une variante d'IMA ADPCM. Par bloc et par canal :         **
**  4 octets d'en-tête (prédicteur int16, index de pas uint8, un octet       **
**  réservé) puis les données par groupes de 4 octets alternant les canaux.  **
**                                                                          **
**  PORTÉE DE CE LOT : la musique (music_pcm.wad). Les flux de voix          **
**  (pcm.wad) partagent exactement le même format et réutiliseront ce        **
**  décodeur ; ils restent neutralisés ici, avec les mêmes valeurs de repli  **
**  qu'avant — « c'est parti » / « rien en cours » — pour que le moteur      **
**  n'attende jamais un flux qui ne démarrera pas.                           **
*****************************************************************************/

#include <core/defines.h>
#include <gel/music/Vita/p_music.h>
#include <core/crc.h>
#include <core/Vita/adpcm.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vita_log.h"

SVitaStreamInfo	gCurrentStreamInfo[ NUM_STREAMS ];

namespace
{

// --- format, tel que mesuré ------------------------------------------------

#define ADPCM_BLOC			72		/* blockAlign, stéréo */
#define ADPCM_ECH_PAR_BLOC	64		/* (72/2 - 4) * 2 */
#define AUDIO_HZ			48000
#define AUDIO_CANAUX		2

// Granularité de sortie. sceAudioOutOutput bloque jusqu'à ce que le tampon
// précédent soit consommé : c'est LUI qui cadence le thread, on n'a aucune
// horloge à tenir nous-mêmes.
#define GRAIN				1024	/* échantillons par appel, multiple de 64 */

// Lecture disque par gros blocs. Une lecture de 576 octets par appel de sortie
// ferait ~90 accès disque par seconde ; sur Vita le stockage est le vrai
// goulot, et c'est exactement le genre d'I/O qui coûte des images.
// 910 blocs = 65520 octets = ~1,2 s d'audio par lecture.
#define BLOCS_PAR_LECTURE	910
#define TAILLE_LECTURE		( BLOCS_PAR_LECTURE * ADPCM_BLOC )

static const char *CHEMIN_MUSIC_DAT = "ux0:data/thug/Data/streams/pcm/music_pcm.dat";
static const char *CHEMIN_MUSIC_WAD = "ux0:data/thug/Data/streams/pcm/music_pcm.wad";

// --- index ------------------------------------------------------------------

struct SEntree
{
	uint32	checksum;
	uint32	offset;
	uint32	taille;
};

static SEntree	*s_index			= NULL;
static int		 s_index_nb			= 0;

// --- état de lecture --------------------------------------------------------

static SceUID	 s_wad				= -1;
static int		 s_port				= -1;
static SceUID	 s_thread			= -1;
static bool		 s_thread_tourne	= false;

// Protège les champs partagés avec le thread. Le moteur appelle Play/Stop
// depuis le thread principal pendant que le thread audio lit : sans verrou on
// libère un tampon en cours de décodage.
static SceUID	 s_mutex			= -1;

static volatile bool	s_joue			= false;
static volatile bool	s_pause			= false;
static volatile bool	s_fini			= true;
static volatile uint32	s_prechargee	= 0;
static volatile bool	s_a_precharger	= false;

// Position dans le morceau courant.
static uint32	s_pos_debut			= 0;	/* offset des données dans le .wad */
static uint32	s_pos_taille		= 0;	/* octets de données ADPCM */
static uint32	s_pos_lue			= 0;	/* octets déjà consommés */

static float	s_volume			= 1.0f;

// Tampons. Alloués une fois : allouer dans le thread audio à chaque tour
// fragmenterait le tas pendant le jeu.
static unsigned char	*s_lecture	= NULL;
static short			*s_sortie	= NULL;

// Le decodeur vit dans Core/Vita/adpcm.cpp : les effets sonores utilisent
// exactement le meme format (mono, blockAlign 36, au lieu de stereo 72), et
// une seule implementation validee vaut mieux que deux copies.

// --- index ------------------------------------------------------------------

static int cmp_entree( const void *a, const void *b )
{
	const uint32 ca = ((const SEntree *)a)->checksum;
	const uint32 cb = ((const SEntree *)b)->checksum;
	return ( ca < cb ) ? -1 : (( ca > cb ) ? 1 : 0 );
}

static bool charge_index( void )
{
	SceUID f = sceIoOpen( CHEMIN_MUSIC_DAT, SCE_O_RDONLY, 0 );
	if( f < 0 )
	{
		VLOG( "PCM", "index musique introuvable : %s", CHEMIN_MUSIC_DAT );
		return false;
	}

	uint32 nb = 0;
	if( sceIoRead( f, &nb, 4 ) != 4 )
	{
		sceIoClose( f );
		return false;
	}

	// Garde-fou : un nombre absurde signifie un fichier tronqué ou d'un autre
	// format. Mieux vaut ne pas jouer de musique que d'allouer 4 Go.
	if(( nb == 0 ) || ( nb > 100000 ))
	{
		VLOG( "PCM", "index musique : nombre d'entrees invraisemblable (%u)", nb );
		sceIoClose( f );
		return false;
	}

	s_index = (SEntree *)malloc( nb * sizeof( SEntree ));
	if( !s_index )
	{
		sceIoClose( f );
		return false;
	}

	const int voulu = (int)( nb * sizeof( SEntree ));
	const int lu    = sceIoRead( f, s_index, voulu );
	sceIoClose( f );

	if( lu != voulu )
	{
		VLOG( "PCM", "index musique tronque : %d octets sur %d", lu, voulu );
		free( s_index );
		s_index = NULL;
		return false;
	}

	s_index_nb = (int)nb;

	// Le backend d'origine compte sur un index déjà trié. On le retrie quand
	// même : c'est instantané sur quelques centaines d'entrées, et une
	// recherche binaire sur des données non triées échouerait silencieusement.
	qsort( s_index, s_index_nb, sizeof( SEntree ), cmp_entree );

	VLOG( "PCM", "index musique : %d pistes", s_index_nb );
	return true;
}

static const SEntree *trouve( uint32 checksum )
{
	int lo = 0, hi = s_index_nb - 1;
	while( lo <= hi )
	{
		const int mid = ( lo + hi ) / 2;
		if( s_index[mid].checksum == checksum )	return &s_index[mid];
		if( s_index[mid].checksum <  checksum )	lo = mid + 1;
		else									hi = mid - 1;
	}
	return NULL;
}

// --- thread de lecture ------------------------------------------------------

// Lit l'en-tête RIFF du morceau et positionne la lecture sur ses données.
// Rend false si l'en-tête n'est pas celui attendu — auquel cas on ne joue
// rien plutôt que d'interpréter des octets au hasard comme du son.
static bool prepare_morceau( const SEntree *p_e )
{
	unsigned char entete[64];

	if( sceIoLseek( s_wad, p_e->offset, SCE_SEEK_SET ) < 0 )
		return false;
	if( sceIoRead( s_wad, entete, sizeof( entete )) != (int)sizeof( entete ))
		return false;

	// « fmt » à l'offset 12, « data » à 40 : disposition RIFF fixe de ces
	// fichiers, vérifiée sur l'ISO.
	if( memcmp( entete + 12, "fmt ", 4 ) != 0 )
	{
		VLOG( "PCM", "morceau %08x : pas de bloc fmt", p_e->checksum );
		return false;
	}
	if( memcmp( entete + 40, "data", 4 ) != 0 )
	{
		VLOG( "PCM", "morceau %08x : pas de bloc data", p_e->checksum );
		return false;
	}

	const uint16 tag = (uint16)( entete[20] | ( entete[21] << 8 ));
	if( tag != 0x0069 )
	{
		VLOG( "PCM", "morceau %08x : format 0x%04x non gere", p_e->checksum, tag );
		return false;
	}

	uint32 taille_data;
	memcpy( &taille_data, entete + 44, 4 );

	// L'en-tête pourrait annoncer plus que ce que l'entrée contient.
	if( taille_data > ( p_e->taille - 48 ))
		taille_data = p_e->taille - 48;

	s_pos_debut  = p_e->offset + 48;
	s_pos_taille = taille_data;
	s_pos_lue    = 0;
	return true;
}

static int thread_audio( SceSize, void * )
{
	while( s_thread_tourne )
	{
		bool joue;
		sceKernelLockMutex( s_mutex, 1, NULL );
		joue = s_joue && !s_pause;
		sceKernelUnlockMutex( s_mutex, 1 );

		if( !joue )
		{
			// Rien à faire : on rend la main. Sans cette pause, ce thread
			// mangerait un cœur entier pendant les menus silencieux.
			sceKernelDelayThread( 10000 );
			continue;
		}

		sceKernelLockMutex( s_mutex, 1, NULL );

		uint32 reste = ( s_pos_taille > s_pos_lue ) ? ( s_pos_taille - s_pos_lue ) : 0;
		if( reste == 0 )
		{
			s_joue = false;
			s_fini = true;
			sceKernelUnlockMutex( s_mutex, 1 );
			continue;
		}

		uint32 a_lire = TAILLE_LECTURE;
		if( a_lire > reste )
			a_lire = ( reste / ADPCM_BLOC ) * ADPCM_BLOC;	/* blocs entiers */
		if( a_lire == 0 )
		{
			s_joue = false;
			s_fini = true;
			sceKernelUnlockMutex( s_mutex, 1 );
			continue;
		}

		sceIoLseek( s_wad, s_pos_debut + s_pos_lue, SCE_SEEK_SET );
		const int lu = sceIoRead( s_wad, s_lecture, a_lire );
		if( lu <= 0 )
		{
			s_joue = false;
			s_fini = true;
			sceKernelUnlockMutex( s_mutex, 1 );
			continue;
		}
		s_pos_lue += lu;

		sceKernelUnlockMutex( s_mutex, 1 );

		// Décodage puis sortie, PAR GRAIN. sceAudioOutOutput bloque : le
		// verrou est relâché avant, sinon le thread principal resterait
		// bloqué sur Stop pendant toute la durée d'un tampon.
		const int blocs = lu / ADPCM_BLOC;
		int bloc = 0;
		while(( bloc < blocs ) && s_thread_tourne )
		{
			const int blocs_grain = GRAIN / ADPCM_ECH_PAR_BLOC;
			int n = blocs_grain;
			if( bloc + n > blocs )
				n = blocs - bloc;

			for( int i = 0; i < n; ++i )
			{
				VitaAdpcm::DecodeBloc(
					s_lecture + ( bloc + i ) * ADPCM_BLOC,
					s_sortie + i * ADPCM_ECH_PAR_BLOC * AUDIO_CANAUX,
					AUDIO_CANAUX, ADPCM_BLOC );
			}

			// Un grain incomplet en fin de morceau : on complète par du
			// silence plutôt que d'envoyer les restes du tampon précédent.
			const int ech_remplis = n * ADPCM_ECH_PAR_BLOC;
			if( ech_remplis < GRAIN )
			{
				memset( s_sortie + ech_remplis * AUDIO_CANAUX, 0,
				        ( GRAIN - ech_remplis ) * AUDIO_CANAUX * sizeof( short ));
			}

			sceAudioOutOutput( s_port, s_sortie );
			bloc += n;

			if( s_pause || !s_joue )
				break;
		}
	}
	return 0;
}

static void applique_volume( void )
{
	if( s_port < 0 )
		return;
	int v = (int)( s_volume * SCE_AUDIO_VOLUME_0DB );
	if( v < 0 )						v = 0;
	if( v > SCE_AUDIO_VOLUME_0DB )	v = SCE_AUDIO_VOLUME_0DB;
	int vols[2] = { v, v };
	sceAudioOutSetVolume( s_port,
	                      (SceAudioOutChannelFlag)( SCE_AUDIO_VOLUME_FLAG_L_CH |
	                                                SCE_AUDIO_VOLUME_FLAG_R_CH ),
	                      vols );
}

// Le moteur fabrique ses noms de piste a la mode PS2 :
// Sk/Scripting/cfuncs.cpp:3724 prefixe � MUSIC\\VAG\\SONGS\\ � -- VAG etant le
// format audio de la PlayStation 2. Or l'index du .wad Xbox est construit sur
// le nom NU.
//
// [VERIFIE] sur table : le CRC de � ACEYALONE � tombe dans l'index, celui de
// � MUSIC\\VAG\\SONGS\\ACEYALONE � non ; 5 noms testes sur 5. Le CRC lui-meme a
// ete valide contre quatre constantes ecrites en dur dans le moteur (loop,
// is_frontend, MusicVolume, MusicStreamVolume).
static uint32 checksum_de_piste( const char *p_nom )
{
	// On ne garde que ce qui suit le dernier separateur, quel qu'il soit.
	const char *p_court = p_nom;
	for( const char *p = p_nom; *p; ++p )
	{
		if(( *p == '\\' ) || ( *p == '/' ))
			p_court = p + 1;
	}
	return Crc::GenerateCRCFromString( p_court );
}

// Le moteur appelle StopMusic et consorts meme quand l'audio n'a pas pu
// s'initialiser (fichiers absents sur la carte). Verrouiller un mutex jamais
// cree n'est pas fatal, mais c'est une erreur silencieuse par appel : on
// enveloppe une fois pour toutes.
static inline void verrouille( void )
{
	if( s_mutex >= 0 )
		sceKernelLockMutex( s_mutex, 1, NULL );
}

static inline void deverrouille( void )
{
	if( s_mutex >= 0 )
		sceKernelUnlockMutex( s_mutex, 1 );
}

static bool demarre( uint32 checksum )
{
	if(( s_wad < 0 ) || !s_index )
		return false;

	const SEntree *p_e = trouve( checksum );
	if( !p_e )
	{
		VLOG( "PCM", "morceau %08x absent de l'index", checksum );
		return false;
	}

	verrouille();
	const bool ok = prepare_morceau( p_e );
	if( ok )
	{
		s_joue  = true;
		s_pause = false;
		s_fini  = false;
		VLOG( "PCM", "lecture %08x : %u octets", checksum, s_pos_taille );
	}
	deverrouille();
	return ok;
}

} // namespace anonyme

// ---------------------------------------------------------------------------
// API attendue par Gel/Music/music.cpp
// ---------------------------------------------------------------------------

void	PCMAudio_Init( void )
{
	if( !charge_index() )
	{
		VLOG( "PCM", "pas de musique : index indisponible" );
		return;
	}

	s_wad = sceIoOpen( CHEMIN_MUSIC_WAD, SCE_O_RDONLY, 0 );
	if( s_wad < 0 )
	{
		VLOG( "PCM", "pas de musique : %s introuvable", CHEMIN_MUSIC_WAD );
		free( s_index );
		s_index = NULL;
		s_index_nb = 0;
		return;
	}

	s_lecture = (unsigned char *)malloc( TAILLE_LECTURE );
	s_sortie  = (short *)malloc( GRAIN * AUDIO_CANAUX * sizeof( short ));
	if( !s_lecture || !s_sortie )
	{
		VLOG( "PCM", "pas de musique : allocation des tampons impossible" );
		return;
	}

	s_port = sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN,
	                              AUDIO_HZ, SCE_AUDIO_OUT_MODE_STEREO );
	if( s_port < 0 )
	{
		VLOG( "PCM", "pas de musique : port audio refuse (0x%08x)", s_port );
		return;
	}
	applique_volume();

	s_mutex = sceKernelCreateMutex( "thug_pcm", 0, 0, NULL );

	s_thread_tourne = true;
	s_thread = sceKernelCreateThread( "thug_pcm", thread_audio,
	                                  0x10000100, 0x10000, 0, 0, NULL );
	if( s_thread >= 0 )
		sceKernelStartThread( s_thread, 0, NULL );
	else
		VLOG( "PCM", "pas de musique : thread refuse" );

	VLOG( "PCM", "audio pret : %d Hz stereo, %d pistes", AUDIO_HZ, s_index_nb );
}

int		PCMAudio_Update( void )
{
	return 0;
}

// --- musique ---------------------------------------------------------------

bool	PCMAudio_PlayMusicStream( uint32 checksum )
{
	return demarre( checksum );
}

bool	PCMAudio_PreLoadMusicStream( uint32 checksum )
{
	// Rien à précharger : la lecture est en flux, elle démarre immédiatement.
	// On mémorise la demande pour que StartPreLoaded l'exécute, comme attendu
	// par music.cpp qui sépare les deux étapes.
	s_prechargee   = checksum;
	s_a_precharger = true;
	return true;
}

bool	PCMAudio_PreLoadMusicStreamDone( void )
{
	// Toujours prêt : aucune étape de chargement séparée n'existe ici.
	// Répondre false ferait attendre le moteur indéfiniment — c'est
	// exactement ce qui figeait les cinématiques.
	return true;
}

bool	PCMAudio_StartPreLoadedMusicStream( void )
{
	if( !s_a_precharger )
		return false;
	s_a_precharger = false;
	return demarre( s_prechargee );
}

void	PCMAudio_StopMusic( bool )
{
	verrouille();
	s_joue = false;
	s_fini = true;
	deverrouille();
}

void	PCMAudio_Pause( bool pause, int )
{
	s_pause = pause;
}

int		PCMAudio_SetMusicVolume( float volume )
{
	s_volume = volume;
	applique_volume();
	return 0;
}

int		PCMAudio_GetMusicStatus( void )
{
	// FREE quand rien ne joue : le moteur enchaîne. RUNNING pendant la
	// lecture, sinon il croirait le morceau fini et en lancerait un autre
	// par-dessus.
	return s_joue ? PCM_STATUS_RUNNING : PCM_STATUS_FREE;
}

// --- pistes nommées et flux de voix ----------------------------------------
//
// Non implémentés dans ce lot. Les valeurs rendues sont celles d'avant, et le
// raisonnement est inchangé : dire « c'est parti » / « rien en cours » évite
// que le moteur attende un flux qui ne démarrera jamais. pcm.wad partage le
// format de music_pcm.wad ; le décodeur ci-dessus le lira tel quel.

// Le chemin � musique � du moteur passe par des NOMS, pas par des checksums :
// music.cpp:1504 (AddTrackToPlaylist) et music.cpp:570 (PlayMusicTrack). Le nom
// est converti avec le CRC du moteur, celui-la meme qui a servi a construire
// l'index -- minuscules, '/' devenant '\\'.
//
// C'est ce maillon qui manquait : tant que TrackExists rendait false, AUCUNE
// piste n'entrait dans les playlists (le return precede numTracks++), donc le
// jeu ne demandait jamais de musique, et l'audio pouvait etre parfaitement
// initialise sans qu'une note ne sorte.
bool	PCMAudio_PlayMusicTrack( const char *p_nom, bool )
{
	if( !p_nom )
		return false;
	return demarre( checksum_de_piste( p_nom ));
}
bool	PCMAudio_PlaySoundtrackMusicTrack( int, int )		{ return true; }

bool	PCMAudio_PlayStream( uint32, int, float, float, float, bool )
															{ return true; }
bool	PCMAudio_PlayStream( uint32, int, Sfx::sVolume *, float, bool )
															{ return true; }

void	PCMAudio_StopStream( int, bool )					{}
void	PCMAudio_StopStreams( void )						{}

bool	PCMAudio_PreLoadStream( uint32, int )				{ return true; }
bool	PCMAudio_PreLoadStreamDone( int )					{ return true; }
bool	PCMAudio_StartPreLoadedStream( int, float, float, float )
															{ return true; }
bool	PCMAudio_StartPreLoadedStream( int, Sfx::sVolume *, float )
															{ return true; }

bool	PCMAudio_SetStreamVolume( float, float, int )		{ return true; }
bool	PCMAudio_SetStreamVolume( Sfx::sVolume *, int )		{ return true; }
bool	PCMAudio_SetStreamPitch( float, int )				{ return true; }

int		PCMAudio_GetStreamStatus( int )						{ return PCM_STATUS_FREE; }

bool	PCMAudio_TrackExists( const char *p_nom, int )
{
	if( !p_nom || !s_index )
		return false;

	const uint32 cs = checksum_de_piste( p_nom );
	const bool   ok = ( trouve( cs ) != NULL );

	// Une trace du couple nom -> checksum vivait ici. Elle a servi une fois,
	// decisivement : elle a montre que le moteur demandait
	// � MUSIC\VAG\SONGS\ACEYALONE � quand l'index attend � ACEYALONE �.
	// A remettre si une piste ne se lance pas.
	return ok;
}

// L'index tient lieu d'en-tete : il est charge une fois pour toutes par
// PCMAudio_Init et contient deja noms (par checksum), offsets et tailles.
bool	PCMAudio_LoadMusicHeader( const char * )
{
	return ( s_index != NULL );
}
bool	PCMAudio_LoadStreamHeader( const char * )			{ return false; }
uint32	PCMAudio_FindNameFromChecksum( uint32, int )		{ return 0; }
