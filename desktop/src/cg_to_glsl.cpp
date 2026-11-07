/*****************************************************************************
**  THUG desktop -- Cg to GLSL translation                                  **
**  desktop/src/cg_to_glsl.cpp                                              **
**                                                                          **
**  The Vita backend writes its shaders in Cg (vitaGL hands them to the     **
**  console's Cg compiler). They all have the same shape:                   **
**                                                                          **
**      void main( float3 aPos, uniform float4 uL0, ...,                    **
**                 float4 out vPosition : POSITION,                         **
**                 float2 out vTexcoord : TEXCOORD0 ) { ... }               **
**      float4 main( float2 vTexcoord : TEXCOORD0, ... ) { ... return c; }  **
**                                                                          **
**  This turns one into GLSL 1.20 for a desktop compatibility context:      **
**  inputs become attributes or varyings, uniform parameters become global **
**  uniforms, outputs travel in vec4 varyings named after their semantic,   **
**  and the Cg body is kept as a function (cg_main) with the Cg types,      **
**  casts, literals and intrinsics rewritten.                               **
*****************************************************************************/

#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdio>

namespace
{

struct Tok
{
	enum Kind { WS, IDENT, NUM, PUNCT } kind;
	std::string s;
};

static bool is_ident_start( char c ) { return isalpha( (unsigned char)c ) || c == '_'; }
static bool is_ident_char( char c )  { return isalnum( (unsigned char)c ) || c == '_'; }

static std::vector<Tok> tokenize( const std::string &src )
{
	std::vector<Tok> t;
	size_t i = 0, n = src.size();
	while( i < n )
	{
		char c = src[i];
		Tok k;
		if( isspace( (unsigned char)c ))
		{
			size_t j = i;
			while( j < n && isspace( (unsigned char)src[j] )) ++j;
			k.kind = Tok::WS; k.s = src.substr( i, j - i ); i = j;
		}
		else if( c == '/' && i + 1 < n && src[i + 1] == '/' )
		{
			size_t j = src.find( '\n', i );
			if( j == std::string::npos ) j = n;
			k.kind = Tok::WS; k.s = " "; i = j;
		}
		else if( c == '/' && i + 1 < n && src[i + 1] == '*' )
		{
			size_t j = src.find( "*/", i + 2 );
			j = ( j == std::string::npos ) ? n : j + 2;
			k.kind = Tok::WS; k.s = " "; i = j;
		}
		else if( is_ident_start( c ))
		{
			size_t j = i;
			while( j < n && is_ident_char( src[j] )) ++j;
			k.kind = Tok::IDENT; k.s = src.substr( i, j - i ); i = j;
		}
		else if( isdigit( (unsigned char)c ) || ( c == '.' && i + 1 < n && isdigit( (unsigned char)src[i + 1] )))
		{
			size_t j = i;
			bool hex = ( c == '0' && i + 1 < n && ( src[i + 1] == 'x' || src[i + 1] == 'X' ));
			if( hex )
			{
				j += 2;
				while( j < n && isxdigit( (unsigned char)src[j] )) ++j;
			}
			else
			{
				while( j < n && ( isdigit( (unsigned char)src[j] ) || src[j] == '.' )) ++j;
				if( j < n && ( src[j] == 'e' || src[j] == 'E' ))
				{
					size_t e = j + 1;
					if( e < n && ( src[e] == '+' || src[e] == '-' )) ++e;
					if( e < n && isdigit( (unsigned char)src[e] ))
					{
						j = e;
						while( j < n && isdigit( (unsigned char)src[j] )) ++j;
					}
				}
			}
			while( j < n && ( src[j] == 'f' || src[j] == 'F' || src[j] == 'h' || src[j] == 'H'
			                  || src[j] == 'u' || src[j] == 'U' )) ++j;
			k.kind = Tok::NUM; k.s = src.substr( i, j - i ); i = j;
		}
		else
		{
			k.kind = Tok::PUNCT; k.s = std::string( 1, c ); ++i;
		}
		t.push_back( k );
	}
	return t;
}

static std::string join( const std::vector<Tok> &t, size_t a, size_t b )
{
	std::string s;
	for( size_t i = a; i < b && i < t.size(); ++i )
		s += t[i].s;
	return s;
}

// Cg type name -> GLSL type name, "" if not a type.
static std::string map_type( const std::string &s )
{
	static const char *scal[] = { "float", "half", "fixed", "double" };
	for( unsigned k = 0; k < 4; ++k )
	{
		const size_t l = strlen( scal[k] );
		if( s.compare( 0, l, scal[k] ) != 0 )
			continue;
		std::string rest = s.substr( l );
		if( rest.empty() )
			return "float";
		if( rest.size() == 1 && rest[0] >= '2' && rest[0] <= '4' )
			return std::string( "vec" ) + rest;
		if( rest.size() == 3 && rest[1] == 'x' && rest[0] >= '2' && rest[0] <= '4' && rest[2] >= '2' && rest[2] <= '4' )
			return ( rest[0] == rest[2] ) ? std::string( "mat" ) + rest[0]
			                              : std::string( "mat" ) + rest[2] + "x" + rest[0];
	}
	if( s == "int" || s == "bool" || s == "void" || s == "sampler2D" || s == "samplerCUBE" )
		return s == "samplerCUBE" ? "samplerCube" : s;
	if(( s.size() == 4 ) && s.compare( 0, 3, "int" ) == 0 && s[3] >= '2' && s[3] <= '4' )
		return std::string( "ivec" ) + s[3];
	if(( s.size() == 5 ) && s.compare( 0, 4, "bool" ) == 0 && s[4] >= '2' && s[4] <= '4' )
		return std::string( "bvec" ) + s[4];
	if( s == "vec2" || s == "vec3" || s == "vec4" || s == "ivec2" || s == "ivec3" || s == "ivec4"
	    || s == "mat2" || s == "mat3" || s == "mat4" )
		return s;
	return "";
}

static int type_components( const std::string &glsl )
{
	if( glsl == "float" || glsl == "int" || glsl == "bool" ) return 1;
	char last = glsl.empty() ? 0 : glsl[glsl.size() - 1];
	if( last >= '2' && last <= '4' && glsl.find( "vec" ) != std::string::npos ) return last - '0';
	return 4;
}

static bool is_glsl_reserved( const std::string &s )
{
	static const char *r[] = {
		"input", "output", "sample", "filter", "active", "common", "partition", "superp",
		"inline", "noinline", "public", "external", "interface", "long", "short", "double",
		"unsigned", "sizeof", "cast", "namespace", "using", "smooth", "flat", "centroid",
		"invariant", "precision", "lowp", "mediump", "highp", "asm", "class", "union", "enum",
		"typedef", "template", "this", "packed", "goto", "switch", "default", "volatile",
		"attribute", "varying", "texture", "gl_Position", "main", "fvec2", "fvec3", "fvec4",
		"hvec2", "hvec3", "hvec4", "dvec2", "dvec3", "dvec4", "sampler3DRect", "image1D",
		"buffer", "resource", "row_major", "mix", "fract", "inversesqrt", "texture2D",
		NULL };
	for( int i = 0; r[i]; ++i )
		if( s == r[i] )
			return true;
	return false;
}

static const char *rename_func( const std::string &s )
{
	if( s == "tex2D" )     return "texture2D";
	if( s == "tex2Dproj" ) return "texture2DProj";
	if( s == "texCUBE" )   return "textureCube";
	if( s == "lerp" )      return "mix";
	if( s == "frac" )      return "fract";
	if( s == "rsqrt" )     return "inversesqrt";
	if( s == "ddx" )       return "dFdx";
	if( s == "ddy" )       return "dFdy";
	if( s == "fmod" )      return "mod";
	if( s == "atan2" )     return "atan";
	return NULL;
}

static size_t next_nonws( const std::vector<Tok> &t, size_t i )
{
	while( i < t.size() && t[i].kind == Tok::WS ) ++i;
	return i;
}

static size_t prev_nonws( const std::vector<Tok> &t, size_t i )
{
	// index of previous non-ws token before i, or (size_t)-1
	while( i > 0 )
	{
		--i;
		if( t[i].kind != Tok::WS )
			return i;
	}
	return (size_t)-1;
}

// Index just past the bracket group opening at i (t[i] is '(' or '[').
static size_t skip_group( const std::vector<Tok> &t, size_t i )
{
	const std::string open = t[i].s;
	const std::string close = ( open == "(" ) ? ")" : ( open == "[" ) ? "]" : "}";
	int depth = 0;
	for( ; i < t.size(); ++i )
	{
		if( t[i].kind != Tok::PUNCT ) continue;
		if( t[i].s == open ) ++depth;
		else if( t[i].s == close )
		{
			if( --depth == 0 )
				return i + 1;
		}
	}
	return t.size();
}

// End of a unary operand starting at i (Cg cast operand).
static size_t operand_end( const std::vector<Tok> &t, size_t i )
{
	i = next_nonws( t, i );
	if( i >= t.size() ) return i;
	// unary prefix
	while( i < t.size() && t[i].kind == Tok::PUNCT && ( t[i].s == "-" || t[i].s == "+" || t[i].s == "!" || t[i].s == "~" ))
		i = next_nonws( t, i + 1 );
	if( i >= t.size() ) return i;
	if( t[i].kind == Tok::PUNCT && t[i].s == "(" )
		i = skip_group( t, i );
	else if( t[i].kind == Tok::IDENT || t[i].kind == Tok::NUM )
		++i;
	else
		return i + 1;
	// postfix: calls, members, indexing
	for( ;; )
	{
		size_t j = next_nonws( t, i );
		if( j >= t.size() ) return i;
		if( t[j].kind == Tok::PUNCT && ( t[j].s == "(" || t[j].s == "[" ))
			i = skip_group( t, j );
		else if( t[j].kind == Tok::PUNCT && t[j].s == "." )
		{
			size_t k = next_nonws( t, j + 1 );
			if( k < t.size() && t[k].kind == Tok::IDENT )
				i = k + 1;
			else
				return i;
		}
		else
			return i;
	}
}

// Rewrites a run of Cg code (body or helper declarations) token by token.
static std::string translate_code( const std::string &src )
{
	std::vector<Tok> t = tokenize( src );

	// 1) literals, types, identifiers, intrinsics
	for( size_t i = 0; i < t.size(); ++i )
	{
		Tok &k = t[i];
		if( k.kind == Tok::NUM )
		{
			std::string s = k.s;
			bool hex = s.size() > 1 && ( s[1] == 'x' || s[1] == 'X' );
			while( !s.empty() && !hex && strchr( "fFhH", s[s.size() - 1] )) s.erase( s.size() - 1 );
			while( !s.empty() && strchr( "uU", s[s.size() - 1] )) s.erase( s.size() - 1 );
			if( !s.empty() && s[s.size() - 1] == '.' ) s += "0";
			if( !s.empty() && s[0] == '.' ) s = "0" + s;
			// "1e-8" is fine; a bare float literal with a stripped suffix but no
			// dot ("2f") must stay a float.
			if( k.s != s && s.find( '.' ) == std::string::npos && s.find( 'e' ) == std::string::npos
			    && s.find( 'E' ) == std::string::npos && !hex )
				s += ".0";
			k.s = s;
		}
		else if( k.kind == Tok::IDENT )
		{
			std::string ty = map_type( k.s );
			if( !ty.empty() )
			{
				k.s = ty;
				continue;
			}
			if( k.s == "static" )
			{
				k.s = "";
				continue;
			}
			size_t j = next_nonws( t, i + 1 );
			bool call = ( j < t.size() && t[j].kind == Tok::PUNCT && t[j].s == "(" );
			const char *r = call ? rename_func( k.s ) : NULL;
			if( r )
				k.s = r;
			else if( is_glsl_reserved( k.s ))
			{
				size_t p = prev_nonws( t, i );
				bool member = ( p != (size_t)-1 && t[p].kind == Tok::PUNCT && t[p].s == "." );
				if( !member )
					k.s += "_cg";
			}
		}
	}

	// 2) casts "(T)x" -> "T(x)". Repeat until none left (nested casts).
	for( int pass = 0; pass < 8; ++pass )
	{
		bool changed = false;
		for( size_t i = 0; i < t.size(); ++i )
		{
			if( t[i].kind != Tok::PUNCT || t[i].s != "(" ) continue;
			size_t a = next_nonws( t, i + 1 );
			if( a >= t.size() || t[a].kind != Tok::IDENT || map_type( t[a].s ).empty() ) continue;
			size_t b = next_nonws( t, a + 1 );
			if( b >= t.size() || t[b].kind != Tok::PUNCT || t[b].s != ")" ) continue;
			size_t p = prev_nonws( t, i );
			if( p != (size_t)-1 && ( t[p].kind == Tok::IDENT || t[p].kind == Tok::NUM
			                         || ( t[p].kind == Tok::PUNCT && ( t[p].s == ")" || t[p].s == "]" ))))
				continue;	// a call or a parenthesised expression, not a cast
			size_t e = operand_end( t, b + 1 );
			std::string ty = t[a].s;
			std::string operand = join( t, b + 1, e );
			Tok nt; nt.kind = Tok::IDENT; nt.s = ty + "(" + operand + ")";
			t.erase( t.begin() + i, t.begin() + e );
			t.insert( t.begin() + i, nt );
			changed = true;
		}
		if( !changed ) break;
		// the merged tokens must be re-tokenized for further casts inside them
		t = tokenize( join( t, 0, t.size() ));
	}
	return join( t, 0, t.size() );
}

struct Param
{
	bool uniform, out;
	std::string type;		// GLSL
	std::string name;
	std::string array;		// "[N]" or ""
	std::string sem;		// upper case, normalised
};

static std::string norm_sem( std::string s )
{
	for( size_t i = 0; i < s.size(); ++i ) s[i] = (char)toupper( (unsigned char)s[i] );
	if( s == "COLOR" ) return "COLOR0";
	if( s == "TEXCOORD" ) return "TEXCOORD0";
	if( s == "POSITION0" ) return "POSITION";
	if( s == "COLOR0" || s == "DIFFUSE" ) return "COLOR0";
	if( s == "SPECULAR" ) return "COLOR1";
	return s;
}

static bool parse_params( const std::string &txt, std::vector<Param> &out, std::string &err )
{
	// split at top-level commas
	std::vector<std::string> parts;
	int depth = 0;
	std::string cur;
	for( size_t i = 0; i < txt.size(); ++i )
	{
		char c = txt[i];
		if( c == '(' || c == '[' ) ++depth;
		if( c == ')' || c == ']' ) --depth;
		if( c == ',' && depth == 0 ) { parts.push_back( cur ); cur.clear(); continue; }
		cur += c;
	}
	if( !cur.empty() ) parts.push_back( cur );
	for( size_t p = 0; p < parts.size(); ++p )
	{
		std::vector<Tok> t = tokenize( parts[p] );
		std::vector<std::string> w;
		std::string array, sem;
		for( size_t i = 0; i < t.size(); ++i )
		{
			if( t[i].kind == Tok::WS ) continue;
			if( t[i].kind == Tok::PUNCT && t[i].s == "[" )
			{
				size_t e = skip_group( t, i );
				array = join( t, i, e );
				i = e - 1;
				continue;
			}
			if( t[i].kind == Tok::PUNCT && t[i].s == ":" )
			{
				size_t j = next_nonws( t, i + 1 );
				if( j < t.size() ) sem = t[j].s;
				break;
			}
			if( t[i].kind == Tok::IDENT )
				w.push_back( t[i].s );
		}
		if( w.empty() ) continue;
		Param pr;
		pr.uniform = false; pr.out = false;
		std::vector<std::string> rest;
		for( size_t i = 0; i < w.size(); ++i )
		{
			if( w[i] == "uniform" ) pr.uniform = true;
			else if( w[i] == "out" ) pr.out = true;
			else if( w[i] == "in" || w[i] == "const" ) {}
			else if( w[i] == "inout" ) { err = "inout parameter not supported"; return false; }
			else rest.push_back( w[i] );
		}
		if( rest.size() != 2 ) { err = "cannot parse parameter '" + parts[p] + "'"; return false; }
		pr.type = map_type( rest[0] );
		if( pr.type.empty() ) { err = "unknown type '" + rest[0] + "'"; return false; }
		pr.name = rest[1];
		if( is_glsl_reserved( pr.name )) pr.name += "_cg";
		pr.array = array;
		pr.sem = sem.empty() ? "" : norm_sem( sem );
		out.push_back( pr );
	}
	return true;
}

static const char *s_prelude_common =
	"float saturate(float x) { return clamp(x, 0.0, 1.0); }\n"
	"vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }\n"
	"vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }\n"
	"vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
	"vec4 mul(mat4 m, vec4 v) { return v * m; }\n"
	"vec3 mul(mat3 m, vec3 v) { return v * m; }\n"
	"vec4 mul(vec4 v, mat4 m) { return m * v; }\n"
	"vec3 mul(vec3 v, mat3 m) { return m * v; }\n";

static const char *s_prelude_fragment =
	"void clip(float x) { if (x < 0.0) discard; }\n"
	"void clip(vec2 x) { if (any(lessThan(x, vec2(0.0)))) discard; }\n"
	"void clip(vec3 x) { if (any(lessThan(x, vec3(0.0)))) discard; }\n"
	"void clip(vec4 x) { if (any(lessThan(x, vec4(0.0)))) discard; }\n";

static std::string pad4( const std::string &expr, int comps )
{
	switch( comps )
	{
		case 1: return "vec4(" + expr + ", 0.0, 0.0, 0.0)";
		case 2: return "vec4(" + expr + ", 0.0, 0.0)";
		case 3: return "vec4(" + expr + ", 0.0)";
		default: return expr;
	}
}

static std::string swz( int comps )
{
	switch( comps )
	{
		case 1: return ".x";
		case 2: return ".xy";
		case 3: return ".xyz";
		default: return "";
	}
}

}	// namespace

