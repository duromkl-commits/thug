///////////////////////////////////////////////////////////////////////////////
// p_occlusion.cpp -- occlusion par volumes d'ombre, cote Vita
//
// Voir p_occlusion.h pour la provenance du mecanisme.
//
// PRINCIPE. Un quad occludeur, vu depuis la camera, definit une pyramide
// infinie : tout ce qui est entierement dedans ET derriere le quad est cache.
// Le volume se decrit par cinq plans -- la face, et les quatre cotes formes
// par la camera et chaque arete. Un objet est occlus s'il est du cote interieur
// des cinq.
//
// CE QUI EST SIMPLIFIE par rapport a l'original : celui-ci tient un score par
// occludeur et fait tourner cycliquement les candidats d'une image a l'autre
// pour amortir le cout du test. Nous choisissons plus simplement les N quads
// au meilleur angle solide apparent (surface / distance au carre). Le test
// coute cinq produits scalaires par occludeur et par objet ; en garder trop
// couterait plus cher que ce qu'il fait economiser.
//
// LE TEST EST CONSERVATEUR. Un faux rejet efface de la geometrie visible, un
// faux positif ne coute qu'un dessin inutile : au moindre doute, on dessine.

#include <core/defines.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "vita_log.h"
#include "p_occlusion.h"

