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
	std::stable_sort( m.begin(), m.end(), par_pos );
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
		{ "endif", 40 }, { "return", 41 }, { "NOT", 57 }, { "begin", 0x20 }, { "repeat", 0x21 } };
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
		if( c == ':' ) { w.octet( 0x42 ); ++t; continue; }
		if( c == '+' ) { w.octet( 11 ); ++t; continue; }
		if( !strncmp( t, "<...>", 5 ))	// all the script's params
		{
			w.octet( 0x2c );
			t += 5;
			continue;
		}
		if( c == '<' )
		{
			const char *f = strchr( t, '>' );
			w.octet( T_ARG );
			w.nom( std::string( t + 1, f ).c_str());
			t = f + 1;
			continue;
		}
		const char *d = t;
		while( *t && !strchr( " \t\r\n\"(){}[]=<,:", *t )) ++t;
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
	"if NOT DesktopDemented\n"		// Demented: the dealt skater is final
	"make_theme_menu_item { text = \"Change Player Model\" id = menu_desktop_model pad_choose_script = desktop_model_menu }\n"
	"endif\n"
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
	"PlaySound boneless09 pitch = 85\n"				// the boneless pop
	"SetTrickName \"Air Shuffle\"\n"
	"SetTrickScore 100\n"
	"Display\n"
	// the grind shuffle's bottom-left popup (grindscripts.qb Awardshuffle)
	"if NOT InSplitscreenGame\n"
	"Create_Panel_Message text = \"Shuffle Bonus\" id = perfect rgba = [ 50 120 200 128 ] pos = (110,340) style = perfect_style\n"
	"Create_Panel_Message text = \"+100 Points\" id = perfect2 rgba = [ 50 120 200 100 ] pos = (110,360) style = perfect_style\n"
	"else\n"
	"PerfectSloppy_2p text = \"Shuffle!\" rgb = [ 50 120 50 128 ]\n"
	"endif\n"
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

// Pause menu: Demented's own items (the Extras menu is gone, Marcus 2026-11-09).
static const char *s_item_pause =
	// Demented: the trick list, read only (the game's own View Tricks mode)
	"if DesktopDemented\n"
	"make_sprite_menu_item text = \"View Tricks\" id = menu_desktop_view_tricks pad_choose_script = create_edit_tricks_menu\n"
	"make_text_sprite texture = <edit_skater_icon> parent = menu_desktop_view_tricks\n"
	"make_sprite_menu_item text = \"View Stats\" id = menu_desktop_view_stats pad_choose_script = desktop_demented_stats\n"
	"make_text_sprite texture = <edit_skater_icon> parent = menu_desktop_view_stats\n"
	"endif\n";