// Translates one Cg shader. Returns false and fills err on failure.
bool cg_to_glsl( const char *p_src, bool vertex, std::string &out, std::string &err )
{
	const std::string src( p_src );
	std::vector<Tok> t = tokenize( src );

	// locate "main ("
	size_t m = (size_t)-1;
	for( size_t i = 0; i < t.size(); ++i )
		if( t[i].kind == Tok::IDENT && t[i].s == "main" )
		{
			size_t j = next_nonws( t, i + 1 );
			if( j < t.size() && t[j].kind == Tok::PUNCT && t[j].s == "(" )
			{
				m = i;
				break;
			}
		}
	if( m == (size_t)-1 ) { err = "no main()"; return false; }
	size_t ret = prev_nonws( t, m );
	if( ret == (size_t)-1 || t[ret].kind != Tok::IDENT ) { err = "no return type"; return false; }
	const std::string ret_type = map_type( t[ret].s );
	size_t po = next_nonws( t, m + 1 );
	size_t pe = skip_group( t, po );
	const std::string params_txt = join( t, po + 1, pe - 1 );
	// optional ": SEMANTIC" after the parameter list, then the body
	size_t b = next_nonws( t, pe );
	if( b < t.size() && t[b].kind == Tok::PUNCT && t[b].s == ":" )
	{
		b = next_nonws( t, b + 1 );
		b = next_nonws( t, b + 1 );
	}
	if( b >= t.size() || t[b].s != "{" ) { err = "no body"; return false; }
	size_t be = skip_group( t, b );
	const std::string prefix = join( t, 0, ret );
	const std::string body = join( t, b, be );
	const std::string suffix = join( t, be, t.size() );

	std::vector<Param> params;
	if( !parse_params( params_txt, params, err ))
		return false;

	std::string g = "#version 120\n";
	g += s_prelude_common;
	if( !vertex )
		g += s_prelude_fragment;

	std::string decl, fn_params, call_args, post;
	int nout = 0;
	for( size_t i = 0; i < params.size(); ++i )
	{
		const Param &p = params[i];
		if( p.uniform )
		{
			decl += "uniform " + p.type + " " + p.name + p.array + ";\n";
			continue;
		}
		const int comps = type_components( p.type );
		if( vertex && !p.out )
		{
			decl += "attribute " + p.type + " " + p.name + ";\n";
			continue;
		}
		if( !vertex && !p.out )
		{
			if( p.sem == "WPOS" || p.sem == "VPOS" )
				call_args += std::string( call_args.empty() ? "" : ", " ) + "gl_FragCoord" + swz( comps );
			else
			{
				const std::string v = "xv_" + ( p.sem.empty() ? p.name : p.sem );
				if( decl.find( "varying vec4 " + v + ";" ) == std::string::npos )
					decl += "varying vec4 " + v + ";\n";
				call_args += std::string( call_args.empty() ? "" : ", " ) + v + swz( comps );
			}
			fn_params += std::string( fn_params.empty() ? "" : ", " ) + p.type + " " + p.name;
			continue;
		}
		// output parameter
		char tmp[16];
		snprintf( tmp, sizeof( tmp ), "o%d", nout++ );
		fn_params += std::string( fn_params.empty() ? "" : ", " ) + "out " + p.type + " " + p.name + p.array;
		call_args += std::string( call_args.empty() ? "" : ", " ) + tmp;
		if( !p.array.empty() )
		{
			// User clip distances (CLP0..): only an optimisation on Vita, the
			// fragment shaders test the same bounds. Computed and dropped.
			if( p.sem.compare( 0, 3, "CLP" ) != 0 )
			{
				err = "unsupported array output " + p.name;
				return false;
			}
			post += "\t" + p.type + " " + tmp + p.array + ";\n";
			continue;
		}
		post += "\t" + p.type + " " + tmp + " = " + p.type + "(0.0);\n";
		std::string assign;
		if( vertex )
		{
			if( p.sem == "POSITION" )
				assign = std::string( "\tgl_Position = " ) + pad4( tmp, comps ) + ";\n";
			else if( p.sem == "PSIZE" )
				assign = std::string( "\tgl_PointSize = " ) + tmp + ";\n";
			else
			{
				const std::string v = "xv_" + ( p.sem.empty() ? p.name : p.sem );
				decl += "varying vec4 " + v + ";\n";
				const bool color = ( p.sem.compare( 0, 5, "COLOR" ) == 0 );
				assign = "\t" + v + " = " + ( color ? "clamp(" + pad4( tmp, comps ) + ", 0.0, 1.0)" : pad4( tmp, comps )) + ";\n";
			}
		}
		else
		{
			if( p.sem.compare( 0, 5, "COLOR" ) == 0 )
				assign = std::string( "\tgl_FragColor = " ) + pad4( tmp, comps ) + ";\n";
			else if( p.sem == "DEPTH" )
				assign = std::string( "\tgl_FragDepth = " ) + tmp + ";\n";
			else
			{
				err = "unsupported fragment output semantic " + p.sem;
				return false;
			}
		}
		// keep declarations first, assignments after the call
		post += "\x01" + assign;
	}

	g += decl;
	g += translate_code( prefix );
	g += ( ret_type.empty() ? "void" : ret_type ) + " cg_main(" + fn_params + ")\n";
	g += translate_code( body );
	g += translate_code( suffix );
	g += "\nvoid main()\n{\n";
	// split post into declarations and assignments
	std::string decls, assigns;
	{
		size_t i = 0;
		while( i < post.size() )
		{
			size_t nl = post.find( '\n', i );
			if( nl == std::string::npos ) nl = post.size() - 1;
			std::string line = post.substr( i, nl - i + 1 );
			if( !line.empty() && line[0] == '\x01' ) assigns += line.substr( 1 );
			else decls += line;
			i = nl + 1;
		}
	}
	g += decls;
	if( !vertex && ret_type != "void" && !ret_type.empty() )
		g += "\tgl_FragColor = " + pad4( "cg_main(" + call_args + ")", type_components( ret_type )) + ";\n";
	else
		g += "\tcg_main(" + call_args + ");\n";
	g += assigns;
	g += "}\n";
	out = g;
	return true;
}
