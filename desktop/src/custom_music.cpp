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

extern "C" void desktop_host_path( const char *vita_path, char *out, size_t out_size );

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
	T_ARG = 45, T_NOMCRC = 43, T_JUMP = 46, T_RANDOM = 47, T_RANDOM2 = 55, T_CASE = 62,
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

// Story pause menu: a "Change Skater" item before "View Goals". It runs the
// game's own load_pro_skater with name = desktop_next_skater, which
// SelectCurrentSkater (skfuncs.cpp) turns into the next unlocked skater.
static void patch_change_skater( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t item = crc( "make_sprite_menu_item" ), id = crc( "id" ), chapitres = crc( "menu_chapters" );
	for( size_t k = 2; k < j.size(); ++k )
	{
		if( !est_nom( qb, j[k], chapitres ) || qb[j[k - 1].pos] != T_EGAL || !est_nom( qb, j[k - 2], id )) continue;
		size_t d = k;
		while( d > 0 && k - d < 12 && !est_nom( qb, j[d], item )) --d;
		if( !est_nom( qb, j[d], item )) return;
		Ecrit w;
		w.nom( "make_sprite_menu_item" );
		w.champ_chaine( "text", "Change Skater" );
		w.nom( "id" ); w.octet( T_EGAL ); w.nom( "menu_desktop_skater" );
		w.nom( "pad_choose_script" ); w.octet( T_EGAL ); w.nom( "load_pro_skater" );
		w.nom( "pad_choose_params" ); w.octet( T_EGAL ); w.octet( T_STRUCT );
		w.nom( "name" ); w.octet( T_EGAL ); w.nom( "desktop_next_skater" );
		w.octet( T_FSTRUCT );
		w.eol();
		w.nom( "make_text_sprite" );
		w.nom( "texture" ); w.octet( T_EGAL ); w.octet( T_ARG ); w.nom( "edit_skater_icon" );
		w.nom( "parent" ); w.octet( T_EGAL ); w.nom( "menu_desktop_skater" );
		w.eol();
		m.push_back( Modif{ j[d].pos, 0, w.o } );
		VLOG( "MUS", "pause menu: Change Skater added" );
		return;
	}
}

// --- a little script compiler for the menus added below -------------------------
//
// Enough of the .q syntax for menu scripts: names, "strings", numbers,
// (x,y) pairs, = { } [ ] ( ) ,, <name> arguments, and the keywords used.

static void compile_q( Ecrit &w, const char *t )
{
	static const struct { const char *mot; unsigned char jeton; } mots[] = {
		{ "script", T_SCRIPT }, { "endscript", T_ENDSCRIPT }, { "if", 37 }, { "else", 38 },
		{ "endif", 40 }, { "return", 41 }, { "NOT", 57 } };
	while( *t )
	{
		const char c = *t;
		if( c == '\n' ) { w.eol(); ++t; continue; }
		if( c == ' ' || c == '\t' || c == '\r' ) { ++t; continue; }
		if( c == '"' )
		{
			const char *f = strchr( t + 1, '"' );
			w.chaine( std::string( t + 1, f ));
			t = f + 1;
			continue;
		}
		if( c == '(' )
		{
			float x, y, z; int n = 0;
			if( sscanf( t, "(%f,%f,%f)%n", &x, &y, &z, &n ) == 3 && n > 0 )
			{
				w.octet( T_VECTEUR );
				const float v[3] = { x, y, z };
				for( int i = 0; i < 3; ++i ) { uint32_t u; memcpy( &u, &v[i], 4 ); w.u32( u ); }
				t += n;
				continue;
			}
			n = 0;
			if( sscanf( t, "(%f,%f)%n", &x, &y, &n ) == 2 && n > 0 )
			{
				w.octet( T_PAIRE );
				uint32_t a, b; memcpy( &a, &x, 4 ); memcpy( &b, &y, 4 );
				w.u32( a ); w.u32( b );
				t += n;
				continue;
			}
			w.octet( 14 ); ++t; continue;
		}
		if( c == ')' ) { w.octet( 15 ); ++t; continue; }
		if( c == '{' ) { w.octet( T_STRUCT ); ++t; continue; }
		if( c == '}' ) { w.octet( T_FSTRUCT ); ++t; continue; }
		if( c == '[' ) { w.octet( T_TAB ); ++t; continue; }
		if( c == ']' ) { w.octet( T_FTAB ); ++t; continue; }
		if( c == '=' ) { w.octet( T_EGAL ); ++t; continue; }
		if( c == ',' ) { w.octet( 9 ); ++t; continue; }
		if( c == '<' )
		{
			const char *f = strchr( t, '>' );
			w.octet( T_ARG );
			w.nom( std::string( t + 1, f ).c_str());
			t = f + 1;
			continue;
		}
		const char *d = t;
		while( *t && !strchr( " \t\r\n\"(){}[]=<,", *t )) ++t;
		const std::string m( d, t );
		if( m[0] == '#' )	// a checksum given as #hex (names with spaces)
		{
			w.octet( T_NOM ); w.u32((uint32_t)strtoul( m.c_str() + 1, NULL, 16 ));
			continue;
		}
		if( isdigit((unsigned char)m[0] ) || ( m[0] == '-' && m.size() > 1 ))
		{
			if( m.find( '.' ) != std::string::npos )
			{
				const float f = (float)atof( m.c_str());
				uint32_t u; memcpy( &u, &f, 4 );
				w.octet( T_FLOTTANT ); w.u32( u );
			}
			else
				w.entier( atoi( m.c_str()));
			continue;
		}
		bool fait = false;
		for( size_t k = 0; k < sizeof( mots ) / sizeof( mots[0] ); ++k )
			if( m == mots[k].mot ) { w.octet( mots[k].jeton ); fait = true; break; }
		if( !fait ) w.nom( m.c_str());
	}
}

