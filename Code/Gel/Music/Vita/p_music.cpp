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
#include <gel/soundfx/soundfx.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

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

// --- voice streams (pcm.wad) ------------------------------------------------
//
// Dialogue, cutscene and announcer voices. Same container as the music:
// pcm.dat is { count, count x { checksum, offset, size } }, pcm.wad is a run
// of 48-byte RIFF headers + Xbox ADPCM. Unlike the music, a voice can be mono
// and use any sample rate, so each stream reads its own header and is
// resampled to the 48 kHz port. One port and one thread per stream slot.

namespace
{

static const char *CHEMIN_VOIX_DAT = "ux0:data/thug/Data/streams/pcm/pcm.dat";
static const char *CHEMIN_VOIX_WAD = "ux0:data/thug/Data/streams/pcm/pcm.wad";

#define VOIX_BLOCS_LECTURE	64		/* ADPCM blocks per read */
#define VOIX_MAX_ECH		( VOIX_BLOCS_LECTURE * 64 + 1 )

struct SFlux
{
	SceUID	wad;
	int		port;
	SceUID	thread;
	SceUID	mutex;
	int		num;

	volatile bool	joue;		// slot busy (playing or preloaded)
	volatile bool	ok;			// allowed to output (false while preloaded)
	volatile bool	pause;

	int		canaux, hz, bloc;
	uint32	debut, taille, lue;

	// Decoded source frames, stereo interleaved; [0] carries the previous
	// read's last frame so interpolation runs across reads.
	short	src[VOIX_MAX_ECH * 2];
	int		src_n;
	double	src_pos;

