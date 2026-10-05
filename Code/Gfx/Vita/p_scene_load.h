///////////////////////////////////////////////////////////////////////////////
// p_scene_load.h â€” lecture du format de scene Xbox
//
// Voir p_scene_load.cpp pour la provenance du format (backend DX9, qui est
// celui que kisak maintient).

#ifndef __GFX_VITA_P_SCENE_LOAD_H__
#define __GFX_VITA_P_SCENE_LOAD_H__

#include <core/defines.h>

namespace NxVita
{

// VERTEX COLOR WIBBLE (issue #45 V2) : couleurs de sommets animees (neons,
// lumieres pulsees). [SOURCE] XBox/NX/material.cpp:157 figure_wibble_vc, et
// XBox/NX/mesh.cpp:368 wibble_vc. Un seul bloc malloc, possede par le
// maillage (free() suffit) :
//   SVitaVcw | num_seqs SVitaVcwSeq | num_cles SVitaVcwCle
//            | num_sommets unsigned short (indice du sommet dans le tampon)
//            | num_sommets unsigned char (sequence, 0-based)
//            | 4 * num_sommets octets (couleur d'origine, RGBA 0..255)
struct SVitaVcwCle				// = sVCWibbleKeyframe (XBox/NX/material.h:51)
{
	int           t;			// millisecondes
	unsigned char r, g, b, a;	// echelle 0..128 (comme les couleurs de sommets)
};
struct SVitaVcwSeq				// = sVCWibbleParams
{
	int phase;
	int num_cles;
	int premiere;				// indice de sa premiere cle dans p_cles
};
struct SVitaVcw
{
	int             num_seqs;
	int             num_cles;
	int             num_sommets;
	SVitaVcwSeq    *p_seqs;
	SVitaVcwCle    *p_cles;
	unsigned short *p_sommets;
	unsigned char  *p_seq;
	unsigned char  *p_orig;
};

// BILLBOARD (issue #2) : quad de 4 sommets qui pivote face a la camera
// (arbres, silhouettes, lueurs). [SOURCE] XBox/p_nxsector.cpp:314 (lecture),
// XBox/NX/mesh.cpp:887 SetBillboardData (repere u, v, n du quad),
// XBox/NX/billboard.cpp:340 et BillboardScreenAlignedVS.vsh (rendu :
// pivot + x * droite + y * haut + z * avant). Le chargeur sautait ces
// donnees : le quad restait fige dans son orientation d'export, une image
// plate posee dans le decor. Un seul bloc malloc, possede par le maillage.
struct SVitaBillboard
{
	int   type;				// 0 face a l'ecran, 1 axe Y, 2 axe quelconque (sBillboardData)
	float pivot[3];
	float axe[3];			// type 2 seulement
	float rayon;			// distance maximale d'un sommet au pivot
	float rel[4][3];		// sommet i relativement au pivot, dans le repere (u, v, n)
	float orig[4][3];		// positions du fichier (remises par « bbd 0 »)
};

// Un maillage reduit a ce dont le palier 3 a besoin : des triangles.
// Une passe de materiau au-dela de la deuxieme (option B, etape 3).
struct SVitaPasse
{
	unsigned int    texture_checksum;	// 0 = passe absente ou non rendue
	unsigned int    blend;				// mode de COMBINAISON (render.cpp:420)
	unsigned int    flags;				// MATFLAG_*
	float           color[3];			// 0.5 = neutre
	unsigned char   fixed_alpha;		// 128 = 1.0
	unsigned char   addr_u, addr_v;
	float          *p_uvs;				// jeu d'UV de la passe (non possede)
	// Passe en ENVIRONMENT MAPPING (flags & MATFLAG_ENVIRONMENT) : p_uvs reste
	// NULL, texture_checksum est garde ; ses coordonnees se calculent depuis
	// la normale, a l'echelle de ce tuilage (XBox/NX/material.cpp:412-420).
	float           env_tile[2];
};

struct SVitaMesh
{
	float          *p_positions;	// 3 flottants par sommet
	float          *p_normals;		// 3 flottants par sommet, NULL si absent
	float          *p_uvs;			// 2 flottants par sommet, NULL si absent
	// Couleurs de sommets, RGBA 0..255 (converties depuis le BGRA 0..128 du
	// fichier). C'est l'eclairage CUIT du decor -- et l'alpha des voiles :
	// mesure sur le menu, le ï¿½ rectangle blanc ï¿½ etait un degrade dont les
	// alphas de sommets (0 en haut, 58/128 en bas) etaient ignores.
	unsigned char  *p_colors;		// 4 octets par sommet, NULL si absent
	unsigned int   *p_weights;		// poids empaquetes, NULL si non skinne
	unsigned short *p_bones;		// 4 indices d'os par sommet

