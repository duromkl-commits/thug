//****************************************************************************
//* MODULE:         Gel/Components
//* FILENAME:       CarPhysicsComponent.cpp
//* OWNER:          Gary Jesdanun
//* CREATION DATE:  11/4/2002
//****************************************************************************

#ifdef __PLAT_VITA__
#include "vita_log.h"
#endif
#include <gel/components/carphysicscomponent.h>

#include <gel/object/compositeobject.h>

#include <gel/components/skeletoncomponent.h>
#include <gel/components/modelcomponent.h>

#include <gel/scripting/checksum.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/symboltable.h>
	 
#include <gfx/nxhierarchy.h>
#include <gfx/nxmodel.h>
#include <gfx/skeleton.h>

namespace Obj
{

//	#define vMAX_WHEEL_Y_ROT ( 70.0f )
//	#define vMAX_CAR_X_ROT ( 5.0f )
//	#define vMIN_CAR_X_ROT ( -5.0f )
//	#define vSTEP_CAR_X_ROT ( 0.125f )
//	#define vACCEL_TO_CAR_ROT_FACTOR ( 3.0f )

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
// This static function is what is registered with the component factory 
// object, (currently the CCompositeObjectManager) 
CBaseComponent* CCarPhysicsComponent::s_create()
{
	return static_cast< CBaseComponent* >( new CCarPhysicsComponent );	
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
CCarPhysicsComponent::CCarPhysicsComponent() : CBaseComponent()
{
	SetType( CRC_CARPHYSICS );
	
	m_wheelRotationX = 0.0f;
	m_wheelRotationY = 0.0f;
	m_targetWheelRotationY = 0.0f;

	m_shadowEnabled = true;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
CCarPhysicsComponent::~CCarPhysicsComponent()
{
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
void CCarPhysicsComponent::InitFromStructure( Script::CStruct* pParams )
{
	// set up some car physics parameters
	m_minCarRot = Script::GetFloat( CRCD(0x877db8da,"accelCarRot"), Script::ASSERT );
	pParams->GetFloat( CRCD(0x877db8da,"accelCarRot"), &m_minCarRot );
	
	m_maxCarRot = Script::GetFloat( CRCD(0xfb3464df,"decelCarRot"), Script::ASSERT );
	pParams->GetFloat( CRCD(0xfb3464df,"decelCarRot"), &m_maxCarRot );
	
	m_stepCarRot = Script::GetFloat( CRCD(0xfd8bc8d6,"speedCarRot"), Script::ASSERT );
	pParams->GetFloat( CRCD(0xfd8bc8d6,"speedCarRot"), &m_stepCarRot );
	
	m_accelToCarRotFactor = Script::GetFloat( CRCD(0xb2c7447b,"accelCarRotFactor"), Script::ASSERT );
	pParams->GetFloat( CRCD(0xb2c7447b,"accelCarRotFactor"), &m_accelToCarRotFactor );
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
void CCarPhysicsComponent::Update()
{
	// Doing nothing, so tell code to do nothing next time around
	Suspend(true);
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
CBaseComponent::EMemberFunctionResult CCarPhysicsComponent::CallMemberFunction( uint32 Checksum, Script::CStruct* pParams, Script::CScript* pScript )
{
	switch ( Checksum )
	{
		case CRCC(0xa2331896,"EnableCarShadow"):
			{
				int enabled = 0;
				pParams->GetInteger( NONAME, &enabled, Script::NO_ASSERT );
				m_shadowEnabled = enabled;
			}
			break;

		default:
			return CBaseComponent::MF_NOT_EXECUTED;
			break;

	}

    return CBaseComponent::MF_TRUE;
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

void CCarPhysicsComponent::init_car_skeleton()
{	
	// Does some sanity checks
	Obj::CSkeletonComponent* pSkeletonComponent = (Obj::CSkeletonComponent*)(GetObject()->GetComponent( CRC_SKELETON ));
	Gfx::CSkeleton* pSkeleton = NULL;
	if ( pSkeletonComponent )
	{
		pSkeleton = pSkeletonComponent->GetSkeleton();
	}
	
	Obj::CModelComponent* pModelComponent = (Obj::CModelComponent*)(GetObject()->GetComponent( CRC_MODEL ));
	Nx::CModel* pModel = NULL;
	if ( pModelComponent )
	{
		pModel = pModelComponent->GetModel();
	}
	
	Dbg_MsgAssert( pSkeleton, ("Car has no skeleton") );
	Dbg_MsgAssert( pModel, ( "Model hasn't been set up yet" ) );
	
	Dbg_MsgAssert( pModel->GetHierarchy(), ("veh flagged as car has no hierarchy from node %s", Script::FindChecksumName(GetObject()->GetID())));
	Nx::CHierarchyObject* pHierarchyObjects = pModel->GetHierarchy();
#ifdef __PLAT_VITA__
	// [PERIME depuis le 2026-08-15] la hierarchie EST desormais lue (voir
	// p_scene_load.cpp, section hierarchie en fin de fichier). Ce garde-fou ne
	// se declenche donc plus en temps normal ; on le garde comme filet, car un
	// modele sans hierarchie reste possible (veh_trolley n'en a pas).
	//
	// Ce qu'il evitait, et qui reste vrai : les trois asserts ci-dessus sont
	// compiles a vide chez nous, donc la ligne suivante lisait
	// (NULL + 2)->GetSetupMatrix(), soit 2 * 80 + 16 = 176 = 0xb0 : exactement
	// le R0 releve dans le psp2core, et le Data abort tombe dans
	// Mth::Vector::operator=.
	//
	// Sans hierarchie il n'y a pas de roues a mesurer. On sort avec une
	// distance par defaut plutot que de lire de la memoire etrangere : la
	// voiture aura une physique fausse, le niveau se chargera.
	if( !pSkeleton || !pModel || !pHierarchyObjects )
	{
		static int s_warned = 0;
		if( s_warned++ < 5 )
			VLOG( "VEH", "voiture sans hierarchie (squelette=%p modele=%p "
			             "hierarchie=%p) -- physique ignoree",
			      (void *)pSkeleton, (void *)pModel, (void *)pHierarchyObjects );
		m_distanceBetweenTires = 100.0f;
		return;
	}
#endif
	Dbg_MsgAssert(( pHierarchyObjects + 0 ),("Missing matrix in car"));
	Dbg_MsgAssert(( pHierarchyObjects + 2 ),("Missing matrix in car"));
	Dbg_MsgAssert(( pHierarchyObjects + 4 ),("Missing matrix in car"));

	// TODO:  This 5-bone hierarchy shouldn't be so hardcoded...
	// We should somehow make this generic so that we 
	// can use different skeletons with the car physics.

	Mth::Matrix frontTire = ( pHierarchyObjects + 2 )->GetSetupMatrix();
	frontTire *= ( pHierarchyObjects + 0 )->GetSetupMatrix();
	Mth::Matrix backTire = ( pHierarchyObjects + 4 )->GetSetupMatrix();
	backTire *= ( pHierarchyObjects + 0 )->GetSetupMatrix();

	Mth::Vector dist = frontTire[Mth::POS] - backTire[Mth::POS];
	m_distanceBetweenTires = fabs( dist.Length() );
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/
	
}