// Story "Change Player Model" (desktop): Pause > Edit Skater/Tricks gets the
// item, under Edit Appearance (created skater) or on its own (a pro in use);
// it opens a list like the cheats menu, one line per unlocked skater (built
// by DesktopAddModelItems, skfuncs.cpp); picking one switches to it and Back
// returns to Edit Skater. Names only, centred; ignore_width keeps the list
// out of widest_menu_item_width, which later menus size themselves from.
static const char *s_menu_modele =
	"script desktop_model_menu\n"
	"if ObjectExists id = current_menu_anchor\n"
	"DestroyScreenElement id = current_menu_anchor\n"
	"endif\n"
	"FormatText ChecksumName = title_icon \"%i_edit_skater\" i = ( THEME_PREFIXES [ current_theme_prefix ] )\n"
	"make_new_themed_scrolling_menu title = \"PLAYER MODEL\" title_icon = <title_icon> dims = (600,237) pos = (229,80) right_bracket_z = 1\n"
	"SetScreenElementProps { id = sub_menu event_handlers = [ { pad_back generic_menu_pad_back params = { callback = edit_skater_options_menu } } ] }\n"
	"create_helper_text generic_helper_text\n"
	"kill_start_key_binding\n"
	"DesktopAddModelItems\n"
	"finish_themed_scrolling_menu bg_width = 4 time = 0.2\n"
	"if GotParam focus_id\n"
	"FireEvent type = focus target = sub_vmenu data = { child_id = <focus_id> }\n"
	"endif\n"
	"endscript\n"
	"script desktop_model_item\n"
	"theme_menu_add_item text = <display_name> id = <item_id> highlight_bar_scale = (2.8,0.8) centered ignore_width no_bg pad_choose_script = desktop_pick_model pad_choose_params = { name = <name> focus_id = <item_id> }\n"
	"endscript\n"
	"script desktop_pick_model\n"
	"if DesktopSelectSkater name = <name>\n"
	"RefreshSkaterModel profile = 0 skater = 0\n"
	"endif\n"
	"desktop_model_menu focus_id = <focus_id>\n"
	"endscript\n";

static const char *s_item_modele =
	"\nif GameModeEquals is_career\n"
	"make_theme_menu_item { text = \"Change Player Model\" id = menu_desktop_model pad_choose_script = desktop_model_menu }\n"
	"endif\n";

static void patch_player_model( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n_script = crc( "edit_skater_options_menu" ), n_apparence = crc( "menu_edit_appearance" ),
		n_courant = crc( "current_skater" ), n_custom = crc( "custom" );
	size_t k = 0;
	for( ; k + 1 < j.size(); ++k )
		if( qb[j[k].pos] == T_SCRIPT && est_nom( qb, j[k + 1], n_script )) break;
	if( k + 1 >= j.size()) return;
	size_t fin = k;
	while( fin < j.size() && qb[j[fin].pos] != T_ENDSCRIPT ) ++fin;
	if( fin >= j.size()) return;

	// if ( <current_skater> = custom ) ... endif: its endif.
	size_t si = 0;
	for( size_t a = k; a + 4 < fin; ++a )
		if( qb[j[a].pos] == 37 )
		{
			size_t b = a + 1;
			while( b < a + 6 && b < fin && !( qb[j[b].pos] == T_ARG && est_nom( qb, j[b + 1], n_courant ))) ++b;
			if( b < a + 6 && b + 3 < fin && qb[j[b + 2].pos] == T_EGAL && est_nom( qb, j[b + 3], n_custom )) { si = a; break; }
		}
	if( !si ) { VLOG( "MUS", "edit skater menu: current_skater test not found" ); return; }
	size_t endif_ = 0;
	int niveau = 0;
	bool sinon = false;
	for( size_t a = si; a < fin; ++a )
	{
		const unsigned char t = qb[j[a].pos];
		if( t == 37 ) ++niveau;
		else if( t == 38 && niveau == 1 ) sinon = true;
		else if( t == 40 && --niveau == 0 ) { endif_ = a; break; }
	}
	if( !endif_ || sinon ) { VLOG( "MUS", "edit skater menu: layout not as expected" ); return; }

	// After the Edit Appearance item's { ... }.
	size_t apres = 0;
	for( size_t a = si; a + 2 < endif_; ++a )
		if( est_nom( qb, j[a], n_apparence ))
		{
			int n = 0;
			size_t b = a;
			while( b > si && qb[j[b].pos] != T_STRUCT ) --b;
			for( ; b < endif_; ++b )
			{
				if( qb[j[b].pos] == T_STRUCT ) ++n;
				else if( qb[j[b].pos] == T_FSTRUCT && --n == 0 ) { apres = b + 1; break; }
			}
			break;
		}
	if( !apres ) { VLOG( "MUS", "edit skater menu: Edit Appearance not found" ); return; }

	Ecrit w1; compile_q( w1, s_item_modele );
	m.push_back( Modif{ j[apres].pos, 0, w1.o } );
	Ecrit w2; w2.octet( 38 ); compile_q( w2, s_item_modele );
	m.push_back( Modif{ j[endif_].pos, 0, w2.o } );
	Ecrit w3; w3.eol(); compile_q( w3, s_menu_modele );
	m.push_back( Modif{ j.back().pos, 0, w3.o } );
	VLOG( "MUS", "edit skater menu: Change Player Model added" );
}

// Story wardrobe (desktop, Marcus's call): chapters no longer dress the
// skater (Birdhouse / Peralta team shirts, Russian coat; change_clothes in
// chapter_info.qb), and a pro picked with Change Player Model keeps their
// own board rather than getting Muska's deck (change_deck_to_muska).
static const char *s_debut_habits = "return\n";
static const char *s_debut_planche =
	"select_skater_get_current_skater_name\n"
	"if NOT ( <current_skater> = custom )\n"
	"return\n"
	"endif\n";

// Air shuffle (as in THUG Pro / Re-THAWed): off the board in the air (a
// caveman), the boneless input (Up + X X) puts you back on the board with a
// small pop. It leaves walking, so it's once per caveman; it ends in plain
// Airborne, so you can go off the board again and shuffle again.
static const char *s_air_shuffle =
	"script WalkingAirShuffle\n"
	"SkaterLoopingSound_TurnOff\n"
	"Walk_ScaleAnimSpeed Off\n"
	"CleanUp_WalkingtoSkating\n"
	"PerhapsAwardCaveman\n"
	"Jump desktop_air_shuffle\n"
	"Vibrate Actuator = 1 Percent = 80 duration = 0.1\n"
	"PlayAnim Anim = Boneless BlendPeriod = 0.2\n"
	"WaitAnimWhilstChecking\n"
	"Goto Airborne\n"
	"endscript\n";