	// Index d'os du SECTEUR, pour les modeles a parties RIGIDES (vehicules).
	// A ne pas confondre avec p_bones, qui est le skinning par sommet : les
	// deux mecanismes ne coexistent jamais sur un meme maillage.
	//
	// [VERIFIE sur table] les vehicules n'ont AUCUN drapeau POIDS (0x10) :
	// leurs six secteurs sont chacun attaches a un os. Le parseur jetait cet
	// index (Â« skip( p_file, 4 ); // index d'os Â»).
	// CHECKSUM DU SECTEUR : la cle par laquelle les scripts le designent.
	//
	// cfuncs.cpp cherche un secteur par ce checksum puis l'active ou le
	// desactive -- c'est ainsi que le jeu masque les barrieres et rampes de
	// mission tant qu'elles ne sont pas requises. Le chargeur sautait ce champ,
	// donc aucun secteur n'existait cote moteur, la recherche echouait et
	// l'ordre de desactivation tombait dans le vide : tout restait dessine.
	uint32          sector_checksum;
	// BOITE DU SECTEUR telle que le fichier la donne (min xyz, max xyz).
	// [SOURCE] XBox/p_nxsector.cpp:303 -- c'est m_bbox du geom, que rend
	// CSector::GetBoundingBox : l'editeur de parc y mesure ses pieces
	// (ParkGen.cpp:917) et en retire la coquille (SetActiveInBox), les
	// menus 3D s'y cadrent (Element3d.cpp:377). Issue #65.
	float           sector_bb[6];
	int             sector_bone;	// -1 si le modele n'est pas hierarchique
	unsigned short *p_indices;		// LOD 0 uniquement
	int             num_vertices;
	int             num_indices;
	// Les maillages d'un meme secteur PARTAGENT leurs sommets : un seul les
	// possede et les libere. Dupliquer coutait 18 transformations identiques
	// par frame sur le skater.
	int             owns_vertices;
	unsigned int    texture_checksum;	// 0 = non texture
	// Blend mode de la premiere passe du materiau (24 bits bas du ï¿½ registre
	// ALPHA ï¿½, cf. DX9/NX/render.cpp set_blend_mode). 0 = opaque. Mesure sur
	// le menu : trois materiaux BLEND rendus opaques donnaient le fameux
	// rectangle blanc -- un panneau de lueur prive de sa transparence.
	unsigned int    blend_mode;
	// Nombre de PASSES du materiau. On n'en dessine qu'une -- or 495 des 1424
	// materiaux de New Jersey en declarent deux a quatre (mesure sur table via
	// pre_scan.py). Retenu ici pour pouvoir MARQUER ces maillages a l'ecran et
	// verifier s'ils coincident avec les surfaces noires observees.
	unsigned int    num_passes;