// Demented's own scripts (appended to gamemenu.qb).
static const char *s_demented_scripts =
	// the modifiers menu, laid out like the difficulty menu
	"script desktop_demented_menu focus = 0\n"
	"dialog_box_exit\n"
	"make_new_menu { pos = (120,100) internal_just = [ center center ] menu_id = career_dif_menu vmenu_id = career_dif_vmenu menu_title = \"\" helper_text = generic_helper_text }\n"
	"FormatText ChecksumName = title_icon \"%i_career_ops\" i = ( THEME_PREFIXES [ current_theme_prefix ] )\n"
	"build_theme_sub_title title_icon = <title_icon> title = \"DEMENTED\"\n"
	"build_top_and_bottom_blocks\n"
	"make_mainmenu_3d_plane model = \"mainmenu_bg\\mainmenu_bg.mdl\" pos = (360,217) scale = (1.25,1.25)\n"
	"CreateScreenElement { Type = SpriteElement parent = current_menu_anchor id = mm_building texture = mm_building just = [ left top ] pos = (0,62) scale = (1.2,1.25) z_priority = -3 alpha = 1 }\n"
	"make_mainmenu_clouds\n"
	"kill_start_key_binding\n"
	"SetScreenElementProps { id = career_dif_vmenu event_handlers = [ { pad_back generic_menu_pad_back params = { callback = career_options_menu_exit new_menu_script = create_career_difficulty_menu } } ] }\n"
	"DesktopDementedText\n"
	// Marcus: the list ran off the screen; the 7 modifiers have their own page
	"make_main_menu_item { text = \"Modifiers\" pad_choose_script = career_options_menu_exit pad_choose_params = { new_menu_script = desktop_demented_mods focus = 0 } }\n"
	// seeds (Marcus's pick): type a code, or RANDOM. Randomizers + Countdown are always on,
	// no Randomizers page, Daily Run or SURPRISE ME! (Marcus 2026-11-10)
	"make_main_menu_item { text = <seed_text> pad_choose_script = career_options_menu_exit pad_choose_params = { new_menu_script = desktop_demented_seed_kb } }\n"
	"make_main_menu_item { text = \"Start Run\" pad_choose_script = desktop_demented_start }\n"
	"FireEvent Type = focus target = current_menu_anchor\n"
	"if NOT ( <focus> = 0 )\n"
	"change disable_menu_sounds = 1\n"
	"begin\n"
	"FireEvent Type = pad_down target = current_menu\n"
	"repeat <focus>\n"
	"change disable_menu_sounds = 0\n"
	"endif\n"
	"endscript\n"
	"script desktop_demented_mods focus = 0\n"
	"dialog_box_exit\n"
	"make_new_menu { pos = (120,100) internal_just = [ center center ] menu_id = career_dif_menu vmenu_id = career_dif_vmenu menu_title = \"\" helper_text = generic_helper_text }\n"
	"FormatText ChecksumName = title_icon \"%i_career_ops\" i = ( THEME_PREFIXES [ current_theme_prefix ] )\n"
	"build_theme_sub_title title_icon = <title_icon> title = \"MODIFIERS\"\n"
	"build_top_and_bottom_blocks\n"
	"make_mainmenu_3d_plane model = \"mainmenu_bg\\mainmenu_bg.mdl\" pos = (360,217) scale = (1.25,1.25)\n"
	"CreateScreenElement { Type = SpriteElement parent = current_menu_anchor id = mm_building texture = mm_building just = [ left top ] pos = (0,62) scale = (1.2,1.25) z_priority = -3 alpha = 1 }\n"
	"make_mainmenu_clouds\n"
	"kill_start_key_binding\n"
	"SetScreenElementProps { id = career_dif_vmenu event_handlers = [ { pad_back generic_menu_pad_back params = { callback = career_options_menu_exit new_menu_script = desktop_demented_menu focus = 0 } } ] }\n"
	"DesktopDementedText\n"
	"make_main_menu_item { text = <text0> id = desktop_mod_0 pad_choose_script = desktop_demented_flip pad_choose_params = { i = 0 focus = 0 } focus_script = desktop_tog_focus unfocus_script = desktop_tog_unfocus }\n"
	"desktop_tog_color id = desktop_mod_0\n"
	"make_main_menu_item { text = <text4> id = desktop_mod_4 pad_choose_script = desktop_demented_flip pad_choose_params = { i = 4 focus = 1 } focus_script = desktop_tog_focus unfocus_script = desktop_tog_unfocus }\n"
	"desktop_tog_color id = desktop_mod_4\n"
	"make_main_menu_item { text = <text1> id = desktop_mod_1 pad_choose_script = desktop_demented_flip pad_choose_params = { i = 1 focus = 2 } focus_script = desktop_tog_focus unfocus_script = desktop_tog_unfocus }\n"
	"desktop_tog_color id = desktop_mod_1\n"
	"make_main_menu_item { text = <text6> id = desktop_mod_6 pad_choose_script = desktop_demented_flip pad_choose_params = { i = 6 focus = 3 } focus_script = desktop_tog_focus unfocus_script = desktop_tog_unfocus }\n"
	"desktop_tog_color id = desktop_mod_6\n"
	"make_main_menu_item { text = \"Done\" pad_choose_script = career_options_menu_exit pad_choose_params = { new_menu_script = desktop_demented_menu focus = 0 } }\n"
	"FireEvent Type = focus target = current_menu_anchor\n"
	"if NOT ( <focus> = 0 )\n"
	"change disable_menu_sounds = 1\n"
	"begin\n"
	"FireEvent Type = pad_down target = current_menu\n"
	"repeat <focus>\n"
	"change disable_menu_sounds = 0\n"
	"endif\n"
	"endscript\n"
	// THUG2-style flips and rolls (trickcomponent.cpp spawns it on a double tap in
	// the air): the body goes round once in 0.8 s, 1000 points each (Marcus). Landing
	// too early is a bail (Land asks DesktopAirFlipBail); a rail, lip or wall ends it
	"script desktop_air_flip k = 0 name = \"Front Flip\"\n"
	"if ( <k> = 0 )\n"
	"RotateDisplay x Duration = 0.8 seconds StartAngle = 0 EndAngle = 360 SinePower = 2 RotationOffset = (0,30,0)\n"
	"endif\n"
	"if ( <k> = 1 )\n"
	"RotateDisplay x Duration = 0.8 seconds StartAngle = 0 EndAngle = -360 SinePower = 2 RotationOffset = (0,30,0)\n"
	"endif\n"
	"if ( <k> = 2 )\n"
	"RotateDisplay z Duration = 0.8 seconds StartAngle = 0 EndAngle = 360 SinePower = 2 RotationOffset = (0,30,0)\n"
	"endif\n"
	"if ( <k> = 3 )\n"
	"RotateDisplay z Duration = 0.8 seconds StartAngle = 0 EndAngle = -360 SinePower = 2 RotationOffset = (0,30,0)\n"
	"endif\n"
	"SetTrickName <name>\n"
	"SetTrickScore 1000\n"
	"Display\n"
	"begin\n"
	"if NOT InAir\n"
	"CancelRotateDisplay\n"
	"return\n"
	"endif\n"
	"wait 1 gameframe\n"
	"repeat 42\n"
	"endscript\n"
	// on = the theme's colour, off = grey (Marcus), on the modifier pages
	"script desktop_tog_focus\n"
	"GetTags\n"
	"theme_item_focus <...>\n"
	"desktop_tog_color id = <id> focus\n"
	"endscript\n"
	"script desktop_tog_unfocus\n"
	"GetTags\n"
	"theme_item_unfocus <...>\n"
	"desktop_tog_color id = <id>\n"
	"endscript\n"
	"script desktop_tog_color\n"
	"if NOT ScreenElementExists id = { <id> child = 0 }\n"
	"return\n"
	"endif\n"
	"if DesktopDementedIsOn id = <id>\n"
	"FormatText ChecksumName = c \"%i_HIGHLIGHTED_TEXT_COLOR\" i = ( THEME_COLOR_PREFIXES [ current_theme_prefix ] )\n"
	"SetScreenElementProps id = { <id> child = 0 } rgba = <c>\n"
	"else\n"
	"if GotParam focus\n"
	"SetScreenElementProps id = { <id> child = 0 } rgba = [ 105 105 105 128 ]\n"
	"else\n"
	"SetScreenElementProps id = { <id> child = 0 } rgba = [ 55 55 55 100 ]\n"
	"endif\n"
	"endif\n"
	"endscript\n"
	// chapter 26 in Demented: a failed pro challenge, its menu back up (only it on it)
	"script desktop_demented_pro_retry\n"
	"if ScreenElementExists id = current_menu_anchor\n"
	"return\n"
	"endif\n"
	"kill_start_key_binding\n"
	"restore_skater_camera\n"
	"launch_chapter_menu\n"
	"endscript\n"
	"script desktop_demented_seed_kb\n"
	"DesktopDementedText\n"
	"create_onscreen_keyboard { allow_cancel = 1 keyboard_cancel_script = desktop_demented_seed_cancel keyboard_done_script = desktop_demented_seed_done keyboard_title = \"ENTER SEED\" text = <seed_str> min_length = 1 max_length = 16 }\n"
	"endscript\n"
	"script desktop_demented_seed_done\n"
	"GetTextElementString id = keyboard_current_string\n"
	"DesktopDementedSeed string = <string>\n"
	"destroy_onscreen_keyboard\n"
	"desktop_demented_menu focus = 1\n"
	"endscript\n"
	"script desktop_demented_seed_cancel\n"
	"destroy_onscreen_keyboard\n"
	"desktop_demented_menu focus = 1\n"
	"endscript\n"
	"script desktop_demented_start\n"
	"career_options_menu_exit new_menu_script = set_career_difficulty dif = 2 desktop_demented\n"
	"endscript\n"
	"script desktop_demented_flip\n"
	"DesktopDementedToggle i = <i>\n"
	"career_options_menu_exit new_menu_script = desktop_demented_mods focus = <focus>\n"
	"endscript\n"
	// after the intro cutscene: the deal, then the second intro
	"script desktop_demented_roll\n"
	// no cheats in a run (their menu is hidden too)
	"UnSetGlobalFlag flag = CHEAT_ALWAYS_SPECIAL\n"
	"UnSetGlobalFlag flag = CHEAT_BIGHEAD\n"
	"UnSetGlobalFlag flag = CHEAT_COOL_SPECIAL_TRICKS\n"
	"UnSetGlobalFlag flag = CHEAT_DISCO\n"
	"UnSetGlobalFlag flag = CHEAT_FLAME\n"
	"UnSetGlobalFlag flag = CHEAT_GORILLA\n"
	"UnSetGlobalFlag flag = CHEAT_HOVERBOARD\n"
	"UnSetGlobalFlag flag = CHEAT_INLINE\n"
	"UnSetGlobalFlag flag = CHEAT_INVISIBLE\n"
	"UnSetGlobalFlag flag = CHEAT_KID\n"
	"UnSetGlobalFlag flag = CHEAT_MATRIX\n"
	"UnSetGlobalFlag flag = CHEAT_MOON\n"
	"UnSetGlobalFlag flag = CHEAT_PERFECT_MANUAL\n"
	"UnSetGlobalFlag flag = CHEAT_PERFECT_RAIL\n"
	"UnSetGlobalFlag flag = CHEAT_PERFECT_SKITCH\n"
	"UnSetGlobalFlag flag = CHEAT_SIM\n"
	"UnSetGlobalFlag flag = CHEAT_SLOMO\n"
	"UnSetGlobalFlag flag = CHEAT_STATS_13\n"
	"UnSetGlobalFlag flag = CHEAT_SUPER_BLOOD\n"
	"DesktopDementedDeal\n"
	"RefreshSkaterModel profile = 0 skater = 0\n"
	"endscript\n"
	"script desktop_demented_deal\n"
	"load_cas_textures_to_main_memory\n"
	"set_cas_cam wait_for_skater\n"
	"if DesktopDementedNeedsDeal\n"
	"desktop_demented_roll\n"
	"endif\n"
	"DesktopDementedDialogShown\n"
	// no debriefing box (Marcus): straight on, View Stats in the pause menu shows the deal
	"desktop_demented_go\n"
	"endscript\n"
	"script desktop_demented_go\n"
	"dialog_box_exit\n"
	"show_intro_2\n"
	"endscript\n"
	// View Stats: laid out like the game's own Game Progress screen
	"script desktop_demented_stats\n"
	"if ObjectExists id = current_menu_anchor\n"
	"DestroyScreenElement id = current_menu_anchor\n"
	"endif\n"
	"SetScreenElementLock id = root_window off\n"
	"CreateScreenElement { type = ContainerElement parent = root_window id = progress_anchor dims = (640,480) pos = (320,240) }\n"
	"AssignAlias id = progress_anchor alias = current_menu_anchor\n"
	"create_helper_text { helper_text_elements = [ { text = \"\\b7/\\b4 = Select\" } { text = \"\\m1 = Back\" } ] }\n"
	"kill_start_key_binding\n"
	"FormatText ChecksumName = title_icon \"%i_pro\" i = ( THEME_PREFIXES [ current_theme_prefix ] )\n"
	"build_theme_sub_title title = \"RUN STATS\" title_icon = <title_icon>\n"
	"FormatText ChecksumName = paused_icon \"%i_paused_icon\" i = ( THEME_PREFIXES [ current_theme_prefix ] )\n"
	"build_theme_box_icons icon_texture = <paused_icon>\n"
	"build_grunge_piece\n"
	"build_top_bar pos = (0,62)\n"
	"CreateScreenElement { type = ContainerElement parent = progress_anchor id = progress_menu dims = (640,480) pos = (320,640) }\n"
	"AssignAlias id = progress_menu alias = current_menu_anchor\n"
	"theme_background width = 6.35 pos = (320,85) num_parts = 10.5\n"
	"FormatText ChecksumName = text_rgba \"%i_HIGHLIGHTED_TEXT_COLOR\" i = ( THEME_COLOR_PREFIXES [ current_theme_prefix ] )\n"
	"CreateScreenElement { type = SpriteElement parent = current_menu_anchor id = view_progress_menu_up_arrow texture = up_arrow rgba = [ 128 128 128 85 ] pos = (300,90) just = [ left top ] z_priority = 3 }\n"
	"CreateScreenElement { type = SpriteElement parent = current_menu_anchor texture = black scale = (2,75) pos = (380,90) just = [ left top ] rgba = [ 0 0 0 128 ] }\n"
	"GetStackedScreenElementPos Y id = view_progress_menu_up_arrow offset = (0,260)\n"
	"CreateScreenElement { type = SpriteElement parent = current_menu_anchor id = view_progress_menu_down_arrow texture = down_arrow rgba = [ 128 128 128 85 ] pos = <pos> just = [ left top ] z_priority = 3 }\n"
	"CreateScreenElement { type = VScrollingMenu parent = current_menu_anchor id = vs1 dims = (640,245) pos = (54,110) just = [ left top ] internal_just = [ center top ] }\n"
	"CreateScreenElement { type = VMenu parent = <id> id = progress_vmenu pos = (0,0) just = [ left top ] internal_just = [ left top ] dont_allow_wrap "
	"event_handlers = [ { pad_up menu_vert_blink_arrow params = { id = view_progress_menu_up_arrow rgba = <text_rgba> } } "
	"{ pad_down menu_vert_blink_arrow params = { id = view_progress_menu_down_arrow rgba = <text_rgba> } } "
	"{ pad_up generic_menu_up_or_down_sound params = { up } } { pad_down generic_menu_up_or_down_sound params = { down } } ] }\n"
	"AssignAlias id = progress_vmenu alias = current_menu\n"
	"SetScreenElementProps { id = current_menu event_handlers = [ { pad_back generic_menu_pad_back params = { callback = desktop_demented_stats_exit } } ] }\n"
	"DesktopDementedStatRows\n"
	"DoScreenElementMorph id = current_menu_anchor pos = (320,240) time = 0.2\n"
	"DoScreenElementMorph id = progress_vmenu time = 0.2 pos = (0,-200)\n"
	"FireEvent type = focus target = current_menu\n"
	"endscript\n"
	"script desktop_demented_stat_row\n"
	"if GotParam sep\n"
	"progress_menu_add_item id1 = progress_vmenu seperator\n"
	"else\n"
	"progress_menu_add_item id1 = progress_vmenu left_col_text = <text> right_col_text = <value>\n"
	"endif\n"
	"endscript\n"
	"script desktop_demented_stats_exit\n"
	"if ObjectExists id = progress_anchor\n"
	"DestroyScreenElement id = progress_anchor\n"
	"wait 1 gameframe\n"
	"endif\n"
	"if ObjectExists id = box_icon\n"
	"DestroyScreenElement id = box_icon\n"
	"wait 1 gameframe\n"
	"endif\n"
	"if ObjectExists id = box_icon_2\n"
	"DestroyScreenElement id = box_icon_2\n"
	"wait 1 gameframe\n"
	"endif\n"
	"if ObjectExists id = grunge_anchor\n"
	"DestroyScreenElement id = grunge_anchor\n"
	"wait 1 gameframe\n"
	"endif\n"
	"create_pause_menu\n"
	"endscript\n"
	// Chaos Fragile / Perma Bail: the out-of-bounds banner's look, kept up
	"script desktop_chaos_banner\n"
	"if ObjectExists id = <id>\n"
	"DestroyScreenElement id = <id>\n"
	"endif\n"
	"Create_Panel_Message style = desktop_chaos_banner_style id = <id> text = <text> pos = <pos> rgba = [ 128 16 16 128 ]\n"
	"endscript\n"
	"script desktop_chaos_banner_style\n"
	"Domorph alpha = 0 scale = 0\n"
	"Domorph alpha = 1 time = 0.25 scale = 1.3\n"
	"Domorph time = 0.15 scale = 1\n"
	"endscript\n"
	"script desktop_chaos_banner_off\n"
	"if ObjectExists id = <id>\n"
	"DestroyScreenElement id = <id>\n"
	"endif\n"
	"endscript\n"
	"script desktop_demented_teleport\n"
	"ResetSkaters node_number = <node_number>\n"
	"endscript\n"
	"script desktop_demented_nosave\n"
	"wait 2 frames\n"
	"FireEvent type = goal_wait_for_save\n"
	"endscript\n"
	// run over (Perma Bail, out of lives): the summary, then the main menu
	"script desktop_demented_end\n"
	"wait 90 frames\n"
	"if ObjectExists id = current_menu_anchor\n"
	"DestroyScreenElement id = current_menu_anchor\n"
	"endif\n"
	"PauseGame\n"
	"FormatText TextName = text2 \"\\c1%r\\n\\n%t\" r = <reason> t = <text>\n"
	"create_dialog_box { title = <title> title_font = testtitle text = <text2> pos = (320,240) just = [ center center ] "
	"text_rgba = [ 88 105 112 128 ] text_dims = (330,0) line_spacing = 0.85 "
	"buttons = [ { font = small text = \"OK\" pad_choose_script = desktop_demented_quit } ] delay_input }\n"
	"endscript\n"
	"script desktop_demented_quit\n"
	"dialog_box_exit\n"
	"DesktopDementedEndRun\n"
	"level_select_change_level_quit level = load_skateshop\n"
	"endscript\n"
	// A small one-line notice top right, under the score, gone after 3 s
	// (Demented lives and time; lost with the achievements in 1a0b8b1, put
	// back). sound = optional.
	"script desktop_toast rgba = [ 128 128 128 110 ]\n"
	"KillSpawnedScript name = desktop_toast_fade\n"
	"if ScreenElementExists id = desktop_toast_el\n"
	"DestroyScreenElement id = desktop_toast_el\n"
	"endif\n"
	"SetScreenElementLock id = root_window off\n"
	"CreateScreenElement { type = TextElement parent = root_window id = desktop_toast_el font = small text = <text> pos = (628,64) just = [ right top ] scale = 0.75 rgba = <rgba> z_priority = 50 }\n"
	"if GotParam sound\n"
	"PlaySound <sound> vol = 130\n"
	"endif\n"
	"SpawnScript desktop_toast_fade\n"
	"endscript\n"
	"script desktop_toast_fade\n"
	"wait 180 frames\n"
	"if ScreenElementExists id = desktop_toast_el\n"
	"DoScreenElementMorph id = desktop_toast_el alpha = 0 time = 0.4\n"
	"endif\n"
	"wait 25 frames\n"
	"if ScreenElementExists id = desktop_toast_el\n"
	"DestroyScreenElement id = desktop_toast_el\n"
	"endif\n"
	"endscript\n"
	"script desktop_demented_msg delay = 0\n"
	"if NOT ( <delay> = 0 )\n"
	"wait <delay> frames\n"
	"endif\n"
	"desktop_toast <...>\n"
	"endscript\n"
	// the run clock, bottom right in the goal timer's font, lives above it
	"script desktop_demented_hud_create\n"
	"desktop_demented_hud_off\n"
	"SetScreenElementLock id = root_window off\n"
	"CreateScreenElement { type = ContainerElement parent = root_window id = desktop_demented_hud pos = (0,0) just = [ left top ] }\n"
	"CreateScreenElement { type = TextElement parent = desktop_demented_hud id = desktop_demented_time font = newtimerfont text = \" \" pos = (638,462) just = [ right bottom ] scale = 0.55 rgba = [ 128 128 128 70 ] z_priority = -1 }\n"
	"CreateScreenElement { type = TextElement parent = desktop_demented_hud id = desktop_demented_lives font = small text = \" \" pos = (30,416) just = [ center top ] scale = 0.6 rgba = [ 128 128 128 90 ] z_priority = -1 }\n"
	// Chaos Bounty: the challenge and its clock, top middle
	"CreateScreenElement { type = TextElement parent = desktop_demented_hud id = desktop_prime_texte font = small text = \" \" pos = (320,58) just = [ center center ] scale = 0.9 rgba = [ 128 128 128 120 ] z_priority = -1 }\n"
	"CreateScreenElement { type = TextElement parent = desktop_demented_hud id = desktop_prime_temps font = newtimerfont text = \" \" pos = (320,84) just = [ center center ] scale = 0.5 rgba = [ 128 128 128 110 ] z_priority = -1 }\n"
	"endscript\n"
	"script desktop_prime_pop\n"
	"if ScreenElementExists id = desktop_prime_texte\n"
	"DoScreenElementMorph id = desktop_prime_texte scale = 0\n"
	"DoScreenElementMorph id = desktop_prime_texte scale = 0.9 time = 0.2\n"
	"endif\n"
	"PlaySound GoalMidGood vol = 200\n"
	"endscript\n"
	"script desktop_prime_fail\n"
	"PlaySound GoalFail vol = 200\n"
	"endscript\n"
	// Chaos Ped Rush: the C++ side moves them (desktop_demented.cpp rush_maj)
	"script desktop_ped_rush\n"
	"Obj_ClearException SkaterInAvoidRadius\n"
	"Obj_ClearException SkaterOutOfAvoidRadius\n"
	"Ped_StopMoving\n"
	"GetTags\n"
	"if GotParam walk\n"
	"Obj_PlayAnim Anim = <walk> Cycle speed = 2.6\n"
	"endif\n"
	"begin\n"
	"wait 1 gameframe\n"
	"repeat\n"
	"endscript\n"
	"script desktop_ped_rush_hit\n"
	"GetTags\n"
	"if GotParam attackAnims\n"
	"ped_play_attack_anim\n"
	"endif\n"
	"begin\n"
	"wait 1 gameframe\n"
	"repeat\n"
	"endscript\n"
	// after the beating the ped just stands there: restarting its walk
	// (ped_initialize_movement / Ped_StartMoving away from its path) is where
	// both Ped Rush crashes happened
	"script desktop_ped_rush_end\n"
	"Ped_StopMoving\n"
	"Obj_PlayAnim Anim = Ped_M_Idle1 Cycle\n"
	"begin\n"
	"wait 1 gameframe\n"
	"repeat\n"
	"endscript\n"
	"script desktop_ped_rush_slap\n"
	"PlaySound BailSlap03\n"
	"endscript\n"
	// one life: the goal givers' star (model arrow), same bob and spin, on screen
	"script desktop_demented_star\n"
	"if ScreenElementExists id = <id>\n"
	"DestroyScreenElement id = <id>\n"
	"endif\n"
	"if GotParam pos\n"
	"if ScreenElementExists id = desktop_demented_hud\n"
	"SetScreenElementLock id = desktop_demented_hud off\n"
	"CreateScreenElement { type = Element3D parent = desktop_demented_hud id = <id> model = \"arrow\" desktop_screen_pos desktop_own_mesh pos = <pos> CameraZ = -6 scale = 0.0055 rgba = <rgba> AngleY = <AngleY> AngVelY = 0.08 HoverParams = { Amp = 9.5 Freq = 1.2 } }\n"
	"endif\n"
	"endif\n"
	"endscript\n"
	"script desktop_demented_hud_off\n"
	"if ScreenElementExists id = desktop_demented_hud\n"
	"DestroyScreenElement id = desktop_demented_hud\n"
	"endif\n"
	"endscript\n";

