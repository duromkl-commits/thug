///////////////////////////////////////////////////////////////////////////////
// p_NxModel.h â€” modeles (skater, objets de niveau)
//
// Decouverte qui structure tout ce fichier : dans ce moteur, un MAILLAGE EST
// UNE SCENE. s_plat_load_mesh du backend DX9 (p_nx.cpp:999) se contente
// d'appeler s_plat_load_scene sur le .mdl. On reutilise donc tel quel le
// decodeur ecrit pour les niveaux.
//
// Chaine du moteur :
//   CModel::AddGeom  -> sLoadMesh          -> s_plat_load_mesh  (le CMesh)
//                    -> sInitGeom          -> s_plat_init_geom  (le CGeom)
//                    -> CGeom::LoadGeomData -> plat_load_geom_data (les relie)
//   puis, chaque frame : CModel::Render -> plat_render( matrice racine, os )
//
// Les matrices d'os sont recues mais IGNOREES pour l'instant : le modele est
// dessine dans sa pose de repos. L'animation demande un skinning, au CPU ou
// par shader, et ce sera un morceau a part entiere.

#ifndef __GFX_VITA_P_NXMODEL_H__
#define __GFX_VITA_P_NXMODEL_H__

#include <gfx/NxMesh.h>
#include <gfx/NxGeom.h>
#include <gfx/NxModel.h>
#include <vitaGL.h>

namespace NxVita
{
	struct SVitaSceneGeom;
	// Incremente a chaque TRANSITION d'etat actif d'un geom : le rendu du
	// decor s'en sert pour savoir que ses plages sont inchangees (issue #18).
	extern unsigned int g_vita_gen_actif;
}

namespace Nx
{

class CVitaModelLights;

// Eclairage par sommet : actif par defaut, coupable a la commande « lum 0 ».
extern bool g_vita_lighting;

// Un morceau de geometrie deja televerse sur le GPU.
struct SGpuMesh
{
	GLuint	vbo;
	GLuint	uvbo;
	GLuint	cbo;			// couleurs de sommets, 0 si absentes
	GLuint	ibo;
	GLuint	texture;
	unsigned int blend;		// blend mode du materiau, 0 = opaque
	// MATFLAG_PASS_COLOR_LOCKED (0x80) de la passe 0 : la teinte du geom ne
	// s'applique PAS. [SOURCE] XBox/NX/mesh.cpp:607 (HandleColorOverride).
	// Yeux, logos, une partie du corps : sans ce drapeau, la couleur CAS
	// d'une piece les noircissait avec elle.
	unsigned char color_locked;
	// Speculaire de la passe 0 (issue #45) : couleur (0-2), puissance (3),
	// puissance 0 = sans. Ajoutee par la peau eclairee vitaGL.
	float	spec[4];
	// GLOSS_MAP (issue #45) : speculaire modulee par l'alpha de la texture de
	// la passe 1, lue avec le jeu d'UV 1 ([SOURCE] XBox/NX/render.cpp:483,
	// "mul v1.rgb,v1.rgb,t1.a"). 0 = sans ; gloss_uvbo propre a la piece.
	GLuint	gloss_tex;
	GLuint	gloss_uvbo;
	unsigned char gloss_clamp;		// bit 0 : U borne, bit 1 : V borne (addr2)
	// [SOURCE] XBox/NX/material.cpp:305 -- CULLMODE CW sauf no_bfc : les
	// personnages ont leurs faces arriere eliminees (#46).
	unsigned char no_bfc;
	// Couleur de MATERIAU modifiable (#58, CAS : couleur des roues, de la peau...) :
	// checksum du materiau et couleurs de sommets BRUTES (avant la teinte cuite
	// dans cbo), pour recalculer cbo quand SetMaterialColor change la couleur.
	unsigned int   mat_checksum;
	unsigned char *p_couleurs_brutes;

	// Os du SECTEUR, pour les modeles a parties rigides (vehicules) : ce
	// maillage doit etre transforme par la matrice de cet os. -1 si le modele
	// n'est pas hierarchique. A ne pas confondre avec p_bones (skinning).
	int		sector_bone;
	int		num_indices;

