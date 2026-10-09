/*****************************************************************************
**  THUG desktop -- custom soundtrack                                       **
**  desktop/src/custom_music.cpp                                            **
**                                                                          **
**  Songs dropped in "custom_music" next to thug.exe (MP3, OGG, FLAC, WAV, **
**  subfolders too) join the game's own playlist:                          **
**                                                                          **
**  - Their names come from the files' tags: album artist (artist when     **
**    there is none) and title; "Artist - Title" file names otherwise.     **
**  - The game's playlist lives in its scripts: the playlist_tracks array  **
**    (band, track_title, genre, path) feeds LoadPermSongs, the playlist   **
**    menu, shuffle and the now-playing text. The songs are appended to    **
**    that array as genre 3 while the .qb loads, so every one of those     **
**    works on them unchanged.                                             **
**  - The playlist menu has three genres written into its scripts; a       **
**    fourth, "Custom", is patched into create_playlist_menu,              **
**    playlist_hmenu_add_item (its width; the four headings shrink to fit  **
**    the bar) and update_genre_checks.                                    **
**  - The engine saves the playlist as 128 on/off bits: the game's own     **
**    songs plus at most as many custom songs as fit in 128.               **
**                                                                          **
**  The patches work on script tokens (Gel/Scripting/tokens.h). Each one   **
**  checks the exact shape it expects and leaves the script alone, with a  **
**  line in thug.log, when it doesn't find it.                             **
*****************************************************************************/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <algorithm>

#include "custom_music.h"
#include "vita_log.h"

