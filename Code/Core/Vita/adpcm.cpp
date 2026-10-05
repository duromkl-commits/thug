/*****************************************************************************
**  THUG-Vita — décodeur Xbox ADPCM partagé                                 **
**  Code/Core/Vita/adpcm.cpp                                                **
**                                                                          **
**  Validé au bit près : compilé sur Mac et comparé à la référence Python   **
**  vita/tools/audio_check.py sur 384 000 échantillons, zéro différence.    **
**  Ne pas modifier sans rejouer cette comparaison — une erreur ici est     **
**  inaudible à l'oreille sur un extrait court et se paie en dérive.        **
*****************************************************************************/

#include "adpcm.h"

namespace VitaAdpcm
{

// Tables IMA ADPCM standard.
static const int STEP[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
	45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
	209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
	796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
	2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
	7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
	20350, 22385, 24623, 27086, 29794, 32767
};

static const int INDEX_ADJ[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

#define MAX_CANAUX	2

struct SEtat
{
	int	predicteur;
	int	index;
};

static inline short decode_nibble( SEtat *p_etat, int nibble )
{
	const int pas = STEP[p_etat->index];

	int delta = pas >> 3;
	if( nibble & 1 )	delta += pas >> 2;
	if( nibble & 2 )	delta += pas >> 1;
	if( nibble & 4 )	delta += pas;

	if( nibble & 8 )	p_etat->predicteur -= delta;
	else				p_etat->predicteur += delta;

	if( p_etat->predicteur >  32767 )	p_etat->predicteur =  32767;
	if( p_etat->predicteur < -32768 )	p_etat->predicteur = -32768;

	p_etat->index += INDEX_ADJ[nibble & 7];
	if( p_etat->index <  0 )	p_etat->index = 0;
	if( p_etat->index > 88 )	p_etat->index = 88;

	return (short)p_etat->predicteur;
}


void DecodeBloc( const unsigned char *p_src, short *p_dst,
                 int canaux, int block_align )
{
	if( canaux > MAX_CANAUX )
		canaux = MAX_CANAUX;

	SEtat etat[MAX_CANAUX];

	for( int c = 0; c < canaux; ++c )
	{
		etat[c].predicteur = (short)( p_src[c * 4] | ( p_src[c * 4 + 1] << 8 ));
		etat[c].index      = p_src[c * 4 + 2];
		if( etat[c].index > 88 )
			etat[c].index = 88;
		// Le prédicteur n'est PAS émis : voir la note d'en-tête d'adpcm.h.
	}

	const unsigned char *p_corps = p_src + canaux * 4;

	// Le corps alterne des groupes de 4 octets par canal ; chaque groupe donne
	// 8 échantillons (2 nibbles par octet), poids faible d'abord. En mono la
	// boucle sur les canaux ne fait qu'un tour et le parcours est séquentiel.
	const int groupes = ( block_align / canaux - 4 ) / 4;
	int ech = 0;

	for( int groupe = 0; groupe < groupes; ++groupe )
	{
		for( int c = 0; c < canaux; ++c )
		{
			const unsigned char *p_g = p_corps + ( groupe * canaux + c ) * 4;
			for( int o = 0; o < 4; ++o )
			{
				const int e = ech + o * 2;
				p_dst[( e     ) * canaux + c] =
					decode_nibble( &etat[c], p_g[o] & 0x0F );
				p_dst[( e + 1 ) * canaux + c] =
					decode_nibble( &etat[c], p_g[o] >> 4 );
			}
		}
		ech += 8;
	}
}

} // namespace VitaAdpcm