	// Cote CPU, conserve UNIQUEMENT pour les maillages skinnes : il faut
	// retransformer les sommets a chaque frame, donc le tampon GPU statique
	// ne suffit plus.
	int				num_vertices;
	float          *p_positions;	// pose de repos
	unsigned int   *p_weights;
	unsigned short *p_bones;
	float          *p_skinned;		// tampon de travail, PARTAGE par secteur
	int				owns_verts;		// qui libere les sommets bruts
	int				owns_skin;		// qui calcule le skinning et libere le tampon

	// Eclairage par sommet (DX9/p_NxLight.cpp:64) : couleur = teinte x
	// ( ambiante + somme( diffuse_i x max( 0, N . dir_i ))). La normale doit
	// suivre le squelette comme la position, sinon l'ombrage tourne avec le
	// personnage au lieu de rester fixe dans le monde.
	float          *p_normals;		// normales de repos, NULL si le format n'en a pas
	// Normales APRES skinning, partagees comme p_skinned : le skater a 18
	// pieces sur le meme jeu de sommets ; les retransformer une fois par piece
	// coutait dix-huit fois le travail necessaire.
	float          *p_skinned_n;
	// Eclairage SANS la teinte, partage comme les sommets : c'est le terme
	// couteux (produits scalaires, normalisation) et il est identique pour
	// toutes les pieces d'un meme groupe. Le calculer par piece coutait 18
	// fois le travail utile -- mesure : 153 ms par frame a lui seul.
	unsigned char  *p_lit_base;
	// Eclairage teinte, propre a la piece. NULL quand la teinte est blanche :
	// le rendu lit alors p_lit_base directement, sans copie ni calcul.
	unsigned char  *p_lit;

	// Tampons GPU des donnees qui changent a chaque frame.
	//
	// Mesure : dessiner depuis des tableaux COTE CLIENT tout en liant un IBO
	// cote GPU coutait 18,5 ms par appel -- vitaGL doit alors recopier la
	// geometrie a chaque glDrawElements. Avec un VBO dynamique, on televerse
	// une fois par frame ce qui a change, et le dessin ne transfere plus rien.
	GLuint	vbo_skin;		// positions skinnees, PARTAGE comme p_skinned
	// Skinning par le GPU (issue #18) : pose de repos, poids (3 flottants
	// depaquetes) et indices d'os (3 flottants), statiques, PARTAGES comme
	// p_skinned. 0 si le groupe n'est pas skinne.
	GLuint	vbo_repos, vbo_poids, vbo_os;
	GLuint	cbo_lit;		// couleurs eclairees, propre a la piece

	// Indices d'ORIGINE, conserves pour le retrait de polygones (CAS).
	// Cacher le torse sous un t-shirt consiste a retirer des triangles ; il
	// faut donc pouvoir repartir de la bande intacte a chaque changement de
	// tenue, et non d'une bande deja amputee.
	unsigned short *p_indices_src;
	// Sommets REFERENCES par la piece (#18) : les pieces d'un secteur
	// partagent tous ses sommets ; l'eclairage CPU ne parcourt que ceux-ci.
	unsigned short *p_lum_idx;
	unsigned long long lum_t;	// dernier reeclairage (us)
	int				num_lum_idx;
	// Taille de p_indices_src. num_indices, lui, est ECRASE par chaque
	// reconstruction (les degeneres inseres l'augmentent) : relire la source
	// avec la taille courante deborderait au deuxieme changement de tenue.
	int				num_indices_src;
	// Recopie depuis SVitaMesh : index du maillage dans son secteur, seule
	// cle valable pour retrouver une entree CAS.
	int				load_order;

	// Descripteur GXM de la texture, pour le skinning en GXM direct (issue
	// #18). Valable tant que gxm_tex_nom == texture : un remplacement CAS
	// change le nom, et donc invalide le cache.
	void           *gxm_tex;
	GLuint			gxm_tex_nom;
	// Plus grand indice d'os reference par les sommets (poids nuls compris),
	// calcule une fois ; max_os_ok = 0 tant qu'il ne l'est pas.
	short			max_os;
	unsigned char	max_os_ok;
	// Normales de repos sur le GPU, pour l'eclairage (issue #4). Creees a la
	// demande, partagees comme vbo_repos.
	GLuint			vbo_normales;