static void patch_air_shuffle( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "WalkAirTricks" );
	for( size_t k = 0; k + 2 < j.size(); ++k )
	{
		if( !est_nom( qb, j[k], n ) || qb[j[k + 1].pos] != T_EGAL || qb[j[k + 2].pos] != T_TAB ) continue;
		if( k > 0 && qb[j[k - 1].pos] != T_EOL && qb[j[k - 1].pos] != T_EOLNUM ) continue;	// the definition
		Ecrit w1; compile_q( w1, s_air_shuffle );
		m.push_back( Modif{ j[k].pos, 0, w1.o } );
		Ecrit w2; w2.eol(); compile_q( w2, "{ Trigger = { TapTwiceRelease , Up , X , 500 } Scr = WalkingAirShuffle }" );
		m.push_back( Modif{ j[k + 2].pos + j[k + 2].taille, 0, w2.o } );
		VLOG( "MUS", "air shuffle added" );
		return;
	}
}

// Main menu (levels\\mainmenu\\mainmenu_scripts.qb): "Quit Game" under
// Options, closing the game through DesktopQuitGame.
static void patch_quitter( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "create_main_menu" ), cible = crc( "attract_container" ), cree = crc( "CreateScreenElement" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT || !est_nom( qb, j[k + 1], n )) continue;
		size_t b = k + 2;
		while( b < j.size() && qb[j[b].pos] != T_ENDSCRIPT && !est_nom( qb, j[b], cible )) ++b;
		if( b >= j.size() || qb[j[b].pos] == T_ENDSCRIPT ) return;
		while( b > k && !est_nom( qb, j[b], cree )) --b;
		if( b == k ) return;
		Ecrit w1; compile_q( w1, "script desktop_quit_game\nDesktopQuitGame\nendscript\n" );
		m.push_back( Modif{ j[k].pos, 0, w1.o } );
		Ecrit w2; compile_q( w2, "make_main_menu_item { text = \"Quit Game\" id = menu_desktop_quit pad_choose_script = desktop_quit_game focus_params = { attract_timer } }\n" );
		m.push_back( Modif{ j[b].pos, 0, w2.o } );
		VLOG( "MUS", "main menu: Quit Game added" );
		return;
	}
}

// Story Mode menu (create_career_options_menu, skateshop.qb): the Pro Skater
// 1-4 careers listed greyed out (not ported yet) above an Underground label,
// ahead of THUG's own Level Select / Continue / Start New / Load items.
static void patch_histoire( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "create_career_options_menu" ), item = crc( "make_main_menu_item" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT || !est_nom( qb, j[k + 1], n )) continue;
		size_t b = k + 2;
		while( b < j.size() && qb[j[b].pos] != T_ENDSCRIPT && !est_nom( qb, j[b], item )) ++b;
		if( b >= j.size() || qb[j[b].pos] == T_ENDSCRIPT ) return;
		while( b > k && qb[j[b].pos] != 37 ) --b;	// the 'if NOT cd' around Level Select
		if( b == k ) return;
		Ecrit w;
		compile_q( w,
			"make_main_menu_item { text = \"PRO SKATER 1\" pad_choose_script = null_script not_focusable = not_focusable }\n"
			"make_main_menu_item { text = \"PRO SKATER 2\" pad_choose_script = null_script not_focusable = not_focusable }\n"
			"make_main_menu_item { text = \"PRO SKATER 3\" pad_choose_script = null_script not_focusable = not_focusable }\n"
			"make_main_menu_item { text = \"PRO SKATER 4\" pad_choose_script = null_script not_focusable = not_focusable }\n"
			"make_main_menu_item { text = \"- UNDERGROUND -\" pad_choose_script = null_script not_focusable = not_focusable }\n" );
		m.push_back( Modif{ j[b].pos, 0, w.o } );
		VLOG( "MUS", "story menu: Pro Skater 1-4 added" );
		return;
	}
}