// Pause menu (create_pause_menu, gamemenu.qb): the item before Change Level,
// and the mode's scripts at the end of the file.
// Demented: the edit tricks menus go read only, as they do during a goal:
// their GoalManager_HasActiveGoals checks become DesktopTricksViewOnly
// (true in Demented, else the goal manager's answer).
static void patch_view_tricks( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	static const char *scripts[] = { "create_edit_tricks_menu", "create_edit_tricks_sub_menu",
		"edit_tricks_sub_menu_add_key_combo", "special_tricks_menu_add_slot" };
	const uint32_t actif = crc( "GoalManager_HasActiveGoals" ), neuf = crc( "DesktopTricksViewOnly" );
	int nb = 0;
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT ) continue;
		bool ok = false;
		for( int i = 0; i < 4; ++i ) ok |= est_nom( qb, j[k + 1], crc( scripts[i] ));
		if( !ok ) continue;
		for( size_t a = k + 2; a < j.size() && qb[j[a].pos] != T_ENDSCRIPT; ++a )
			if( est_nom( qb, j[a], actif ))
			{
				std::vector<unsigned char> b( 4 );
				for( int i = 0; i < 4; ++i ) b[i] = (unsigned char)( neuf >> ( 8 * i ));
				m.push_back( Modif{ j[a].pos + 1, 4, b } );
				++nb;
			}
	}
	if( nb ) VLOG( "MUS", "edit tricks: %d goal checks now view-only checks", nb );
}

// Demented: single lines of a script wrapped in "if NOT DesktopDemented".
// A line qualifies when it starts with 'debut' and (if 'id_' is set) names
// id = id_ somewhere on it.
static int wrap_lignes( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m,
	const char *script, const char *debut, const char *id_ )
{
	const uint32_t n = crc( script ), d = crc( debut ), id = crc( "id" ), cible = id_ ? crc( id_ ) : 0;
	int nb = 0;
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT || !est_nom( qb, j[k + 1], n )) continue;
		for( size_t a = k + 2; a < j.size() && qb[j[a].pos] != T_ENDSCRIPT; ++a )
		{
			const unsigned char pre = qb[j[a - 1].pos];
			if( !( pre == T_EOL || pre == T_EOLNUM ) || !est_nom( qb, j[a], d )) continue;
			size_t f = a + 1;
			bool ok = !cible;
			int prof = 0;	// a { ... } spanning lines belongs to this line
			for( ; f < j.size() && qb[j[f].pos] != T_ENDSCRIPT && ( prof > 0 || ( qb[j[f].pos] != T_EOL && qb[j[f].pos] != T_EOLNUM )); ++f )
			{
				if( qb[j[f].pos] == T_STRUCT ) ++prof;
				else if( qb[j[f].pos] == T_FSTRUCT ) --prof;
				if( cible && f + 2 < j.size() && est_nom( qb, j[f], id ) && qb[j[f + 1].pos] == T_EGAL && est_nom( qb, j[f + 2], cible )) ok = true;
			}
			if( !ok || f >= j.size() || qb[j[f].pos] == T_ENDSCRIPT ) continue;
			Ecrit w1; compile_q( w1, "if NOT DesktopDemented\n" );
			m.push_back( Modif{ j[a].pos, 0, w1.o } );
			Ecrit w2; w2.eol(); compile_q( w2, "endif" );
			m.push_back( Modif{ j[f].pos, 0, w2.o } );
			++nb;
			a = f;
		}
		break;
	}
	if( nb ) VLOG( "MUS", "%s: %d %s lines kept out of Demented", script, nb, debut );
	return nb;
}

// Demented: the pause menu doesn't freeze the game (as in online play). In
// handle_start_pressed the offline block from PauseGame down to its endif
// (pause, goal pause, hiding the goal dialogs) is skipped; the skater
// ignores the pad while menu_state is on, as online.
static void patch_pause_live( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "handle_start_pressed" ), pause = crc( "PauseGame" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT || !est_nom( qb, j[k + 1], n )) continue;
		size_t d = k + 2;
		while( d < j.size() && qb[j[d].pos] != T_ENDSCRIPT && !est_nom( qb, j[d], pause )) ++d;
		if( d >= j.size() || qb[j[d].pos] == T_ENDSCRIPT ) { VLOG( "MUS", "start pressed: PauseGame not found" ); return; }
		int prof = 0;
		size_t f = d;
		for( ; f < j.size() && qb[j[f].pos] != T_ENDSCRIPT; ++f )
		{
			if( qb[j[f].pos] == 37 ) ++prof;
			else if( qb[j[f].pos] == 40 ) { if( prof == 0 ) break; --prof; }
		}
		if( f >= j.size() || qb[j[f].pos] != 40 ) { VLOG( "MUS", "start pressed: layout not as expected" ); return; }
		Ecrit w1; compile_q( w1, "if NOT DesktopDemented\n" );
		m.push_back( Modif{ j[d].pos, 0, w1.o } );
		Ecrit w2; compile_q( w2, "endif\n" );
		m.push_back( Modif{ j[f].pos, 0, w2.o } );
		VLOG( "MUS", "start pressed: no freeze in Demented" );
		return;
	}
}

static void patch_pause_extras( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "create_pause_menu" ), item = crc( "make_sprite_menu_item" ), niveau = crc( "menu_level_select" ), id = crc( "id" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT || !est_nom( qb, j[k + 1], n )) continue;
		size_t b = k + 2;
		for( ; b + 2 < j.size() && qb[j[b].pos] != T_ENDSCRIPT; ++b )
			if( est_nom( qb, j[b], id ) && qb[j[b + 1].pos] == T_EGAL && est_nom( qb, j[b + 2], niveau )) break;
		if( b + 2 >= j.size() || qb[j[b].pos] == T_ENDSCRIPT ) { VLOG( "MUS", "pause menu: Change Level not found" ); return; }
		while( b > k && !est_nom( qb, j[b], item )) --b;
		if( b == k ) return;
		// Demented: no Change Level, Edit Skater/Tricks, Retry Last Goal, End
		// Current Goal or View Goals: each item and its SetScreenElementProps
		// get an "if NOT DesktopDemented" around them. Change
		// Level's opening goes in with the Extras item: the two share a position.
		size_t fin_ = b;
		while( fin_ < j.size() && qb[j[fin_].pos] != T_ENDSCRIPT ) ++fin_;
		struct Local { static size_t endif_apres( const unsigned char *qb, const std::vector<Jeton> &j, size_t e, size_t fin )
		{
			int prof = 0;
			for( size_t f = e; f < fin; ++f )
			{
				if( qb[j[f].pos] == 37 ) ++prof;
				else if( qb[j[f].pos] == 40 ) { if( prof == 0 ) return f; --prof; }
			}
			return 0;
		} };
		{
			const size_t f = Local::endif_apres( qb, j, b, fin_ );
			std::string t = s_item_pause;
			if( f ) t += "\nif NOT DesktopDemented\n";
			Ecrit w1; compile_q( w1, t.c_str());
			m.push_back( Modif{ j[b].pos, 0, w1.o } );
			if( f ) { Ecrit w4; compile_q( w4, "endif\n" ); m.push_back( Modif{ j[f].pos, 0, w4.o } ); }
		}
		const uint32_t caches[] = { crc( "menu_cas" ), crc( "menu_retry_goal" ), crc( "menu_end_current_goal" ), crc( "menu_chapters" ), crc( "menu_skip_to_gap" ), crc( "menu_save_game" ) };	// + View Gaps (Marcus)
		const uint32_t props = crc( "SetScreenElementProps" );
		int nb = 0;
		for( size_t e = k + 2; e + 2 < fin_; ++e )
		{
			if( !( est_nom( qb, j[e], id ) && qb[j[e + 1].pos] == T_EGAL )) continue;
			bool ok = false;
			for( int c = 0; c < (int)( sizeof( caches ) / sizeof( caches[0] )); ++c ) ok |= est_nom( qb, j[e + 2], caches[c] );
			if( !ok ) continue;
			size_t d = e;
			while( d > k && !est_nom( qb, j[d], item ) && qb[j[d].pos] != T_EOL && qb[j[d].pos] != T_EOLNUM ) --d;	// this line's item
			if( !est_nom( qb, j[d], item )) continue;	// "id = menu_cas" inside SetScreenElementProps
			// the item's lines end with its SetScreenElementProps { ... }
			size_t g = d;
			while( g < fin_ && !est_nom( qb, j[g], props )) ++g;
			while( g < fin_ && qb[j[g].pos] != T_STRUCT ) ++g;
			int n = 0;
			size_t f = 0;
			for( ; g < fin_; ++g )
			{
				if( qb[j[g].pos] == T_STRUCT ) ++n;
				else if( qb[j[g].pos] == T_FSTRUCT && --n == 0 ) { f = g + 1; break; }
			}
			if( !f || f >= fin_ ) continue;
			Ecrit w3; compile_q( w3, "if NOT DesktopDemented\n" );
			m.push_back( Modif{ j[d].pos, 0, w3.o } );
			Ecrit w4; w4.eol(); compile_q( w4, "endif" );
			m.push_back( Modif{ j[f].pos, 0, w4.o } );
			++nb;
		}
		VLOG( "MUS", "pause menu: %d items hidden in Demented", nb );
		Ecrit w2; w2.eol(); compile_q( w2, s_demented_scripts );
		m.push_back( Modif{ j.back().pos, 0, w2.o } );
		VLOG( "MUS", "pause menu: Demented items added" );
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