	// Eclairage CPU des pieces RIGIDES eclairables (XBox instance.cpp:200) :
	// normales et couleurs de base (celles du cbo) gardees, couleurs eclairees
	// dans cbo_rig, recalculees seulement quand la cle (lumieres dans l'espace
	// du modele) change. NULL / 0 pour toute autre piece.
	float          *p_n_rig;
	unsigned char  *p_c_base;
	unsigned char  *p_c_lit;
	GLuint			cbo_rig;
	// Normales des pieces rigides sur le GPU (#67, "rgp 1") : eclairage par
	// le shader rigide eclaire au lieu d'eclairer_piece. Cree au premier
	// dessin par ce chemin, propre a la piece. 0 sinon.
	GLuint			nbo_rig;
	float			lum_cle[21];
	unsigned char	lum_ok;

	// DEUXIEME PASSE DE MATERIAU des personnages (#55) : logos, imprimes,
	// decalques des vetements CAS. [SOURCE] XBox/NX/render.cpp:302 compose
	// les passes dans un seul pixel shader ; pour la passe 1 en BLEND, cela
	// revient a peindre la texture de la passe 1 (jeu d'UV 1) en alpha
	// par-dessus la passe 0. On la dessine donc comme une piece a part,
	// translucide, qui PARTAGE les sommets de sa piece de base (owns_* a 0)
	// mais a ses propres uvbo, cbo et ibo. vbo vaut 0 : elle ne passe que par
	// les chemins skinnes du GPU (shader vitaGL), les autres l'ignorent.
	unsigned char	decal;
	// Adressage de la passe 1 (0 repetition, sinon bloque au bord) : les UV
	// des logos debordent largement de [0,1] (t-shirt : -1,8..1,3).
	unsigned char	decal_au, decal_av;

	// GYROPHARES DES VEHICULES (issue #77). La rampe de veh_policecar_* est
	// faite de maillages a UV WIBBLE : une tache claire au centre de la
	// texture defile en U (1,5 a 2 largeurs par seconde), teinte en rouge ou
	// bleu par la couleur de materiau -- c'est la rotation du gyrophare. Les
	// maillages opaques ont en plus une passe 1 ADDITIVE (meme texture, meme
	// defilement) qui fait l'eclat. [SOURCE] XBox/NX/material.cpp:311
	// (figure_wibble_uv a chaque Submit, decor ET modeles) et
	// XBox/NX/render.cpp:424 (passe 1 ADD : r0 += r1.rgb * r1.a).
	// UV wibble de la passe 0 : bit 0 = actif, parametres de sUVWibbleParams.
	unsigned char	uvw0;
	float			uvw_par0[8];
	// Passe 1 ADD d'une piece RIGIDE opaque, redessinee par-dessus la base
	// (memes sommets, meme ibo, meme matrice d'os) en additif. add_tex = 0 :
	// pas de telle passe. add_uvbo et add_cbo sont propres a la piece.
	GLuint			add_tex;
	GLuint			add_uvbo;
	GLuint			add_cbo;
	unsigned char	add_uvw;
	unsigned char	add_locked;
	float			add_uvw_par[8];
};

// Une entree CAS designe UN triangle a retirer quand le masque correspond.
// Format releve sur DX9/p_NxMesh.cpp:69-97 -- fichier ï¿½ xxx.cas.xbx ï¿½ associe
// a chaque ï¿½ xxx.skin.xbx ï¿½.
struct SCasEntry
{
	unsigned int mask;
	unsigned int data0;		// maillage = data0 >> 16, i0 = data0 & 0xFFFF
	unsigned int data1;		// i1 = data1 >> 16,        i2 = data1 & 0xFFFF
};


class CVitaMesh : public CMesh
{
public:
	CVitaMesh();
	virtual ~CVitaMesh();