	// DEUXIEME COUCHE de texture, quand le materiau en declare une.
	//
	// Ce ne sont PAS des passes de rendu : XBox compose toutes les couches en
	// un seul appel, dans un pixel shader genere a la volee
	// (XBox/NX/render.cpp:302). La traduction en pipeline fixe est le
	// multitexturing -- vitaGL expose 2 unites, mesure par sonde.
	//
	// Correspondance UV <-> couche, lue sur mesh.cpp:1210 : la couche N
	// consomme le jeu d'UV N, SAUF si elle est en environment mapping, auquel
	// cas ses coordonnees sont generees et elle n'a pas de jeu propre. C'est le
	// piege : appliquer le jeu 1 a une couche envmap donne n'importe quoi.
	// 47 des 495 materiaux multi-couches de New Jersey sont dans ce cas.
	// CE QUI PILOTE LE RENDU, et que le chargeur sautait.
	//
	// [SOURCE] XBox/NX/scene.cpp:234 -- ce qui classe un maillage comme
	// semi-transparent est le drapeau MATFLAG_TRANSPARENT (0x40) de la passe 0,
	// PAS son mode de melange. Nous testions « blend != 0 » : mesure sur New
	// Jersey, 35 materiaux opaques partaient en translucide (d'ou des surfaces
	// qui laissent voir le ciel) et 33 translucides en opaque.
	// Mode d'adressage, par couche et par axe : 0 = repetition, 1 = bloque au
	// bord, 2 = bordure. Le chargeur SAUTAIT ces champs, et le backend ne
	// reglait rien -- donc TOUTE texture du jeu etait en repetition, valeur par
	// defaut d'OpenGL. Symptome : le logo de la boite aux lettres repete en
	// damier, l'enseigne du magasin carrelee. [SOURCE] XBox/NX/render.cpp:1583.
	unsigned char   addr_u, addr_v;			// couche 0
	unsigned char   addr2_u, addr2_v;		// couche 1
	unsigned int    mat_flags0;
	// [SOURCE] XBox/NX/material.cpp:302 -- pose comme D3DRS_ALPHAREF. 1169 des
	// 1424 materiaux du niveau en portent un : c'est ce qui decoupe grillages
	// et feuillages. Jamais implemente chez nous.
	unsigned int    alpha_cutoff;
	// [SOURCE] XBox/NX/material.cpp:615 -- z-bias du materiau (0-16).
	unsigned char   zbias;
	// UV WIBBLE (issue #43) : bit p = la passe p fait defiler/onduler ses UV
	// ([SOURCE] XBox/NX/material.cpp:118, figure_wibble_uv). Parametres par
	// passe : UVel, VVel, UFreq, VFreq, UAmpl, VAmpl, UPhase, VPhase.
	unsigned char   uvw;
	float           uvw_par[4][8];
	// [SOURCE] XBox/NX/material.cpp:306 -- 1 = double face (pas de culling).
	unsigned char   no_bfc;
	// Bit 0x400 des drapeaux du maillage dans le .scn : ce maillage ne recoit
	// pas l'ombre du skater. [SOURCE] XBox/p_nxsector.cpp:172.
	unsigned char   no_ombre;
	// Bit 0x20000 : maillage NON eclaire (XBox sMesh::MESH_FLAG_UNLIT,
	// p_nxsector.cpp:176). Un seul suffit a eteindre l'instance (instance.cpp:268).
	unsigned char   unlit;
	// [SOURCE] XBox/NX/scene.cpp:265 -- la liste est triee par draw_order, et
	// les materiaux « sorted » sont en plus tries par profondeur a chaque image.
	unsigned int    mat_sorted;
	float           draw_order;

	float          *p_uvs2;				// 2 flottants par sommet, NULL si absent
	unsigned int    texture_checksum2;	// 0 = pas de deuxieme couche
	unsigned int    blend_mode2;		// son mode de COMBINAISON, pas de framebuffer
	unsigned int    mat_flags2;			// MATFLAG_* de la couche 1
	// Index du maillage DANS SON SECTEUR -- c'est le ï¿½ load order ï¿½ du moteur
	// d'origine (DX9/p_nxsector.cpp:222 : p_mesh->m_load_order = m). Il repart
	// a zero a chaque secteur, contrairement a notre numerotation globale des
	// morceaux : les tables CAS s'y referent, et les confondre revient a viser
	// le mauvais maillage.
	int             load_order;