// Main menu Options (create_setup_options_menu): "Build Portraits" before
// Credits. It clears the menu, poses every skater in turn for the portrait
// camera (desktop_portraits.cpp) and brings the menu back when done.
static const char *s_portrait_scripts =
	"script desktop_build_portraits\n"
	"if ScreenElementExists id = current_menu_anchor\n"
	"DestroyScreenElement id = current_menu_anchor\n"
	"endif\n"
	"KillSpawnedScript name = skaterDoOptionsTricks\n"
	"Change in_options_menu = 0\n"
	"select_skater_get_current_skater_name\n"
	"if NOT DesktopBuildPortraits restore = <current_skater> <...>\n"
	"launch_setup_options_menu\n"
	"desktop_toast text = \"No new skaters\"\n"
	"endif\n"
	"endscript\n"
	"script desktop_portrait_pose\n"
	"SelectCurrentSkater name = <name>\n"
	"GetCurrentSkaterProfileIndex\n"
	"RefreshSkaterModel profile = <currentSkaterProfileIndex> skater = 0 no_board = no_board\n"
	"MakeSkaterGoto SkateshopAI params = { }\n"
	"skater : SwitchOffBoard\n"
	"endscript\n"
	// a few frames later (desktop_portraits.cpp): Gene, Iron Man and THUD
	// have their own entrance in SkateshopAI; everyone stands in the idle
	"script desktop_portrait_idle\n"
	"KillSpawnedScript name = wait_start_kill_ironman_dust\n"
	"kill name = manhole_cover_smoke\n"
	"kill name = Gene_Simmons_pyro\n"
	"kill name = Gene_Simmons_pyro01\n"
	"kill name = Iron_Man_Dust_1\n"
	"skater : unhide\n"
	"skater : PlayAnim Anim = Ped_M_Idle1 BlendPeriod = 0\n"
	"skater : CancelRotateDisplay\n"
	"skater : Obj_SetLightAmbientColor r = 73 g = 70 b = 80\n"
	"skater : Obj_SetLightDiffuseColor Index = 0 r = 110 g = 125 b = 155\n"
	"skater : Obj_SetLightDiffuseColor Index = 1 r = 10 g = 3 b = 5\n"
	"skater : Obj_SetLightDirection Index = 0 heading = skater_select_light0_heading pitch = 350\n"
	"skater : Obj_SetLightDirection Index = 1 heading = skater_select_light1_heading pitch = 270\n"
	"endscript\n"
	"script desktop_portrait_progress\n"
	"if ScreenElementExists id = desktop_portrait_text\n"
	"SetScreenElementProps id = desktop_portrait_text text = <text>\n"
	"else\n"
	"CreateScreenElement { Type = TextElement parent = root_window id = desktop_portrait_text font = small text = <text> pos = (620,440) just = [ right bottom ] scale = 0.8 rgba = [ 128 128 128 128 ] z_priority = 100 }\n"
	"endif\n"
	"endscript\n"
	"script desktop_portrait_done\n"
	"if ScreenElementExists id = desktop_portrait_text\n"
	"DestroyScreenElement id = desktop_portrait_text\n"
	"endif\n"
	"launch_setup_options_menu\n"
	"desktop_toast text = <text>\n"
	"endscript\n";

static void patch_portraits_menu( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "create_setup_options_menu" ), cible = crc( "options_credits" ), item = crc( "theme_menu_add_item" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT || !est_nom( qb, j[k + 1], n )) continue;
		size_t b = k + 2;
		while( b < j.size() && qb[j[b].pos] != T_ENDSCRIPT && !est_nom( qb, j[b], cible )) ++b;
		if( b >= j.size() || qb[j[b].pos] == T_ENDSCRIPT ) { VLOG( "MUS", "options menu: Credits not found" ); return; }
		while( b > k && !est_nom( qb, j[b], item )) --b;
		if( b == k ) return;
		Ecrit w1; compile_q( w1, s_portrait_scripts );
		m.push_back( Modif{ j[k].pos, 0, w1.o } );
		Ecrit w2; compile_q( w2,
			"theme_menu_add_item { text = \"Build New Portraits\" id = options_desktop_portraits pad_choose_script = desktop_build_portraits centered no_bg }\n"
			"theme_menu_add_item { text = \"Rebuild All Portraits\" id = options_desktop_portraits_all pad_choose_script = desktop_build_portraits pad_choose_params = { all } centered no_bg }\n" );
		m.push_back( Modif{ j[b].pos, 0, w2.o } );
		VLOG( "MUS", "options menu: Build Portraits added" );
		return;
	}
}

// Skater select head strip: a built portrait replaces SS_<first_name>.
static void patch_portraits_select( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "select_skater_menu_add_hmenu_items" ), ft = crc( "FormatText" ), hs = crc( "headshot" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT || !est_nom( qb, j[k + 1], n )) continue;
		int nb = 0;
		for( size_t b = k + 2; b + 3 < j.size() && qb[j[b].pos] != T_ENDSCRIPT; ++b )
		{
			if( !est_nom( qb, j[b], ft ) || !est_nom( qb, j[b + 3], hs )) continue;
			size_t f = b;
			while( f < j.size() && qb[j[f].pos] != T_EOL && qb[j[f].pos] != T_EOLNUM ) ++f;
			if( f >= j.size()) break;
			Ecrit w; compile_q( w, "DesktopPortrait name = <name>\n" );
			m.push_back( Modif{ j[f].pos + j[f].taille, 0, w.o } );
			++nb;
			b = f;
		}
		// The created skater's ss_cas: DesktopPortrait name = custom puts the
		// built portrait over that picture, before its element is made.
		const uint32_t cas = crc( "ss_cas" ), cree = crc( "CreateScreenElement" );
		for( size_t b = k + 2; b < j.size() && qb[j[b].pos] != T_ENDSCRIPT; ++b )
		{
			if( !est_nom( qb, j[b], cas )) continue;
			size_t d = b;
			while( d > k && !est_nom( qb, j[d], cree )) --d;
			if( d == k ) break;
			Ecrit w; compile_q( w, "DesktopPortrait name = custom\n" );
			m.push_back( Modif{ j[d].pos, 0, w.o } );
			++nb;
			break;
		}
		VLOG( "MUS", "skater select: %d portrait hooks", nb );
		return;
	}
}