// Playable peds (desktop): THUG's named peds from profile_ped.qb, as pro
// skaters after the secret ones (skater_index 100 up, so they stay out of
// saves). Each gets its own appearance (the ped's parts plus the ped
// skateboard) and SS_ped as its picture. Peds whose parts aren't in THUG's
// files (THPS4 leftovers in profile_ped.qb) are left out by the generator.
// Generated from scripts\\profile_ped.qb (named peds whose models ship with THUG)
struct PietonJouable { const char *nom, *affiche; int femme; const char *apparence; };
static const PietonJouable s_pietons[] = {
	{ "Ped_Skeezo", "Skeezo", 0, "ped_m_head = { desc_id = Ped_Skeezo_Head } ped_m_torso = { desc_id = Torso_Wht_Tshirt_01 } ped_m_legs = { desc_id = Legs_Cargos_02 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Ralphie", "Ralphie", 0, "ped_m_head = { desc_id = Ped_Ralph_Head } ped_m_torso = { desc_id = Torso_Wht_Tshirt_03 } ped_m_legs = { desc_id = Legs_Jeans_03 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Bender", "Bender", 0, "ped_m_head = { desc_id = Ped_Bender_Head } ped_m_torso = { desc_id = Torso_Brn_Tshirt_02 } ped_m_legs = { desc_id = Legs_Jeans_05 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Jordan", "Jordan", 0, "ped_body = { desc_id = Ped_Jordan } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Eric", "Eric", 0, "ped_body = { desc_id = Eric } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Bum_01", "Bum 1", 0, "ped_body = { desc_id = #45aac045 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Crackhead_01", "Crackhead 1", 0, "ped_body = { desc_id = #7baacce9 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Drugdealer_01", "Drugdealer 1", 0, "ped_body = { desc_id = #06842f4e } Stocky_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_Drugdealer_02", "Drugdealer 2", 0, "ped_body = { desc_id = #9f8d7ef4 } scale = (0.9,1.13,0.9) ped_board = { desc_id = Skateboard }" },
	{ "Ped_Drugdealer_03", "Drugdealer 3", 0, "ped_body = { desc_id = #e88a4e62 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_FactoryWorker_01", "FactoryWorker 1", 0, "ped_body = { desc_id = #5c545ca3 } body_shape = Fatman_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_FactoryWorker_02", "FactoryWorker 2", 0, "ped_body = { desc_id = #c55d0d19 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Security_Train", "Security Train", 0, "ped_body = { desc_id = #b55147e2 } body_shape = Fatman_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_Street_Warrior_01", "Street Warrior 1", 0, "ped_body = { desc_id = #97b4489a } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Street_Warrior_02", "Street Warrior 2", 0, "ped_body = { desc_id = #0ebd1920 } ped_m_legs = { randomized_desc_id } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Street_Warrior_03", "Street Warrior 3", 0, "ped_body = { desc_id = #79ba29b6 } ped_m_legs = { randomized_desc_id } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Tombstone", "Tombstone", 0, "ped_body = { desc_id = Tombstone } scale = (1.1,0.9,0.9) ped_board = { desc_id = Skateboard }" },
	{ "Ped_Businesswoman_01", "Businesswoman 1", 1, "ped_body = { desc_id = #15d96c43 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_M_NYPD_01", "M NYPD 1", 0, "ped_body = { desc_id = #343bf7cb } Stocky_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_NYPD_01", "F NYPD 1", 1, "ped_body = { desc_id = #b66e6916 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Construction_Jhammer", "Construction Jhammer", 0, "ped_m_head = { desc_id = Ped_Worker_Head01 } ped_m_torso = { desc_id = Ped_Worker_Torso } ped_m_legs = { desc_id = Ped_Worker_Legs } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Construction_Manhole", "Construction Manhole", 0, "ped_m_head = { desc_id = Ped_Worker_Head02 } ped_m_torso = { desc_id = Ped_Worker_Torso } ped_m_legs = { desc_id = Ped_Worker_Legs } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Skater_NY1", "Skater NY1", 0, "ped_body = { desc_id = Skater_NY1 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Skater_NY2", "Skater NY2", 0, "ped_body = { desc_id = Skater_NY2 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Peralta", "Peralta", 0, "ped_body = { desc_id = Ped_Peralta } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Chef", "Chef", 0, "ped_body = { desc_id = Ped_Chef } ped_board = { desc_id = Skateboard }" },
	{ "Ped_BusinessMan_01", "BusinessMan 1", 0, "ped_body = { desc_id = #c7f0e71f } ped_board = { desc_id = Skateboard }" },
	{ "Ped_BusinessMan_02", "BusinessMan 2", 0, "ped_body = { desc_id = #5ef9b6a5 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_BusinessMan_03", "BusinessMan 3", 0, "ped_body = { desc_id = #29fe8633 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_BlkKid_01", "BlkKid 1", 0, "ped_body = { desc_id = #91161540 } body_shape = kid1_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_BlkKid_02", "BlkKid 2", 0, "ped_body = { desc_id = #081f44fa } body_shape = kid1_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_BlkKid_03", "BlkKid 3", 0, "ped_body = { desc_id = #7f18746c } body_shape = kid3_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_Trooper", "Trooper", 0, "ped_body = { desc_id = Ped_Trooper } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Todd", "Todd", 0, "ped_body = { desc_id = Ped_Todd } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_Dancer_01", "F Dancer 1", 1, "ped_body = { desc_id = #77e03193 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_Dancer_02", "F Dancer 2", 1, "ped_body = { desc_id = #eee96029 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Clemens", "Clemens", 0, "ped_m_head = { desc_id = Ped_Clemens_head } ped_m_torso = { desc_id = Torso_Wht_Tshirt_03 } ped_m_legs = { randomized_desc_id } ped_board = { desc_id = Skateboard }" },
	{ "Ped_AfroJim", "AfroJim", 0, "ped_m_head = { desc_id = Ped_AfroJim_head } ped_m_torso = { desc_id = Torso_Wht_Tshirt_04 } ped_m_legs = { randomized_desc_id } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Doorman_01", "Doorman 1", 0, "ped_body = { desc_id = #488bbc83 } Stocky_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_Deskclerk_01", "Deskclerk 1", 0, "ped_body = { desc_id = #de1ed9db } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_Deskclerk_01", "F Deskclerk 1", 1, "ped_body = { desc_id = #2e9540a8 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Leafblower", "Leafblower", 0, "ped_body = { desc_id = Ped_Leafblower } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Security_01", "Security 1", 0, "ped_m_head = { desc_id = Ped_Security_Head01 } ped_body = { desc_id = Ped_Security_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Security_02", "Security 2", 0, "ped_m_head = { desc_id = Ped_Security_Head02 } ped_body = { desc_id = Ped_Security_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Security_03", "Security 3", 0, "ped_m_head = { desc_id = Ped_Security_Head03 } ped_body = { desc_id = Ped_Security_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_Security", "F Security", 1, "ped_body = { desc_id = Ped_F_Security } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Gardener_01", "Gardener 1", 0, "ped_m_head = { desc_id = Ped_Gardener_Head01 } ped_body = { desc_id = Ped_Gardener_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Gardener_02", "Gardener 2", 0, "ped_m_head = { desc_id = Ped_Gardener_Head02 } ped_body = { desc_id = Ped_Gardener_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_Gardener", "F Gardener", 1, "ped_body = { desc_id = Ped_F_Gardener } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_MCeleb", "F MCeleb", 1, "ped_body = { desc_id = Ped_F_MCeleb } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_KGB", "F KGB", 1, "ped_body = { desc_id = Ped_F_KGB } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KGB_01", "KGB 1", 0, "ped_m_head = { desc_id = Ped_KGB_Head01 } ped_body = { desc_id = Ped_KGB_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KGB_02", "KGB 2", 0, "ped_m_head = { desc_id = Ped_KGB_Head02 } ped_body = { desc_id = Ped_KGB_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_RGuard_01", "RGuard 1", 0, "ped_m_head = { desc_id = Ped_RGuard_Head01 } ped_body = { desc_id = Ped_RGuard_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_RGuard_02", "RGuard 2", 0, "ped_m_head = { desc_id = Ped_RGuard_Head02 } ped_body = { desc_id = Ped_RGuard_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_Babushka", "F Babushka", 1, "ped_f_head = { desc_id = Head_f_Bab_head } ped_f_torso = { desc_id = Torso_f_bab_coat } ped_f_legs = { desc_id = Legs_f_jeans01 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Cameraman", "Cameraman", 0, "ped_body = { desc_id = Ped_Cameraman } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Official_SC_01", "Official SC 1", 0, "ped_m_head = { desc_id = Head_OfficialA } ped_body = { desc_id = #0a3260e4 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Official_SC_02", "Official SC 2", 0, "ped_m_head = { desc_id = Head_OfficialB } ped_body = { desc_id = #0a3260e4 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_A_01", "Monsterped A 1", 0, "ped_body = { desc_id = #113610cb } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_A_01B", "Monsterped A 01B", 0, "ped_body = { desc_id = #af1b2d5c } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_B_01", "Monsterped B 1", 0, "ped_body = { desc_id = #0383bf25 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_C_01", "Monsterped C 1", 0, "ped_body = { desc_id = #bb3fd840 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_A_02", "Monsterped A 2", 0, "ped_body = { desc_id = #883f4171 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_B_02", "Monsterped B 2", 0, "ped_body = { desc_id = #9a8aee9f } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_C_02", "Monsterped C 2", 0, "ped_body = { desc_id = #223689fa } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_A_03", "Monsterped A 3", 0, "ped_body = { desc_id = #ff3871e7 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_B_03", "Monsterped B 3", 0, "ped_body = { desc_id = #ed8dde09 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_C_03", "Monsterped C 3", 0, "ped_body = { desc_id = #5531b96c } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_A_04", "Monsterped A 4", 0, "ped_body = { desc_id = #615ce444 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_B_04", "Monsterped B 4", 0, "ped_body = { desc_id = #73e94baa } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Monsterped_C_04", "Monsterped C 4", 0, "ped_body = { desc_id = #cb552ccf } ped_board = { desc_id = Skateboard }" },
	{ "Ped_QBert", "QBert", 0, "ped_body = { desc_id = Ped_QBert } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Video", "Video", 0, "ped_body = { desc_id = Ped_Video } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Photo", "Photo", 0, "ped_body = { desc_id = Ped_Photo } ped_board = { desc_id = Skateboard }" },
	{ "Ped_SCJ_Worker", "SCJ Worker", 0, "ped_m_head = { desc_id = Ped_Worker_Head01 } ped_m_torso = { desc_id = Ped_Worker_Torso } ped_m_legs = { desc_id = Ped_Worker_Legs } ped_board = { desc_id = Skateboard }" },
	{ "Ped_SCJ_Judge_01", "SCJ Judge 1", 0, "ped_m_head = { desc_id = Head_Wht_01 } ped_m_torso = { desc_id = Torso_Wht_Judge } ped_m_legs = { desc_id = Legs_Jeans_01 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_SCJ_Judge_02", "SCJ Judge 2", 0, "ped_m_head = { desc_id = Head_Wht_02 } ped_m_torso = { desc_id = Torso_Wht_Judge } ped_m_legs = { desc_id = Legs_Jeans_01 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_SCJ_Judge_03", "SCJ Judge 3", 0, "ped_m_head = { desc_id = Head_Wht_03 } ped_m_torso = { desc_id = Torso_Wht_Judge } ped_m_legs = { desc_id = Legs_Cargos_04 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_SCJ_Judge_04", "SCJ Judge 4", 0, "ped_m_head = { desc_id = Head_JJagger } ped_m_torso = { desc_id = Torso_Wht_Judge } ped_m_legs = { desc_id = Legs_Jeans_01 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_SCJ_Judge_05", "SCJ Judge 5", 0, "ped_m_head = { desc_id = KGutierez } ped_m_torso = { desc_id = Torso_Wht_Judge } ped_m_legs = { desc_id = Legs_Jeans_02 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_SCJ_Judge_06", "SCJ Judge 6", 0, "ped_m_head = { desc_id = Head_CRausch } ped_m_torso = { desc_id = Torso_Wht_Judge } ped_m_legs = { desc_id = Legs_Cargos_01 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Hula", "Hula", 1, "ped_body = { desc_id = Ped_Hula } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Hula2", "Hula2", 1, "ped_body = { desc_id = Ped_Hula2 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Bartender", "Bartender", 0, "ped_m_head = { desc_id = Head_TBarber } ped_m_torso = { desc_id = Torso_wht_hawaii } ped_m_legs = { desc_id = Legs_Cargos_02 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Film", "Film", 0, "ped_body = { desc_id = Ped_Film } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Bikini_1", "Bikini 1", 1, "ped_body = { desc_id = Ped_Bikini_1 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Bikini_2", "Bikini 2", 1, "ped_body = { desc_id = Ped_Bikini_2 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Bikini_3", "Bikini 3", 1, "ped_body = { desc_id = Ped_Bikini_3 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Bride", "Bride", 1, "ped_body = { desc_id = Ped_Bride } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Surfer", "Surfer", 0, "ped_body = { desc_id = Ped_Surfer } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KISS_Paul", "KISS Paul", 0, "ped_body = { desc_id = Ped_KISS_Paul } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KISS_Peter", "KISS Peter", 0, "ped_body = { desc_id = Ped_KISS_Peter } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KISS_Ace", "KISS Ace", 0, "ped_body = { desc_id = Ped_KISS_Ace } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KISS_Gene", "KISS Gene", 0, "ped_body = { desc_id = Ped_KISS_Gene } ped_board = { desc_id = Skateboard }" },
	{ "Skate_KISS_Paul", "KISS Paul (Skating)", 0, "ped_body = { desc_id = Skate_KISS_Paul } ped_board = { desc_id = Skateboard }" },
	{ "Skate_KISS_Peter", "KISS Peter (Skating)", 0, "ped_body = { desc_id = Skate_KISS_Peter } ped_board = { desc_id = Skateboard }" },
	{ "Skate_KISS_Ace", "KISS Ace (Skating)", 0, "ped_body = { desc_id = Skate_KISS_Ace } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Hawk", "Ped Hawk", 0, "ped_body = { desc_id = PedPro_Hawk } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Koston", "Ped Koston", 0, "ped_body = { desc_id = PedPro_Koston } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Burnquist", "Ped Burnquist", 0, "ped_body = { desc_id = PedPro_Burnquist } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Lasek", "Ped Lasek", 0, "ped_body = { desc_id = PedPro_Lasek } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Mullen", "Ped Mullen", 0, "ped_body = { desc_id = PedPro_Mullen } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Muska", "Ped Muska", 0, "ped_body = { desc_id = PedPro_Muska } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Margera", "Ped Margera", 0, "ped_body = { desc_id = PedPro_Margera } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Rodrigez", "Ped Rodrigez", 0, "ped_body = { desc_id = PedPro_Rodrigez } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Reynolds", "Ped Reynolds", 0, "ped_body = { desc_id = PedPro_Reynolds } ped_board = { desc_id = Skateboard }" },
	{ "PedPro_Vallely", "Ped Vallely", 0, "ped_body = { desc_id = PedPro_Vallely } ped_board = { desc_id = Skateboard }" },
};

