/*****************************************************************************
**  THUG desktop -- custom soundtrack                                       **
**  desktop/src/custom_music.cpp                                            **
**                                                                          **
**  Songs dropped in "custom_music" next to thug.exe (MP3, OGG, FLAC, WAV, **
**  subfolders too) join the game's own playlist:                          **
**                                                                          **
**  - Their names come from the files' tags: album artist (artist when     **
**    there is none) and title; "Artist - Title" file names otherwise.     **
**  - Their genre tag puts them under the menu's Punk, Hip Hop or          **
**    Rock/Other heading (anything else is Other, like the game's own).    **
**  - The game's playlist lives in its scripts: the playlist_tracks array  **
**    (band, track_title, genre, path) feeds LoadPermSongs, the playlist   **
**    menu, shuffle and the now-playing text. The songs are appended to    **
**    that array while the .qb loads, so every one of those works on them  **
**    unchanged.                                                           **
**  - The engine saves the playlist as 128 on/off bits: the game's own     **
**    songs plus at most as many custom songs as fit in 128.               **
**                                                                          **
**  The patch works on script tokens (Gel/Scripting/tokens.h).            **
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
	int         genre;		// playlist genre: 0 punk, 1 hip hop, 2 rock/other
};

static std::vector<Piste> s_pistes;			// every file found
static int  s_nb_actives = -1;				// how many joined playlist_tracks
static bool s_scanne = false;

const int MAX_PISTES_JEU = 512;				// MAX_NUM_TRACKS (Gel/Music/music.h)
const int GENRE_AUTRE = 2;		// the menu's Rock/Other

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

struct Tags { std::string album_artiste, artiste, titre, genre; };

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
		else if( id == "TCON" || id == "TCO" )	t.genre = texte_id3( d, taille );
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
	if( t.genre.empty() && p[127] != 255 )
	{
		char n[8]; snprintf( n, sizeof( n ), "%d", p[127] );
		t.genre = n;
	}
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
		else if( cle == "GENRE" && t.genre.empty())		t.genre = v;
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
				else if( !memcmp( &b[j], "IGNR", 4 )) t.genre = v;
				j += 8 + l + ( l & 1 );
			}
			return;
		}
		i += 8 + taille + ( taille & 1 );
	}
}

// --- genre ---------------------------------------------------------------------

// ID3v1 genre numbers (ID3v2 writes them too, as "17" or "(17)") that land
// in Punk or Hip Hop (or plainly rock); any other number is Rock/Other.
static const char *genre_id3( int n )
{
	switch( n )
	{
		case 43: case 121: case 129: case 133:		return "punk";
		case 7: case 15:							return "hip hop";
		case 1: case 6: case 9: case 17: case 20: case 22: case 40: case 47:
		case 56: case 79: case 81: case 91: case 92: case 93: case 94: case 131:
		case 137: case 138: case 141: case 144:		return "rock";
	}
	return "";
}

// The genre tag -> the playlist menu's genres: Punk, Hip Hop, Rock/Other.
// Anything that isn't punk or hip hop (electronic, jazz, untagged...) is
// Other, like the game's own. Punk wins over rock ("punk rock"), rock over
// rap ("rap metal").
static int genre_du_jeu( std::string g )
{
	// "(17)Rock", "(17)", "17": the number's name.
	size_t i = 0;
	while( i < g.size() && ( g[i] == '(' || g[i] == ' ' )) ++i;
	if( i < g.size() && isdigit((unsigned char)g[i] ))
	{
		const int n = atoi( g.c_str() + i );
		size_t f = g.find( ')' );
		std::string reste = ( f != std::string::npos ) ? g.substr( f + 1 ) : "";
		g = reste.empty() ? genre_id3( n ) : reste;
	}
	// Words, lower case, separators as spaces: " hip hop / rap " -> " hip hop rap ".
	std::string m = " ";
	for( size_t k = 0; k < g.size(); ++k )
	{
		const unsigned char c = (unsigned char)g[k];
		m += isalnum( c ) ? (char)tolower( c ) : ' ';
	}
	m += " ";
	struct { const char *mot; int genre; } const mots[] =
	{
		{ " punk", 0 }, { " hardcore", 0 }, { " emo ", 0 }, { " ska ", 0 }, { " oi ", 0 }, { " crust", 0 },
		{ "rock", 2 }, { " metal", 2 }, { "core ", 2 }, { " grunge", 2 }, { " alternative", 2 }, { " alt ", 2 },
		{ " indie", 2 }, { " shoegaze", 2 }, { " stoner", 2 }, { " sludge", 2 }, { " doom", 2 },
		{ " hip hop", 1 }, { " hiphop", 1 }, { " rap ", 1 }, { " trap ", 1 }, { " grime", 1 }, { " drill", 1 },
		{ " boom bap", 1 }, { " gangsta", 1 }, { " horrorcore", 1 },
	};
	for( size_t k = 0; k < sizeof( mots ) / sizeof( mots[0] ); ++k )
		if( m.find( mots[k].mot ) != std::string::npos )
			return mots[k].genre;
	return GENRE_AUTRE;
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
	p.genre = genre_du_jeu( t.genre );
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

// Sort key for band names, as a music player would: case and punctuation
// ignored ("P.U.T.S." sorts as "puts"), "The" kept (the game files its own
// "The ..." bands under T).
static std::string cle_tri( const std::string &t )
{
	std::string k;
	for( size_t i = 0; i < t.size(); ++i )
	{
		const unsigned char c = (unsigned char)t[i];
		if( isalnum( c )) k += (char)tolower( c );
		else if( c == ' ' && !k.empty() && k[k.size() - 1] != ' ' ) k += ' ';
	}
	return k;
}

static bool par_cle( const Piste &a, const Piste &b )
{
	// Band, then title, then file: the playlist's order.
	const std::string ba = cle_tri( a.bande ), bb = cle_tri( b.bande );
	if( ba != bb ) return ba < bb;
	const std::string ta = cle_tri( a.titre ), tb = cle_tri( b.titre );
	if( ta != tb ) return ta < tb;
	return a.cle < b.cle;
}

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
	void nom( const char *n ) { octet( T_NOM ); u32( crc( n )); }
	void entier( int v ) { octet( T_ENTIER ); u32((uint32_t)v ); }
	void chaine( const std::string &s ) { octet( T_CHAINE ); u32((uint32_t)s.size() + 1 ); for( size_t i = 0; i < s.size(); ++i ) octet((unsigned char)s[i] ); octet( 0 ); }
	void eol() { octet( T_EOL ); }
	void champ_chaine( const char *n, const std::string &s ) { nom( n ); octet( T_EGAL ); chaine( s ); }
	void champ_entier( const char *n, int v ) { nom( n ); octet( T_EGAL ); entier( v ); }
};

// Skips end-of-line tokens from k.
static size_t sans_eol( const unsigned char *qb, const std::vector<Jeton> &j, size_t k )
{
	while( k < j.size() && ( qb[j[k].pos] == T_EOL || qb[j[k].pos] == T_EOLNUM )) ++k;
	return k;
}

static bool est_nom( const unsigned char *qb, const Jeton &t, uint32_t n ) { return qb[t.pos] == T_NOM && lit32( qb + t.pos + 1 ) == n; }

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
// The string value of field 'champ' in the struct of tokens [d, f), or "".
static std::string champ_texte( const unsigned char *qb, const std::vector<Jeton> &j, size_t d, size_t f, uint32_t champ )
{
	for( size_t k = d; k + 2 < f; ++k )
		if( est_nom( qb, j[k], champ ) && qb[j[k + 1].pos] == T_EGAL && qb[j[k + 2].pos] == T_CHAINE )
			return std::string((const char *)qb + j[k + 2].pos + 5 );
	return "";
}

// playlist_tracks = [ {...} ... ]: the custom songs are merged in by band
// name, the way the game's own are listed. The game names two of its songs
// by position (locked_track1/2, the unlockable KISS songs, same file): those
// numbers are moved with them; if they aren't found the songs go at the end
// instead, leaving every position as it was.
static void patch_liste( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n_liste = crc( "playlist_tracks" ), n_band = crc( "band" );
	for( size_t k = 0; k + 2 < j.size(); ++k )
	{
		if( !est_nom( qb, j[k], n_liste ) || qb[j[k + 1].pos] != T_EGAL ) continue;
		size_t a = sans_eol( qb, j, k + 2 );
		if( a >= j.size() || qb[j[a].pos] != T_TAB ) continue;

		// The game's entries: token ranges and band names. Find ']'.
		struct Entree { size_t d, f; std::string cle; };
		std::vector<Entree> jeu;
		int niveau = 0;
		size_t e = a + 1;
		for( ; e < j.size(); ++e )
		{
			const unsigned char c = qb[j[e].pos];
			if( c == T_STRUCT || c == T_TAB )
			{
				if( niveau == 0 && c == T_STRUCT ) jeu.push_back( Entree{ e, 0, "" } );
				++niveau;
			}
			else if( c == T_FSTRUCT )
			{
				if( --niveau == 0 && !jeu.empty())
				{
					jeu.back().f = e + 1;
					jeu.back().cle = cle_tri( champ_texte( qb, j, jeu.back().d, e, n_band ));
				}
			}
			else if( c == T_FTAB ) { if( niveau == 0 ) break; --niveau; }
			else if( c == T_FIN ) return;
		}
		if( e >= j.size()) return;
		const int nb = (int)jeu.size();
		const int place = MAX_PISTES_JEU - nb;
		const int n = std::min((int)s_pistes.size(), std::max( place, 0 ));
		s_nb_actives = n;
		if( n < (int)s_pistes.size())
			VLOG( "MUS", "custom music: room for %d of %d songs (the game has %d, the playlist holds %d)",
			      n, (int)s_pistes.size(), nb, MAX_PISTES_JEU );

		// locked_track1 = 33 / locked_track2 = 34: where those ints are.
		size_t verrou[2] = { 0, 0 };
		const uint32_t n_verrou[2] = { crc( "locked_track1" ), crc( "locked_track2" ) };
		for( size_t v = 0; v + 2 < j.size(); ++v )
			for( int w = 0; w < 2; ++w )
				if( est_nom( qb, j[v], n_verrou[w] ) && qb[j[v + 1].pos] == T_EGAL && qb[j[v + 2].pos] == T_ENTIER )
					verrou[w] = v + 2;
		const bool trie = verrou[0] && verrou[1];
		if( !trie )
			VLOG( "MUS", "custom music: locked_track1/2 not in this file, songs go after the game's" );

		// Merge: a custom song goes before the first game song whose band
		// sorts after it (custom songs are already in band order).
		std::vector<int> ordre;		// >= 0: game entry, < 0: custom song -1-i
		std::vector<int> nouvelle( nb );
		int g = 0, p = 0;
		while( g < nb || p < n )
		{
			const bool prendre_perso = p < n &&
				( g >= nb || ( trie && cle_tri( s_pistes[p].bande ) < jeu[g].cle ));
			if( prendre_perso ) ordre.push_back( -1 - p++ );
			else { nouvelle[g] = (int)ordre.size(); ordre.push_back( g++ ); }
		}

		Ecrit w;
		for( size_t i = 0; i < ordre.size(); ++i )
		{
			w.eol();
			if( ordre[i] >= 0 )
			{
				const Entree &x = jeu[ordre[i]];
				for( size_t t = x.d; t < x.f; ++t )
					w.o.insert( w.o.end(), qb + j[t].pos, qb + j[t].pos + j[t].taille );
				continue;
			}
			const int c = -1 - ordre[i];
			char chemin[64];
			snprintf( chemin, sizeof( chemin ), "music\\vag\\songs\\THUGCUSTOM%03d", c );
			w.octet( T_STRUCT );
			w.champ_chaine( "band", s_pistes[c].bande );
			w.champ_chaine( "track_title", s_pistes[c].titre );
			w.champ_entier( "genre", s_pistes[c].genre );
			w.champ_chaine( "path", chemin );
			w.octet( T_FSTRUCT );
		}
		w.eol();
		m.push_back( Modif{ j[a + 1].pos, j[e].pos - j[a + 1].pos, w.o } );
		if( trie )
			for( int v = 0; v < 2; ++v )
			{
				const int ancien = (int)lit32( qb + j[verrou[v]].pos + 1 );
				if( ancien < 0 || ancien >= nb ) continue;
				Ecrit x; x.entier( nouvelle[ancien] );
				m.push_back( Modif{ j[verrou[v]].pos, j[verrou[v]].taille, x.o } );
			}
		VLOG( "MUS", "custom music: %d songs %s the game's %d", n, trie ? "sorted in with" : "added after", nb );
		return;
	}
}

} // namespace

// --- on/off by song name ------------------------------------------------------
//
// thug_playlist.txt next to thug.exe: the songs switched off in the playlist
// menu, one "band: title" per line. The save game keeps on/off as bits by
// position (and only 128 of them); custom songs move the game's own songs
// around and outnumber that, so on the desktop the names decide.

namespace
{
static bool s_liste_lue = false, s_liste_existe = false;
static std::vector<std::string> s_eteints;

static std::string chemin_liste( void )
{
	char *base = SDL_GetBasePath();
	std::string c = std::string( base ? base : "" ) + "thug_playlist.txt";
	SDL_free( base );
	return c;
}

static void lit_liste( void )
{
	if( s_liste_lue ) return;
	s_liste_lue = true;
	FILE *f = fopen( chemin_liste().c_str(), "r" );
	if( !f ) return;
	s_liste_existe = true;
	char l[256];
	while( fgets( l, sizeof( l ), f ))
	{
		size_t n = strlen( l );
		while( n && ( l[n - 1] == '\n' || l[n - 1] == '\r' )) l[--n] = 0;
		if( n && l[0] != ';' ) s_eteints.push_back( l );
	}
	fclose( f );
}
} // namespace

extern "C" int custom_music_playlist_known( void )
{
	lit_liste();
	return s_liste_existe;
}

extern "C" int custom_music_playlist_off( const char *title )
{
	lit_liste();
	return title && std::find( s_eteints.begin(), s_eteints.end(), std::string( title )) != s_eteints.end();
}

extern "C" void custom_music_playlist_set( const char *title, int off )
{
	if( !title ) return;
	lit_liste();
	std::vector<std::string>::iterator it = std::find( s_eteints.begin(), s_eteints.end(), std::string( title ));
	const bool est = it != s_eteints.end();
	if( est == ( off != 0 ) && s_liste_existe ) return;
	if( off && !est ) s_eteints.push_back( title );
	if( !off && est ) s_eteints.erase( it );
	FILE *f = fopen( chemin_liste().c_str(), "w" );
	if( !f ) return;
	fputs( "; Songs switched off in the playlist menu (written by the game).\n", f );
	for( size_t i = 0; i < s_eteints.size(); ++i )
		fprintf( f, "%s\n", s_eteints[i].c_str());
	fclose( f );
	s_liste_existe = true;
}

// The sound options' "Soundtrack" item (the Xbox's own user-soundtrack
// switch): the whole theme_menu_add_item { ... id = menu_soundtrack ... }
// call is taken out, so the menu never shows it.
static void patch_soundtrack( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t ajoute = crc( "theme_menu_add_item" ), id = crc( "id" ), menu = crc( "menu_soundtrack" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( !est_nom( qb, j[k], ajoute )) continue;
		const size_t d = sans_eol( qb, j, k + 1 );
		if( d >= j.size() || qb[j[d].pos] != T_STRUCT ) continue;
		int niveau = 0;
		size_t f = d;
		for( ; f < j.size(); ++f )
		{
			const unsigned char t = qb[j[f].pos];
			if( t == T_STRUCT ) ++niveau;
			else if( t == T_FSTRUCT && --niveau == 0 ) break;
			else if( t == T_FIN ) break;
		}
		if( f >= j.size() || qb[j[f].pos] != T_FSTRUCT ) continue;
		bool trouve = false;
		for( size_t n = d; n + 2 < f; ++n )
			if( est_nom( qb, j[n], id ) && qb[j[n + 1].pos] == T_EGAL && est_nom( qb, j[n + 2], menu )) trouve = true;
		if( !trouve ) continue;
		Modif x;
		x.pos = j[k].pos;
		x.enleve = j[f].pos + j[f].taille - j[k].pos;
		m.push_back( x );
		VLOG( "MUS", "removed the Soundtrack menu item" );
		k = f;
	}
}

extern "C" unsigned char *custom_music_patch_qb( const char *file_name, const unsigned char *qb )
{
	if( !qb ) return NULL;
	scanne();
	std::vector<Jeton> j;
	if( !decoupe( qb, j ))
	{
		VLOG( "MUS", "custom music: could not read %s", file_name ? file_name : "?" );
		return NULL;
	}
	std::vector<Modif> m;
	if( !s_pistes.empty()) patch_liste( qb, j, m );
	patch_soundtrack( qb, j, m );
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