namespace
{

#ifdef _WIN32
typedef std::wstring Chemin;
#else
typedef std::string Chemin;
#endif

struct Piste
{
	Chemin      chemin;
	std::string cle;		// relative path, lower case: the order
	std::string bande;		// album artist (or artist)
	std::string titre;
};

static std::vector<Piste> s_pistes;			// every file found
static int  s_nb_actives = -1;				// how many joined playlist_tracks
static bool s_scanne = false;

const int MAX_PISTES_JEU = 128;				// the engine's on/off bits
const int GENRE_PERSO = 3;

// --- text --------------------------------------------------------------------

// UTF-8 -> what the game's fonts draw: printable ASCII. Accented Latin
// letters lose their accent, typographic quotes and dashes become plain
// ones, anything else becomes nothing. Backslashes go too: in game text
// they start button icons ("\b7").
static std::string vers_jeu( const std::string &u )
{
	static const char *latin =	// U+00C0 - U+00FF
		"AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
	std::string o;
	for( size_t i = 0; i < u.size(); )
	{
		unsigned c = (unsigned char)u[i];
		unsigned cp; int n;
		if( c < 0x80 )			{ cp = c; n = 1; }
		else if(( c >> 5 ) == 6 )	{ cp = c & 0x1f; n = 2; }
		else if(( c >> 4 ) == 14 )	{ cp = c & 0x0f; n = 3; }
		else if(( c >> 3 ) == 30 )	{ cp = c & 0x07; n = 4; }
		else					{ ++i; continue; }
		if( i + n > u.size()) break;
		for( int k = 1; k < n; ++k )
			cp = ( cp << 6 ) | ((unsigned char)u[i + k] & 0x3f );
		i += n;
		if( cp >= 0x20 && cp < 0x7f && cp != '\\' )	o += (char)cp;
		else if( cp == '\t' )						o += ' ';
		else if( cp >= 0xc0 && cp <= 0xff )			o += latin[cp - 0xc0];
		else if( cp == 0x2018 || cp == 0x2019 || cp == 0xb4 )	o += '\'';
		else if( cp == 0x201c || cp == 0x201d )		o += '\'';
		else if( cp == 0x2013 || cp == 0x2014 )		o += '-';
		else if( cp == 0x2026 )						o += "...";
		else if( cp == 0xdf )						o += "ss";
	}
	// Trim, squeeze spaces.
	std::string r;
	for( size_t i = 0; i < o.size(); ++i )
		if( o[i] != ' ' || ( !r.empty() && r[r.size() - 1] != ' ' ))
			r += o[i];
	while( !r.empty() && r[r.size() - 1] == ' ' )
		r.erase( r.size() - 1 );
	return r;
}

// Shortens to at most n characters, at a word break, with "...".
static void coupe( std::string &s, int n )
{
	if((int)s.size() <= n ) return;
	s = s.substr( 0, n - 3 );
	size_t esp = s.rfind( ' ' );
	if( esp != std::string::npos && esp > s.size() / 2 ) s = s.substr( 0, esp );
	while( !s.empty() && ( s[s.size() - 1] == ' ' || s[s.size() - 1] == ',' || s[s.size() - 1] == '-' )) s.erase( s.size() - 1 );
	s += "...";
}

static std::string latin1_vers_utf8( const unsigned char *p, size_t n )
{
	std::string o;
	for( size_t i = 0; i < n && p[i]; ++i )
	{
		if( p[i] < 0x80 ) o += (char)p[i];
		else { o += (char)( 0xc0 | ( p[i] >> 6 )); o += (char)( 0x80 | ( p[i] & 0x3f )); }
	}
	return o;
}

static void ajoute_utf8( std::string &o, unsigned cp )
{
	if( cp < 0x80 ) o += (char)cp;
	else if( cp < 0x800 ) { o += (char)( 0xc0 | ( cp >> 6 )); o += (char)( 0x80 | ( cp & 0x3f )); }
	else if( cp < 0x10000 ) { o += (char)( 0xe0 | ( cp >> 12 )); o += (char)( 0x80 | (( cp >> 6 ) & 0x3f )); o += (char)( 0x80 | ( cp & 0x3f )); }
	else { o += (char)( 0xf0 | ( cp >> 18 )); o += (char)( 0x80 | (( cp >> 12 ) & 0x3f )); o += (char)( 0x80 | (( cp >> 6 ) & 0x3f )); o += (char)( 0x80 | ( cp & 0x3f )); }
}

static std::string utf16_vers_utf8( const unsigned char *p, size_t n, bool be )
{
	std::string o;
	for( size_t i = 0; i + 1 < n; i += 2 )
	{
		unsigned u = be ? ( p[i] << 8 | p[i + 1] ) : ( p[i + 1] << 8 | p[i] );
		if( !u ) break;
		if( u >= 0xd800 && u < 0xdc00 && i + 3 < n )
		{
			unsigned v = be ? ( p[i + 2] << 8 | p[i + 3] ) : ( p[i + 3] << 8 | p[i + 2] );
			u = 0x10000 + (( u - 0xd800 ) << 10 ) + ( v - 0xdc00 );
			i += 2;
		}
		ajoute_utf8( o, u );
	}
	return o;
}

// --- tags ----------------------------------------------------------------------

struct Tags { std::string album_artiste, artiste, titre; };

// ID3v2 text frame: encoding byte, then text. Only the first value of a
// multi-value frame (v2.4 separates them with NUL).
static std::string texte_id3( const unsigned char *p, size_t n )
{
	if( n < 1 ) return "";
	const unsigned char e = p[0];
	++p; --n;
	if( e == 0 ) return latin1_vers_utf8( p, n );
	if( e == 3 ) { size_t k = 0; while( k < n && p[k] ) ++k; return std::string((const char *)p, k ); }
	if( e == 1 && n >= 2 )
	{
		const bool be = ( p[0] == 0xfe && p[1] == 0xff );
		const bool bom = be || ( p[0] == 0xff && p[1] == 0xfe );
		return bom ? utf16_vers_utf8( p + 2, n - 2, be ) : utf16_vers_utf8( p, n, false );
	}
	if( e == 2 ) return utf16_vers_utf8( p, n, true );
	return "";
}

static uint32_t synchsafe( const unsigned char *p )
{
	return ( p[0] & 0x7f ) << 21 | ( p[1] & 0x7f ) << 14 | ( p[2] & 0x7f ) << 7 | ( p[3] & 0x7f );
}

static void lit_id3v2( const std::vector<unsigned char> &b, Tags &t )
{
	if( b.size() < 10 || memcmp( &b[0], "ID3", 3 )) return;
	const int ver = b[3];
	const unsigned flags = b[5];
	size_t fin = 10 + synchsafe( &b[6] );
	if( fin > b.size()) fin = b.size();
	if( flags & 0x80 ) return;		// whole-tag unsynchronisation: rare, skipped
	size_t i = 10;
	if(( flags & 0x40 ) && ver >= 3 && i + 4 <= fin )	// extended header
		i += ( ver == 4 ) ? synchsafe( &b[i] ) : ( 4 + ( b[i] << 24 | b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3] ));
	while( true )
	{
		std::string id; size_t taille, entete;
		if( ver == 2 )
		{
			if( i + 6 > fin ) break;
			id.assign((const char *)&b[i], 3 );
			taille = b[i + 3] << 16 | b[i + 4] << 8 | b[i + 5];
			entete = 6;
		}
		else
		{
			if( i + 10 > fin ) break;
			id.assign((const char *)&b[i], 4 );
			taille = ( ver == 4 ) ? synchsafe( &b[i + 4] ) : (size_t)( b[i + 4] << 24 | b[i + 5] << 16 | b[i + 6] << 8 | b[i + 7] );
			entete = 10;
		}
		if( id[0] == 0 || i + entete + taille > fin ) break;
		const unsigned char *d = &b[i + entete];
		if( id == "TPE2" || id == "TP2" )		t.album_artiste = texte_id3( d, taille );
		else if( id == "TPE1" || id == "TP1" )	t.artiste = texte_id3( d, taille );
		else if( id == "TIT2" || id == "TT2" )	t.titre = texte_id3( d, taille );
		i += entete + taille;
	}
}