static std::string profil_pieton( int i, const PietonJouable &p )
{
	char t[2048];
	snprintf( t, sizeof( t ),
		"{ display_name = \"%s\" first_name = \"Ped\" file_name = \"Unimplemented\" skater_index = %d\n"
		"default_appearance = desktop_pedlook_%d name = desktop_ped_%d stance = regular pushstyle = mongo_when_switch trickstyle = street\n"
		"is_pro = 1 is_head_locked = 1 is_locked = 1 is_male = %d age = -1 hometown = \"\" points_available = 0\n"
		"air = 6 run = 6 ollie = 6 speed = 6 spin = 6 switch = 6 flip_speed = 6 rail_balance = 6 lip_balance = 6 manual_balance = 6\n"
		"sponsors = [ ] trick_mapping = { } default_trick_mapping = WolverineTricks no_edit no_edit_groups = [ secret_options ]\n"
		"max_specials = 11 specials = { [\n"
		"{ trickslot = SpAir_L_D_Circle trickname = Trick_1234 }\n"
		"{ trickslot = SpAir_R_U_Circle trickname = Trick_The900 }\n"
		"{ trickslot = SpGrind_U_D_Triangle trickname = Trick_FlipKickDad }\n"
		"{ trickslot = SpMan_L_R_Triangle trickname = Trick_OneFootOneWheel }\n"
		"{ trickslot = Unassigned trickname = Unassigned } { trickslot = Unassigned trickname = Unassigned }\n"
		"{ trickslot = Unassigned trickname = Unassigned } { trickslot = Unassigned trickname = Unassigned }\n"
		"{ trickslot = Unassigned trickname = Unassigned } { trickslot = Unassigned trickname = Unassigned }\n"
		"{ trickslot = Unassigned trickname = Unassigned } { trickslot = Unassigned trickname = Unassigned }\n"
		"] } }\n",
		p.affiche, 100 + i, i, i, p.femme ? 0 : 1 );
	return t;
}

