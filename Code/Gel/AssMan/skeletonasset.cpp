//****************************************************************************
//* MODULE:         Ass
//* FILENAME:       skeletonasset.cpp
//* OWNER:          Gary Jesdanun
//* CREATION DATE:  ??/??/????
//****************************************************************************

#ifdef __PLAT_VITA__
#include "vita_log.h"
#endif

#include <gel/assman/skeletonasset.h>

#include <gel/assman/assettypes.h>

#include <gfx/nx.h>
#include <gfx/skeleton.h>

namespace Ass
{

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
int CSkeletonAsset::Load( const char *p_file, bool async_load, bool use_pip, void* pExtraData , Script::CStruct *pStruct)     // create or load the asset
{																			   
	Dbg_MsgAssert( !async_load, ( "Async load not supported on CSkeletonAsset" ) );

	// Load the data, add it to the list:
	Gfx::CSkeletonData* pSkeletonData = new Gfx::CSkeletonData;
	
	char fullName[256];
	
	// add extension to create name of platform-specific SKE file
	sprintf( fullName, "%s.%s", p_file, Nx::CEngine::sGetPlatformExtension() );
	
#ifdef __PLAT_VITA__
	// Compteur : on veut savoir si le moteur enchaine les squelettes ou reste
	// bloque sur le premier. Il y en a beaucoup, donc on ne loggue que le
	// numero et le nom, pas le detail.
	{
		static int s_n = 0;
		VLOG( "ASSET", "skeleton #%d: '%s'", ++s_n, fullName );
	}
#endif
	if ( !pSkeletonData->Load( fullName, true ) )
	{
#ifdef __PLAT_VITA__
		VLOG( "ASSET", "skeleton: ECHEC de Load('%s')", fullName );
#endif
		Dbg_MsgAssert( 0,( "File %s doesn't exist.", fullName ));
		return -1;
	}
	SetData( (void*)pSkeletonData );

	return 0;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
int CSkeletonAsset::Load( uint32* p_data, int data_size )
{
	char pDebugAssetString[256];
	sprintf( pDebugAssetString, "skeleton from data stream" );
	
	Mem::PushMemProfile((char*)pDebugAssetString);

	// Load the data, add it to the list:
	Gfx::CSkeletonData* pSkeletonData = new Gfx::CSkeletonData;
	
	if ( !pSkeletonData->Load( p_data, data_size, true ) )
	{
		Dbg_MsgAssert( 0,( "Couldn't create skeleton from data stream." ));
		return -1;
	}

	SetData((void*)pSkeletonData);

	Mem::PopMemProfile(/*"skeleton from data stream"*/);

	return 0;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
int CSkeletonAsset::Unload()                     // Unload the asset
{
	Gfx::CSkeletonData* pData = (Gfx::CSkeletonData*)GetData();
	if ( pData )
	{
        delete pData;
        SetData(NULL);
	}
	return 0;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
int CSkeletonAsset::Reload( const char *p_file )
{
	return 0;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
bool CSkeletonAsset::LoadFinished()
{
	Dbg_MsgAssert( GetData(), ( "LoadFinished(): Data pointer NULL (load probably was never started)" ) );

	// Since we don't support async, this is always true
	return true;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
const char *  CSkeletonAsset::Name()            // printable name, for debugging
{
	return "Skeleton";	
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
EAssetType CSkeletonAsset::GetType()         // type is hard wired into asset class 
{
	return ASSET_SKELETON; 					// for now return 0, not sure if this should return the EAssetType
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
}