static void lit_id3v1( const std::vector<unsigned char> &fin, Tags &t )
{
	if( fin.size() < 128 ) return;
	const unsigned char *p = &fin[fin.size() - 128];
	if( memcmp( p, "TAG", 3 )) return;
	std::string titre = latin1_vers_utf8( p + 3, 30 ), artiste = latin1_vers_utf8( p + 33, 30 );
	if( t.titre.empty())   t.titre = titre;
	if( t.artiste.empty()) t.artiste = artiste;
}

// Vorbis comments (OGG and FLAC): little-endian lengths, KEY=value.
static void lit_vorbis( const unsigned char *p, size_t n, Tags &t )
{
	size_t i = 0;
	if( i + 4 > n ) return;
	uint32_t l = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
	i += 4 + l;
	if( i + 4 > n ) return;
	uint32_t nb = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
	i += 4;
	for( uint32_t k = 0; k < nb && i + 4 <= n; ++k )
	{
		l = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
		i += 4;
		if( l > n - i ) break;
		std::string c((const char *)p + i, l );
		i += l;
		size_t eg = c.find( '=' );
		if( eg == std::string::npos ) continue;
		std::string cle = c.substr( 0, eg );
		for( size_t j = 0; j < cle.size(); ++j ) cle[j] = (char)toupper((unsigned char)cle[j] );
		std::string v = c.substr( eg + 1 );
		if(( cle == "ALBUMARTIST" || cle == "ALBUM ARTIST" || cle == "ALBUM_ARTIST" ) && t.album_artiste.empty())
			t.album_artiste = v;
		else if( cle == "ARTIST" && t.artiste.empty())	t.artiste = v;
		else if( cle == "TITLE" && t.titre.empty())		t.titre = v;
	}
}

static void lit_flac( const std::vector<unsigned char> &b, Tags &t )
{
	if( b.size() < 8 || memcmp( &b[0], "fLaC", 4 )) return;
	size_t i = 4;
	while( i + 4 <= b.size())
	{
		const int type = b[i] & 0x7f;
		const bool dernier = ( b[i] & 0x80 ) != 0;
		const size_t taille = b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3];
		i += 4;
		if( type == 4 )
		{
			lit_vorbis( &b[i], std::min( taille, b.size() - i ), t );
			return;
		}
		i += taille;
		if( dernier ) break;
	}
}

static void lit_ogg( const std::vector<unsigned char> &b, Tags &t )
{
	static const unsigned char marque[7] = { 3, 'v', 'o', 'r', 'b', 'i', 's' };
	for( size_t i = 0; i + 7 <= b.size(); ++i )
		if( !memcmp( &b[i], marque, 7 ))
		{
			lit_vorbis( &b[i + 7], b.size() - i - 7, t );
			return;
		}
}

static void lit_wav( const std::vector<unsigned char> &b, Tags &t )
{
	if( b.size() < 12 || memcmp( &b[0], "RIFF", 4 ) || memcmp( &b[8], "WAVE", 4 )) return;
	size_t i = 12;
	while( i + 8 <= b.size())
	{
		const size_t taille = b[i + 4] | b[i + 5] << 8 | b[i + 6] << 16 | (size_t)b[i + 7] << 24;
		if( !memcmp( &b[i], "LIST", 4 ) && i + 12 <= b.size() && !memcmp( &b[i + 8], "INFO", 4 ))
		{
			size_t j = i + 12, f = std::min( i + 8 + taille, b.size());
			while( j + 8 <= f )
			{
				const size_t l = b[j + 4] | b[j + 5] << 8 | b[j + 6] << 16 | (size_t)b[j + 7] << 24;
				if( j + 8 + l > f ) break;
				std::string v = latin1_vers_utf8( &b[j + 8], l );
				if( !memcmp( &b[j], "IART", 4 )) t.artiste = v;
				else if( !memcmp( &b[j], "INAM", 4 )) t.titre = v;
				j += 8 + l + ( l & 1 );
			}
			return;
		}
		i += 8 + taille + ( taille & 1 );
	}
}

