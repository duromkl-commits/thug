/*****************************************************************************
**  THUG desktop -- the "GXM direct" draw paths, on OpenGL                  **
**  Code/Gfx/Vita/p_gxm_desktop.inc (included by p_shader_decor.cpp)        **
**                                                                          **
**  On Vita, world batches bypass vitaGL and go straight to the GPU (GXM):  **
**  same Cg variants, same uniforms, but programs, textures and vertex      **
**  streams are set by hand to save CPU time. A desktop driver doesn't need **
**  that, so the same entry points are implemented here with plain GL      **
**  calls on the variants' GL programs. What GXM baked into a fragment      **
**  program (the blend mode) is GL blend state, set per draw and restored   **
**  by GxmMateriauFin. Command lists and the worker thread don't exist on   **
**  desktop; character skinning uses the GL path (ShaderPeau*).             **
*****************************************************************************/

bool g_vita_gxm_direct = true;
bool g_vita_vpr = true;
bool g_vita_gxm_peau = false;
int  g_vita_gp_reutil[2] = { 0, 0 };
bool g_vita_listes_gxm = false;
bool g_vita_lots_mt = false;

// GL locations a variant needs beyond what SVarianteMateriau records.
struct SDkVariante
{
	GLuint prog;
	GLint  a_normal;
	GLint  v[3];		// uV0..2, view rows (environment mapping)
	GLint  e[4];		// uE0..3, environment tiling per pass
};
#define DK_MAX_VAR ( MAX_VARIANTES )
static SDkVariante s_dk_var[DK_MAX_VAR];
static int         s_dk_nb = 0;

static const SDkVariante *dk_extra( const SVarianteMateriau *v )
{
	for( int i = 0; i < s_dk_nb; ++i )
		if( s_dk_var[i].prog == v->prog )
			return &s_dk_var[i];
	if( s_dk_nb >= DK_MAX_VAR )
		return NULL;
	SDkVariante *d = &s_dk_var[s_dk_nb++];
	d->prog = v->prog;
	d->a_normal = v->env ? glGetAttribLocation( v->prog, "aNormal" ) : -1;
	static const char *noms_v[3] = { "uV0", "uV1", "uV2" };
	static const char *noms_e[4] = { "uE0", "uE1", "uE2", "uE3" };
	for( int r = 0; r < 3; ++r )
		d->v[r] = v->env ? glGetUniformLocation( v->prog, noms_v[r] ) : -1;
	for( int k = 0; k < 4; ++k )
		d->e[k] = ( v->env & ( 1u << k )) ? glGetUniformLocation( v->prog, noms_e[k] ) : -1;
	return d;
}

static const float              *s_dk_mvp = NULL;
static const float              *s_dk_vue = NULL;
static const SVarianteMateriau  *s_dk_cour = NULL;
static bool                      s_dk_actif = false;
static GLboolean                 s_dk_blend_avant = GL_FALSE, s_dk_alpha_avant = GL_FALSE;
static GLint                     s_dk_src = GL_ONE, s_dk_dst = GL_ZERO, s_dk_eq = GL_FUNC_ADD;
static unsigned int              s_dk_attr = 0;		// attribute arrays enabled (bit = index)

// Blend families, same table as dessiner_lot (p_world_render.cpp):
//   0 opaque  1 alpha  2 add  3 reverse subtract  4 modulate  5 (dst, 1)
//   6 brighten fixed (dst, src alpha)
int GxmFamilleMelange( unsigned int blend, bool translucide )
{
	if( !translucide )
		return 0;
	switch( blend )
	{
		case 1: case 2:  return 2;
		case 3: case 4:  return 3;
		case 7: case 8:  return 4;
		case 9:          return 5;
		case 10:         return 6;
		default:         return 1;
	}
}

static void dk_melange( int famille )
{
	if( famille == 0 )
	{
		glDisable( GL_BLEND );
		return;
	}
	glEnable( GL_BLEND );
	glBlendEquation( famille == 3 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD );
	switch( famille )
	{
		case 2: case 3: glBlendFunc( GL_SRC_ALPHA, GL_ONE ); break;
		// [SOURCE] XBox/NX/render.cpp, set_blend_mode : MODULATE = (ZERO, SRCALPHA)
		case 4: glBlendFunc( GL_ZERO, GL_SRC_ALPHA ); break;
		case 5: glBlendFunc( GL_DST_COLOR, GL_ONE ); break;
		case 6: glBlendFunc( GL_DST_COLOR, GL_SRC_ALPHA ); break;
		default: glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA ); break;
	}
}