static void patch_pietons( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "master_skater_list" );
	for( size_t k = 0; k + 2 < j.size(); ++k )
	{
		if( !est_nom( qb, j[k], n ) || qb[j[k + 1].pos] != T_EGAL ) continue;
		size_t a = sans_eol( qb, j, k + 2 );
		if( a >= j.size() || qb[j[a].pos] != T_TAB ) continue;
		int niveau = 0;
		for( ; a < j.size(); ++a )
		{
			const unsigned char c = qb[j[a].pos];
			if( c == T_TAB || c == T_STRUCT ) ++niveau;
			else if(( c == T_FTAB || c == T_FSTRUCT ) && --niveau == 0 ) break;
		}
		if( a >= j.size() || qb[j[a].pos] != T_FTAB ) return;
		const int nb = (int)( sizeof( s_pietons ) / sizeof( s_pietons[0] ));
		Ecrit w1;
		for( int i = 0; i < nb; ++i )
			compile_q( w1, profil_pieton( i, s_pietons[i] ).c_str());
		m.push_back( Modif{ j[a].pos, 0, w1.o } );
		Ecrit w2; w2.eol();
		for( int i = 0; i < nb; ++i )
		{
			char t[1536];
			// Not named like the profile: the select screen uses the profile name as a
			// screen element id, and a global struct of that name would replace it.
			snprintf( t, sizeof( t ), "desktop_pedlook_%d = { %s }\n", i, s_pietons[i].apparence );
			compile_q( w2, t );
		}
		m.push_back( Modif{ j.back().pos, 0, w2.o } );
		VLOG( "MUS", "skaters: %d peds added", nb );
		return;
	}
}

// Text naming the Xbox reads as the PS2 version's (Marcus): saves go to a
// "memory card (8MB) (for PlayStation 2)", the console is a PlayStation 2.
static const char *s_xbox_ps2[][2] = {
	{ "Please do not turn off your Xbox console.", "Do not remove the memory card (8MB) (for PlayStation 2) or switch off the console." },
	{ "Your Xbox does not have enough free blocks", "Your memory card (8MB) (for PlayStation 2) does not have enough free space" },
	{ "the hard disk", "the memory card (8MB) (for PlayStation 2)" },
	{ "hard disk", "memory card (8MB) (for PlayStation 2)" },
	{ "Hard Disk", "Memory Card (8MB) (for PlayStation 2)" },
	{ "HARD DISK", "MEMORY CARD (8MB) (FOR PLAYSTATION 2)" },
	{ "Xbox", "PlayStation 2" },
	{ "XBox", "PlayStation 2" },
	{ "XBOX", "PLAYSTATION 2" },
	// Save sizes: the desktop already counts 1 KB blocks, so only the unit changes
	{ "Free: 50000+", "Free: 50000+ KB" },
	{ " blocks", " KB" },
	{ "Free More Blocks", "Free More Space" },
};

static void patch_xbox( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	for( size_t k = 0; k < j.size(); ++k )
	{
		const unsigned char t = qb[j[k].pos];
		if( t != T_CHAINE && t != T_CHAINE_LOC ) continue;
		const uint32_t n = lit32( qb + j[k].pos + 1 );
		if( !n ) continue;
		std::string texte( (const char *)qb + j[k].pos + 5, n - 1 );
		bool change = false;
		for( size_t r = 0; r < sizeof( s_xbox_ps2 ) / sizeof( s_xbox_ps2[0] ); ++r )
			for( size_t i = 0; ( i = texte.find( s_xbox_ps2[r][0], i )) != std::string::npos; change = true )
			{
				texte.replace( i, strlen( s_xbox_ps2[r][0] ), s_xbox_ps2[r][1] );
				i += strlen( s_xbox_ps2[r][1] );
			}
		if( !change ) continue;
		Ecrit w; w.chaine( texte );
		w.o[0] = t;
		m.push_back( Modif{ j[k].pos, j[k].taille, w.o } );
	}
}

// Every secret skater in the select screen's head strip (it shows the ones
// whose unlock flag is set). Pedestrian stays locked: its ped list isn't in
// the game's files.
static const char *s_debut_secrets =
	"SetGlobalFlag flag = SKATER_UNLOCKED_IRONMAN\n"
	"SetGlobalFlag flag = SKATER_UNLOCKED_KISSDUDE\n"
	"SetGlobalFlag flag = SKATER_UNLOCKED_CREATURE\n";