// --- files -------------------------------------------------------------------------

static FILE *ouvre( const Chemin &c )
{
#ifdef _WIN32
	return _wfopen( c.c_str(), L"rb" );
#else
	return fopen( c.c_str(), "rb" );
#endif
}

static void lit_debut_et_fin( const Chemin &c, std::vector<unsigned char> &debut, std::vector<unsigned char> &fin )
{
	FILE *f = ouvre( c );
	if( !f ) return;
	// The tags sit at the start; ID3 tags with cover art can be big, so the
	// first read follows the ID3 size (capped).
	debut.resize( 10 );
	size_t n = fread( &debut[0], 1, 10, f );
	size_t voulu = 512 * 1024;
	if( n == 10 && !memcmp( &debut[0], "ID3", 3 ))
		voulu = std::min<size_t>( 10 + synchsafe( &debut[6] ), 16 * 1024 * 1024 );
	debut.resize( std::max<size_t>( voulu, 10 ));
	n += fread( &debut[10], 1, debut.size() - 10, f );
	debut.resize( n );
	if( fseek( f, -128, SEEK_END ) == 0 )
	{
		fin.resize( 128 );
		fin.resize( fread( &fin[0], 1, 128, f ));
	}
	fclose( f );
}

static std::string extension( const std::string &nom )
{
	size_t p = nom.rfind( '.' );
	std::string e = ( p == std::string::npos ) ? "" : nom.substr( p + 1 );
	for( size_t i = 0; i < e.size(); ++i ) e[i] = (char)tolower((unsigned char)e[i] );
	return e;
}

static void ajoute_fichier( const Chemin &chemin, const std::string &relatif )
{
	const std::string ext = extension( relatif );
	if( ext != "mp3" && ext != "ogg" && ext != "flac" && ext != "wav" )
		return;
	std::vector<unsigned char> debut, fin;
	lit_debut_et_fin( chemin, debut, fin );
	Tags t;
	if( ext == "mp3" )			{ lit_id3v2( debut, t ); lit_id3v1( fin, t ); }
	else if( ext == "flac" )	lit_flac( debut, t );
	else if( ext == "ogg" )		lit_ogg( debut, t );
	else						lit_wav( debut, t );

	// File name: "Artist - Title.mp3", or the whole name as the title.
	std::string base = relatif.substr( relatif.find_last_of( "/\\" ) == std::string::npos ? 0 : relatif.find_last_of( "/\\" ) + 1 );
	base = base.substr( 0, base.rfind( '.' ));
	std::string f_artiste, f_titre = base;
	size_t tiret = base.find( " - " );
	if( tiret != std::string::npos )
	{
		f_artiste = base.substr( 0, tiret );
		f_titre = base.substr( tiret + 3 );
	}

	Piste p;
	p.chemin = chemin;
	p.cle = relatif;
	for( size_t i = 0; i < p.cle.size(); ++i ) p.cle[i] = (char)tolower((unsigned char)p.cle[i] );
	p.bande = vers_jeu( t.album_artiste );
	if( p.bande.empty()) p.bande = vers_jeu( t.artiste );
	if( p.bande.empty()) p.bande = vers_jeu( f_artiste );
	if( p.bande.empty()) p.bande = "Unknown Artist";
	p.titre = vers_jeu( t.titre );
	if( p.titre.empty()) p.titre = vers_jeu( f_titre );
	if( p.titre.empty()) p.titre = "Untitled";
	// The engine's title buffer holds "band: title" in 100 bytes (99 + NUL).
	coupe( p.bande, 44 );
	coupe( p.titre, 97 - (int)p.bande.size());
	s_pistes.push_back( p );
}

#ifdef _WIN32
static std::string utf8( const std::wstring &w )
{
	if( w.empty()) return "";
	int n = WideCharToMultiByte( CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL );
	std::string s( n, 0 );
	WideCharToMultiByte( CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL );
	return s;
}