	void		Build( const char *p_filename, CTexDict *p_tex_dict );
	// Chemin Create-A-Skater : la piece arrive en RAM (skaterparts.pre), avec
	// ses donnees CAS a part. Meme format que le fichier, meme construction.
	void		BuildFromMemory( const void *p_data, int size,
	                             const unsigned char *p_cas_data,
	                             CTexDict *p_tex_dict, const char *p_label );
	// p_bone_mats : matrices d'os du modele, pour les secteurs RIGIDES
	// (vehicules). Chaque morceau porte l'index de SON os ; sans ces
	// matrices, toutes les parties se dessinent empilees a l'origine.
	void		Draw( const float *p_rgba = NULL,
	                  Mth::Matrix *p_bone_mats = NULL, int num_bones = 0,
	                  CVitaModelLights *p_lights = NULL ) const;
	// Applique les matrices d'os puis dessine. Sans os, retombe sur Draw().
	// p_lights : lumieres propres au modele, NULL pour celles du monde.
	void		DrawSkinned( Mth::Matrix *p_bones, int num_bones,
	                         const float *p_rgba = NULL,
	                         CVitaModelLights *p_lights = NULL );
	bool		IsSkinned() const;
	int			NumPieces() const	{ return m_num; }
	// Pieces GPU, en lecture : la carte d'ombre les redessine (p_ombre.cpp).
	const SGpuMesh *	Pieces() const	{ return mp_pieces; }
	const char *Name() const		{ return m_vita_name; }
	// Dossier parent du fichier source (« ped_male »...) : la sonde « pdt »
	// (issue #46) en a besoin, le nom court l'a perdu.
	const char *Dossier() const	{ return m_vita_dossier; }

	// Retire les triangles dont le masque CAS correspond, et remet les autres.
	// Repart toujours des indices d'origine : appeler avec 0 restaure tout.
	void		HidePolys( unsigned int mask );

	// Boite englobante de tous les morceaux, en espace modele. Calculee une
	// fois au chargement : CElement3d::AutoComputeScale s'en sert pour
	// dimensionner ET centrer les modeles affiches dans l'interface.
	const Mth::CBBox &	BBox() const	{ return m_bbox; }

	// Duree de vie (#32). Les geoms -- y compris les CLONES, qui partagent le
	// maillage au lieu de le recopier comme XBox (sMesh::Clone) -- le
	// retiennent. sUnloadMesh ne le detruit que si plus personne ne le tient ;
	// sinon le dernier geom s'en charge. Sans cela un clone survivant dessinait
	// avec des noms de tampons GL liberes : adresses au hasard, plantage GPU.
	void		Retenir()			{ ++m_vita_refs; }
	void		Lacher();
	void		Decharger();
	friend void VitaBilanMeshes( void );	// bilan des fuites (#47)
private:
	int			m_vita_refs;
	bool		m_vita_orphelin;

	// Charge ï¿½ xxx.cas.xbx ï¿½ a cote de ï¿½ xxx.skin.xbx ï¿½. Sans lui, aucun
	// triangle n'est retirable et le corps traverse les vetements.
	void		load_cas_table( const char *p_skin_filename );
	// Variante RAM : memes champs (version, masque si v>=2, compte, entrees),
	// depuis le tampon fourni par le moteur.
	void		load_cas_from_memory( const unsigned char *p_cas_data );
	// Construction GPU commune aux deux chemins (fichier et memoire).
	void		build_pieces( NxVita::SVitaSceneGeom *p_geom, CTexDict *p_tex_dict );

	SGpuMesh *	mp_pieces;
	int			m_num;

	// Nom court du fichier source : sans lui, aucune trace de rendu ne peut
	// dire QUELLE piece est dessinee ou absente -- et c'est exactement la
	// question quand un pantalon entier manque a l'ecran.
	char		m_vita_name[48];
	// Modele non skinne ECLAIRABLE comme XBox (instance.cpp:259) : tous ses
	// maillages ont des normales et aucun n'est UNLIT.
	bool		m_vita_eclairable;
	char		m_vita_dossier[32];

	Mth::CBBox	m_bbox;