	unsigned char lecture[VOIX_BLOCS_LECTURE * 72];
	short	tmp[64 * 2];
	short	sortie[GRAIN * 2];
};

static SEntree	*s_vindex		= NULL;
static int		 s_vindex_nb	= 0;
static SFlux	*s_flux			= NULL;
static volatile bool s_voix_tourne = false;

static const SEntree *vtrouve( uint32 checksum )
{
	int lo = 0, hi = s_vindex_nb - 1;
	while( lo <= hi )
	{
		const int mid = ( lo + hi ) / 2;
		if( s_vindex[mid].checksum == checksum )	return &s_vindex[mid];
		if( s_vindex[mid].checksum <  checksum )	lo = mid + 1;
		else										hi = mid - 1;
	}
	return NULL;
}

static bool vcharge_index( void )
{
	SceUID f = sceIoOpen( CHEMIN_VOIX_DAT, SCE_O_RDONLY, 0 );
	if( f < 0 )
	{
		VLOG( "PCM", "voice index missing: %s", CHEMIN_VOIX_DAT );
		return false;
	}
	uint32 nb = 0;
	if( sceIoRead( f, &nb, 4 ) != 4 || nb == 0 || nb > 200000 )
	{
		sceIoClose( f );
		return false;
	}
	s_vindex = (SEntree *)malloc( nb * sizeof( SEntree ));
	const int voulu = (int)( nb * sizeof( SEntree ));
	const int lu = s_vindex ? sceIoRead( f, s_vindex, voulu ) : 0;
	sceIoClose( f );
	if( lu != voulu )
	{
		free( s_vindex );
		s_vindex = NULL;
		return false;
	}
	s_vindex_nb = (int)nb;
	qsort( s_vindex, s_vindex_nb, sizeof( SEntree ), cmp_entree );
	return true;
}

// Called with the stream's mutex held.
static bool vprepare( SFlux &F, const SEntree *p_e )
{
	unsigned char e[48];
	if( sceIoLseek( F.wad, p_e->offset, SCE_SEEK_SET ) < 0 ||
	    sceIoRead( F.wad, e, sizeof( e )) != (int)sizeof( e ))
		return false;
	if( memcmp( e + 12, "fmt ", 4 ) || memcmp( e + 40, "data", 4 ))
		return false;
	const int tag	= e[20] | ( e[21] << 8 );
	const int can	= e[22] | ( e[23] << 8 );
	const int hz	= e[24] | ( e[25] << 8 ) | ( e[26] << 16 ) | ( e[27] << 24 );
	const int bloc	= e[32] | ( e[33] << 8 );
	if( tag != 0x0069 || ( can != 1 && can != 2 ) || hz < 4000 || hz > 96000 ||
	    bloc != 36 * can )
	{
		VLOG( "PCM", "voice %08x: unsupported format %04x %dch %dHz block %d",
		      p_e->checksum, tag, can, hz, bloc );
		return false;
	}
	uint32 taille;
	memcpy( &taille, e + 44, 4 );
	if( taille > p_e->taille - 48 )
		taille = p_e->taille - 48;

	F.canaux = can;
	F.hz	 = hz;
	F.bloc	 = bloc;
	F.debut	 = p_e->offset + 48;
	F.taille = taille;
	F.lue	 = 0;
	F.src[0] = F.src[1] = 0;
	F.src_n	 = 1;
	F.src_pos = 0.0;
	return true;
}

// Decode the next blocks after the carried frame. Mutex held.
static bool vremplit( SFlux &F )
{
	const uint32 reste = F.taille > F.lue ? F.taille - F.lue : 0;
	int blocs = (int)( reste / F.bloc );
	if( blocs > VOIX_BLOCS_LECTURE )
		blocs = VOIX_BLOCS_LECTURE;
	if( blocs <= 0 )
		return false;
	sceIoLseek( F.wad, F.debut + F.lue, SCE_SEEK_SET );
	const int lu = sceIoRead( F.wad, F.lecture, blocs * F.bloc );
	if( lu < F.bloc )
		return false;
	blocs = lu / F.bloc;
	F.lue += blocs * F.bloc;

	// Keep the last frame as the new [0].
	F.src[0] = F.src[( F.src_n - 1 ) * 2];
	F.src[1] = F.src[( F.src_n - 1 ) * 2 + 1];
	F.src_pos -= ( F.src_n - 1 );
	F.src_n = 1;
	for( int b = 0; b < blocs; ++b )
	{
		VitaAdpcm::DecodeBloc( F.lecture + b * F.bloc, F.tmp, F.canaux, F.bloc );
		for( int i = 0; i < 64; ++i )
		{
			short *d = F.src + F.src_n * 2;
			d[0] = F.tmp[i * F.canaux];
			d[1] = F.tmp[i * F.canaux + F.canaux - 1];
			++F.src_n;
		}
	}
	return true;
}

static int thread_voix( SceSize, void *p_arg )
{
	SFlux &F = s_flux[*(int *)p_arg];
	while( s_voix_tourne )
	{
		sceKernelLockMutex( F.mutex, 1, NULL );
		if( !F.joue || !F.ok || F.pause )
		{
			sceKernelUnlockMutex( F.mutex, 1 );
			sceKernelDelayThread( 5000 );
			continue;
		}
		const double pas = (double)F.hz / AUDIO_HZ;
		int i = 0;
		bool fini = false;
		for( ; i < GRAIN; ++i )
		{
			while( F.src_pos + 1.0 >= F.src_n )
				if( !vremplit( F ))
				{
					fini = true;
					break;
				}
			if( fini )
				break;
			const int k = (int)F.src_pos;
			const float t = (float)( F.src_pos - k );
			const short *a = F.src + k * 2;
			F.sortie[i * 2]     = (short)( a[0] + ( a[2] - a[0] ) * t );
			F.sortie[i * 2 + 1] = (short)( a[1] + ( a[3] - a[1] ) * t );
			F.src_pos += pas;
		}
		if( fini )
		{
			memset( F.sortie + i * 2, 0, ( GRAIN - i ) * 2 * sizeof( short ));
			F.joue = false;
		}
		sceKernelUnlockMutex( F.mutex, 1 );
		if( i > 0 )
			sceAudioOutOutput( F.port, F.sortie );
	}
	return 0;
}

#ifdef THUG_DESKTOP
extern "C" int desktop_voices( void );
#endif

static void voix_init( void )
{
#ifdef THUG_DESKTOP
	if( !desktop_voices())
	{
		VLOG( "PCM", "voices off (thug_desktop.ini)" );
		return;
	}
#endif
	if( !vcharge_index() )
		return;
	s_flux = (SFlux *)calloc( NUM_STREAMS, sizeof( SFlux ));
	if( !s_flux )
		return;
	s_voix_tourne = true;
	int prets = 0;
	for( int s = 0; s < NUM_STREAMS; ++s )
	{
		SFlux &F = s_flux[s];
		F.num	 = s;
		F.thread = -1;
		F.wad	 = sceIoOpen( CHEMIN_VOIX_WAD, SCE_O_RDONLY, 0 );
		F.port	 = sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_VOICE, GRAIN,
		                                AUDIO_HZ, SCE_AUDIO_OUT_MODE_STEREO );
		F.mutex	 = sceKernelCreateMutex( "thug_voix", 0, 0, NULL );
		if( F.wad < 0 || F.port < 0 || F.mutex < 0 )
		{
			VLOG( "PCM", "voice stream %d unavailable (wad %d, port 0x%08x)", s, F.wad, F.port );
			if( F.wad >= 0 ) sceIoClose( F.wad );
			F.wad = -1;
			continue;
		}
		F.thread = sceKernelCreateThread( "thug_voix", thread_voix, 0x10000100, 0x10000, 0, 0, NULL );
		if( F.thread >= 0 )
		{
			sceKernelStartThread( F.thread, sizeof( int ), &F.num );
			++prets;
		}
	}
	VLOG( "PCM", "voices: %d streams, %d ready", s_vindex_nb, prets );
}

static inline bool vflux_ok( int s )
{
	return s_flux && s >= 0 && s < NUM_STREAMS && s_flux[s].thread >= 0;
}

static void vvolume( int s, float l, float r )
{
	if( !vflux_ok( s ))
		return;
	int v[2];
	v[0] = (int)( fabsf( l ) * ( 1.0f / 100.0f ) * SCE_AUDIO_VOLUME_0DB );
	v[1] = (int)( fabsf( r ) * ( 1.0f / 100.0f ) * SCE_AUDIO_VOLUME_0DB );
	for( int c = 0; c < 2; ++c )
		if( v[c] > SCE_AUDIO_VOLUME_0DB ) v[c] = SCE_AUDIO_VOLUME_0DB;
	sceAudioOutSetVolume( s_flux[s].port,
	                      (SceAudioOutChannelFlag)( SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH ), v );
}