static void parcourt( const std::wstring &dossier, const std::string &relatif, int profondeur )
{
	WIN32_FIND_DATAW d;
	HANDLE h = FindFirstFileW(( dossier + L"\\*" ).c_str(), &d );
	if( h == INVALID_HANDLE_VALUE ) return;
	do
	{
		std::wstring nom = d.cFileName;
		if( nom == L"." || nom == L".." ) continue;
		std::string rel = relatif.empty() ? utf8( nom ) : relatif + "/" + utf8( nom );
		if( d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
		{
			if( profondeur < 4 ) parcourt( dossier + L"\\" + nom, rel, profondeur + 1 );
		}
		else
			ajoute_fichier( dossier + L"\\" + nom, rel );
	}
	while( FindNextFileW( h, &d ));
	FindClose( h );
}
#else
static void parcourt( const std::string &dossier, const std::string &relatif, int profondeur )
{
	DIR *d = opendir( dossier.c_str());
	if( !d ) return;
	while( struct dirent *e = readdir( d ))
	{
		std::string nom = e->d_name;
		if( nom == "." || nom == ".." ) continue;
		std::string plein = dossier + "/" + nom;
		std::string rel = relatif.empty() ? nom : relatif + "/" + nom;
		struct stat st;
		if( stat( plein.c_str(), &st )) continue;
		if( S_ISDIR( st.st_mode ))
		{
			if( profondeur < 4 ) parcourt( plein, rel, profondeur + 1 );
		}
		else
			ajoute_fichier( plein, rel );
	}
	closedir( d );
}
#endif

static bool par_cle( const Piste &a, const Piste &b ) { return a.cle < b.cle; }

static void scanne( void )
{
	if( s_scanne ) return;
	s_scanne = true;
	char *base = SDL_GetBasePath();
	std::string dossier = std::string( base ? base : "" ) + "custom_music";
	SDL_free( base );
#ifdef _WIN32
	int n = MultiByteToWideChar( CP_UTF8, 0, dossier.c_str(), -1, NULL, 0 );
	std::wstring w( n > 0 ? n - 1 : 0, 0 );
	if( n > 1 ) MultiByteToWideChar( CP_UTF8, 0, dossier.c_str(), -1, &w[0], n );
	CreateDirectoryW( w.c_str(), NULL );	// there to be found
	parcourt( w, "", 0 );
#else
	mkdir( dossier.c_str(), 0755 );
	parcourt( dossier, "", 0 );
#endif
	std::sort( s_pistes.begin(), s_pistes.end(), par_cle );
	VLOG( "MUS", "custom music: %d songs in %s", (int)s_pistes.size(), dossier.c_str());
}

// --- script tokens (Gel/Scripting/tokens.h) ----------------------------------------

enum
{
	T_FIN = 0, T_EOL = 1, T_EOLNUM = 2, T_STRUCT = 3, T_FSTRUCT = 4, T_TAB = 5, T_FTAB = 6, T_EGAL = 7,
	T_NOM = 22, T_ENTIER = 23, T_HEX = 24, T_FLOTTANT = 26, T_CHAINE = 27, T_CHAINE_LOC = 28,
	T_VECTEUR = 30, T_PAIRE = 31, T_REPEAT = 33, T_SCRIPT = 35, T_ENDSCRIPT = 36,
	T_NOMCRC = 43, T_JUMP = 46, T_RANDOM = 47, T_RANDOM2 = 55, T_CASE = 62,
	T_RANDOM_NR = 64, T_RANDOM_P = 65, T_RT_C = 67, T_RT_M = 68
};

static uint32_t crc_table[256];

static uint32_t crc( const char *s )		// Crc::GenerateCRCFromString
{
	if( !crc_table[1] )
		for( uint32_t i = 0; i < 256; ++i )
		{
			uint32_t c = i;
			for( int k = 0; k < 8; ++k ) c = ( c & 1 ) ? 0xedb88320u ^ ( c >> 1 ) : c >> 1;
			crc_table[i] = c;
		}
	uint32_t r = 0xffffffffu;
	for( ; *s; ++s )
	{
		char c = *s;
		if( c >= 'A' && c <= 'Z' ) c = c - 'A' + 'a';
		if( c == '/' ) c = '\\';
		r = crc_table[( r ^ (unsigned char)c ) & 0xff] ^ ( r >> 8 );
	}
	return r;
}

static uint32_t lit32( const unsigned char *p ) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Size of the token at p (SkipToken); 0 if unknown.
static size_t taille_jeton( const unsigned char *p )
{
	switch( *p )
	{
		case T_NOM: case T_ENTIER: case T_HEX: case T_FLOTTANT: case T_EOLNUM:
		case T_JUMP: case T_RT_C: case T_RT_M:
			return 5;
		case T_VECTEUR: return 13;
		case T_PAIRE: return 9;
		case T_CHAINE: case T_CHAINE_LOC: return 5 + lit32( p + 1 );
		case T_NOMCRC: { size_t n = 5; while( p[n] ) ++n; return n + 1; }
		case T_RANDOM: case T_RANDOM2: case T_RANDOM_NR: case T_RANDOM_P:
			return 5 + 6 * lit32( p + 1 );
		default:
			return ( *p <= 66 ) ? 1 : 0;
	}
}

struct Jeton { size_t pos, taille; };

static bool decoupe( const unsigned char *qb, std::vector<Jeton> &j )
{
	size_t i = 0;
	while( true )
	{
		const size_t t = taille_jeton( qb + i );
		if( !t ) return false;
		j.push_back( Jeton{ i, t } );
		if( qb[i] == T_FIN ) return true;
		i += t;
		if( i > 64 * 1024 * 1024 ) return false;
	}
}

// --- token writer ---------------------------------------------------------------------

struct Ecrit
{
	std::vector<unsigned char> o;
	void octet( unsigned char b ) { o.push_back( b ); }
	void u32( uint32_t v ) { for( int k = 0; k < 4; ++k ) o.push_back((unsigned char)( v >> ( 8 * k ))); }
	void f32( float f ) { uint32_t v; memcpy( &v, &f, 4 ); u32( v ); }
	void nom( const char *n ) { octet( T_NOM ); u32( crc( n )); }
	void entier( int v ) { octet( T_ENTIER ); u32((uint32_t)v ); }
	void chaine( const std::string &s ) { octet( T_CHAINE ); u32((uint32_t)s.size() + 1 ); for( size_t i = 0; i < s.size(); ++i ) octet((unsigned char)s[i] ); octet( 0 ); }
	void paire( float a, float b ) { octet( T_PAIRE ); f32( a ); f32( b ); }
	void eol() { octet( T_EOL ); }
	void champ_chaine( const char *n, const std::string &s ) { nom( n ); octet( T_EGAL ); chaine( s ); }
	void champ_entier( const char *n, int v ) { nom( n ); octet( T_EGAL ); entier( v ); }
};

// Finds the tokens of "script <name> ... endscript": [debut, fin).
static bool trouve_script( const unsigned char *qb, const std::vector<Jeton> &j, uint32_t nom, size_t &debut, size_t &fin )
{
	for( size_t k = 0; k + 1 < j.size(); ++k )
		if( qb[j[k].pos] == T_SCRIPT && qb[j[k + 1].pos] == T_NOM && lit32( qb + j[k + 1].pos + 1 ) == nom )
		{
			for( size_t e = k + 2; e < j.size(); ++e )
				if( qb[j[e].pos] == T_ENDSCRIPT )
				{
					debut = k; fin = e + 1;
					return true;
				}
		}
	return false;
}

// Skips end-of-line tokens from k.
static size_t sans_eol( const unsigned char *qb, const std::vector<Jeton> &j, size_t k )
{
	while( k < j.size() && ( qb[j[k].pos] == T_EOL || qb[j[k].pos] == T_EOLNUM )) ++k;
	return k;
}

static bool est_nom( const unsigned char *qb, const Jeton &t, uint32_t n ) { return qb[t.pos] == T_NOM && lit32( qb + t.pos + 1 ) == n; }
static bool est_entier( const unsigned char *qb, const Jeton &t, int v ) { return qb[t.pos] == T_ENTIER && (int)lit32( qb + t.pos + 1 ) == v; }

// A script must not hold jumps (random blocks): inserting bytes would move
// their targets.
static bool sans_sauts( const unsigned char *qb, const std::vector<Jeton> &j, size_t d, size_t f )
{
	for( size_t k = d; k < f; ++k )
	{
		const unsigned char c = qb[j[k].pos];
		if( c == T_JUMP || c == T_RANDOM || c == T_RANDOM2 || c == T_RANDOM_NR || c == T_RANDOM_P )
			return false;
	}
	return true;
}

// Edits: at byte position pos, drop 'enleve' bytes and put 'ajout' in.
struct Modif { size_t pos, enleve; std::vector<unsigned char> ajout; };

static bool par_pos( const Modif &a, const Modif &b ) { return a.pos < b.pos; }

static unsigned char *applique( const unsigned char *qb, size_t taille, std::vector<Modif> &m )
{
	std::sort( m.begin(), m.end(), par_pos );
	std::vector<unsigned char> o;
	size_t i = 0;
	for( size_t k = 0; k < m.size(); ++k )
	{
		o.insert( o.end(), qb + i, qb + m[k].pos );
		o.insert( o.end(), m[k].ajout.begin(), m[k].ajout.end());
		i = m[k].pos + m[k].enleve;
	}
	o.insert( o.end(), qb + i, qb + taille );
	unsigned char *r = (unsigned char *)malloc( o.size());
	if( r ) memcpy( r, &o[0], o.size());
	return r;
}

// --- the patches --------------------------------------------------------------------------

// playlist_tracks = [ {...} ... ]: the custom songs go in before ']'.
static void patch_liste( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n_liste = crc( "playlist_tracks" );
	for( size_t k = 0; k + 2 < j.size(); ++k )
	{
		if( !est_nom( qb, j[k], n_liste ) || qb[j[k + 1].pos] != T_EGAL ) continue;
		size_t a = sans_eol( qb, j, k + 2 );
		if( a >= j.size() || qb[j[a].pos] != T_TAB ) continue;
		// Count the top-level entries, find the closing bracket.
		int niveau = 0, nb = 0;
		size_t e = a + 1;
		for( ; e < j.size(); ++e )
		{
			const unsigned char c = qb[j[e].pos];
			if( c == T_STRUCT || c == T_TAB ) { if( niveau == 0 && c == T_STRUCT ) ++nb; ++niveau; }
			else if( c == T_FSTRUCT ) --niveau;
			else if( c == T_FTAB ) { if( niveau == 0 ) break; --niveau; }
			else if( c == T_FIN ) return;
		}
		if( e >= j.size()) return;
		const int place = MAX_PISTES_JEU - nb;
		const int n = std::min((int)s_pistes.size(), std::max( place, 0 ));
		s_nb_actives = n;
		if( n < (int)s_pistes.size())
			VLOG( "MUS", "custom music: room for %d of %d songs (the game has %d, the playlist holds %d)",
			      n, (int)s_pistes.size(), nb, MAX_PISTES_JEU );
		Ecrit w;
		for( int i = 0; i < n; ++i )
		{
			char chemin[64];
			snprintf( chemin, sizeof( chemin ), "music\\vag\\songs\\THUGCUSTOM%03d", i );
			w.eol();
			w.octet( T_STRUCT );
			w.champ_chaine( "band", s_pistes[i].bande );
			w.champ_chaine( "track_title", s_pistes[i].titre );
			w.champ_entier( "genre", GENRE_PERSO );
			w.champ_chaine( "path", chemin );
			w.octet( T_FSTRUCT );
		}
		w.eol();
		m.push_back( Modif{ j[e].pos, 0, w.o } );
		VLOG( "MUS", "custom music: %d songs added after the game's %d", n, nb );
		return;
	}
}

// The playlist menu's genre bar: Punk, Hip Hop, Rock/Other, + Custom.
static void patch_menu( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n_item = crc( "playlist_hmenu_add_item" ), n_genre = crc( "genre" ),
	               n_dims = crc( "dims" ), n_scale = crc( "scale" ), n_maj = crc( "update_genre_checks" ),
	               n_menu = crc( "create_playlist_menu" );
	size_t d1, f1, d2, f2, d3, f3;
	if( !trouve_script( qb, j, n_menu, d1, f1 ))
		return;		// not this file
	if( !trouve_script( qb, j, n_item, d2, f2 ) || !trouve_script( qb, j, n_maj, d3, f3 ))
	{
		VLOG( "MUS", "custom music: playlist scripts not as expected, no Custom genre heading" );
		return;
	}
	std::vector<Modif> mm;

	// 1. create_playlist_menu: after "playlist_hmenu_add_item { ... genre = 2 }".
	bool ok1 = false;
	for( size_t k = d1; k + 1 < f1 && !ok1; ++k )
	{
		if( !est_nom( qb, j[k], n_item ) || qb[j[k + 1].pos] != T_STRUCT ) continue;
		size_t e = k + 2; bool g2 = false;
		for( ; e < f1 && qb[j[e].pos] != T_FSTRUCT; ++e )
			if( e + 2 < f1 && est_nom( qb, j[e], n_genre ) && qb[j[e + 1].pos] == T_EGAL && est_entier( qb, j[e + 2], 2 ))
				g2 = true;
		if( !g2 || e >= f1 ) continue;
		Ecrit w;
		w.eol();
		w.nom( "playlist_hmenu_add_item" ); w.octet( T_STRUCT );
		w.champ_chaine( "text", "Custom" ); w.champ_entier( "genre", GENRE_PERSO );
		w.octet( T_FSTRUCT );
		mm.push_back( Modif{ j[e].pos + j[e].taille, 0, w.o } );
		ok1 = true;
	}

	// 2. playlist_hmenu_add_item: a "case 3" width, and the four headings
	// narrowed to 70% of their text (+ the checkbox) so they fit the bar the
	// three filled. Text scale 1.5 -> 1.05.
	bool ok2 = false;
	int largeurs = 0;
	for( size_t k = d2; k < f2; ++k )
	{
		if( est_nom( qb, j[k], n_dims ) && k + 2 < f2 && qb[j[k + 1].pos] == T_EGAL && qb[j[k + 2].pos] == T_PAIRE )
		{
			float a, b;
			memcpy( &a, qb + j[k + 2].pos + 1, 4 );
			memcpy( &b, qb + j[k + 2].pos + 5, 4 );
			Ecrit w; w.paire( 35.0f + ( a - 35.0f ) * 0.7f, b );
			mm.push_back( Modif{ j[k + 2].pos, j[k + 2].taille, w.o } );
			++largeurs;
		}
		if( est_nom( qb, j[k], n_scale ) && k + 2 < f2 && qb[j[k + 1].pos] == T_EGAL && qb[j[k + 2].pos] == T_FLOTTANT )
		{
			float s; memcpy( &s, qb + j[k + 2].pos + 1, 4 );
			if( s == 1.5f )
			{
				Ecrit w; w.octet( T_FLOTTANT ); w.f32( 1.05f );
				mm.push_back( Modif{ j[k + 2].pos, j[k + 2].taille, w.o } );
			}
		}
		if( qb[j[k].pos] == T_CASE && !ok2 )
		{
			size_t v = sans_eol( qb, j, k + 1 );
			if( v < f2 && est_entier( qb, j[v], 2 ))
			{
				size_t n = sans_eol( qb, j, v + 1 );
				if( n + 2 < f2 && est_nom( qb, j[n], n_dims ) && qb[j[n + 1].pos] == T_EGAL && qb[j[n + 2].pos] == T_PAIRE )
				{
					// "Custom" is 6 letters: between Punk (4) and Hip Hop (7).
					Ecrit w;
					w.eol(); w.octet( T_CASE ); w.entier( GENRE_PERSO );
					w.eol(); w.nom( "dims" ); w.octet( T_EGAL ); w.paire( 35.0f + 110.0f * 0.7f, 50.0f );
					mm.push_back( Modif{ j[n + 2].pos + j[n + 2].taille, 0, w.o } );
					ok2 = true;
				}
			}
		}
	}

	// 3. update_genre_checks: "repeat 3" -> "repeat 4".
	bool ok3 = false;
	for( size_t k = d3; k + 1 < f3 && !ok3; ++k )
		if( qb[j[k].pos] == T_REPEAT && est_entier( qb, j[k + 1], 3 ))
		{
			Ecrit w; w.entier( 4 );
			mm.push_back( Modif{ j[k + 1].pos, j[k + 1].taille, w.o } );
			ok3 = true;
		}

	if( !ok1 || !ok2 || !ok3 || largeurs != 3 || !sans_sauts( qb, j, d1, f1 ) || !sans_sauts( qb, j, d2, f2 ))
	{
		VLOG( "MUS", "custom music: playlist menu not as expected (%d %d %d %d), no Custom genre heading",
		      ok1, ok2, ok3, largeurs );
		return;
	}
	m.insert( m.end(), mm.begin(), mm.end());
	VLOG( "MUS", "custom music: Custom genre added to the playlist menu" );
}

} // namespace