static void dk_attrib( unsigned int voulu )
{
	const unsigned int diff = voulu ^ s_dk_attr;
	for( int i = 0; i < 16; ++i )
		if( diff & ( 1u << i ))
		{
			if( voulu & ( 1u << i ))
				glEnableVertexAttribArray( i );
			else
				glDisableVertexAttribArray( i );
		}
	s_dk_attr = voulu;
}

void GxmMateriauDebut( const float *mvp )
{
	s_dk_mvp = mvp;
	s_dk_cour = NULL;
	if( !s_dk_actif )
	{
		s_dk_actif = true;
		s_dk_blend_avant = glIsEnabled( GL_BLEND );
		s_dk_alpha_avant = glIsEnabled( GL_ALPHA_TEST );
		glGetIntegerv( GL_BLEND_SRC, &s_dk_src );
		glGetIntegerv( GL_BLEND_DST, &s_dk_dst );
		glGetIntegerv( GL_BLEND_EQUATION, &s_dk_eq );
		s_dk_attr = 0;
	}
	// GXM has no fixed alpha test: the variants discard below uSeuil.
	glDisable( GL_ALPHA_TEST );
}

void GxmMateriauVue( const float *view )
{
	s_dk_vue = view;
}

// Program, uniforms, textures, vertex streams and blend for one draw.
static const SVarianteMateriau *dk_poser( const SShaderMateriau *p, SceGxmTexture *const *p_tex, int famille )
{
	if( !s_dk_mvp )
		return NULL;
	const SVarianteMateriau *v = variante( p );
	if( !v )
		return NULL;
	const SDkVariante *x = dk_extra( v );
	if( !x || ( v->env && !s_dk_vue ))
		return NULL;
	if( v != s_dk_cour )
	{
		glUseProgram( v->prog );
		for( int r = 0; r < 4; ++r )
			glUniform4f( v->l[r], s_dk_mvp[r], s_dk_mvp[4 + r], s_dk_mvp[8 + r], s_dk_mvp[12 + r] );
		s_dk_cour = v;
		++s_vh_poses_vs;
	}
	SVarianteMateriau *vm = (SVarianteMateriau *)v;
	for( int k = 0; k < p->passes; ++k )
		if( memcmp( vm->c_pose[k], p->c[k], sizeof( float ) * 4 ) != 0 )
		{
			glUniform4f( v->c[k], p->c[k][0], p->c[k][1], p->c[k][2], p->c[k][3] );
			memcpy( vm->c_pose[k], p->c[k], sizeof( float ) * 4 );
		}
	if( v->seuil >= 0 )
		glUniform1f( v->seuil, p->seuil );
	{
		float f[8];
		BrouillardUniformes( p->fog_noir, false, f, f + 4 );
		if( memcmp( vm->fog_pose, f, sizeof( f )) != 0 )
		{
			glUniform4f( v->fogp, f[0], f[1], f[2], f[3] );
			glUniform4f( v->fogc, f[4], f[5], f[6], f[7] );
			memcpy( vm->fog_pose, f, sizeof( f ));
		}
	}
	for( int k = 0; k < p->passes; ++k )
		if( v->w[k] >= 0 )
			glUniform4f( v->w[k], p->wib_uv[k][0], p->wib_uv[k][1], 0.0f, 0.0f );
	if( v->env )
	{
		// Rows of the VIEW matrix (column-major GL), as poser_uniformes_env_e.
		for( int r = 0; r < 3; ++r )
			if( x->v[r] >= 0 )
				glUniform4f( x->v[r], s_dk_vue[r], s_dk_vue[4 + r], s_dk_vue[8 + r], s_dk_vue[12 + r] );
		for( int k = 0; k < 4; ++k )
			if( x->e[k] >= 0 )
				glUniform4f( x->e[k], p->env_tile[k][0], p->env_tile[k][1], 0.0f, 0.0f );
	}

	// Vertex streams: one buffer per array, as on Vita.
	static const int attr_uv[4] = { ATTR_UV, ATTR_UV2, ATTR_UV3, ATTR_UV4 };
	unsigned int voulu = ( 1u << ATTR_POSITION ) | ( 1u << ATTR_COULEUR );
	glBindBuffer( GL_ARRAY_BUFFER, p->vbo );
	glVertexAttribPointer( ATTR_POSITION, 3, GL_FLOAT, GL_FALSE, 0, NULL );
	glBindBuffer( GL_ARRAY_BUFFER, p->cbo );
	glVertexAttribPointer( ATTR_COULEUR, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, NULL );
	for( int k = 0; k < p->passes; ++k )
	{
		if( v->env & ( 1u << k ))
			continue;
		glBindBuffer( GL_ARRAY_BUFFER, p->uvbo[k] );
		glVertexAttribPointer( attr_uv[k], 2, GL_FLOAT, GL_FALSE, 0, NULL );
		voulu |= 1u << attr_uv[k];
	}
	if( v->env && ( x->a_normal >= 0 ))
	{
		glBindBuffer( GL_ARRAY_BUFFER, p->nbo );
		glVertexAttribPointer( x->a_normal, 3, GL_FLOAT, GL_FALSE, 0, NULL );
		voulu |= 1u << x->a_normal;
	}
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	dk_attrib( voulu );

	for( int k = p->passes - 1; k >= 0; --k )
	{
		glActiveTexture( GL_TEXTURE0 + k );
		glBindTexture( GL_TEXTURE_2D, p_tex[k] ? p_tex[k]->gl_name : 0 );
	}
	dk_melange( famille );
	return v;
}

