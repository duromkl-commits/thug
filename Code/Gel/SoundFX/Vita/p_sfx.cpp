/*****************************************************************************
**  THUG-Vita â€” effets sonores                                              **
**  Code/Gel/SoundFX/Vita/p_sfx.cpp                                         **
**                                                                          **
**  Les .pcm du jeu sont de l'Xbox ADPCM mono, 44100 Hz, blockAlign 36 â€”     **
**  le meme format que la musique, a la stereo pres. Controle arithmetique  **
**  fait sur le fichier : 44100 / 64 echantillons par bloc * 36 octets =    **
**  24806, exactement l'avgBytesPerSec annonce. Le decodeur est partage :   **
**  Core/Vita/adpcm.cpp, valide au bit pres.                                **
**                                                                          **
**  CE QUI CHANGE PAR RAPPORT A LA MUSIQUE : le jeu joue plusieurs effets   **
**  simultanement (roulettes, grind, collisions, voix), alors que           **
**  sceAudioOut ne fournit qu'un flux. Il faut donc MIXER nous-memes.       **
**                                                                          **
**  Choix retenus, et pourquoi :                                            **
**                                                                          **
**  - Sons decodes ENTIEREMENT a la lecture du fichier, gardes en PCM. Un   **
**    effet fait ~12 Ko d'ADPCM, soit ~48 Ko decode ; meme quelques         **
**    centaines tiennent largement. Decoder a la volee dans le mixeur       **
**    couterait du CPU a chaque image, sur une console ou on se bat deja    **
**    pour la fluidite.                                                     **
**                                                                          **
**  - Mixage en entier 32 bits puis ecretage. Additionner 24 voix           **
**    directement en 16 bits deborde des que trois sons forts se            **
**    superposent, et le debordement s'entend comme un craquement.          **
**                                                                          **
**  - Le pas de lecture est en virgule fixe 16.16 : il porte a la fois le   **
**    reechantillonnage 44100 -> 48000 et le pitch demande par le jeu, sans **
**    calcul flottant par echantillon.                                      **
*****************************************************************************/

#include <core/defines.h>
#include <core/Vita/adpcm.h>

#include <gel/soundfx/soundfx.h>
#include <gel/soundfx/Vita/p_sfx.h>

#include <sys/file/filesys.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "vita_log.h"