extern "C" unsigned char *custom_music_patch_qb( const char *file_name, const unsigned char *qb )
{
	if( !qb ) return NULL;
	scanne();
	if( s_pistes.empty()) return NULL;
	std::vector<Jeton> j;
	if( !decoupe( qb, j ))
	{
		VLOG( "MUS", "custom music: could not read %s", file_name ? file_name : "?" );
		return NULL;
	}
	std::vector<Modif> m;
	patch_liste( qb, j, m );
	patch_menu( qb, j, m );
	if( m.empty()) return NULL;
	return applique( qb, j.back().pos + 1, m );
}

extern "C" int custom_music_index( const char *track_name )
{
	if( !track_name ) return -1;
	const char *p = track_name;
	for( const char *q = track_name; *q; ++q )
		if( *q == '\\' || *q == '/' ) p = q + 1;
	if( strncasecmp( p, "THUGCUSTOM", 10 ) || !isdigit((unsigned char)p[10] )) return -1;
	const int i = atoi( p + 10 );
	return ( i >= 0 && i < s_nb_actives && i < (int)s_pistes.size()) ? i : -1;
}

extern "C" int custom_music_open( int i )
{
	if( i < 0 || i >= (int)s_pistes.size()) return 0;
	const int ok = cm_open( s_pistes[i].chemin.c_str());
	VLOG( "MUS", "custom music: %s %s - %s", ok ? "playing" : "could not open", s_pistes[i].bande.c_str(), s_pistes[i].titre.c_str());
	return ok;
}