// Engine volumes are percentages, scaled by the sound effects volume like
// the Xbox backend does.
static void vvolume( int s, Sfx::sVolume *p_volume )
{
	if( !p_volume )
		return;
	Spt::SingletonPtr< Sfx::CSfxManager > sfx_manager;
	const float main = sfx_manager->GetMainVolume() * ( 1.0f / 100.0f );
	const float l = p_volume->GetChannelVolume( 0 );
	const float r = ( p_volume->GetVolumeType() == Sfx::VOLUME_TYPE_BASIC_2_CHANNEL ) ? l : p_volume->GetChannelVolume( 1 );
	vvolume( s, l * main, r * main );
}

static bool vdemarre( uint32 checksum, int s, bool ok )
{
	if( !vflux_ok( s ))
		return false;
	const SEntree *p_e = vtrouve( checksum );
	if( !p_e )
	{
		VLOG( "PCM", "voice %08x not in pcm.dat", checksum );
		return false;
	}
	SFlux &F = s_flux[s];
	sceKernelLockMutex( F.mutex, 1, NULL );
	const bool pret = vprepare( F, p_e );
	F.joue	= pret;
	F.ok	= ok;
	F.pause	= false;
	sceKernelUnlockMutex( F.mutex, 1 );
	return pret;
}

static void varrete( int s )
{
	if( !vflux_ok( s ))
		return;
	SFlux &F = s_flux[s];
	sceKernelLockMutex( F.mutex, 1, NULL );
	F.joue = false;
	sceKernelUnlockMutex( F.mutex, 1 );
}

} // namespace anonyme

// ---------------------------------------------------------------------------
// API attendue par Gel/Music/music.cpp
// ---------------------------------------------------------------------------

void	PCMAudio_Init( void )
{
	voix_init();
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

void	PCMAudio_Pause( bool pause, int ch )
{
	if( ch == MUSIC_CHANNEL )
	{
		s_pause = pause;
		return;
	}
	if( s_flux )
		for( int i = 0; i < NUM_STREAMS; ++i )
			s_flux[i].pause = pause;
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


bool	PCMAudio_PlayStream( uint32 checksum, int s, float l, float r, float, bool preload )
{
	if( !vdemarre( checksum, s, !preload ))
		return false;
	vvolume( s, l, r );
	return true;
}
bool	PCMAudio_PlayStream( uint32 checksum, int s, Sfx::sVolume *p_volume, float, bool preload )
{
	if( !vdemarre( checksum, s, !preload ))
		return false;
	vvolume( s, p_volume );
	return true;
}

void	PCMAudio_StopStream( int s, bool )
{
	varrete( s );
}
void	PCMAudio_StopStreams( void )
{
	for( int s = 0; s < NUM_STREAMS; ++s )
		varrete( s );
}

bool	PCMAudio_PreLoadStream( uint32 checksum, int s )
{
	return vdemarre( checksum, s, false );
}
// Nothing to load ahead: the stream reads as it plays.
bool	PCMAudio_PreLoadStreamDone( int )					{ return true; }
bool	PCMAudio_StartPreLoadedStream( int s, float l, float r, float )
{
	if( !vflux_ok( s ) || !s_flux[s].joue )
		return false;
	vvolume( s, l, r );
	s_flux[s].ok = true;
	return true;
}
bool	PCMAudio_StartPreLoadedStream( int s, Sfx::sVolume *p_volume, float )
{
	if( !vflux_ok( s ) || !s_flux[s].joue )
		return false;
	vvolume( s, p_volume );
	s_flux[s].ok = true;
	return true;
}

bool	PCMAudio_SetStreamVolume( float l, float r, int s )	{ vvolume( s, l, r ); return true; }
bool	PCMAudio_SetStreamVolume( Sfx::sVolume *p_volume, int s ) { vvolume( s, p_volume ); return true; }
bool	PCMAudio_SetStreamPitch( float, int )				{ return true; }

int		PCMAudio_GetStreamStatus( int s )
{
	if( s == -1 )
	{
		for( int i = 0; i < NUM_STREAMS; ++i )
			if( PCMAudio_GetStreamStatus( i ) == PCM_STATUS_FREE )
				return PCM_STATUS_FREE;
		return PCM_STATUS_RUNNING;
	}
	return ( vflux_ok( s ) && s_flux[s].joue ) ? PCM_STATUS_RUNNING : PCM_STATUS_FREE;
}

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
// pcm.dat stands in for the per-level headers: it lists every voice.
bool	PCMAudio_LoadStreamHeader( const char * )			{ return s_vindex != NULL; }
uint32	PCMAudio_FindNameFromChecksum( uint32 checksum, int ch )
{
	if( ch != EXTRA_CHANNEL )
		return 0;
	return vtrouve( checksum ) ? checksum : 0;
}