bool GxmMateriauLot( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                     const int *p_first, const int *p_count, int n_plages,
                     unsigned int ibo, int famille )
{
	if( !dk_poser( p, p_tex, famille ))
		return false;
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, ibo );
	for( int i = 0; i < n_plages; ++i )
		glDrawElements( GL_TRIANGLES, p_count[i], GL_UNSIGNED_SHORT,
		                (const void *)(uintptr_t)( p_first[i] * sizeof( unsigned short )));
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	return true;
}

bool GxmMateriauMaillage( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                          unsigned int ibo, int num_indices, int famille )
{
	if( !dk_poser( p, p_tex, famille ))
		return false;
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, ibo );
	glDrawElements( GL_TRIANGLE_STRIP, num_indices, GL_UNSIGNED_SHORT, NULL );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	return true;
}

// "Precomputed" batch: a copy of everything the draw needs.
struct SDkLotPre
{
	SShaderMateriau m;
	SceGxmTexture  *tex[4];
	int             first, count;
	unsigned int    ibo;
	int             famille;
};

void *GxmMateriauPreparer( const SShaderMateriau *p, SceGxmTexture *const *p_tex,
                           int first, int count, unsigned int ibo, int famille )
{
	if( !variante( p ))
		return NULL;
	SDkLotPre *pre = (SDkLotPre *)calloc( 1, sizeof( SDkLotPre ));
	if( !pre )
		return NULL;
	pre->m = *p;
	for( int k = 0; k < 4; ++k )
		pre->tex[k] = ( k < p->passes ) ? p_tex[k] : NULL;
	pre->first = first;
	pre->count = count;
	pre->ibo = ibo;
	pre->famille = famille;
	return pre;
}

void GxmMateriauLotPre( void *p_pre )
{
	SDkLotPre *pre = (SDkLotPre *)p_pre;
	if( pre )
		GxmMateriauLot( &pre->m, pre->tex, &pre->first, &pre->count, 1, pre->ibo, pre->famille );
}

void GxmMateriauLibererPre( void *p_pre )
{
	free( p_pre );
}

void GxmMateriauFin()
{
	if( !s_dk_actif )
		return;
	s_dk_actif = false;
	dk_attrib( 0 );
	glUseProgram( 0 );
	glBindBuffer( GL_ARRAY_BUFFER, 0 );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, 0 );
	for( int k = 3; k >= 0; --k )
	{
		glActiveTexture( GL_TEXTURE0 + k );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	if( s_dk_blend_avant ) glEnable( GL_BLEND ); else glDisable( GL_BLEND );
	if( s_dk_alpha_avant ) glEnable( GL_ALPHA_TEST ); else glDisable( GL_ALPHA_TEST );
	glBlendFunc( s_dk_src, s_dk_dst );
	glBlendEquation( s_dk_eq );
	s_dk_cour = NULL;
}

// Characters: drawn by the GL path (ShaderPeau*), never through these.
bool GxmPeauPret() { return false; }
bool GxmPeauEclaireePret() { return false; }
void GxmPeauDebut( const float *, const float *, int, bool, const float * ) {}
void GxmPeauPiece( unsigned int, unsigned int, unsigned int, unsigned int, void *, bool, const float *,
                   unsigned int, int, unsigned int ) {}
void GxmPeauFin() {}

// Command lists and the batch worker thread: GXM only.
bool GxmListeDebut( void ) { return false; }
bool GxmListeOuverte( void ) { return false; }
void GxmListeFin( void ) {}
bool GxmTravDebut( const float *, const float * ) { return false; }
bool GxmTravPousser( void * ) { return false; }
void GxmTravFin( void ) {}