// Puts text at the start of script 'nom', after its header line.
static void patch_debut( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m,
                         const char *nom, const char *texte )
{
	const uint32_t n = crc( nom );
	for( size_t k = 0; k + 1 < j.size(); ++k )
		if( qb[j[k].pos] == T_SCRIPT && est_nom( qb, j[k + 1], n ))
		{
			size_t a = k + 2;
			while( a < j.size() && qb[j[a].pos] != T_EOL && qb[j[a].pos] != T_EOLNUM ) ++a;
			if( a >= j.size()) return;
			Ecrit w; compile_q( w, texte );
			m.push_back( Modif{ j[a].pos + j[a].taille, 0, w.o } );
			VLOG( "MUS", "%s: patched", nom );
			return;
		}
}

// Removes the bare calls to 'appel' (a line holding just that name) from
// script 'nom'.
static void patch_retire( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m,
                          const char *nom, const char *appel )
{
	const uint32_t n = crc( nom ), a = crc( appel );
	for( size_t k = 0; k + 1 < j.size(); ++k )
		if( qb[j[k].pos] == T_SCRIPT && est_nom( qb, j[k + 1], n ))
		{
			for( size_t b = k + 2; b + 1 < j.size() && qb[j[b].pos] != T_ENDSCRIPT; ++b )
				if( est_nom( qb, j[b], a )
				 && ( qb[j[b - 1].pos] == T_EOL || qb[j[b - 1].pos] == T_EOLNUM )
				 && ( qb[j[b + 1].pos] == T_EOL || qb[j[b + 1].pos] == T_EOLNUM ))
				{
					m.push_back( Modif{ j[b].pos, j[b].taille, std::vector<unsigned char>() } );
					VLOG( "MUS", "%s: %s removed", nom, appel );
				}
			return;
		}
}