namespace Sfx
{

float	gSfxVolume = 100.0f;

namespace
{

#define SORTIE_HZ		48000	/* le port BGM tourne deja a 48000 */
#define SORTIE_CANAUX	2
// 512 et non 1024 : le port MAIN n'accepte pas plus (64..512, multiple de 64).
#define GRAIN			512

// Etat d'une voix du mixeur.
struct SVoix
{
	const SSonVita	*p_son;
	unsigned int	 position;		// virgule fixe 16.16 dans le son
	unsigned int	 pas;			// increment 16.16 par echantillon de sortie
	int				 vol_g;			// 0..256
	int				 vol_d;
	bool			 active;
	bool			 boucle;
	// #57 : age de la voix ; une voix en boucle active plus de 20 s est
	// nommee une fois dans le journal (son de casse joue sans fin, revu le
	// 2026-10-05 sans aucun appel de son en cours : une seule voix en boucle).
	unsigned long long	 t_debut;
	bool			 signalee;
};

static SVoix	s_voix[NUM_VOICES];

static int		s_port			= -1;
static SceUID	s_thread		= -1;
static SceUID	s_mutex			= -1;
static bool		s_tourne		= false;
static short	*s_melange		= NULL;
static int		s_volume_global	= 256;	// 0..256

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


// Le moteur donne des volumes par canal, en pourcentage, avec un signe qui
// encode la position derriere l'auditeur sur PS2. On ne garde que l'amplitude.
static void volumes_depuis( sVolume *p_vol, int *p_g, int *p_d )
{
	float g = 100.0f, d = 100.0f;
	if( p_vol )
	{
		g = p_vol->GetChannelVolume( 0 );
		d = p_vol->GetChannelVolume( 1 );
	}
	if( g < 0.0f )	g = -g;
	if( d < 0.0f )	d = -d;

	*p_g = (int)( g * 2.56f );
	*p_d = (int)( d * 2.56f );
	if( *p_g > 256 )	*p_g = 256;
	if( *p_d > 256 )	*p_d = 256;
	if( *p_g < 0 )		*p_g = 0;
	if( *p_d < 0 )		*p_d = 0;
}


static int thread_mixeur( SceSize, void * )
{

	while( s_tourne )
	{
		// Un battement periodique du thread vivait ici. Il a servi une fois,
		// decisivement : il a montre que le thread CESSAIT de tourner ~8 s
		// avant le premier son joue, ce qui a designe CleanUpSoundFX et non
		// la boucle de melange. A remettre en premier si un silence revient.
		// Le melange se fait en 32 bits : la somme de plusieurs voix fortes
		// depasse allegrement le 16 bits, et l'ecretage doit se faire UNE
		// fois, a la fin, pas a chaque addition.
		static int accu[GRAIN * SORTIE_CANAUX];
		memset( accu, 0, sizeof( accu ));

		bool quelque_chose = false;

		verrouille();
		for( int v = 0; v < NUM_VOICES; ++v )
		{
			SVoix *p_v = &s_voix[v];
			if( !p_v->active || !p_v->p_son )
				continue;

			const SSonVita *p_s = p_v->p_son;
			quelque_chose = true;

			for( int i = 0; i < GRAIN; ++i )
			{
				const unsigned int idx = p_v->position >> 16;
				if( (int)idx >= p_s->nb_echantillons )
				{
					if( p_v->boucle )
					{
						p_v->position = 0;
						continue;
					}
					p_v->active = false;
					break;
				}

				const int e = p_s->p_echantillons[idx];
				accu[i * 2    ] += ( e * p_v->vol_g ) >> 8;
				accu[i * 2 + 1] += ( e * p_v->vol_d ) >> 8;

				p_v->position += p_v->pas;
			}
		}
		const int vg = s_volume_global;
		deverrouille();

		if( !quelque_chose )
		{
			// Aucune voix : on ne pousse pas de silence en boucle serree, on
			// rend la main. Sans cela ce thread tournerait en continu pour
			// rien, sur une console ou chaque coeur compte.
			sceKernelDelayThread( 5000 );
			continue;
		}

		for( int i = 0; i < GRAIN * SORTIE_CANAUX; ++i )
		{
			int e = ( accu[i] * vg ) >> 8;
			if( e >  32767 )	e =  32767;
			if( e < -32768 )	e = -32768;
			s_melange[i] = (short)e;
		}

		sceAudioOutOutput( s_port, s_melange );
	}
	return 0;
}

} // namespace anonyme


void InitSoundFX( CSfxManager * )
{
	memset( s_voix, 0, sizeof( s_voix ));

	// memalign et non malloc : la sortie audio de la Vita exige un tampon
	// aligne. Un tampon mal aligne ne provoque pas d'erreur franche, juste
	// du silence -- exactement le symptome le plus couteux a diagnostiquer.
	s_melange = (short *)memalign( 64, GRAIN * SORTIE_CANAUX * sizeof( short ));
	if( !s_melange )
	{
		VLOG( "SFX", "pas d'effets : allocation du tampon impossible" );
		return;
	}

	// Port MAIN, pas BGM : la Vita n'accorde qu'UN SEUL port BGM et la musique
	// l'a deja pris. Le second appel echouait avec 0x80260005.
	s_port = sceAudioOutOpenPort( SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN,
	                              SORTIE_HZ, SCE_AUDIO_OUT_MODE_STEREO );
	if( s_port < 0 )
	{
		VLOG( "SFX", "pas d'effets : port audio refuse (0x%08x)", s_port );
		return;
	}

	int vols[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume( s_port,
	                      (SceAudioOutChannelFlag)( SCE_AUDIO_VOLUME_FLAG_L_CH |
	                                                SCE_AUDIO_VOLUME_FLAG_R_CH ),
	                      vols );

	s_mutex = sceKernelCreateMutex( "thug_sfx", 0, 0, NULL );

	s_tourne = true;
	s_thread = sceKernelCreateThread( "thug_sfx", thread_mixeur,
	                                  0x10000100, 0x10000, 0, 0, NULL );
	if( s_thread >= 0 )
	{
		// Le retour est verifie : un demarrage refuse passerait sinon pour un
		// succes, avec pour seul symptome un silence total.
		const int r = sceKernelStartThread( s_thread, 0, NULL );
		if( r < 0 )
			VLOG( "SFX", "pas d'effets : demarrage refuse (0x%08x)", r );
	}
	else
	{
		VLOG( "SFX", "pas d'effets : thread refuse (0x%08x)", s_thread );
	}

	VLOG( "SFX", "effets prets : %d voix, melange logiciel a %d Hz",
	      (int)NUM_VOICES, (int)SORTIE_HZ );
}


void CleanUpSoundFX( void )
{
	// [REFUTE] ï¿½ nettoyer, c'est arreter le thread ï¿½. Le commentaire d'origine
	// (soundfx.cpp:375) est explicite : ï¿½ so soundfx can be used in the NEXT
	// PHASE (next level, frontend, whatever) ï¿½. Cette fonction est appelee a
	// CHAQUE changement de phase, pas a l'extinction.
	//
	// Poser s_tourne = false ici tuait donc le thread de melange au premier
	// chargement de niveau, definitivement. Symptome : les sons se chargeaient
	// bien, PlaySoundPlease attribuait des voix, et il ne sortait rien -- avec
	// pour seule trace un battement du thread qui cessait ~8 s avant le
	// premier son joue.
	//
	// Nettoyer, ici, c'est faire taire les voix ET rendre les sons du niveau.
	StopAllSoundFX();

	// [SOURCE] XBox/p_sfx.cpp:843 : « on Xbox it needs to explicitly delete any
	// sounds that were not marked as permanent at load time ». Oubli ici : ~10 Mo
	// de PCM decode perdus a CHAQUE changement de niveau, tas newlib epuise apres
	// ~9 niveaux -- plantage du Story Mode au chargement de Vancouver (#47, #51).
	// Les voix sont arretees (StopAllSoundFX) ; le verrou du mixeur garantit
	// qu'aucune ne lit plus ces echantillons.
	verrouille();
	for( int i = 0; i < NumWavesInTable; ++i )
	{
		PlatformWaveInfo *p_info = &( WaveTable[PERM_WAVE_TABLE_MAX_ENTRIES + i].platformWaveInfo );
		SSonVita *p_son = (SSonVita *)p_info->p_sound_data;
		if( p_son )
		{
			free( p_son->p_echantillons );
			free( p_son );
		}
		p_info->p_sound_data = NULL;
	}
	NumWavesInTable = 0;
	deverrouille();
}


// Le thread ne s'arrete qu'a l'extinction du programme. Garde pour memoire :
// personne ne l'appelle aujourd'hui, et c'est voulu.
void ArreteThreadSfx( void )
{
	s_tourne = false;
}


bool LoadSoundPlease( const char *sfxName, uint32, PlatformWaveInfo *pInfo,
                      bool loadPerm )
{
	if( !pInfo || !sfxName )
		return false;

	pInfo->p_sound_data = NULL;

	// Chemin construit comme sur Xbox (SoundFX/Xbox/p_sfx.cpp) :
	// Â« sounds\pcm\ Â» + nom + Â« .pcm Â». Ces fichiers vivent dans les archives
	// PRE (skater_sounds.prx, parked_sounds.prx...), que la couche fichier
	// sait deja ouvrir de maniere transparente.
	char chemin[256];
	snprintf( chemin, sizeof( chemin ), "sounds\\pcm\\%s.pcm", sfxName );

	void *p_fic = File::Open( chemin, "rb" );
	if( !p_fic )
		return false;

	const int taille = File::GetFileSize( p_fic );
	if( taille < 64 )
	{
		File::Close( p_fic );
		return false;
	}

	unsigned char *p_brut = (unsigned char *)malloc( taille );
	if( !p_brut )
	{
		File::Close( p_fic );
		return false;
	}
	File::Read( p_brut, 1, taille, p_fic );
	File::Close( p_fic );

	// PARCOURS DES BLOCS RIFF, et non des offsets fixes.
	//
	// [REFUTE] ï¿½ fmt est a 12 et data a 40, comme dans les .wad ï¿½. C'est vrai
	// pour certains effets, faux pour d'autres : BailBodyPunch01_11 porte un
	// bloc ï¿½ bext ï¿½ (Broadcast Wave Extension) juste apres WAVE, et tout se
	// decale. Constate sur console -- les octets 12..15 valaient 62 65 78 74.
	if(( memcmp( p_brut, "RIFF", 4 ) != 0 ) ||
	   ( memcmp( p_brut + 8, "WAVE", 4 ) != 0 ))
	{
		VLOG( "SFX", "%s : ce n'est pas un RIFF/WAVE", sfxName );
		free( p_brut );
		return false;
	}

	int off_fmt = -1, off_data = -1, taille_data = 0;
	bool boucle = false;
	int pos = 12;
	while( pos + 8 <= taille )
	{
		unsigned int taille_bloc;
		memcpy( &taille_bloc, p_brut + pos + 4, 4 );

		if( memcmp( p_brut + pos, "fmt ", 4 ) == 0 )
		{
			off_fmt = pos + 8;
		}
		else if( memcmp( p_brut + pos, "data", 4 ) == 0 )
		{
			off_data    = pos + 8;
			taille_data = (int)taille_bloc;
		}
		// Son en boucle : bloc « smpl » avec au moins une boucle, comme
		// CWaveFile::ContainsLoop (Xbox/p_sfx.cpp:555). Il est APRES « data »
		// dans les 94 sons concernes (roulement, grind, moteurs...), d'ou le
		// parcours complet. La Xbox boucle alors tout le tampon
		// (DSBPLAY_LOOPING), sans tenir compte des points de boucle.
		else if(( memcmp( p_brut + pos, "smpl", 4 ) == 0 ) && ( taille_bloc >= 36 )
		        && ( pos + 8 + 32 <= taille ))
		{
			unsigned int nb_boucles;
			memcpy( &nb_boucles, p_brut + pos + 8 + 28, 4 );
			boucle = ( nb_boucles > 0 );
		}

		// Les blocs RIFF sont alignes sur 2 octets.
		pos += 8 + (int)taille_bloc;
		if( taille_bloc & 1 )
			++pos;
	}

#ifdef THUG_DESKTOP
	// The loop flag as the Xbox reads it: CRiffChunk::Open (Xbox/p_sfx.cpp:373)
	// steps chunk to chunk WITHOUT the RIFF word padding. After an odd-sized
	// chunk it is lost and never finds 'smpl', so on the Xbox those sounds play
	// once. Reading the padding found their 'smpl' and looped them forever
	// (FallWater repeating after a fall into water).
	{
		bool boucle_xbox = false;
		int p = 12;
		while( p + 8 <= taille )
		{
			unsigned int t;
			memcpy( &t, p_brut + p + 4, 4 );
			if( memcmp( p_brut + p, "smpl", 4 ) == 0 )
			{
				if( p + 8 + 32 <= taille )
				{
					unsigned int n;
					memcpy( &n, p_brut + p + 8 + 28, 4 );
					boucle_xbox = ( n > 0 );
				}
				break;
			}
			if( t > (unsigned int)taille )
				break;
			p += 8 + (int)t;
		}
		if( boucle != boucle_xbox )
			VLOG( "SFX", "%s : loop %d -> %d (Xbox chunk walk)", sfxName, (int)boucle, (int)boucle_xbox );
		boucle = boucle_xbox;
	}
#endif

	if(( off_fmt < 0 ) || ( off_data < 0 ))
	{
		VLOG( "SFX", "%s : bloc fmt ou data introuvable", sfxName );
		free( p_brut );
		return false;
	}

	const uint16 tag    = (uint16)( p_brut[off_fmt] | ( p_brut[off_fmt + 1] << 8 ));
	const int    canaux = (int)( p_brut[off_fmt + 2] | ( p_brut[off_fmt + 3] << 8 ));
	int          hz;
	memcpy( &hz, p_brut + off_fmt + 4, 4 );
	const int    block  = (int)( p_brut[off_fmt + 12] | ( p_brut[off_fmt + 13] << 8 ));

	if(( tag != 0x0069 ) || ( canaux < 1 ) || ( block < 8 ))
	{
		VLOG( "SFX", "%s : format 0x%04x non gere", sfxName, tag );
		free( p_brut );
		return false;
	}

	if( taille_data > ( taille - off_data ))
		taille_data = taille - off_data;

	const int blocs = taille_data / block;
	const int epb   = VitaAdpcm::EchantillonsParBloc( block, canaux );
	const int total = blocs * epb;
	if( total <= 0 )
	{
		free( p_brut );
		return false;
	}

	// On ne garde qu'un canal : le mixeur spatialise lui-meme en repartissant
	// sur gauche et droite. Les effets du jeu sont mono de toute facon.
	short *p_pcm = (short *)malloc( total * sizeof( short ));
	if( !p_pcm )
	{
		free( p_brut );
		return false;
	}

	// Garde explicite : DecodeBloc ecrit epb * canaux valeurs. Un blockAlign
	// inattendu deborderait ce tampon et corromprait la pile.
	if(( epb * canaux ) > 256 )
	{
		VLOG( "SFX", "%s : bloc de %d echantillons, trop grand", sfxName,
		      epb * canaux );
		free( p_brut );
		return false;
	}
	short tampon[256];
	int   ecrit = 0;
	for( int b = 0; b < blocs; ++b )
	{
		VitaAdpcm::DecodeBloc( p_brut + off_data + b * block, tampon, canaux, block );
		for( int i = 0; i < epb; ++i )
			p_pcm[ecrit++] = tampon[i * canaux];
	}
	free( p_brut );

	SSonVita *p_son = (SSonVita *)malloc( sizeof( SSonVita ));
	if( !p_son )
	{
		free( p_pcm );
		return false;
	}
	p_son->p_echantillons  = p_pcm;
	p_son->nb_echantillons = ecrit;
	p_son->hz              = hz;
	p_son->boucle          = boucle;
	{
		const char *b = sfxName;
		for( const char *q = sfxName; *q; ++q )
			if(( *q == '\\' ) || ( *q == '/' ))
				b = q + 1;
		strncpy( p_son->nom, b, sizeof( p_son->nom ) - 1 );
		p_son->nom[sizeof( p_son->nom ) - 1] = 0;
	}

	pInfo->p_sound_data = p_son;
	pInfo->looping      = boucle;
	pInfo->permanent    = loadPerm;
	return true;
}


int PlaySoundPlease( PlatformWaveInfo *pInfo, sVolume *p_vol, float pitch )
{
	if( !pInfo || !pInfo->p_sound_data || ( s_port < 0 ))
		return -1;

	verrouille();

	int v = -1;
	for( int i = 0; i < NUM_VOICES; ++i )
	{
		if( !s_voix[i].active )
		{
			v = i;
			break;
		}
	}
	if( v < 0 )
	{
		deverrouille();
		return -1;
	}

	SVoix *p_v = &s_voix[v];
	p_v->p_son    = pInfo->p_sound_data;
	p_v->position = 0;
	p_v->boucle   = pInfo->looping;
	p_v->t_debut  = sceKernelGetProcessTimeWide();
	p_v->signalee = false;

	// Un seul pas porte le reechantillonnage ET le pitch : le son est a
	// 44100 Hz, la sortie a 48000, et le jeu exprime le pitch en pourcentage.
	float rapport = (float)pInfo->p_sound_data->hz / (float)SORTIE_HZ;
	if( pitch > 1.0f )
		rapport *= ( pitch / 100.0f );
	p_v->pas = (unsigned int)( rapport * 65536.0f );
	if( p_v->pas == 0 )
		p_v->pas = 1;

	volumes_depuis( p_vol, &p_v->vol_g, &p_v->vol_d );
	p_v->active = true;

	deverrouille();
	return v;
}


void StopSoundPlease( int whichVoice )
{
	if(( whichVoice < 0 ) || ( whichVoice >= NUM_VOICES ))
		return;
	verrouille();
	s_voix[whichVoice].active = false;
	deverrouille();
}


bool VoiceIsOn( int whichVoice )
{
	if(( whichVoice < 0 ) || ( whichVoice >= NUM_VOICES ))
		return false;
	return s_voix[whichVoice].active;
}


void SetVoiceParameters( int whichVoice, sVolume *p_vol, float pitch )
{
	if(( whichVoice < 0 ) || ( whichVoice >= NUM_VOICES ))
		return;

	verrouille();
	SVoix *p_v = &s_voix[whichVoice];
	volumes_depuis( p_vol, &p_v->vol_g, &p_v->vol_d );

	// Â« pitch a zero Â» veut dire Â« ne change pas le pitch Â» (cf. p_sfx.h).
	if(( pitch > 1.0f ) && p_v->p_son )
	{
		float rapport = (float)p_v->p_son->hz / (float)SORTIE_HZ;
		rapport *= ( pitch / 100.0f );
		p_v->pas = (unsigned int)( rapport * 65536.0f );
		if( p_v->pas == 0 )
			p_v->pas = 1;
	}
	deverrouille();
}


void StopAllSoundFX( void )
{
	verrouille();
	for( int i = 0; i < NUM_VOICES; ++i )
		s_voix[i].active = false;
	deverrouille();
}


void PauseSoundsPlease( void )
{
	StopAllSoundFX();
}


void SetVolumePlease( float volumeLevel )
{
	int v = (int)( volumeLevel * 2.56f );
	if( v < 0 )		v = 0;
	if( v > 256 )	v = 256;
	if(( v == 0 ) && ( s_volume_global != 0 ))
		VLOG( "SFX", "volume global mis a ZERO par le moteur" );
	s_volume_global = v;
}


// Sans effet : la Vita n'a pas de reverbe materielle et une reverbe logicielle
// couterait plus cher qu'elle ne rapporte ici. Ne rien faire est correct â€” le
// son sort simplement sec.
void SetReverbPlease( float, int, bool )	{}

// Bilan des voix toutes les 10 s, seulement s'il change : sert a reperer un
// son en boucle jamais arrete (#57).
void PerFrameUpdate( void )
{
	static unsigned long long s_t = 0;
	static int s_dernier = -1;
	const unsigned long long t = sceKernelGetProcessTimeWide();
	if(( t - s_t ) < 10000000ULL )
		return;
	s_t = t;
	int actives = 0, boucles = 0;
	for( int i = 0; i < NUM_VOICES; ++i )
		if( s_voix[i].active )
		{
			++actives;
			if( s_voix[i].boucle )
			{
				++boucles;
				if( !s_voix[i].signalee && ( t - s_voix[i].t_debut > 20000000ULL ))
				{
					s_voix[i].signalee = true;
					VLOG( "SND", "voix %d en boucle depuis %d s : '%s' (volumes %d/%d)",
					      i, (int)(( t - s_voix[i].t_debut ) / 1000000ULL ),
					      s_voix[i].p_son ? s_voix[i].p_son->nom : "?",
					      s_voix[i].vol_g, s_voix[i].vol_d );
				}
			}
		}
	if(( actives * 1000 + boucles ) != s_dernier )
	{
		s_dernier = actives * 1000 + boucles;
		VLOG( "SND", "voix actives %d dont %d en boucle", actives, boucles );
	}
}

void VitaListeVoix( void )
{
	const unsigned long long t = sceKernelGetProcessTimeWide();
	int n = 0;
	for( int i = 0; i < NUM_VOICES; ++i )
		if( s_voix[i].active )
		{
			++n;
			VLOG( "SND", "  voix %d : '%s'%s, volumes %d/%d, depuis %.1f s", i,
			      s_voix[i].p_son ? s_voix[i].p_son->nom : "?",
			      s_voix[i].boucle ? " EN BOUCLE" : "",
			      s_voix[i].vol_g, s_voix[i].vol_d,
			      (float)( t - s_voix[i].t_debut ) / 1000000.0f );
		}
	VLOG( "SND", "voix actives : %d", n );
}

// Le moteur s'en sert pour decider s'il peut charger d'autres sons. La Vita
// n'a pas de memoire audio dediee : on annonce une reserve confortable prise
// sur la memoire principale, plutot que zero, qui ferait renoncer le moteur.
int GetMemAvailable( void )					{ return 8 * 1024 * 1024; }

} // namespace Sfx
