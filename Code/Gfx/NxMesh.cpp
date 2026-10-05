//****************************************************************************
//* MODULE:         Gfx
//* FILENAME:       NxMesh.cpp
//* OWNER:          Gary Jesdanun
//* CREATION DATE:  2/15/2002
//****************************************************************************

#ifdef __PLAT_VITA__
#include "vita_log.h"
#endif
#include <gfx/nx.h>
#include <gfx/nxmesh.h>
#include <gfx/nxhierarchy.h>

#include <gel/collision/collision.h>
#include <gel/collision/colltridata.h>

#include <sys/file/pip.h>

#ifdef __PLAT_NGC__
#include <gfx/ngc/p_nxmesh.h>
#endif		// __PLAT_NGC__

namespace Nx
{

/*****************************************************************************
**							   Private Functions							**
*****************************************************************************/

///////////////////////////////////////////////////////////////////////////////
// Stub versions of all platform specific functions are provided here:
// so engine implementors can leave certain functionality until later
						
/*****************************************************************************
**								Public Functions							**
*****************************************************************************/

// These functions are the platform independent part of the interface to 
// the platform specific code
// parameter checking can go here....
// although we might just want to have these functions inline, or not have them at all?


/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

CMesh::CMesh()
{
	m_CASRemovalMask = 0;

	// In case it isn't loaded below the p-line
	mp_hierarchyObjects = NULL;
	m_numHierarchyObjects = 0;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

CMesh::~CMesh()
{
	// Remove Collision
	if (mp_coll_objects)
	{
		Pip::Unload(m_coll_filename);
	}
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

bool			CMesh::LoadCollision(const char *p_name)
{
	
	// for now collision is kind of assumed to be platform independent 
	strcpy(m_coll_filename, p_name);
	char *p_ext = strstr(m_coll_filename, ".");
	if (p_ext)
	{
		strcpy(p_ext, ".col.");
	} else {
		strcat(m_coll_filename, ".col.");
	}
	strcat(m_coll_filename, CEngine::sGetPlatformExtension());

//	Dbg_Message ( "Loading collision %s....", m_coll_filename );

	Mem::PushMemProfile((char*)m_coll_filename);

	uint8 *p_base_addr = (uint8 *) Pip::Load(m_coll_filename);
	if (p_base_addr)
	{
		Nx::CCollObjTriData::SReadHeader *p_header = (Nx::CCollObjTriData::SReadHeader *) p_base_addr;
		p_base_addr += sizeof(Nx::CCollObjTriData::SReadHeader);

//		Dbg_Message ( "Version # %d header sizeof %d", p_header->m_version, sizeof(Nx::CCollObjTriData));
#ifdef __PLAT_NGC__
		Dbg_Message ( "Number of objects: %d verts: %d faces: %d", p_header->m_num_objects, p_header->m_total_num_verts, p_header->m_total_num_faces );
#else
//		Dbg_Message ( "Number of objects: %d verts: %d faces: %d", p_header->m_num_objects, p_header->m_total_num_verts, p_header->m_total_num_faces_large + p_header->m_total_num_faces_small);
//		Dbg_Message ( "Small (%d) verts: %d Large (%d) verts: %d", Nx::CCollObjTriData::GetVertSmallElemSize(), p_header->m_total_num_verts_small, Nx::CCollObjTriData::GetVertElemSize(), p_header->m_total_num_verts_large);
#endif		// __PLAT_NGC__
		Dbg_MsgAssert(p_header->m_version >= 9, ("Collision version must be at least 9."));

		// reserve space for objects
		m_num_coll_objects = p_header->m_num_objects;
		mp_coll_objects = (Nx::CCollObjTriData *) p_base_addr;

		// Calculate base addresses for vert and face arrays
		uint8 *p_base_vert_addr = (uint8 *) (mp_coll_objects + m_num_coll_objects);
#ifndef __PLAT_NGC__
#ifdef __PLAT_VITA__
		// [VERIFIE] Alignements RELATIFS au debut du fichier, comme le format
		// les suppose. L'original aligne l'adresse absolue, ce qui revient au
		// meme seulement si le tampon est lui-meme aligne sur 16 -- vrai dans
		// un .pre, faux pour un fichier lu par File::LoadAlloc (malloc, 8
		// octets) : veh_pickup.col.xbx arrive en 0x...d8, tout est lu 8
		// octets trop loin, le tableau de noeuds BSP fait « 8 » au lieu de
		// 120 et s_init_tree plante. Intermittent, puisque l'adresse varie
		// (chargements de Tampa et de San Diego).
		p_base_vert_addr = (uint8 *)p_header + ((( p_base_vert_addr - (uint8 *)p_header ) + 15 ) & ~15 );
#else
		p_base_vert_addr = (uint8 *)(((uint)(p_base_vert_addr+15)) & 0xFFFFFFF0);	// Align to 128 bit boundary
#endif
#ifdef FIXED_POINT_VERTICES
		uint8 *p_base_intensity_addr = p_base_vert_addr + (p_header->m_total_num_verts_large * Nx::CCollObjTriData::GetVertElemSize() +
														   p_header->m_total_num_verts_small * Nx::CCollObjTriData::GetVertSmallElemSize());
		uint8 *p_base_face_addr = p_base_intensity_addr + p_header->m_total_num_verts;
#ifdef __PLAT_VITA__
		p_base_face_addr = (uint8 *)p_header + ((( p_base_face_addr - (uint8 *)p_header ) + 3 ) & ~3 );
#else
		p_base_face_addr = (uint8 *)(((uint)(p_base_face_addr+3)) & 0xFFFFFFFC);	// Align to 32 bit boundary
#endif
#else
		uint8 *p_base_intensity_addr = NULL;
		uint8 *p_base_face_addr = p_base_vert_addr + (p_header->m_total_num_verts * Nx::CCollObjTriData::GetVertElemSize());
		p_base_face_addr = (uint8 *)(((uint)(p_base_face_addr+15)) & 0xFFFFFFF0);	// Align to 128 bit boundary
#endif // FIXED_POINT_VERTICES
#else
		uint8 *p_base_face_addr = p_base_vert_addr + (p_header->m_total_num_faces * Nx::CCollObjTriData::GetVertElemSize());
		p_base_face_addr = (uint8 *)(((uint)(p_base_face_addr+3)) & 0xFFFFFFFC);	// Align to 32 bit boundary
#endif		// __PLAT_NGC__

		// Calculate addresses for BSP arrays
#ifndef __PLAT_NGC__
		uint8 *p_node_array_size = p_base_face_addr + (p_header->m_total_num_faces_large * Nx::CCollObjTriData::GetFaceElemSize() +
													   p_header->m_total_num_faces_small * Nx::CCollObjTriData::GetFaceSmallElemSize());
		p_node_array_size += ( p_header->m_total_num_faces_large & 1 ) ? 2 : 0;
#else
		uint8 *p_node_array_size = p_base_face_addr + ( p_header->m_total_num_faces * Nx::CCollObjTriData::GetFaceElemSize() );
		p_node_array_size += ( p_header->m_total_num_faces & 1 ) ? 2 : 0;
#endif		// __PLAT_NGC__
		uint32 node_array_size = *((uint32*)p_node_array_size);
		uint8 *p_base_node_addr = p_node_array_size + 4;
		uint8 *p_base_face_idx_addr = p_base_node_addr + node_array_size;
#ifdef __PLAT_VITA__
		// Fichier et tailles lus, avant la relocalisation : c'est cette trace
		// qui a montre « noeuds 8 o » au lieu de 120 (voir l'alignement plus haut).
		{
			const int taille = (int)Pip::GetFileSize( m_coll_filename );
			VLOG( "COL", "mesh '%s' en %p (%d o) : version %d, %d objets, noeuds %u o, pad3 0x%08x, bsp0 brut 0x%08x",
			      m_coll_filename, (void *)p_header, taille, p_header->m_version, p_header->m_num_objects,
			      (unsigned)node_array_size, (unsigned)p_header->m_pad3,
			      p_header->m_num_objects ? *(unsigned int *)&mp_coll_objects[0] : 0u );
		}
#endif

#ifdef __PLAT_VITA__
		// [VERIFIE] Issue #26 : la relocalisation se fait EN PLACE, dans le
		// tampon rendu par Pip::Load. Si ce tampon est encore en memoire --
		// fichier contenu dans un .pre toujours charge, comme skaterparts.pre
		// entre l'editeur de skater et le retour au niveau --, un second
		// chargement retraitait comme des decalages des pointeurs deja
		// absolus, et s_init_tree partait dans le decor. On marque donc le
		// tampon relocalise avec sa propre adresse (m_pad3, inutilise).
		const bool deja_reloge = ( p_header->m_pad3 == (int)p_header );
		if( deja_reloge )
			VLOG( "COL", "'%s' deja relocalise en %p : relocalisation sautee", m_coll_filename, (void *)p_header );
#endif
		// Read objects
		for (int oidx = 0; oidx < p_header->m_num_objects; oidx++)
		{
			if (!node_array_size)
			{
				m_num_coll_objects = 0;
				break;
			}
#ifdef __PLAT_VITA__
			if( !deja_reloge )
#endif
#ifdef __PLAT_NGC__
			CScene * p_scene = static_cast<CScene*>( (static_cast<CNgcMesh*>( this ))->GetScene() );
			mp_coll_objects[oidx].InitCollObjTriData(p_scene, p_base_vert_addr, NULL, p_base_face_addr, p_base_node_addr, p_base_face_idx_addr);
#else
			mp_coll_objects[oidx].InitCollObjTriData(NULL, p_base_vert_addr, p_base_intensity_addr, p_base_face_addr,
													 p_base_node_addr, p_base_face_idx_addr);
#endif		// __PLAT_NGC__
			mp_coll_objects[oidx].InitBSPTree();

			// Add to mesh bbox
			m_collision_bbox.AddPoint(mp_coll_objects[oidx].GetBBox().GetMin());
			m_collision_bbox.AddPoint(mp_coll_objects[oidx].GetBBox().GetMax());
		}
#ifdef __PLAT_VITA__
		p_header->m_pad3 = (int)p_header;
#endif

//		Dbg_Message("Mesh bounding box: min (%f, %f, %f) max (%f, %f, %f)", 
//					m_collision_bbox.GetMin()[X], m_collision_bbox.GetMin()[Y], m_collision_bbox.GetMin()[Z], 
//					m_collision_bbox.GetMax()[X], m_collision_bbox.GetMax()[Y], m_collision_bbox.GetMax()[Z]);

	} else {
		Dbg_Error ( "Could not open collision file\n" );
		return false;
	}

//	Dbg_Message ( "successfully loaded collision" );

	if (m_num_coll_objects > 0)
	{
#if 0
		// Add to CSectors
		for (int i = 0; i < m_num_coll_objects; i++)
		{
			CSector *p_sector = GetSector(mp_coll_objects[i].GetChecksum());
			if (p_sector)	// Don't assert now since there may not be renderable data
			{
				Dbg_MsgAssert(p_sector, ("LoadCollision: Can't find CSector with checksum %x", mp_coll_objects[i].GetChecksum()));
				p_sector->AddCollSector(&(mp_coll_objects[i]));
			}
		}
#endif
	}

	Mem::PopMemProfile(/*(char*)m_coll_filename*/);

	return true;
}



/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

Nx::CHierarchyObject* CMesh::GetHierarchy()
{
	return mp_hierarchyObjects;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

int CMesh::GetNumObjectsInHierarchy()
{
	return m_numHierarchyObjects;
}


} // Nx

