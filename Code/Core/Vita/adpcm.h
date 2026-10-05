///////////////////////////////////////////////////////////////////////////////
// adpcm.h — décodeur Xbox ADPCM partagé
//
// Les assets audio de THUG (ISO Xbox) sont tous au même format, que ce soit la
// musique ou les effets — seuls le nombre de canaux et la fréquence changent :
//
//   music_pcm.wad   stéréo, 48000 Hz, blockAlign 72
//   sounds\pcm\*    mono,   44100 Hz, blockAlign 36
//
// Un seul décodeur sert donc les deux. Il vivait d'abord dans le backend
// musique ; l'extraire évite la copie et, surtout, garde UNE implémentation à
// valider — celle-ci l'est au bit près contre vita/tools/audio_check.py.
//
// Xbox ADPCM est une variante d'IMA ADPCM. Par bloc et par canal : 4 octets
// d'en-tête (prédicteur int16, index de pas uint8, un octet réservé) puis les
// données par groupes de 4 octets alternant les canaux.

#ifndef __CORE_VITA_ADPCM_H__
#define __CORE_VITA_ADPCM_H__

namespace VitaAdpcm
{

// Échantillons rendus par bloc et par canal. Vaut 64 pour les deux formats du
// jeu. ATTENTION : le prédicteur de l'en-tête initialise l'état SANS être
// émis — le fichier l'impose, son avgBytesPerSec vaut 48000/64*72 = 54000
// pour la musique et 44100/64*36 = 24806 pour les effets. Compter 65 ferait
// dériver la sortie d'un échantillon par bloc.
inline int EchantillonsParBloc( int block_align, int canaux )
{
	return ( block_align / canaux - 4 ) * 2;
}

// Décode UN bloc vers des échantillons 16 bits entrelacés.
// p_dst doit accueillir EchantillonsParBloc() * canaux valeurs.
void DecodeBloc( const unsigned char *p_src, short *p_dst,
                 int canaux, int block_align );

} // namespace VitaAdpcm

#endif // __CORE_VITA_ADPCM_H__