// Story Mode menu (create_career_options_menu, skateshop.qb): the Pro Skater
// 1-4 careers listed greyed out (not ported yet) above an Underground label,
// ahead of THUG's own Level Select / Continue / Start New / Load items.
static bool niveau_installe( const char *dossier );
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
		// PRO SKATER 4: a career-mode game at College (THPS4's goals only run
		// in career mode), once the converted College is installed.
		const bool th4 = niveau_installe( "Sch" );
		if( th4 )
		{
			Ecrit w0;
			compile_q( w0,
				"script desktop_th4_career\n"
				"change_gamemode_career\n"
				// its own goal memory and save (desktop_th4_glue.cpp)
				"DesktopTH4Career on\n"
				// no THUG story startup menu: it parks the skater (physics
				// paused, out of the world, start key off) and never showed
				"change show_career_startup_menu = 0\n"
				"story_options_change_level level = load_TH4_Sch\n"
				"endscript\n" );
			m.push_back( Modif{ j[k].pos, 0, w0.o } );
		}
		Ecrit w;
		compile_q( w,
			"make_main_menu_item { text = \"PRO SKATER 1\" pad_choose_script = null_script not_focusable = not_focusable }\n"
			"make_main_menu_item { text = \"PRO SKATER 2\" pad_choose_script = null_script not_focusable = not_focusable }\n"
			"make_main_menu_item { text = \"PRO SKATER 3\" pad_choose_script = null_script not_focusable = not_focusable }\n" );
		compile_q( w, th4
			? "make_main_menu_item { text = \"PRO SKATER 4\" pad_choose_script = career_options_menu_exit pad_choose_params = { new_menu_script = desktop_th4_career kill_all } }\n"
			: "make_main_menu_item { text = \"PRO SKATER 4\" pad_choose_script = null_script not_focusable = not_focusable }\n" );
		compile_q( w,
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
	{ "Ped_M_NYPD_01", "M NYPD 1", 0, "ped_body = { desc_id = #343bf7cb } Stocky_scale_info ped_board = { desc_id = Skateboard }" },
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
	{ "Ped_Clemens", "Clemens", 0, "ped_m_head = { desc_id = Ped_Clemens_head } ped_m_torso = { desc_id = Torso_Wht_Tshirt_03 } ped_m_legs = { randomized_desc_id } ped_board = { desc_id = Skateboard }" },
	{ "Ped_AfroJim", "AfroJim", 0, "ped_m_head = { desc_id = Ped_AfroJim_head } ped_m_torso = { desc_id = Torso_Wht_Tshirt_04 } ped_m_legs = { randomized_desc_id } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Doorman_01", "Doorman 1", 0, "ped_body = { desc_id = #488bbc83 } Stocky_scale_info ped_board = { desc_id = Skateboard }" },
	{ "Ped_Deskclerk_01", "Deskclerk 1", 0, "ped_body = { desc_id = #de1ed9db } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Leafblower", "Leafblower", 0, "ped_body = { desc_id = Ped_Leafblower } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Security_01", "Security 1", 0, "ped_m_head = { desc_id = Ped_Security_Head01 } ped_body = { desc_id = Ped_Security_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Security_02", "Security 2", 0, "ped_m_head = { desc_id = Ped_Security_Head02 } ped_body = { desc_id = Ped_Security_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Security_03", "Security 3", 0, "ped_m_head = { desc_id = Ped_Security_Head03 } ped_body = { desc_id = Ped_Security_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_Security", "F Security", 1, "ped_body = { desc_id = Ped_F_Security } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Gardener_01", "Gardener 1", 0, "ped_m_head = { desc_id = Ped_Gardener_Head01 } ped_body = { desc_id = Ped_Gardener_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Gardener_02", "Gardener 2", 0, "ped_m_head = { desc_id = Ped_Gardener_Head02 } ped_body = { desc_id = Ped_Gardener_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_F_Gardener", "F Gardener", 1, "ped_body = { desc_id = Ped_F_Gardener } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KGB_01", "KGB 1", 0, "ped_m_head = { desc_id = Ped_KGB_Head01 } ped_body = { desc_id = Ped_KGB_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KGB_02", "KGB 2", 0, "ped_m_head = { desc_id = Ped_KGB_Head02 } ped_body = { desc_id = Ped_KGB_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_RGuard_01", "RGuard 1", 0, "ped_m_head = { desc_id = Ped_RGuard_Head01 } ped_body = { desc_id = Ped_RGuard_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_RGuard_02", "RGuard 2", 0, "ped_m_head = { desc_id = Ped_RGuard_Head02 } ped_body = { desc_id = Ped_RGuard_Body } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Cameraman", "Cameraman", 0, "ped_body = { desc_id = Ped_Cameraman } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Official_SC_01", "Official SC 1", 0, "ped_m_head = { desc_id = Head_OfficialA } ped_body = { desc_id = #0a3260e4 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Official_SC_02", "Official SC 2", 0, "ped_m_head = { desc_id = Head_OfficialB } ped_body = { desc_id = #0a3260e4 } ped_board = { desc_id = Skateboard }" },
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
	{ "Ped_Bartender", "Bartender", 0, "ped_m_head = { desc_id = Head_TBarber } ped_m_torso = { desc_id = Torso_wht_hawaii } ped_m_legs = { desc_id = Legs_Cargos_02 } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Film", "Film", 0, "ped_body = { desc_id = Ped_Film } ped_board = { desc_id = Skateboard }" },
	{ "Ped_Surfer", "Surfer", 0, "ped_body = { desc_id = Ped_Surfer } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KISS_Paul", "KISS Paul", 0, "ped_body = { desc_id = Ped_KISS_Paul } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KISS_Peter", "KISS Peter", 0, "ped_body = { desc_id = Ped_KISS_Peter } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KISS_Ace", "KISS Ace", 0, "ped_body = { desc_id = Ped_KISS_Ace } ped_board = { desc_id = Skateboard }" },
	{ "Ped_KISS_Gene", "KISS Gene", 0, "ped_body = { desc_id = Ped_KISS_Gene } ped_board = { desc_id = Skateboard }" },
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

// GTA San Andreas peds (desktop, Marcus's own models): converted from his
// XNALara .xps rips by desktop/tools/xps_to_thug.py into
// data/models/gta/<name>/. Each one, when its files are there, becomes a
// ped_body part (cas_ped_m.qb) and a pro skater after THUG's peds. GTA's
// women are converted at a man's height and shrunk by female_scale_info.
struct PersoGTA { const char *nom, *affiche; int femme; const char *dossier; float taille; };
static const PersoGTA s_gta[] = {
	{ "andre", "Andre", 0 },
	{ "male01", "GTA MALE01", 0 },
	{ "bfost", "GTA BFOST", 1 },
	{ "bmori", "GTA BMORI", 0 },
	{ "bmost", "GTA BMOST", 0 },
	{ "bmyap", "GTA BMYAP", 0 },
	{ "bmybu", "GTA BMYBU", 0 },
	{ "bmybe", "GTA BMYBE", 0 },
	{ "bmydj", "GTA BMYDJ", 0 },
	{ "bmyri", "GTA BMYRI", 0 },
	{ "bmycr", "GTA BMYCR", 0 },
	{ "bmyst", "GTA BMYST", 0 },
	{ "wmybmx", "GTA WMYBMX", 0 },
	{ "wbdyg1", "GTA WBDYG1", 0 },
	{ "wbdyg2", "GTA WBDYG2", 0 },
	{ "wmybp", "GTA WMYBP", 0 },
	{ "wmycon", "GTA WMYCON", 0 },
	{ "bmydrug", "GTA BMYDRUG", 0 },
	{ "wmydrug", "GTA WMYDRUG", 0 },
	{ "hmydrug", "GTA HMYDRUG", 0 },
	{ "dwmolc1", "GTA DWMOLC1", 0 },
	{ "dwmolc2", "GTA DWMOLC2", 0 },
	{ "dwfolc", "GTA DWFOLC", 1 },
	{ "dwmylc1", "GTA DWMYLC1", 0 },
	{ "hmogar", "GTA HMOGAR", 0 },
	{ "wmygol1", "GTA WMYGOL1", 0 },
	{ "wmygol2", "GTA WMYGOL2", 0 },
	{ "hfori", "GTA HFORI", 1 },
	{ "hfost", "GTA HFOST", 1 },
	{ "hmori", "GTA HMORI", 0 },
	{ "hmybe", "GTA HMYBE", 0 },
	{ "hmyri", "GTA HMYRI", 0 },
	{ "hmycr", "GTA HMYCR", 0 },
	{ "hmyst", "GTA HMYST", 0 },
	{ "omokung", "GTA OMOKUNG", 0 },
	{ "wmymech", "GTA WMYMECH", 0 },
	// GTA SA extras (Marcus's "GTA EXTRAS" rips): rigged .dae by
	// desktop/tools/gtadae_to_thug.py, plain .obj rigged off the closest .dae by
	// desktop/tools/gtaobj_to_thug.py; data/models/gta/<name>/.
	{ "ogloc", "GTA OG Loc", 0 },
	{ "dnb3", "GTA Da Nang Boy 3", 0 },
	// THPS2 skaters (Marcus's own DAE rips, desktop/tools/dae_to_thug.py),
	// data/models/thps2/<name>/.
	{ "andrewreynolds", "THPS2 Andrew Reynolds", 0, "thps2" },
	{ "andrewreynolds_styleb", "THPS2 Andrew Reynolds B", 0, "thps2" },
	{ "bobburnquist", "THPS2 Bob Burnquist", 0, "thps2" },
	{ "bobburnquist_styleb", "THPS2 Bob Burnquist B", 0, "thps2" },
	{ "buckylasek", "THPS2 Bucky Lasek", 0, "thps2" },
	{ "buckylasek_styleb", "THPS2 Bucky Lasek B", 0, "thps2" },
	{ "chadmuska", "THPS2 Chad Muska", 0, "thps2" },
	{ "chadmuska_styleb", "THPS2 Chad Muska B", 0, "thps2" },
	{ "erickoston", "THPS2 Eric Koston", 0, "thps2" },
	{ "erickoston_styleb", "THPS2 Eric Koston B", 0, "thps2" },
	{ "jamiethomas", "THPS2 Jamie Thomas", 0, "thps2" },
	{ "jamiethomas_styleb", "THPS2 Jamie Thomas B", 0, "thps2" },
	{ "kareemcampbell", "THPS2 Kareem Campbell", 0, "thps2" },
	{ "kareemcampbell_styleb", "THPS2 Kareem Campbell B", 0, "thps2" },
	{ "rodneymullen", "THPS2 Rodney Mullen", 0, "thps2" },
	{ "rodneymullen_styleb", "THPS2 Rodney Mullen B", 0, "thps2" },
	{ "runeglifberg", "THPS2 Rune Glifberg", 0, "thps2" },
	{ "runeglifberg_styleb", "THPS2 Rune Glifberg B", 0, "thps2" },
	{ "stevecaballero", "THPS2 Steve Caballero", 0, "thps2" },
	{ "stevecaballero_styleb", "THPS2 Steve Caballero B", 0, "thps2" },
	{ "hawk2", "THPS2 Tony Hawk", 0, "thps2" },
	{ "hawk2b", "THPS2 Tony Hawk B", 0, "thps2" },
	{ "secret4", "THPS2 Tony Hawk C", 0, "thps2" },
	{ "officerdick", "THPS2 Officer Dick", 0, "thps2" },
	{ "officerdick_styleb", "THPS2 Officer Dick B", 0, "thps2" },
	{ "privatecarrera", "THPS2 Private Carrera", 1, "thps2" },
	{ "spiderman", "THPS2 Spider-Man", 0, "thps2" },
	{ "spiderman_styleb", "THPS2 Spider-Man B", 0, "thps2" },
	{ "spiderman_stylec", "THPS2 Spider-Man C", 0, "thps2" },
	{ "spiderman_styled", "THPS2 Spider-Man D", 0, "thps2" },
	// Red Dead Revolver characters (Marcus's PS2 disc, SWAT.DAT,
	// desktop/tools/rdr_to_thug.py), data/models/rdr/<name>/. Unnamed ones keep
	// their game file code.
	{ "smileyfawler", "RDR Smiley Fawler", 0, "rdr" },
	{ "dan", "RDR Dan", 0, "rdr" },
	{ "buffalosoldier", "RDR Buffalo Soldier", 0, "rdr" },
	{ "cooper", "RDR Cooper", 0, "rdr" },
	{ "mrblack", "RDR Mr. Black", 0, "rdr" },
	{ "rdr_brb", "RDR BRB", 0, "rdr" },
	{ "rdr_brc", "RDR BRC", 0, "rdr" },
	{ "rdr_bsd", "RDR BSD", 0, "rdr" },
	{ "rdr_bt1", "RDR BT1", 0, "rdr" },
	{ "rdr_bt2", "RDR BT2", 0, "rdr" },
	{ "clydetheblade", "RDR Clyde 'The Blade' Slade", 0, "rdr" },
	{ "christina", "RDR Christina", 1, "rdr" },
	{ "nateharlow", "RDR Nate Harlow", 0, "rdr" },
	{ "generaldiego", "RDR General Diego", 0, "rdr" },
	{ "rdr_dvy", "RDR DVY", 0, "rdr" },
	{ "bloodytom", "RDR Bloody Tom", 0, "rdr" },
	{ "charliehancock", "RDR Charlie 'Giggolo' Hancock", 0, "rdr" },
	{ "rdr_gip", "RDR GIP", 0, "rdr" },
	{ "rdr_gsg", "RDR GSG", 0, "rdr" },
	{ "rdr_gsh", "RDR GSH", 0, "rdr" },
	{ "rdr_gsw", "RDR GSW", 0, "rdr" },
	{ "hedgehogthornton", "RDR Hedgehog Thornton", 0, "rdr" },
	{ "jackswift", "RDR Jack Swift", 0, "rdr" },
	{ "sheriffbartlett", "RDR Sheriff Bartlett", 0, "rdr" },
	{ "rdr_kya", "RDR KYA", 0, "rdr" },
	{ "fallingstar", "RDR Falling Star", 1, "rdr" },
	{ "udokang", "RDR Udo Kang", 0, "rdr" },
	{ "youngred", "RDR Young Red", 0, "rdr" },
	{ "mrkelly", "RDR Mr. Kelly", 0, "rdr" },
	{ "cornetbrother", "RDR Cornet Brother", 0, "rdr" },
	{ "standingsnake", "RDR Standing Snake", 0, "rdr" },
	{ "smitty", "RDR Smitty", 0, "rdr" },
	{ "rdr_np1", "RDR NP1", 0, "rdr" },
	{ "rdr_np2", "RDR NP2", 0, "rdr" },
	{ "rdr_np4", "RDR NP4", 0, "rdr" },
	{ "rdr_npa", "RDR NPA", 1, "rdr" },
	{ "natlie", "RDR Natlie", 1, "rdr" },
	{ "rdr_owb", "RDR OWB", 0, "rdr" },
	{ "rdr_own02", "RDR OWN02", 0, "rdr" },
	{ "rdr_own03", "RDR OWN03", 0, "rdr" },
	{ "professorperry", "RDR Professor Perry", 0, "rdr" },
	{ "redharlow", "RDR Red Harlow", 0, "rdr" },
	{ "twiggy", "RDR Twiggy", 0, "rdr" },
	// 2026-11-10 pack (Marcus's "THUG models for conversion"): GTA SA/VC story cast
	// (xps_to_thug.py), GTA III, The Warriors, Half-Life, Kingdom Hearts
	// (gtaobj_to_thug.py), the Simpsons (tpose_to_thug.py); data/models/gta/<name>/.
	// The last field shrinks the whole skater (body_shape scale): kids, cartoon heads.
	{ "burgershot", "Burger Shot Guy", 0 },
	{ "catalina", "Catalina", 1 },
	{ "cesar", "Cesar Vialpando", 0 },
	{ "claude", "Claude", 0 },
	{ "dwayne", "Dwayne", 0 },
	{ "emmet", "Emmet", 0 },
	{ "farlie", "Farlie", 0 },
	{ "grove1", "Grove Street Member", 0 },
	{ "heckler1", "Heckler 1", 0 },
	{ "heckler2", "Heckler 2", 0 },
	{ "janitor", "Janitor", 0 },
	{ "jethro", "Jethro", 0 },
	{ "jizzy", "Jizzy B", 0 },
	{ "kentpaul", "Kent Paul", 0 },
	{ "maccer", "Maccer", 0 },
	{ "madddogg", "Madd Dogg", 0 },
	{ "oglocburger", "OG Loc B", 0 },
	{ "salvatore", "Salvatore Leone", 0 },
	{ "sindacco", "Johnny Sindacco", 0 },
	{ "suzie", "Su Xi Mu", 0 },
	{ "sweet", "Sweet", 0 },
	{ "tbone", "T-Bone Mendez", 0 },
	{ "truth", "The Truth", 0 },
	{ "triada", "Triad A", 0 },
	{ "triadb", "Triad B", 0 },
	{ "triadc", "Triad C", 0 },
	{ "wuzimu", "Woozie", 0 },
	{ "alexshrub", "Alex Shrub", 0 },
	{ "alexshrub_b", "Alex Shrub B", 0 },
	{ "avery", "Avery Carrington", 0 },
	{ "camjones", "Cam Jones", 0 },
	{ "colonel", "Colonel Cortez", 0 },
	{ "diaz", "Ricardo Diaz", 0 },
	{ "vc_dick", "Dick", 0 },
	{ "director", "Steve Scott", 0 },
	{ "donaldlove", "Donald Love", 0 },
	{ "dwayne_b", "Dwayne B", 0 },
	{ "gonzalez", "Gonzalez", 0 },
	{ "hilary", "Hilary King", 0 },
	{ "jethro_b", "Jethro B", 0 },
	{ "jezz", "Jezz Torrent", 0 },
	{ "kelly", "Kelly", 0 },
	{ "kentpaul_b", "Kent Paul B", 0 },
	{ "lance", "Lance Vance", 0 },
	{ "mitch", "Mitch Baker", 0 },
	{ "papa", "Papa", 0 },
	{ "percy", "Percy", 0 },
	{ "rosenberg_b", "Ken Rosenberg B", 0 },
	{ "sonny", "Sonny Forelli", 0 },
	{ "tommy", "Tommy Vercetti", 0 },
	{ "umberto", "Umberto Robina", 0 },
	{ "einstein", "Einstein", 0 },
	{ "luther", "Luther", 0 },
	{ "slick", "Slick", 0 },
	{ "walter", "Walter", 0 },
	{ "wr_abe", "Abe", 0 },
	{ "wr_ace", "Ace", 0 },
	{ "wr_aj", "AJ", 0 },
	{ "wr_ajax", "Ajax", 0 },
	{ "wr_apephibian", "Ape Phibian", 0 },
	{ "wr_ash", "Ash", 0 },
	{ "wr_bj", "BJ", 0 },
	{ "wr_bullet", "Bullet", 0 },
	{ "wr_charlie", "Charlie", 0 },
	{ "wr_cleon", "Cleon", 0 },
	{ "wr_cobb", "Cobb", 0 },
	{ "wr_cochise", "Cochise", 0 },
	{ "wr_cody", "Cody", 0 },
	{ "wr_cooper", "Cooper B", 0 },
	{ "wr_cowboy", "Cowboy", 0 },
	{ "wr_cstyle", "C-Style", 0 },
	{ "wr_darien", "Darien", 0 },
	{ "wr_deke", "Deke", 0 },
	{ "wr_ed", "Ed", 0 },
	{ "wr_fox", "Fox", 0 },
	{ "wr_glenn", "Glenn", 0 },
	{ "wr_goober", "Goober", 0 },
	{ "wr_grimes", "Grimes", 0 },
	{ "wr_gus", "Gus", 0 },
	{ "wr_gypsy", "Gypsy", 1 },
	{ "wr_hog", "Hog", 0 },
	{ "wr_hornet", "Hornet", 0 },
	{ "wr_hotdogjimmy", "Hot Dog Jimmy", 0 },
	{ "wr_isaac", "Isaac", 0 },
	{ "wr_jerome", "Jerome", 0 },
	{ "wr_jonah", "Jonah", 0 },
	{ "wr_jones", "Jones", 0 },
	{ "wr_keenan", "Keenan", 0 },
	{ "wr_kevink", "Kevin K", 0 },
	{ "wr_lamont", "Lamont", 0 },
	{ "wr_lumpy", "Lumpy", 0 },
	{ "wr_luna51", "Luna 51", 0 },
	{ "wr_lynx", "Lynx", 0 },
	{ "wr_malcolm", "Malcolm", 0 },
	{ "wr_maurice", "Maurice", 0 },
	{ "wr_rembrandt", "Rembrandt", 0 },
	{ "wr_rhino", "Rhino", 0 },
	{ "wr_royce", "Royce", 0 },
	{ "wr_ryan", "Ryan", 0 },
	{ "wr_screetch", "Screetch", 0 },
	{ "wr_seal", "Seal", 0 },
	{ "wr_skinnypete", "Skinny Pete", 0 },
	{ "wr_slim", "Slim", 0 },
	{ "wr_snow", "Snow", 0 },
	{ "wr_spazz", "Spazz", 0 },
	{ "wr_spice", "Spice", 0 },
	{ "wr_spider", "Spider", 0 },
	{ "wr_stefano", "Stefano", 0 },
	{ "wr_swan", "Swan", 0 },
	{ "wr_ted", "Ted", 0 },
	{ "wr_theconeykid", "The Coney Kid", 0 },
	{ "wr_theo", "Theo", 0 },
	{ "wr_thurman", "Thurman", 0 },
	{ "wr_tiny", "Tiny", 0 },
	{ "wr_twiggy", "Twiggy B", 0 },
	{ "wr_vance", "Vance", 0 },
	{ "wr_vermin", "Vermin", 0 },
	{ "wr_west", "West", 0 },
	{ "wr_zack", "Zack", 0 },
};
static const char *gta_dossier( const PersoGTA &g ) { return g.dossier ? g.dossier : "gta"; }
enum { NB_GTA = sizeof( s_gta ) / sizeof( s_gta[0] ) };

static bool gta_installe( const PersoGTA &g )
{
	char vita[256], chemin[1024];
	snprintf( vita, sizeof( vita ), "ux0:data/thug/Data/models/%s/%s/%s.skin.xbx", gta_dossier( g ), g.nom, g.nom );
	desktop_host_path( vita, chemin, sizeof( chemin ));
	FILE *f = fopen( chemin, "rb" );
	if( f ) fclose( f );
	return f != NULL;
}

// The installed ones, looked up once.
static const std::vector<int> &gta_presents( void )
{
	static std::vector<int> v;
	static bool vu = false;
	if( !vu )
	{
		vu = true;
		for( int i = 0; i < NB_GTA; ++i ) if( gta_installe( s_gta[i] )) v.push_back( i );
		VLOG( "MUS", "GTA skaters: %d of %d installed", (int)v.size(), (int)NB_GTA );
	}
	return v;
}

// ped_body = [ ... ] (cas_ped_m.qb): the GTA bodies go first.
static void patch_gta_corps( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "ped_body" );
	for( size_t k = 0; k + 2 < j.size(); ++k )
	{
		if( !est_nom( qb, j[k], n ) || qb[j[k + 1].pos] != T_EGAL ) continue;
		size_t a = sans_eol( qb, j, k + 2 );
		if( a >= j.size() || qb[j[a].pos] != T_TAB ) continue;
		const std::vector<int> &g = gta_presents();
		if( g.empty()) return;
		Ecrit w;
		for( size_t i = 0; i < g.size(); ++i )
		{
			char t[256];
			snprintf( t, sizeof( t ), "{ desc_id = desktop_gta_%s mesh = \"models/%s/%s/%s.skin\" no_random }\n",
			          s_gta[g[i]].nom, gta_dossier( s_gta[g[i]] ), s_gta[g[i]].nom, s_gta[g[i]].nom );
			compile_q( w, t );
		}
		m.push_back( Modif{ j[a].pos + j[a].taille, 0, w.o } );
		VLOG( "MUS", "ped_body: %d GTA bodies added", (int)g.size());
		return;
	}
}