	SCasEntry *	mp_cas;
	int			m_num_cas;
	unsigned int m_cas_applied;		// dernier masque applique
};


// Couleurs eclairees d'une piece rigide PROPRES A UNE INSTANCE (#78). Le
// maillage est PARTAGE par toutes les instances du meme modele (gestionnaire
// d'assets, modelcomponent.cpp:209 ; clones, plat_clone) : garder la cle et
// le cbo eclaire dans la piece faisait de chaque camion le dernier eclaire
// par un autre camion du meme modele. XBox eclaire chaque instance a chaque
// dessin (D3DRS_LIGHTING, NX/instance.cpp:200). Une entree par piece, tenue
// par le geom ; cbo cree au premier eclairage (4 x num_vertices octets).
struct SLumInstance
{
	GLuint				cbo;
	unsigned long long	t;			// dernier reeclairage (us)
	float				cle[21];
	unsigned char		ok;
};


class CVitaGeom : public CGeom
{
public:
	CVitaGeom() : mp_mesh( NULL ), m_vita_active( true ), m_vita_visible( 0xFF ), m_vita_affiche( true ),
	              mp_vita_lights( NULL ), m_vita_secteur( 0 ), m_vita_clone( false ),
	              m_vita_rot( 0 ), m_vita_suit_modele( false ), m_vita_racine_ok( false ),
	              mp_vita_modele( NULL ), mp_vita_pose_os( NULL ), m_vita_pose_nos( 0 ),
	              m_vita_pose_image( 0 ), m_vita_rejeu_image( 0 ), m_vita_pose_ok( false ), m_vita_pose_inscrit( false ), m_vita_os_vus( false ),
	              m_vita_scene( -1 ), m_vita_bbox_ok( false ),
	              mp_vita_lum( NULL ), m_vita_lum_n( 0 ), mp_vita_lum_mesh( NULL )
	{
		m_vita_rgba[0] = m_vita_rgba[1] = m_vita_rgba[2] = m_vita_rgba[3] = 1.0f;
		m_vita_color = Image::RGBA( 0x80, 0x80, 0x80, 0x80 );
		m_vita_pos.Set( 0.0f, 0.0f, 0.0f, 1.0f );
	}
	virtual ~CVitaGeom();

	// Geom d'un secteur du DECOR (issue #29) : le checksum de son secteur, par
	// lequel le rendu retrouve ses maillages. Un CLONE (editeur de parc) les
	// dessine a sa propre place : sommet = R x source + position, R par quarts
	// de tour -- la semantique de XBox/NX/mesh.cpp:436 et :496.
	void			VitaSetSecteur( unsigned int c )	{ m_vita_secteur = c; }
	unsigned int	VitaSecteur() const				{ return m_vita_secteur; }
	// Scene d'origine dans le monde Vita (p_world_render, SceneMondeCourante)
	// et boite du secteur lue dans le fichier -- [SOURCE] XBox/p_nxsector.cpp
	// :303, m_bbox du geom, recopiee telle quelle par le clone
	// (p_NxGeom.cpp:1405). Issue #65.
	void			VitaSetOrigine( int scene, const float *p_bb )
	{
		m_vita_scene = scene;
		if( p_bb )
		{
			m_vita_bbox.Set( Mth::Vector( p_bb[0], p_bb[1], p_bb[2] ),
			                 Mth::Vector( p_bb[3], p_bb[4], p_bb[5] ));
			m_vita_bbox_ok = true;
		}
	}
	int				VitaScene() const				{ return m_vita_scene; }
	bool			VitaEstClone() const			{ return m_vita_clone; }
	bool			VitaActif() const				{ return m_vita_affiche; }
	const Mth::Vector &	VitaPos() const			{ return m_vita_pos; }
	int				VitaRot() const					{ return m_vita_rot; }

	// Objet de niveau mobile (Class = LevelObject, issue #40) : clone d'un
	// secteur porte par un CModel, place par la matrice racine du modele et
	// non par position + quarts de tour. [SOURCE] XBox/p_NxGeom.cpp:493,
	// plat_render fait mp_instance->SetTransform( *pRootMatrix ).
	// VitaRacine() rend NULL tant que le modele n'a rien pose.
	bool			VitaSuitModele() const			{ return m_vita_suit_modele; }
	const float *	VitaRacine() const
	{
		return m_vita_racine_ok ? (const float *)&m_vita_racine : NULL;
	}

	// Adresse de l'etat actif, pour que le rendu du DECOR la consulte sans
	// remonter jusqu'au secteur a chaque maillage et a chaque image.
	// Actif ET visible (issue #29) : c'est ce que le rendu doit consulter.
	const bool *	VitaActivePtr() const			{ return &m_vita_affiche; }
	void			VitaTraceActif( bool active ) const;

	CVitaMesh *	Mesh() const	{ return mp_mesh; }

private:
	virtual bool plat_load_geom_data( CMesh *pMesh, CModel *pModel,
	                                  bool color_per_material );

	// C'est ICI que le moteur dessine, pas dans CModel : CModel::Render
	// (NxModel.cpp:329) delegue a CGeom::Render pour chaque geom, qui appelle
	// cette virtuelle. Mon premier essai plaÃ§ait le rendu dans
	// CModel::plat_render -- jamais appele.
	virtual bool plat_render( Mth::Matrix *pRootMatrix,
	                          Mth::Matrix *ppBoneMatrices, int numBones );

