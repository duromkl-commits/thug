#ifdef __PLAT_VITA__
#include "vita_log.h"
#endif
#include <core/defines.h>
#include <gel/scripting/checksum.h>
#include <gel/scripting/struct.h>
#include <gel/scripting/array.h>
#include <gfx/2D/ScreenElemMan.h>
#include <gfx/2D/SpriteElement.h>
#include <gfx/2D/Window.h>
#include <gfx/Nx.h>
#include <gfx/nxtexture.h>
#include <gfx/NxTexMan.h>
#include <gfx/NxSprite.h>

namespace Front
{




CSpriteElement::CSpriteElement()
{
	//m_rgba = 0x40909090;
	m_texture = 0;
	mp_sprite = Nx::CEngine::sCreateSprite(NULL);
	
	SetType(CScreenElement::TYPE_SPRITE_ELEMENT);
}




CSpriteElement::~CSpriteElement()
{
	if (mp_sprite)
	{
		Nx::CEngine::sDestroySprite(mp_sprite);
	}
	//Ryan("Destroying CSpriteElement\n");
}




void CSpriteElement::SetProperties(Script::CStruct *pProps)
{
	CScreenElement::SetProperties(pProps);

	uint32 texture_crc;
	if (pProps->GetChecksum("texture", &texture_crc))
		SetTexture(texture_crc);
	float rot_angle;
	if ( pProps->GetFloat( "rot_angle", &rot_angle ) )
		SetRotate( rot_angle );
	// Dbg_MsgAssert(m_texture, ("no texture loaded"));
}

void CSpriteElement::SetMorph( Script::CStruct *pProps )
{
	CScreenElement::SetMorph( pProps );

	float rot_angle;
	if ( pProps->GetFloat( "rot_angle", &rot_angle ) )
		SetRotate( rot_angle, DONT_FORCE_INSTANT );
}


void CSpriteElement::SetTexture(uint32 texture_checksum)
{
	Nx::CTexture *p_texture;

	m_texture = texture_checksum;

	p_texture = Nx::CTexDictManager::sp_sprite_tex_dict->GetTexture(texture_checksum);
	Dbg_MsgAssert(p_texture, ("no texture found for sprite 0x%x %s on element 0x%x %s",
					texture_checksum,Script::FindChecksumName(texture_checksum),
					m_id,Script::FindChecksumName(m_id)));
#ifdef __PLAT_VITA__
	// Au palier 3 les dictionnaires de textures sont vides : GetTexture rend
	// NULL, et l'assert cense l'attraper est compile a vide chez nous. Le
	// GetWidth() plus bas dereferencait donc NULL -- plantage net (verifie sur
	// psp2core : Data abort dans CTexture::GetWidth, this=0).
	//
	// Garde LOCALE et volontairement etroite : mettre un placeholder dans
	// CTexDict::GetTexture ferait croire a TOUT le moteur que la texture
	// existe. A retirer au palier 4, quand les textures seront reelles.
	if( !p_texture )
	{
		static int s_warned = 0;
		if( s_warned++ < 10 )
			VLOG( "2D", "texture absente 0x%08x pour un sprite -- ignoree",
			      (unsigned)texture_checksum );
		mp_sprite->SetTexture( NULL );
		m_object_flags |= CScreenElement::vCHANGED_STATIC_PROPS;
		return;
	}
#endif
	mp_sprite->SetTexture(p_texture);

	Dbg_MsgAssert(!(m_object_flags & vFORCED_DIMS), ("Trying to override sprite texture size"));

#ifdef __PLAT_VITA__
	// Un sprite prend la taille de sa texture, point. Si le fond du menu est
	// mal cadre, c'est soit cette taille qui ment (dimensions lues de
	// travers), soit la position/echelle appliquee ensuite. On mesure la
	// taille ici, a la source, et on la comparera aux vraies dimensions des
	// .tex lues hors console.
	{
		static int s_n = 0;
		if( s_n++ < 60 )
			VLOG( "2D", "sprite tex 0x%08x -> dims %d x %d",
			      (unsigned)texture_checksum,
			      (int)p_texture->GetWidth(), (int)p_texture->GetHeight() );
	}
#endif

	SetDims(p_texture->GetWidth(), p_texture->GetHeight());

	m_object_flags |= CScreenElement::vCHANGED_STATIC_PROPS;	
}



void CSpriteElement::SetRotate( float angle, EForceInstant forceInstant )
{
	float rot = angle * Mth::PI / 180.0f;
	rot += 0.001f;
	Dbg_Assert( mp_sprite );

	if ( m_target_local_props.GetRotate() != rot )
	{
		m_target_local_props.SetRotate( rot );
		m_object_flags |= vNEEDS_LOCAL_POS_CALC;
	}
	if ( forceInstant )
	{
		if ( m_local_props.GetRotate() != rot )
		{
			// on the next update, this element will arrive at the target alpha
			mp_sprite->SetRotation( rot );
			m_local_props.SetRotate( rot );
			m_object_flags |= vNEEDS_LOCAL_POS_CALC;
		}
	}
}


void CSpriteElement::update()
{
	// HACK
	bool offscreen = false;
	if (m_summed_props.GetScreenUpperLeftY() < -200.0f || m_summed_props.GetScreenUpperLeftY() > 648.0f)
		offscreen = true;
	
	// IsHidden can be relatively slow (it's recursive), so only call once
	bool hidden = IsHidden();

	mp_sprite->SetHidden( hidden );

#ifdef __PLAT_VITA__
	// Un rideau noir opaque couvre le niveau et ne se leve jamais. Le bloc
	// ci-dessous, qui seul reapplique la couleur (donc l'alpha du fondu), est
	// conditionne a deux drapeaux. On journalise donc, pour les seuls elements
	// assez grands pour etre ce rideau : l'identifiant, les deux alphas qui se
	// multiplient, et SI le bloc s'execute. Cela separe « le fondu ne s'anime
	// pas » de « il s'anime mais n'est jamais reapplique au sprite ».
	{
		const float ew = (float)m_base_w * m_summed_props.GetScaleX();
		const float eh = (float)m_base_h * m_summed_props.GetScaleY();
		if(( ew >= 600.0f ) && ( eh >= 400.0f ))
		{
			static int s_c = 0;
			if(( ++s_c % 120 ) == 1 )
			{
				// Chaine des parents : c'est elle qui dit A QUOI ce rideau
				// appartient. Supposer qu'il est le visuel de l'ecran de
				// chargement parce que HideLoadingScreen tourne en boucle
				// serait un raccourci -- le conteneur, lui, se mesure.
				uint32 p1 = mp_parent ? mp_parent->GetID() : 0;
				VLOG( "2D", "rideau : parent 0x%08x, texture 0x%08x",
				      (unsigned)p1, (unsigned)m_texture );
			}
			if( s_c % 120 == 1 )
				VLOG( "2D", "grand element 0x%08x : %.0fx%.0f  alpha_summed=%.3f "
				            "alpha_local=%d  cache=%d  bloc_maj=%d",
				      (unsigned)m_id, ew, eh, m_summed_props.alpha,
				      (int)m_local_props.GetRGBA().a, (int)hidden,
				      (int)(( m_object_flags & CScreenElement::vDID_SUMMED_POS_CALC )
				         || ( m_object_flags & CScreenElement::vCHANGED_STATIC_PROPS )));
		}
	}
#endif

	// only change state of underlying sprite if there's been a change to CScreenElement/CSpriteElement visual state
	if ( ( m_object_flags & CScreenElement::vDID_SUMMED_POS_CALC ) || ( m_object_flags & CScreenElement::vCHANGED_STATIC_PROPS ) )
	{
		Image::RGBA true_rgba = m_local_props.GetRGBA();
		if (m_summed_props.alpha >= .0001f)
			true_rgba.a = (uint8) ((float) m_local_props.GetRGBA().a * m_summed_props.alpha);
		else
			true_rgba.a = 0;
		
		// GARRETT: 
		// Do all drawing here
		//
		// -screen coordinates of upper-left of sprite: m_summed_props.ulx, m_summed_props.uly
		// -scale to apply: m_summed_props.scale
		// -screen W,H of sprite (if needed): m_summed_props.scale * m_base_w, m_summed_props.scale * m_base_h
		//mp_sprite->SetPos(m_summed_props.ulx, m_summed_props.uly);
		mp_sprite->SetPos(m_summed_props.GetScreenPosX(), m_summed_props.GetScreenPosY());
		mp_sprite->SetSize(m_base_w, m_base_h);
		mp_sprite->SetAnchor(m_just_x, m_just_y);
		mp_sprite->SetScale(m_summed_props.GetScaleX(), m_summed_props.GetScaleY());
		
		if ( m_object_flags & CScreenElement::v3D_POS )
		{
			mp_sprite->SetZValue(m_summed_props.GetScreenPosZ());
			mp_sprite->SetHidden( ( ( m_object_flags & CScreenElement::v3D_CULLED ) || hidden ) );
		}
		else
		{
			mp_sprite->SetPriority( m_z_priority );
			mp_sprite->SetHidden( offscreen || hidden );
		}
		
		mp_sprite->SetRGBA(true_rgba);
		// mp_sprite->SetHidden( ( offscreen || hidden ) );
	

#if 0  // Garrett: Can't find window
		// Update the clip window.  This should only be done at init time, instead.
		CWindowElement *p_window = get_window();
		mp_sprite->SetWindow(p_window->GetClipWindow());
#endif

		// update the rotation angle
		mp_sprite->SetRotation( m_local_props.GetRotate() );
		// mp_sprite->SetRotation(mp_sprite->GetRotation() + 0.001f);

	}
}

void CSpriteElement::WritePropertiesToStruct( Script::CStruct* pStruct )
{
	CScreenElement::WritePropertiesToStruct( pStruct );

	// rotation angle
	pStruct->AddFloat( "rot_angle", m_local_props.GetRotate() );

	// texture name
	pStruct->AddChecksum( "texture", this->m_texture );
}

}