// Pro-style profile for each extra skater (gen_skater_meta.py from
// skater_roster.py): own name, stats, trick list, specials, deck and voice.
struct MetaSkater
{
	const char *nom, *affiche, *prenom;
	int age;
	const char *ville, *stance, *pousse, *style;
	int stats[10];
	const char *tricks, *specials, *planche;
	int voix;		// bail vocal pitch in percent, 0 = silent like the real pros
};
#include "skater_meta.inc"

static const MetaSkater *meta_de( const char *nom )
{
	for( const MetaSkater &m : s_meta )
		if( !strcmp( m.nom, nom )) return &m;
	return NULL;
}

// Profile name checksum (desktop_ped_N) -> bail vocal pitch.
static std::vector<std::pair<uint32_t, int>> s_voix;

// Bail voice clips (data/voices/<skater>.txt from voice_banks.py): "pitch N"
// then one clip per line, relative to data/voices.
struct BanqueVoix { uint32_t nom; int pitch; std::vector<std::string> clips; int dernier; };
static std::vector<BanqueVoix> s_banques;

static void charge_banque( uint32_t nom, const char *cle )
{
	char vita[256], chemin[1024];
	snprintf( vita, sizeof( vita ), "ux0:data/thug/Data/voices/%s.txt", cle );
	desktop_host_path( vita, chemin, sizeof( chemin ));
	FILE *f = fopen( chemin, "rb" );
	if( !f )
	{
		static int n_log;
		if( n_log++ < 3 ) VLOG( "PCM", "bail voices: no %s", chemin );
		return;
	}
	BanqueVoix b = { nom, 100, {}, -1 };
	char l[512];
	while( fgets( l, sizeof( l ), f ))
	{
		size_t n = strlen( l );
		while( n && ( l[n - 1] == '\n' || l[n - 1] == '\r' || l[n - 1] == ' ' )) l[--n] = 0;
		if( !n || l[0] == '#' ) continue;
		if( !strncmp( l, "pitch ", 6 )) { b.pitch = atoi( l + 6 ); continue; }
		if( strchr( l, '"' ) || strstr( l, ".." )) continue;
		b.clips.push_back( std::string( "ux0:data/thug/Data/voices/" ) + l );
	}
	fclose( f );
	if( !b.clips.empty()) s_banques.push_back( b );
}

static BanqueVoix *banque_de( unsigned int nom )
{
	for( BanqueVoix &b : s_banques )
		if( b.nom == nom ) return &b;
	return NULL;
}

extern "C" int custom_skater_voice_pitch( unsigned int nom )
{
	if( const BanqueVoix *b = banque_de( nom )) return b->pitch > 0 ? b->pitch : 100;
	for( const auto &v : s_voix )
		if( v.first == nom ) return v.second;
	return 0;
}

extern "C" int custom_skater_voice_clip( unsigned int nom, char *out, int taille )
{
	BanqueVoix *b = banque_de( nom );
	if( !b || !out || taille <= 0 ) return 0;
	const int n = (int)b->clips.size();
	int k = rand() % n;
	if( n > 1 && k == b->dernier ) k = ( k + 1 + rand() % ( n - 1 )) % n;	// never the same twice running
	b->dernier = k;
	snprintf( out, taille, "%s", b->clips[k].c_str());
	return 1;
}

static std::string profil_pieton( int i, const PietonJouable &p, const MetaSkater *m )
{
	static const char *noms[10] = { "air", "run", "ollie", "speed", "spin", "switch",
		"flip_speed", "rail_balance", "lip_balance", "manual_balance" };
	static const MetaSkater defaut = { "", "", "Ped", -1, "", "regular", "mongo_when_switch", "street",
		{ 6, 6, 6, 6, 6, 6, 6, 6, 6, 6 }, "WolverineTricks",
		"{ trickslot = SpAir_L_D_Circle trickname = Trick_1234 } { trickslot = SpAir_R_U_Circle trickname = Trick_The900 } "
		"{ trickslot = SpGrind_U_D_Triangle trickname = Trick_FlipKickDad } { trickslot = SpMan_L_R_Triangle trickname = Trick_OneFootOneWheel }",
		"", 0 };
	const MetaSkater &d = m ? *m : defaut;
	std::string stats;
	for( int k = 0; k < 10; ++k )
	{
		char t[40];
		snprintf( t, sizeof( t ), "%s = %d ", noms[k], d.stats[k] );
		stats += t;
	}
	char tricks[64];
	if( d.tricks[0] == '{' ) snprintf( tricks, sizeof( tricks ), "desktop_tricks_%d", i );
	else snprintf( tricks, sizeof( tricks ), "%s", d.tricks );
	char t[4096];
	snprintf( t, sizeof( t ),
		"{ display_name = \"%s\" first_name = \"%s\" file_name = \"Unimplemented\" skater_index = %d\n"
		"default_appearance = desktop_pedlook_%d name = desktop_ped_%d stance = %s pushstyle = %s trickstyle = %s\n"
		"is_pro = 1 is_head_locked = 1 is_locked = 1 is_male = %d age = %d hometown = \"%s\" points_available = 0 desktop_headshot = \"ped\"\n"
		"%s\n"
		"sponsors = [ ] trick_mapping = { } default_trick_mapping = %s no_edit no_edit_groups = [ secret_options ]\n"
		"max_specials = 11 specials = { [\n"
		"%s\n"
		"{ trickslot = Unassigned trickname = Unassigned } { trickslot = Unassigned trickname = Unassigned }\n"
		"{ trickslot = Unassigned trickname = Unassigned } { trickslot = Unassigned trickname = Unassigned }\n"
		"{ trickslot = Unassigned trickname = Unassigned } { trickslot = Unassigned trickname = Unassigned }\n"
		"{ trickslot = Unassigned trickname = Unassigned } { trickslot = Unassigned trickname = Unassigned }\n"
		"] } }\n",
		m ? d.affiche : p.affiche, d.prenom, 100 + i, i, i, d.stance, d.pousse, d.style,
		p.femme ? 0 : 1, d.age, d.ville, stats.c_str(), tricks, d.specials );
	return t;
}

// A real board (deck graphic + griptape) in place of the ped prop board.
static std::string avec_planche( std::string a, const MetaSkater *m )
{
	if( !m || !m->planche[0] ) return a;
	static const char *prop = "ped_board = { desc_id = Skateboard }";
	const size_t k = a.find( prop );
	if( k != std::string::npos ) a.replace( k, strlen( prop ), m->planche );
	else a += std::string( " " ) + m->planche;
	return a;
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
		// GTA peds after THUG's, numbered on from them.
		const std::vector<int> &g = gta_presents();
		std::vector<std::string> looks;
		std::vector<PietonJouable> tous( s_pietons, s_pietons + nb );
		for( int i = 0; i < nb; ++i ) looks.push_back( s_pietons[i].apparence );
		for( size_t i = 0; i < g.size(); ++i )
		{
			const PersoGTA &p = s_gta[g[i]];
			PietonJouable pj = { p.nom, p.affiche, p.femme, "" };
			tous.push_back( pj );
			char t[256];
			char forme[64] = "";
			if( p.taille > 0 ) snprintf( forme, sizeof( forme ), " body_shape = { scale = %.2f }", p.taille );
			else if( p.femme ) strcpy( forme, " body_shape = female_scale_info" );
			snprintf( t, sizeof( t ), "ped_body = { desc_id = desktop_gta_%s } ped_board = { desc_id = Skateboard }%s",
			          p.nom, forme );
			looks.push_back( t );
		}
		Ecrit w1;
		Ecrit w2; w2.eol();
		s_voix.clear();
		s_banques.clear();
		int sans = 0;
		for( size_t i = 0; i < tous.size(); ++i )
		{
			const MetaSkater *me = meta_de( tous[i].nom );
			if( !me ) ++sans;
			compile_q( w1, profil_pieton( (int)i, tous[i], me ).c_str());
			// Not named like the profile: the select screen uses the profile name as a
			// screen element id, and a global struct of that name would replace it.
			std::string t = "desktop_pedlook_" + std::to_string( i ) + " = { " + avec_planche( looks[i], me ) + " }\n";
			if( me && me->tricks[0] == '{' )
				t += "desktop_tricks_" + std::to_string( i ) + " = " + me->tricks + "\n";
			compile_q( w2, t.c_str());
			const uint32_t nom_crc = crc(( "desktop_ped_" + std::to_string( i )).c_str());
			if( me && me->voix > 0 )
				s_voix.push_back( std::make_pair( nom_crc, me->voix ));
			charge_banque( nom_crc, tous[i].nom );
		}
		if( sans ) VLOG( "MUS", "skaters: %d without a profile in skater_meta.inc", sans );
		VLOG( "PCM", "bail voices: %d skaters with clips in data/voices", (int)s_banques.size());
		m.push_back( Modif{ j[a].pos, 0, w1.o } );
		m.push_back( Modif{ j.back().pos, 0, w2.o } );
		VLOG( "MUS", "skaters: %d peds + %d GTA added", nb, (int)g.size());
		return;
	}
}