	// La version de base rend NULL (NxGeom.cpp:44). CGeom::Clone appelle
	// ensuite SetActive() sur ce NULL -- releve dans un psp2core lors d'une
	// CHUTE du skater, qui reconstruit son modele. Deux surcharges portent ce
	// nom : c'est celle a CModel* qui est empruntee ici.
	virtual CGeom *	plat_clone( bool instance, CScene *pDestScene = NULL );
	virtual CGeom *	plat_clone( bool instance, CModel *pDestModel );

	// Etat actif. Les versions de base JETTENT la valeur a l'ecriture et
	// rendent false a la lecture (NxGeom.cpp:161-175). Or CModel::Render pose
	// cet etat sur chaque geom depuis son masque (NxModel.cpp:292) : c'est
	// ainsi que le moteur cache le torse sous un t-shirt. Sans le conserver,
	// on dessinait TOUT, et le corps traversait les vetements.
	// Journalise les TRANSITIONS d'etat. Le chemin qui eteint un secteur est
	// verifie ; celui qui le rallume ne l'est pas encore, et il n'y a aucune
	// raison de le supposer bon. Cette trace le dira au moment ou une mission
	// reclamera ses barrieres.
	virtual bool	plat_set_material_color( uint32 mat_checksum, int pass, Image::RGBA rgba );
	virtual void	plat_set_active( bool active )
	{
		if( active != m_vita_active )
		{
			VitaTraceActif( active );
			++NxVita::g_vita_gen_actif;
		}
		m_vita_active = active;
		maj_affiche();
	}
	virtual bool	plat_is_active() const			{ return m_vita_active; }

	// Masque de visibilite par vue. Les versions de base le jettent et rendent
	// 0 (NxGeom.cpp:142-152) ; XBox le garde par maillage (p_NxGeom.cpp:543).
	// L'editeur de parc cache ainsi ses pieces (ParkGen.cpp:348, masque 0 ou
	// 0xFF) : sans lui, toutes restaient dessinees (#29). Une seule vue
	// rendue sur Vita : le bit 0 decide.
	virtual void	plat_set_visibility( uint32 mask )
	{
		m_vita_visible = mask;
		maj_affiche();
	}
	virtual uint32	plat_get_visibility() const		{ return m_vita_visible; }
	void			maj_affiche()
	{
		const bool a = m_vita_active && (( m_vita_visible & 1 ) != 0 );
		if( a != m_vita_affiche )
			++NxVita::g_vita_gen_actif;
		m_vita_affiche = a;
	}


	// Retrait de polygones. La version de base rend TRUE sans rien faire
	// (NxGeom.cpp:336) -- elle affirme avoir masque le torse sous le t-shirt,
	// et aucun appelant ne pouvait s'apercevoir du contraire.
	virtual bool	plat_hide_polys( uint32 mask );

	// Modulation de couleur. La version de base jette la valeur
	// (NxGeom.cpp:87). C'est par elle que le menu principal transforme le
	// skater en SILHOUETTE NOIRE -- sans elle, il s'affiche texture.
	virtual void	plat_set_color( Image::RGBA rgba );
	// Retour au neutre, et relecture. Les versions de base etaient des stubs,
	// et plat_get_color rendait (0,0,0,0) -- du noir. [SOURCE]
	// XBox/p_NxGeom.cpp:575-650.
	virtual void		plat_clear_color();
	virtual Image::RGBA	plat_get_color() const	{ return m_vita_color; }

	// Boite englobante. La version de base rend une boite VIDE et affiche un
	// ï¿½ STUB: PlatGetBoundingBox ï¿½ (NxGeom.cpp:182) : avec elle,
	// AutoComputeScale dimensionne et centre les elements 3D de l'interface a
	// partir de rien -- panneau du menu mal echelle, skater mal place.
	virtual const Mth::CBBox &	plat_get_bounding_box() const;