namespace NxVita
{

// A L'ARRET par defaut : la premiere version effacait du decor a tort
// (position de camera fausse, corrigee depuis). Elle ne se rallumera par
// defaut qu'une fois verifiee a l'ecran. � occ 1 � pour l'essayer.
bool g_vita_occlusion = false;

// Au-dela, le test coute plus qu'il ne rapporte. Mesure a faire : « occ 0/1 »
// compare les deux etats a chaud.
#define MAX_OCCLUDEURS_ACTIFS 6
#define MAX_QUADS             256

struct SQuadOcclusion
{
	float        v[4][3];
	float        normale[3];
	float        centre[3];
	float        aire;			// approximee, pour classer les candidats
	unsigned int checksum;
	bool         actif;
};

struct SOccludeur
{
	// Cinq plans (nx, ny, nz, d). Un point p est du cote interieur du plan i
	// quand dot(n, p) - d < 0.
	float plans[5][4];
};

static SQuadOcclusion s_quads[MAX_QUADS];
static int            s_nb_quads = 0;

static SOccludeur     s_occ[MAX_OCCLUDEURS_ACTIFS];
static int            s_nb_occ = 0;


static void produit_vectoriel( float *out, const float *a, const float *b )
{
	out[0] = ( a[1] * b[2] ) - ( a[2] * b[1] );
	out[1] = ( a[2] * b[0] ) - ( a[0] * b[2] );
	out[2] = ( a[0] * b[1] ) - ( a[1] * b[0] );
}

static float produit_scalaire( const float *a, const float *b )
{
	return ( a[0] * b[0] ) + ( a[1] * b[1] ) + ( a[2] * b[2] );
}


void OcclusionAjouter( const float *p_verts, unsigned int checksum )
{
	if( s_nb_quads >= MAX_QUADS )
	{
		static bool s_dit = false;
		if( !s_dit )
		{
			s_dit = true;
			VLOG( "OCC", "!! plus de place pour les quads d'occlusion (%d)",
			      MAX_QUADS );
		}
		return;
	}

	SQuadOcclusion *q = &s_quads[s_nb_quads];
	for( int i = 0; i < 4; ++i )
		for( int k = 0; k < 3; ++k )
			q->v[i][k] = p_verts[( i * 3 ) + k];

	// Normale et aire, depuis les deux diagonales : bon marche et suffisant
	// pour classer les candidats.
	float d0[3], d1[3], n[3];
	for( int k = 0; k < 3; ++k )
	{
		d0[k] = q->v[2][k] - q->v[0][k];
		d1[k] = q->v[3][k] - q->v[1][k];
		q->centre[k] = ( q->v[0][k] + q->v[1][k] + q->v[2][k] + q->v[3][k] )
		               * 0.25f;
	}
	produit_vectoriel( n, d0, d1 );
	const float l = sqrtf( produit_scalaire( n, n ));
	if( l > 0.0001f )
	{
		q->normale[0] = n[0] / l;
		q->normale[1] = n[1] / l;
		q->normale[2] = n[2] / l;
		q->aire = l * 0.5f;
	}
	else
	{
		// Quad degenere : inutilisable comme occludeur.
		return;
	}

	q->checksum = checksum;
	q->actif    = true;
	++s_nb_quads;
}


void OcclusionActiver( unsigned int checksum, bool actif )
{
	for( int i = 0; i < s_nb_quads; ++i )
		if( s_quads[i].checksum == checksum )
			s_quads[i].actif = actif;
}


void OcclusionVider( void )
{
	s_nb_quads = 0;
	s_nb_occ   = 0;
}


int OcclusionNbActifs( void ) { return s_nb_occ; }
int OcclusionNbConnus( void ) { return s_nb_quads; }


// Construit le volume d'ombre d'un quad vu depuis la camera. Rend false si le
// quad ne peut pas servir depuis ce point de vue.
static bool construire_volume( SOccludeur *p_out, const SQuadOcclusion *q,
                               const float *cam )
{
	// De quel cote la camera est-elle ? La normale du plan de face doit
	// pointer VERS elle, pour que « derriere le quad » soit le cote negatif.
	float vers_cam[3];
	for( int k = 0; k < 3; ++k )
		vers_cam[k] = cam[k] - q->v[0][k];
	const float sens = produit_scalaire( vers_cam, q->normale );

	// Camera dans le plan du quad : le volume est degenere.
	if(( sens > -0.001f ) && ( sens < 0.001f ))
		return false;

	const float signe = ( sens >= 0.0f ) ? 1.0f : -1.0f;
	for( int k = 0; k < 3; ++k )
		p_out->plans[0][k] = q->normale[k] * signe;
	p_out->plans[0][3] = produit_scalaire( p_out->plans[0], q->v[0] );

	// Un point franchement a l'interieur du volume : le centre du quad,
	// pousse loin dans la direction qui s'eloigne de la camera.
	float dedans[3], dir[3];
	float dl = 0.0f;
	for( int k = 0; k < 3; ++k )
	{
		dir[k] = q->centre[k] - cam[k];
		dl += dir[k] * dir[k];
	}
	dl = sqrtf( dl );
	if( dl < 0.001f )
		return false;
	for( int k = 0; k < 3; ++k )
		dedans[k] = q->centre[k] + ( dir[k] / dl ) * 100.0f;

	// Les quatre cotes : chaque plan passe par la camera et une arete.
	for( int e = 0; e < 4; ++e )
	{
		const float *a = q->v[e];
		const float *b = q->v[( e + 1 ) & 3];
		float ca[3], cb[3], n[3];
		for( int k = 0; k < 3; ++k )
		{
			ca[k] = a[k] - cam[k];
			cb[k] = b[k] - cam[k];
		}
		produit_vectoriel( n, ca, cb );
		const float l = sqrtf( produit_scalaire( n, n ));
		if( l < 0.0001f )
			return false;			// arete vue par la tranche
		for( int k = 0; k < 3; ++k )
			n[k] /= l;

		float d = produit_scalaire( n, cam );
		// L'interieur doit etre du cote negatif : on retourne le plan si le
		// point temoin tombe du mauvais cote. C'est plus sur que de raisonner
		// sur l'ordre des sommets, qui depend du sens du quad d'origine.
		if(( produit_scalaire( n, dedans ) - d ) > 0.0f )
		{
			for( int k = 0; k < 3; ++k )
				n[k] = -n[k];
			d = -d;
		}
		for( int k = 0; k < 3; ++k )
			p_out->plans[e + 1][k] = n[k];
		p_out->plans[e + 1][3] = d;
	}

	// Verification finale : le point temoin doit etre interieur aux cinq
	// plans. S'il ne l'est pas, le volume est mal forme et on renonce plutot
	// que de risquer d'effacer de la geometrie visible.
	for( int i = 0; i < 5; ++i )
	{
		if(( produit_scalaire( p_out->plans[i], dedans )
		     - p_out->plans[i][3] ) >= 0.0f )
			return false;
	}
	return true;
}


void OcclusionConstruire( const float *p_cam )
{
	s_nb_occ = 0;
	if( !g_vita_occlusion || ( s_nb_quads <= 0 ))
		return;

	// Classement des candidats par angle solide apparent : un grand mur tout
	// pres cache beaucoup, un petit panneau au loin ne cache rien.
	float meilleurs[MAX_OCCLUDEURS_ACTIFS];
	int   indices[MAX_OCCLUDEURS_ACTIFS];
	int   n = 0;

	for( int i = 0; i < s_nb_quads; ++i )
	{
		const SQuadOcclusion *q = &s_quads[i];
		if( !q->actif )
			continue;

		float d[3];
		for( int k = 0; k < 3; ++k )
			d[k] = q->centre[k] - p_cam[k];
		const float d2 = produit_scalaire( d, d );
		if( d2 < 1.0f )
			continue;
		const float score = q->aire / d2;

		// Insertion dans le classement, qui est court.
		int pos = n;
		while(( pos > 0 ) && ( meilleurs[pos - 1] < score ))
		{
			if( pos < MAX_OCCLUDEURS_ACTIFS )
			{
				meilleurs[pos] = meilleurs[pos - 1];
				indices[pos]   = indices[pos - 1];
			}
			--pos;
		}
		if( pos < MAX_OCCLUDEURS_ACTIFS )
		{
			meilleurs[pos] = score;
			indices[pos]   = i;
			if( n < MAX_OCCLUDEURS_ACTIFS )
				++n;
		}
	}

	for( int i = 0; i < n; ++i )
	{
		if( construire_volume( &s_occ[s_nb_occ], &s_quads[indices[i]], p_cam ))
			++s_nb_occ;
	}

	// Trace periodique : sans elle, « l'occlusion est en place » ne dit rien
	// de ce qu'elle retire.
	{
		static int s_f = 0;
		if((( ++s_f ) % 600 ) == 0 )
			VLOG( "OCC", "occlusion : %d quads connus, %d volumes actifs",
			      s_nb_quads, s_nb_occ );
	}
}


bool OcclusionTesteSphere( float x, float y, float z, float rayon )
{
	if( !g_vita_occlusion )
		return false;

	for( int o = 0; o < s_nb_occ; ++o )
	{
		const float (*p)[4] = s_occ[o].plans;
		// Il suffit d'un plan qui laisse la sphere dehors pour conclure que
		// cet occludeur ne la cache pas.
		if((( p[0][0] * x ) + ( p[0][1] * y ) + ( p[0][2] * z ) - p[0][3] ) >= -rayon )
			continue;
		if((( p[1][0] * x ) + ( p[1][1] * y ) + ( p[1][2] * z ) - p[1][3] ) >= -rayon )
			continue;
		if((( p[2][0] * x ) + ( p[2][1] * y ) + ( p[2][2] * z ) - p[2][3] ) >= -rayon )
			continue;
		if((( p[3][0] * x ) + ( p[3][1] * y ) + ( p[3][2] * z ) - p[3][3] ) >= -rayon )
			continue;
		if((( p[4][0] * x ) + ( p[4][1] * y ) + ( p[4][2] * z ) - p[4][3] ) >= -rayon )
			continue;
		return true;
	}
	return false;
}

} // namespace NxVita