// Bail vocals (animevents.qb): THUG only gives them to the create-a-skater
// ("ProfileEquals is_custom"). desktop_voiced also takes the extra skaters
// that have a voice (skater.cpp, pitched per skater in ScriptPlayStream).
static void patch_voix( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	static const char *scripts[] = { "HandleBailKneeStream", "HandleBailAnkleStream", "HandleBailKickStream" };
	const uint32_t c = crc( "is_custom" );
	for( const char *nom : scripts )
	{
		const uint32_t n = crc( nom );
		for( size_t k = 0; k + 1 < j.size(); ++k )
			if( qb[j[k].pos] == T_SCRIPT && est_nom( qb, j[k + 1], n ))
			{
				for( size_t b = k + 2; b < j.size() && qb[j[b].pos] != T_ENDSCRIPT; ++b )
					if( est_nom( qb, j[b], c ))
					{
						Ecrit w; w.nom( "desktop_voiced" );
						m.push_back( Modif{ j[b].pos, j[b].taille, w.o } );
						VLOG( "MUS", "%s: bail vocals for the extra skaters", nom );
					}
				break;
			}
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
	"SetGlobalFlag flag = SKATER_UNLOCKED_CREATURE\n"
	// Extra skaters' select-screen picture is SS_ped (headshot is "SS_" + first
	// name, and their real first names have no picture, so the sprite was lost).
	"if GotParam desktop_headshot\n"
	"<first_name> = <desktop_headshot>\n"
	"endif\n";

// Difficulty menu (create_career_difficulty_menu): Twisted and Demented
// after Sick, new stories only, before the description box is built.
static const char *s_difficultes =
	"if NOT GotParam from_ss_menu\n"
	"if GameModeEquals is_career\n"
	"make_main_menu_item { text = \"Twisted\" pad_choose_script = career_options_menu_exit pad_choose_params = { new_menu_script = set_career_difficulty dif = 2 desktop_twisted } }\n"
	"SetScreenElementProps { id = { current_menu child = 5 } event_handlers = [ { focus difficulty_menu_item_focus params = { description = \"Sick goals with every stat locked at 0 for the whole story\" } } { unfocus difficulty_menu_item_unfocus } ] replace_handlers }\n"
	"make_main_menu_item { text = \"Demented\" pad_choose_script = career_options_menu_exit pad_choose_params = { new_menu_script = desktop_demented_menu } }\n"
	"SetScreenElementProps { id = { current_menu child = 6 } event_handlers = [ { focus difficulty_menu_item_focus params = { description = \"Random skater, no saves. One run to beat it\" } } { unfocus difficulty_menu_item_unfocus } ] replace_handlers }\n"
	"endif\n"
	"endif\n";

static void patch_difficultes( const unsigned char *qb, const std::vector<Jeton> &j, std::vector<Modif> &m )
{
	const uint32_t n = crc( "create_career_difficulty_menu" ), barre = crc( "bar_color" );
	for( size_t k = 0; k + 1 < j.size(); ++k )
	{
		if( qb[j[k].pos] != T_SCRIPT || !est_nom( qb, j[k + 1], n )) continue;
		size_t b = k + 2;
		for( ; b < j.size() && qb[j[b].pos] != T_ENDSCRIPT; ++b )
			if( est_nom( qb, j[b], barre )) break;
		if( b >= j.size() || qb[j[b].pos] == T_ENDSCRIPT ) { VLOG( "MUS", "difficulty menu: bar_color not found" ); return; }
		while( b > k && qb[j[b].pos] != T_EOL && qb[j[b].pos] != T_EOLNUM ) --b;
		Ecrit w; compile_q( w, s_difficultes );
		m.push_back( Modif{ j[b].pos + j[b].taille, 0, w.o } );
		VLOG( "MUS", "difficulty menu: Twisted and Demented added" );
		return;
	}
}

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
	patch_portraits_menu( qb, j, m );
	patch_portraits_select( qb, j, m );
	patch_pause_extras( qb, j, m );
	patch_pause_live( qb, j, m );
	patch_view_tricks( qb, j, m );
	wrap_lignes( qb, j, m, "create_edit_tricks_menu", "PauseMusicAndStreams", NULL );
	wrap_lignes( qb, j, m, "goal_fail", "create_speech_box", NULL );	// "Press START to retry goal"
	// Demented: the music plays on in the pause menu (the game doesn't
	// freeze either), and no cheats in Options
	wrap_lignes( qb, j, m, "create_pause_menu", "PauseMusicAndStreams", NULL );
	wrap_lignes( qb, j, m, "create_options_menu", "make_theme_menu_item", "menu_real_cheats" );
	wrap_lignes( qb, j, m, "create_options_menu", "make_theme_menu_item", "menu_cheats" );
	// Demented (Marcus): no Watch Cutscenes or Game Progress, and the sound
	// options leave the music playing
	wrap_lignes( qb, j, m, "create_options_menu", "make_theme_menu_item", "menu_watch_cutscenes" );
	wrap_lignes( qb, j, m, "create_options_menu", "make_theme_menu_item", "menu_game_progress" );
	{
		static const char *son[] = { "create_sound_options_menu", "sound_options_exit", "focus_skip_track", "unfocus_skip_track",
			"menu_music_level_focus", "menu_music_level_unfocus", "menu_music_special_focus", "menu_music_special_unfocus",
			"skip_track_focus", "skip_track_unfocus", "playlist_menu_unfocus" };
		for( size_t i = 0; i < sizeof( son ) / sizeof( son[0] ); ++i ) wrap_lignes( qb, j, m, son[i], "PauseMusic", NULL );
	}
	patch_histoire( qb, j, m );
	// Use the existing save selection with the desktop filesystem-backed card.
	patch_debut( qb, j, m, "maybe_auto_load_from_memory_card",
		"if DesktopFirstSaveCheck\n"
		"change DoAutoload = 0\n"
		"SpawnScript wait_and_pause_skater params = { time = 1 }\n"
		"CheckForCardOnBootup\n"
		"return\nendif\nchange DoAutoload = 0\n" );
	patch_debut(qb, j, m, "create_main_menu", "change attempted_xbox_autoload = 1\n");
	patch_debut(qb, j, m, "xbox_autoload_message",
		"change attempted_xbox_autoload = 1\nlaunch_main_menu\nreturn\n");
	patch_debut( qb, j, m, "career_autoload_launch_current_level",
		"change show_career_startup_menu = 1\n" );
	patch_debut( qb, j, m, "mcmess_CheckingCard",
		"create_dialog_box_with_wait { title = \"Checking ...\" text_dims = (256.0, 0.0)\n"
		"text = \"Checking memory card (8MB) (for PlayStation 2) in MEMORY CARD slot 1. Do not remove memory card (8MB) (for PlayStation 2), controller, or reset/switch off the console.\"\n"
		"add_loading_anim = add_loading_anim no_bg = <no_bg> }\nreturn\n" );
	patch_debut( qb, j, m, "mcmess_LoadingData",
		"create_dialog_box_with_wait { title = \"Loading ...\" text_dims = (256.0, 0.0)\n"
		"text = \"Loading data. Do not remove memory card (8MB) (for PlayStation 2), controller, or reset/switch off the console.\"\n"
		"add_loading_anim = add_loading_anim }\nno_animate = <no_animate>\nreturn\n" );
	// THUG Zero: armed by its story menu item, disarmed by any other way out.
	// flips/rolls landed before they come round bail (trickcomponent.cpp)
	patch_debut( qb, j, m, "Land", "if DesktopAirFlipBail\nGoto DoingTrickBail\nendif\n" );
	// THUG2: flips and rolls only while a flip or grab trick is going
	patch_debut( qb, j, m, "FlipTrick", "DesktopAirTrickMark\n" );
	patch_debut( qb, j, m, "HoldAbleFlipTrick", "DesktopAirTrickMark\n" );
	patch_debut( qb, j, m, "GrabTrick", "DesktopAirTrickMark\n" );
	patch_debut( qb, j, m, "create_career_options_menu", "DesktopZeroArm off\n" );
	patch_debut( qb, j, m, "cancel_new_career", "DesktopZeroArm off\n" );
	// Difficulty menu: Twisted (THUG Zero) and Demented after Sick.
	patch_difficultes( qb, j, m );
	patch_debut( qb, j, m, "set_career_difficulty",
		"if NOT GotParam from_ss_menu\n"
		"if GotParam desktop_twisted\n"
		"DesktopZeroArm on\n"
		"endif\n"
		"if DesktopZeroArmed\n"
		"<dif> = 2\n"
		"SetGlobalFlag flag = 511\n"
		"else\n"
		"UnSetGlobalFlag flag = 511\n"
		"endif\n"
		"DesktopZeroArm off\n"
		"if GotParam desktop_demented\n"
		"DesktopDementedStart\n"
		"else\n"
		"DesktopDementedStop\n"
		"endif\n"
		"endif\n" );
	// Demented: dealt a skater instead of the create-a-skater, no saving,
	// missing goal tricks slotted in, a life per chapter, its own ending.
	patch_debut( qb, j, m, "launch_pre_cas_menu",
		"if DesktopDementedNeedsDialog\n"
		"if NOT ( goto_cad = 1 )\n"
		"desktop_demented_deal\n"
		"return\n"
		"endif\n"
		"endif\n" );
	patch_debut( qb, j, m, "goal_check_for_required_tricks",
		"if DesktopDemented\n"
		"GoalManager_GetGoalParams name = <goal_id>\n"
		"if GotParam required_tricks\n"
		"DesktopDementedSlotTricks required_tricks = <required_tricks>\n"
		"endif\n"
		"endif\n" );
	patch_debut( qb, j, m, "goal_want_to_save",
		"if DesktopDemented\n"
		"SpawnScript desktop_demented_nosave\n"
		"return\n"
		"endif\n" );
	patch_debut( qb, j, m, "launch_pause_menu_save_game_sequence", "if DesktopDemented\nreturn\nendif\n" );
	// no goal retries in Demented (pause menu items are hidden; this also
	// covers retry-on-Select)
	patch_debut( qb, j, m, "RetryCurrentGoal", "if DesktopDemented\nreturn\nendif\n" );
	// one goal at a time in Demented: no new goal offers while one runs
	patch_debut( qb, j, m, "goal_got_trigger", "if DesktopDemented\nif GoalManager_HasActiveGoals\nreturn\nendif\nendif\n" );
	// chapter 26 in Demented (Marcus): only the pro challenge dealt up now is
	// offered, in the chapter menu and by its pro
	patch_debut( qb, j, m, "goal_got_trigger", "if DesktopDementedHidesGoal goal_id = <goal_id>\nreturn\nendif\n" );
	patch_debut( qb, j, m, "chapter_goal_add_item", "if DesktopDementedHidesGoal <...>\nreturn\nendif\n" );
	// the Big Apple / San Diego / Moscow filmers stand with the photographer's
	// anims (Photographer_goal_wait) but their levels only load the video
	// guy's: they T-posed. The video set brings the photo set along.
	patch_debut( qb, j, m, "animload_Ped_Video", "animload_Ped_Photo LoadFunction = <LoadFunction>\n" );
	// Demented (Marcus): no Set Restart / Goto Cust Restart in the pause options,
	// and coming back from Watch Cutscenes doesn't freeze the game
	patch_debut( qb, j, m, "create_options_menu", "if DesktopDemented\nNoCustomRestarts = 1\nendif\n" );
	patch_debut( qb, j, m, "script_change_tod", "DesktopRandoTod\n" );	// Demented Random World
	patch_debut( qb, j, m, "watch_cutscene_done", "if DesktopDemented\nDebounce X 0.5\npause_menu_gradient on\ncreate_watch_cutscenes_menu\nreturn\nendif\n" );
	// the skater is dealt before the first intro, so it's in it
	patch_debut( qb, j, m, "play_first_cutscene", "if DesktopDementedNeedsDeal\ndesktop_demented_roll\nendif\n" );
	// View Tricks (Demented) goes back to the pause menu
	patch_debut( qb, j, m, "edit_tricks_menu_exit",
		"if DesktopDemented\n"
		"if NOT GotParam just_remove\n"
		"if NOT LevelIs load_skateshop\n"
		"restore_start_key_binding\n"
		"create_pause_menu\n"
		"return\n"
		"endif\n"
		"endif\n"
		"endif\n" );
	patch_debut( qb, j, m, "launch_options_menu_save_game_sequence", "if DesktopDemented\nreturn\nendif\n" );
	// Demented (Marcus): no Create-A-Trick menu after the CAT goals (Nut
	// Buster, Soul Skating): "Return to Game" is taken for you.
	patch_debut( qb, j, m, "goal_cat_create_menu",
		"if DesktopDemented\n"
		"GoalManager_GetGoalParams name = <goal_id>\n"
		"if ( <should_create_cat_menu> = 1 )\n"
		"goal_cat_refuse_cat_menu goal_id = <goal_id>\n"
		"endif\n"
		"return\n"
		"endif\n" );
	// ...no Peralta skateshop visit: straight back to New York, as leaving
	// the shop would (the shop still gets unlocked)...
	patch_debut( qb, j, m, "NY03_SkateshopUnlocked",
		"if DesktopDemented\n"
		"SetGlobalFlag flag = BOARDSHOP_UNLOCKED\n"
		"change in_ny03_sequence = 0\n"
		"change_level level = load_ny next_level_script = NY03_back_from_boardshop\n"
		"return\n"
		"endif\n" );
	// ...and no Create-A-Deck before Vancouver.
	patch_debut( qb, j, m, "change_level_to_cad",
		"if DesktopDemented\n"
		"if ( in_cad_cutscene_sequence = 1 )\n"
		"cad_exit_to_vc\n"
		"return\n"
		"endif\n"
		"endif\n" );
	patch_debut( qb, j, m, "ShownChapterTitle_Spawned",
		"GoalManager_GetCurrentChapterAndStage\n"
		"DesktopDementedChapter chapter = <currentchapter>\n" );
	patch_debut( qb, j, m, "HI_Endgame_show_messages_spawned",
		"if DesktopDemented\n"
		"pausegame\n"
		"StopStream\n"
		"PlayStream FoundAllGaps vol = 150\n"
		"DesktopDementedWon\n"
		"create_dialog_box { title = \"RUN COMPLETE\" title_font = testtitle text = <text> pos = (320,240) just = [ center center ] "
		"text_rgba = [ 88 105 112 128 ] text_dims = (330,0) line_spacing = 0.85 "
		"buttons = [ { font = small text = \"OK\" pad_choose_script = desktop_demented_quit } ] delay_input }\n"
		"return\n"
		"endif\n" );
	patch_pietons( qb, j, m );
	patch_gta_corps( qb, j, m );
	patch_voix( qb, j, m );
	patch_xbox( qb, j, m );
	patch_debut( qb, j, m, "select_skater_menu_add_hmenu_items", s_debut_secrets );
	// Pro Skater 4 career (desktop_th4_glue.cpp): the level select lists only
	// the THPS4 levels, each open once its flag is set; THUG's story doesn't
	// list them. Saving writes the PS4 career's own file, never a story save.
	patch_debut( qb, j, m, "level_select_menu_add_item",
		"if DesktopTH4Level level = <level>\n"
		"if DesktopTH4Active\n"
		"<flag> = <th4_flag>\n"
		"else\n"
		"if GameModeEquals is_career\n"
		"return\n"
		"endif\n"
		"endif\n"
		"else\n"
		"if DesktopTH4Active\n"
		"return\n"
		"endif\n"
		"endif\n" );
	patch_debut( qb, j, m, "launch_pause_menu_save_game_sequence",
		"if DesktopTH4Active\n"
		"DesktopTH4Career save\n"
		"desktop_th4_toast text = \"Pro Skater 4 career saved\"\n"
		"return\n"
		"endif\n" );
	patch_debut( qb, j, m, "goal_save_accept",
		"if DesktopTH4Active\n"
		"DesktopTH4Career save\n"
		"desktop_th4_toast text = \"Pro Skater 4 career saved\"\n"
		"goal_save_reject\n"
		"return\n"
		"endif\n" );
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

// --- Random Names' extra pool --------------------------------------------------
//
// data\custom\names.txt and last-names.txt: one name per line (# comments),
// added to the stock first and last names the Random Names randomizer picks from.
static std::vector<std::string> s_noms_extra[2];	// first names, last names
static bool s_noms_extra_lu = false;

static void noms_fichier( const std::string &chemin, std::vector<std::string> &v )
{
	FILE *f = fopen( chemin.c_str(), "rb" );
	if( !f ) return;
	char l[256];
	while( fgets( l, sizeof( l ), f ))
	{
		size_t n = strlen( l );
		while( n && ( l[n - 1] == '\n' || l[n - 1] == '\r' || l[n - 1] == ' ' || l[n - 1] == '\t' )) l[--n] = 0;
		char *d = l;
		if((unsigned char)d[0] == 0xef && (unsigned char)d[1] == 0xbb && (unsigned char)d[2] == 0xbf ) d += 3;	// UTF-8 mark
		while( *d == ' ' || *d == '\t' ) ++d;
		if( !*d || *d == '#' || strlen( d ) > 40 ) continue;
		// SMITH -> Smith (McX, O'X kept capitalised)
		bool maj = true;
		for( char *q = d; *q; ++q ) if( *q >= 'a' && *q <= 'z' ) { maj = false; break; }
		if( maj )
			for( char *q = d + 1; *q; ++q )
			{
				const char p = q[-1];
				const bool debut = p == '\'' || p == '-' || p == ' ' || ( q - d == 2 && ( d[0] == 'M' && d[1] == 'C' ));
				if( !debut && *q >= 'A' && *q <= 'Z' ) *q = (char)( *q - 'A' + 'a' );
			}
		if( d[0] == 'M' && d[1] == 'c' && d[2] >= 'a' && d[2] <= 'z' && maj ) d[2] = (char)( d[2] - 'a' + 'A' );
		v.push_back( d );
	}
	fclose( f );
}

static void noms_extra_lit( void )
{
	if( s_noms_extra_lu ) return;
	s_noms_extra_lu = true;
	char *base = SDL_GetBasePath();
#ifdef _WIN32
	const std::string dossier = std::string( base ? base : "" ) + "data\\custom\\";
#else
	const std::string dossier = std::string( base ? base : "" ) + "data/custom/";
#endif
	SDL_free( base );
	noms_fichier( dossier + "names.txt", s_noms_extra[0] );
	noms_fichier( dossier + "last-names.txt", s_noms_extra[1] );
	VLOG( "DSK", "random names: %d first names, %d last names in data\\custom", (int)s_noms_extra[0].size(), (int)s_noms_extra[1].size());
}

// famille 0: first names (names.txt), 1: last names (last-names.txt)
extern "C" int desktop_noms_extra_count( int famille )
{
	noms_extra_lit();
	return (int)s_noms_extra[famille ? 1 : 0].size();
}

extern "C" const char *desktop_noms_extra( int famille, int i )
{
	noms_extra_lit();
	const std::vector<std::string> &v = s_noms_extra[famille ? 1 : 0];
	return ( i >= 0 && i < (int)v.size()) ? v[i].c_str() : NULL;
}

// --- Sound Salad's extra pool ----------------------------------------------------
//
// Loose .wav files Marcus adds from other games: data\<anything>\custom\sounds
// (data\thps3\custom\sounds first). Paths go back relative to data, the way
// the engine's File::Open takes them; soundfx.cpp loads one the first time a
// swap lands on it.
static std::vector<std::string> s_sfx_extra;
static bool s_sfx_extra_lu = false;

static bool est_wav( const std::string &n )
{
	if( n.size() <= 4 ) return false;
	std::string e = n.substr( n.size() - 4 );
	for( size_t i = 0; i < e.size(); ++i ) e[i] = (char)tolower((unsigned char)e[i] );
	return e == ".wav";
}

#ifdef _WIN32
static void sfx_extra_parcourt( const std::string &plein, const std::string &rel, int prof )
{
	WIN32_FIND_DATAA d;
	HANDLE h = FindFirstFileA(( plein + "\\*" ).c_str(), &d );
	if( h == INVALID_HANDLE_VALUE ) return;
	do
	{
		const std::string nom = d.cFileName;
		if( nom == "." || nom == ".." || nom.find( '?' ) != std::string::npos ) continue;
		if( d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) { if( prof < 3 ) sfx_extra_parcourt( plein + "\\" + nom, rel + "\\" + nom, prof + 1 ); }
		else if( est_wav( nom )) s_sfx_extra.push_back( rel + "\\" + nom );
	}
	while( FindNextFileA( h, &d ));
	FindClose( h );
}
static void sfx_extra_jeux( const std::string &data, std::vector<std::string> &jeux )
{
	WIN32_FIND_DATAA d;
	HANDLE h = FindFirstFileA(( data + "\\*" ).c_str(), &d );
	if( h == INVALID_HANDLE_VALUE ) return;
	do
		if(( d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) && strcmp( d.cFileName, "." ) && strcmp( d.cFileName, ".." )) jeux.push_back( d.cFileName );
	while( FindNextFileA( h, &d ));
	FindClose( h );
}
#else
static void sfx_extra_parcourt( const std::string &plein, const std::string &rel, int prof )
{
	DIR *d = opendir( plein.c_str());
	if( !d ) return;
	while( struct dirent *e = readdir( d ))
	{
		const std::string nom = e->d_name;
		if( nom == "." || nom == ".." ) continue;
		struct stat st;
		if( stat(( plein + "/" + nom ).c_str(), &st )) continue;
		if( S_ISDIR( st.st_mode )) { if( prof < 3 ) sfx_extra_parcourt( plein + "/" + nom, rel + "\\" + nom, prof + 1 ); }
		else if( est_wav( nom )) s_sfx_extra.push_back( rel + "\\" + nom );
	}
	closedir( d );
}
static void sfx_extra_jeux( const std::string &data, std::vector<std::string> &jeux )
{
	DIR *d = opendir( data.c_str());
	if( !d ) return;
	while( struct dirent *e = readdir( d ))
		if( e->d_name[0] != '.' ) jeux.push_back( e->d_name );
	closedir( d );
}
#endif

static void sfx_extra_lit( void )
{
	if( s_sfx_extra_lu ) return;
	s_sfx_extra_lu = true;
	char *base = SDL_GetBasePath();
	const std::string data = std::string( base ? base : "" ) + "data";
	SDL_free( base );
	std::vector<std::string> jeux;
	sfx_extra_jeux( data, jeux );
	std::sort( jeux.begin(), jeux.end());
	for( size_t i = 0; i < jeux.size(); ++i )
	{
#ifdef _WIN32
		const std::string plein = data + "\\" + jeux[i] + "\\custom\\sounds";
#else
		const std::string plein = data + "/" + jeux[i] + "/custom/sounds";
#endif
		const size_t avant = s_sfx_extra.size();
		sfx_extra_parcourt( plein, jeux[i] + "\\custom\\sounds", 0 );
		std::sort( s_sfx_extra.begin() + avant, s_sfx_extra.end());
		if( s_sfx_extra.size() > avant )
			VLOG( "SND", "sound salad: %d extra sounds in data\\%s\\custom\\sounds", (int)( s_sfx_extra.size() - avant ), jeux[i].c_str());
	}
}

extern "C" int desktop_sfx_extra_count( void )
{
	sfx_extra_lit();
	return (int)s_sfx_extra.size();
}

extern "C" const char *desktop_sfx_extra_path( int i )
{
	sfx_extra_lit();
	return ( i >= 0 && i < (int)s_sfx_extra.size()) ? s_sfx_extra[i].c_str() : NULL;
}