	// #56 : sommets et couleurs de rendu. Les versions de base (NxGeom.cpp:360-
	// 431) rendent 0 ou ne font rien, mais IMPRIMENT un << STUB >> sur stdout a
	// chaque appel. FakeLights (NxLightMan.cpp) en fait au moins un par secteur
	// et par lumiere a chaque changement d'heure du jour. Meme resultat (aucun
	// sommet expose, rien n'est reeclaire), sans impression.
	virtual int		plat_get_num_render_polys()				{ return 0; }
	virtual int		plat_get_num_render_base_polys()		{ return 0; }
	virtual int		plat_get_num_render_verts()				{ return 0; }
	virtual void	plat_get_render_verts( Mth::Vector * )	{}
	virtual void	plat_get_render_colors( Image::RGBA * )	{}
	virtual void	plat_set_render_verts( Mth::Vector * )	{}
	virtual void	plat_set_render_colors( Image::RGBA * )	{}

	virtual void				plat_set_world_position( const Mth::Vector &pos );
	virtual const Mth::Vector &	plat_get_world_position() const	{ return m_vita_pos; }
	virtual void				plat_rotate_y( Mth::ERot90 rot );

	// Lumieres du modele. La version de base JETTE le pointeur (NxGeom.cpp:307
	// affiche Â« STUB: PlatSetModelLights Â») : on perdait donc la luminosite du
	// modele, celle que les cinematiques mettent a zero pour assombrir le
	// skater (cutscenedetails.cpp:4732).
	virtual void	plat_set_model_lights( CModelLights *p_lights )
	{
		mp_vita_lights = p_lights;
	}

	CVitaMesh *	mp_mesh;
	// Vrai par defaut : un geom qu'on n'a jamais desactive doit se voir.
	bool		m_vita_active;
	uint32		m_vita_visible;
	bool		m_vita_affiche;		// actif et visible
	// Teinte appliquee au dessin. Blanc opaque = pas de modulation.
	float		m_vita_rgba[4];
	// La valeur recue, telle quelle, pour plat_get_color.
	Image::RGBA	m_vita_color;
	CModelLights *	mp_vita_lights;
	unsigned int	m_vita_secteur;
	bool			m_vita_clone;
	Mth::Vector		m_vita_pos;
	int				m_vita_rot;
	// Clone de secteur sans scene de destination = porte par un modele
	// (modelcomponent.cpp:67, init_model_from_level_object). Sa place est
	// la matrice racine recue au rendu, copiee telle quelle.
	bool			m_vita_suit_modele;
	bool			m_vita_racine_ok;
	Mth::Matrix		m_vita_racine;
	// Modele porteur (plat_load_geom_data, plat_clone vers un modele) : l'ombre
	// portee capture la pose des geoms du modele projete (p_ombre.cpp).
	CModel *		mp_vita_modele;
	// Derniere pose recue par plat_render (#54) : rejouee par le rendu quand
	// le moteur n'a pas appele plat_render dans l'image (jeu en pause, objet
	// suspendu par la distance). XBox garde une instance persistante, dessinee
	// a chaque image avec sa derniere matrice (p_NxGeom.cpp:493).
public:
	void			VitaRetenirPose( const Mth::Matrix *pRoot, Mth::Matrix *pOs, int nOs );
	bool			VitaRejouerPose( unsigned image, unsigned fenetre = 0 );
	void			VitaOublierPose();
	const char *	VitaNomTrou( unsigned image ) const;
private:
	Mth::Matrix		m_vita_pose_racine;
	Mth::Matrix *	mp_vita_pose_os;
	int				m_vita_pose_nos;
	unsigned		m_vita_pose_image;
	unsigned		m_vita_rejeu_image;
	bool			m_vita_pose_ok;
	bool			m_vita_pose_inscrit;
	// Le moteur a deja fourni des matrices d'os a ce geom (vehicule anime) :
	// les images sans animation reprennent les dernieres (#48).
	bool			m_vita_os_vus;
	// Voir VitaSetOrigine (#65).
	int				m_vita_scene;
	Mth::CBBox		m_vita_bbox;
	bool			m_vita_bbox_ok;
	// Eclairage des pieces rigides propre a cette instance (#78, "lpi").
	SLumInstance *	mp_vita_lum;
	int				m_vita_lum_n;
	const CVitaMesh *	mp_vita_lum_mesh;
	void			liberer_lum();
};


class CVitaModel : public CModel
{
public:
	CVitaModel() {}
	virtual ~CVitaModel() {}
};

} // namespace Nx

#endif // __GFX_VITA_P_NXMODEL_H__