	// COULEUR DU MATERIAU, passe 0, telle qu'elle est dans le fichier :
	// neutre = 0.5, pas 1.0. [SOURCE] XBox/NX/material.cpp:651, consommee
	// par PixelShader0.psh -- saturate( 4 * v0.rgb * t0.rgb * c0.rgb ).
	float           mat_color[3];
	// SPECULAIRE de la passe 0 : couleur (0-2) et puissance (3), 0 = sans.
	// [SOURCE] XBox/NX/material.cpp:629 (m_specular_color), :830
	// (MATFLAG_SPECULAR si puissance > 0).
	float           spec[4];
	// Passe 1, et alphas fixes des passes 0 et 1 (0..255, 128 = 1.0) : la
	// formule Xbox les applique passe par passe (option B, etape 2).
	float           mat_color2[3];
	unsigned char   fixed_alpha0, fixed_alpha2;
	// Passes 2 et 3, et les jeux d'UV 3 et 4 (possedes comme p_uvs2 : par le
	// premier maillage du secteur).
	SVitaPasse      passe_x[2];
	float          *p_uvs_x[2];
	// Identite du materiau : la cle des lots (deux materiaux de meme texture
	// mais de couleurs differentes ne doivent pas etre fusionnes).
	unsigned int    mat_checksum;
	unsigned int    mat_nom;		// checksum du NOM du materiau (SetMaterialColor, #58)

	// Couche 1 en ENVIRONMENT MAPPING (issue #5) : sa texture et son tuilage.
	// texture_checksum2 reste 0 pour elle (pas de jeu d'UV) ; les coordonnees
	// se calculent dans le shader depuis la normale, d'ou p_normals conserve
	// pour ces maillages.
	unsigned int    texture_env2;
	float           env_tiling2[2];
	// Passe 0 en reflet (issue #5) : sa texture est texture_checksum, sans jeu
	// d'UV ; la couche 1 consomme alors le jeu 0 (mesh.cpp:1210).
	unsigned char   env0;
	float           env_tiling0[2];
	// Jeu d'UV REELLEMENT consomme par la couche 1 -- p_uvs2, ou p_uvs quand
	// la passe 0 est en reflet. Non proprietaire.
	float          *p_uvs_couche1;
	// Vertex color wibble (issue #45 V2), NULL si le maillage n'en porte pas
	// (drapeau 0x800 du maillage ET du secteur, materiau MATFLAG_VC_WIBBLE).
	// Possede : AddSceneToWorld le reprend (et remet NULL), sinon
	// FreeSceneGeometry le libere.
	SVitaVcw       *p_vcw;
	// Billboard (issue #2), NULL sinon. Possede : AddSceneToWorld le reprend
	// (et remet NULL), sinon FreeSceneGeometry le libere.
	SVitaBillboard *p_bb;
};

struct SVitaSceneGeom
{
	SVitaMesh *p_meshes;
	int        num_meshes;
	int        capacity;

	// HIERARCHIE des objets, lue en FIN de fichier, apres tous les secteurs.
	//
	// [SOURCE] XBox/p_nx.cpp:583, Â« Read hierarchy information Â» : un entier
	// donnant le nombre d'objets, puis autant de CHierarchyObject bruts de
	// 80 octets. C'est LA donnee qui place les parties rigides des vehicules
	// â€” l'objet racine porte la rotation du modele, les suivants la position
	// de chaque roue.
	//
	// Sans elle, CalculateCarHierarchyMatrices sortait sans rien poser et les
	// six parties du vehicule se dessinaient empilees a l'origine du modele.
	void      *p_hierarchy;		// tableau brut de CHierarchyObject
	int        num_hierarchy;
};

// Modeles (.mdl, CVitaMesh::Build) : garder les normales de tous les
// secteurs, pour l'eclairage des modeles rigides. Pas pour les niveaux.
extern bool g_vita_garder_normales;
bool LoadSceneGeometry( const char *p_name, SVitaSceneGeom *p_geom );

// Meme format, depuis un tampon en RAM : le chemin Create-A-Skater, dont les
// pieces vivent dans skaterparts.pre deja charge. p_label ne sert qu'aux
// traces.
bool LoadSceneGeometryFromMemory( const void *p_data, int size,
                                  const char *p_label, SVitaSceneGeom *p_geom );
void FreeSceneGeometry( SVitaSceneGeom *p_geom );

} // namespace NxVita

#endif // __GFX_VITA_P_SCENE_LOAD_H__