// Imported levels (desktop): THPS4's levels, converted to THUG files by
// desktop/tools/thps4_to_thug.py into Data\\levels\\<L>. levels.qb gets a
// level struct and load script for each (level numbers past THUG's own; no
// scene .pre, so the loose .xbx files load; THPS4's lighting; NJ.pre for its
// terrain sounds, which THUG keeps in each level's .pre); gamemenu.qb lists
// them in the level select with THUG's own levels, ahead of Created Park
// (Marcus's call: real names, not with the secret levels). Their startup
// scripts run (they load the level's sounds, shipped as loose .wav by the
// converter) and so do their THPS4 goals (TH4_Goals_<L>); setup scripts
// don't yet, they call THPS4 functions.
// A level whose files aren't installed is left out of the list.
struct NiveauTH4 { const char *dossier, *nom; int numero; const char *startup, *lumieres; };
static const NiveauTH4 s_niveaux_th4[] = {
	{ "Sch", "College", 22, "sch_Startup",
	  "ambient_red = 72 ambient_green = 72 ambient_blue = 72 ambient_mod_factor = 0.2 heading_0 = -30 pitch_0 = -60 red_0 = 110 green_0 = 110 blue_0 = 105 mod_factor_0 = 0.8 heading_1 = 200 pitch_1 = 33 red_1 = 23 green_1 = 24 blue_1 = 30 mod_factor_1 = 0.1 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
	{ "SF2", "San Francisco", 23, "sf2_Startup",
	  "ambient_red = 69 ambient_green = 72 ambient_blue = 79 ambient_mod_factor = 0.3 heading_0 = -50 pitch_0 = -127 red_0 = 130 green_0 = 120 blue_0 = 97 mod_factor_0 = 1.4 heading_1 = 50 pitch_1 = -117 red_1 = 55 green_1 = 58 blue_1 = 61 mod_factor_1 = 0.2 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
	{ "Alc", "Alcatraz", 24, "ALC_Startup",
	  "ambient_red = 69 ambient_green = 72 ambient_blue = 79 ambient_mod_factor = 0.3 heading_0 = 120 pitch_0 = -127 red_0 = 130 green_0 = 120 blue_0 = 97 mod_factor_0 = 1.4 heading_1 = -45 pitch_1 = -110 red_1 = 58 green_1 = 62 blue_1 = 66 mod_factor_1 = 0.1 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
	{ "Kon", "Kona", 25, "KON_Startup",
	  "ambient_red = 72 ambient_green = 72 ambient_blue = 72 ambient_mod_factor = 0.2 heading_0 = -30 pitch_0 = -60 red_0 = 110 green_0 = 110 blue_0 = 105 mod_factor_0 = 0.8 heading_1 = 200 pitch_1 = 33 red_1 = 23 green_1 = 24 blue_1 = 30 mod_factor_1 = 0.1 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
	{ "Jnk", "Shipyard", 26, "JNK_Startup",
	  "ambient_red = 72 ambient_green = 72 ambient_blue = 72 ambient_mod_factor = 0.5 heading_0 = 90 pitch_0 = -60 red_0 = 75 green_0 = 75 blue_0 = 75 mod_factor_0 = 0.7 heading_1 = 0 pitch_1 = -90 red_1 = 0 green_1 = 0 blue_1 = 0 mod_factor_1 = 1 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
	{ "Lon", "London", 27, "Lon_Startup",
	  "ambient_red = 73 ambient_green = 73 ambient_blue = 73 ambient_mod_factor = 0.2 heading_0 = -30 pitch_0 = -60 red_0 = 110 green_0 = 110 blue_0 = 105 mod_factor_0 = 0.8 heading_1 = 200 pitch_1 = 33 red_1 = 22 green_1 = 23 blue_1 = 30 mod_factor_1 = 0.1 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
	{ "Zoo", "Zoo", 28, "zoo_Startup",
	  "ambient_red = 72 ambient_green = 72 ambient_blue = 72 ambient_mod_factor = 0.2 heading_0 = -30 pitch_0 = -60 red_0 = 110 green_0 = 110 blue_0 = 105 mod_factor_0 = 0.8 heading_1 = 200 pitch_1 = 33 red_1 = 24 green_1 = 25 blue_1 = 30 mod_factor_1 = 0.1 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
	{ "Cnv", "Carnival", 29, "Cnv_Startup",
	  "ambient_red = 72 ambient_green = 72 ambient_blue = 72 ambient_mod_factor = 0.5 heading_0 = 90 pitch_0 = -60 red_0 = 75 green_0 = 75 blue_0 = 75 mod_factor_0 = 0.7 heading_1 = 0 pitch_1 = -90 red_1 = 0 green_1 = 0 blue_1 = 0 mod_factor_1 = 1 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
	{ "Hof", "Chicago", 31, "hof_Startup",
	  "ambient_red = 72 ambient_green = 72 ambient_blue = 72 ambient_mod_factor = 0.5 heading_0 = 90 pitch_0 = -60 red_0 = 75 green_0 = 75 blue_0 = 75 mod_factor_0 = 0.7 heading_1 = 0 pitch_1 = -90 red_1 = 0 green_1 = 0 blue_1 = 0 mod_factor_1 = 1 fog_red = 0 fog_green = 0 fog_blue = 0 fog_alpha = 0 fog_dist = 0" },
};

static bool niveau_installe( const char *dossier )
{
	char vita[256], chemin[1024];
	snprintf( vita, sizeof( vita ), "ux0:data/thug/Data/levels/%s/%s.scn.xbx", dossier, dossier );
	desktop_host_path( vita, chemin, sizeof( chemin ));
	FILE *f = fopen( chemin, "rb" );
	if( f ) fclose( f );
	return f != NULL;
}

static std::string struct_niveau( const NiveauTH4 &n )
{
	char t[4096];
	snprintf( t, sizeof( t ),
		"Level_TH4_%s = {\n"
		"structure_name = Level_TH4_%s\n"
		"load_script = Load_TH4_%s\n"
		"name = \"%s\"\n"
		"loading_screen = \"loadscrn_generic\"\n"
		"loading_time = 9\n"
		"pre = \"NJ.pre\"\n"
		"level = \"%s\"\n"
		"sky = \"%s_Sky\"\n"
		"qb = \"levels\\%s\\%s.qb\"\n"
		"level_qb = \"levels\\%s\\%s_scripts.qb\"\n"
		"default_stats = 5\n"
		"level_number = %d\n"
		"startup_script = %s\n"
		"goals_script = TH4_Goals_%s\n"
		"%s\n"
		"}\n"
		"script Load_TH4_%s\n"
		"load_level Level_TH4_%s\n"
		"endscript\n"
		// THPS4's own <L>_Goals, once the converter has added the AddGoal_
		// wrappers it needs (older conversions don't have them).
		"script TH4_Goals_%s\n"
		"if ScriptExists AddGoal_Skate\n"
		"if ScriptExists %s_Goals\n"
		"%s_Goals\n"
		"endif\n"
		"endif\n"
		"endscript\n",
		n.dossier, n.dossier, n.dossier, n.nom, n.dossier, n.dossier, n.dossier, n.dossier, n.dossier, n.dossier,
		n.numero, n.startup, n.dossier, n.lumieres, n.dossier, n.dossier, n.dossier, n.dossier, n.dossier );
	return t;
}

static std::string choix_niveau( const NiveauTH4 &n )
{
	char t[512];
	snprintf( t, sizeof( t ),
		"{ text = \"%s\" level_num = %d points_to_unlock = 0 num_am_goals = 0 num_goals = 0 flag = LEVEL_UNLOCKED_NJ level = load_TH4_%s }\n",
		n.nom, n.numero, n.dossier );
	return t;
}

static void patch_niveaux( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	// levels.qb: the one with 'script load_level'
	const uint32_t n_load = crc( "load_level" ), n_info = crc( "level_select_menu_level_info" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
		if( qb[j[k].pos] == T_SCRIPT && est_nom( qb, j[k + 1], n_load ))
		{
			Ecrit w; w.eol();
			for( const NiveauTH4 &n : s_niveaux_th4 )
				if( niveau_installe( n.dossier ))
				{
					compile_q( w, struct_niveau( n ).c_str());
					VLOG( "MUS", "levels: THPS4 %s added", n.nom );
				}
			m.push_back( Modif{ j.back().pos, 0, w.o } );
			break;
		}
	// gamemenu.qb: level_select_menu_level_info = [ ... ]
	for( size_t k = 0; k + 2 < j.size(); ++k )
	{
		if( !est_nom( qb, j[k], n_info ) || qb[j[k + 1].pos] != T_EGAL ) continue;
		size_t a = sans_eol( qb, j, k + 2 );
		if( a >= j.size() || qb[j[a].pos] != T_TAB ) continue;
		// Before the Created Park entry (the struct holding level = load_sk5ed_gameplay),
		// else at the end.
		const uint32_t n_level = crc( "level" ), n_parc = crc( "load_sk5ed_gameplay" );
		int n = 0;
		size_t entree = 0, ou = 0;
		for( ; a < j.size(); ++a )
		{
			const unsigned char c = qb[j[a].pos];
			if( c == T_TAB || c == T_STRUCT )
			{
				if( n == 1 && c == T_STRUCT ) entree = a;
				++n;
			}
			else if(( c == T_FTAB || c == T_FSTRUCT ) && --n == 0 ) break;
			else if( !ou && n == 2 && a + 2 < j.size() && est_nom( qb, j[a], n_level ) && qb[j[a + 1].pos] == T_EGAL && est_nom( qb, j[a + 2], n_parc ))
				ou = entree;
		}
		if( a >= j.size()) return;
		if( !ou ) ou = a;
		Ecrit w;
		for( const NiveauTH4 &l : s_niveaux_th4 )
			if( niveau_installe( l.dossier ))
				compile_q( w, choix_niveau( l ).c_str());
		m.push_back( Modif{ j[ou].pos, 0, w.o } );
		VLOG( "MUS", "level select: THPS4 levels added" );
		return;
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
	patch_player_model( qb, j, m );
	patch_niveaux( qb, j, m );
	patch_air_shuffle( qb, j, m );
	patch_quitter( qb, j, m );
	patch_histoire( qb, j, m );
	patch_pietons( qb, j, m );
	patch_xbox( qb, j, m );
	patch_debut( qb, j, m, "select_skater_menu_add_hmenu_items", s_debut_secrets );
	patch_debut( qb, j, m, "change_clothes", s_debut_habits );
	patch_debut( qb, j, m, "change_deck_to_muska", s_debut_planche );
	// Music keeps going, where it was, through level loads and run starts
	// (desktop, Marcus's call): no stop and pause before a level loads, no
	// skip to the next song when a run starts.
	patch_retire( qb, j, m, "cleanup_before_loading_level", "StopMusic" );
	patch_retire( qb, j, m, "cleanup_before_loading_level", "PauseMusic" );
	patch_retire( qb, j, m, "GameFlow_StartRun", "SkipMusicTrack" );
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
