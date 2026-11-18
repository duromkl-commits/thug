//****************************************************************************
//* MODULE:         Gel/Components
//* FILENAME:       CameraUtil.cpp
//* OWNER:          Dan
//* CREATION DATE:  4/10/3
//****************************************************************************

/*
 * Utility functions for camera behavior components.
 */

#include <gel/components/camerautil.h>

#include <sk/engine/feeler.h>

#ifdef THUG_DESKTOP
#include <gfx/camera.h>
#include <sys/timer.h>
#endif

namespace Obj
{

/*******************************************************************************************************
logic for getTimeAdjustedSlerp()

If the camera is lagging a distance D behind the target, and the target is moving with velocity V, and the lerp is set to L.
Then when the camera is stable, it will be moving at the same rate as the target (V), and since the distance it will move is
equal to the lerp multiplied by the distance from the target (which will be the old distance plus the new movement of the
target V)

Then:

L*(D+V) = V 
L*D + L*V = V
D = V * (1 - L) / L

Assuming in the above T = 1, then if we have a variable T, the speed of the skater will be T*V, yet we want the distance (Dt)
moved to remain unchanged (D), so for a time adjusted lerp Lt

Dt = T * V * (1-Lt)/Lt

Since D = Dt

V * (1 - L) / L = T * V * (1-Lt)/Lt

V cancels out, and we get

Lt = TL / (1 - L + TL)

Sanity check,  

if L is 0.25, and T is 1, then Lt = 1*0.25 / (1 - 0.25 + 1*0.25)  = 0.25
if L is 0.25, and T is 2, then Lt = 2*0.25 / (1 - 0.25 + 2*0.25)  = 0.5 / 1.25 = 0.40
if L is 0.25, and T is 5, then Lt = 5*0.25 / (1 - 0.25 + 5*0.25)  = 1.25 / 2 = 0.625

Sounds about right.
*/

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

float GetTimeAdjustedSlerp( float slerp, float delta )
{
	// Mick's method, tries to guarantee that the distance from the target point
	// will never alter at constant velocity, with changing frame rate
	// Lt = TL / (1 - L + TL)
	float t		= delta * 60.0f;
	float Lt	= t * slerp / ( 1.0f - slerp + t * slerp );
	return Lt;	
}

/******************************************************************/
/*                                                                */
/*                                                                */
/******************************************************************/

#define vMIN_WALL_DISTANCE (2.0f)

void ApplyCameraCollisionDetection ( Mth::Vector& camera_pos, Mth::Matrix& camera_matrix, const Mth::Vector& target_pos, const Mth::Vector& focus_pos, bool side_feelers, bool refocus )
{
	// camera collision detection
	
	Mth::Vector target_to_camera_direction = camera_pos - target_pos;
	target_to_camera_direction.Normalize();
	
	CFeeler feeler;
	
	// Ignore faces based on face falgs
	feeler.SetIgnore(IGNORE_FACE_FLAGS_1, IGNORE_FACE_FLAGS_0);
	feeler.SetLine(target_pos, camera_pos + vMIN_WALL_DISTANCE * target_to_camera_direction);
	bool collision = feeler.GetCollision(true);
	if( collision )
	{
//			Gfx::AddDebugLine( at_pos, cam_pos, MAKE_RGB( 128, 0, 0 ), MAKE_RGB( 128, 0, 0 ), 2000 );

		// Limits the camera to getting nearer than 11.9 inches to the focus point.
		float distance = feeler.GetDist();
		float length = feeler.Length();
		if( length * distance < 11.9f + vMIN_WALL_DISTANCE )
		{
			distance = (11.9f + vMIN_WALL_DISTANCE) / length;
		}
		camera_pos = target_pos + (distance * length - vMIN_WALL_DISTANCE) * target_to_camera_direction;
	}
	
	if (!side_feelers) return;

//#	if defined( __PLAT_NGPS__ ) || defined ( __PLAT_XBOX__ )
	// Now do two additional checks 1 foot to either side of the camera.
	Mth::Vector	left( camera_matrix[X][X], 0.0f, camera_matrix[X][Z], 0.0f );
	left.Normalize( 8.0f );
	left += camera_pos;

	collision = feeler.GetCollision(camera_pos, left, true);
	if( collision )
	{
		left		   -= feeler.GetPoint();
		camera_pos		   -= left;
	}
	else
	{
		Mth::Vector	right( -camera_matrix[X][X], 0.0f, -camera_matrix[X][Z], 0.0f );
		right.Normalize( 8.0f );
		right += camera_pos;

		collision = feeler.GetCollision(camera_pos, right, true);
		if( collision )
		{
			right		   -= feeler.GetPoint();
			camera_pos		   -= right;
		}
	}
	
	if (!refocus) return;

	if( collision )
	{
		// Re-orient the camera again.
		camera_matrix[Z].Set( focus_pos.GetX() - camera_pos.GetX(), focus_pos.GetY() - camera_pos.GetY(), focus_pos.GetZ() - camera_pos.GetZ());
		camera_matrix[Z].Normalize();

		// Read back the Y from the current matrix.
//			target[Y][0]	= p_frame_matrix->up.x;
//			target[Y][1]	= p_frame_matrix->up.y;
//			target[Y][2]	= p_frame_matrix->up.z;

		// Generate new orthonormal X and Y axes.
		camera_matrix[X]		= Mth::CrossProduct( camera_matrix[Y], camera_matrix[Z] );
		camera_matrix[X].Normalize();
		camera_matrix[Y]		= Mth::CrossProduct( camera_matrix[Z], camera_matrix[X] );
		camera_matrix[Y].Normalize();

		// Write back into camera matrix.
		camera_matrix[X]		= -camera_matrix[X];
		camera_matrix[Z]		= -camera_matrix[Z];
		
		// Fix the final column
		camera_matrix[X][W] = 0.0f;
		camera_matrix[Y][W] = 0.0f;
		camera_matrix[Z][W] = 0.0f;
		camera_matrix[W][W] = 1.0f;
		
	}
//#endif
}

}

#ifdef THUG_DESKTOP

extern "C" float desktop_camera_shake( void );
extern "C" float desktop_camera_fov_push( void );
extern "C" float desktop_head_bob( void );

namespace Obj
{

// Camera weight (desktop port): a sprung dip and nod when a landing or a
// slam stops the target hard, a short shake when a bail starts, a few degrees
// of extra view at top skating speed, and a step bob on foot. The skater and
// walk cameras both call this on their finished frame, before camera
// collision; the state is shared, there is one local skater.
static struct
{
	bool		valide;
	Mth::Vector	vel;
	bool		bail;
	float		dip, dip_vit;		// inches below the camera's own place, and its speed
	float		secousse, secousse_t;	// bail shake strength 0-1, time since it started
	float		fov;			// degrees added right now
	float		pas, bob;		// step phase (1 = one step), bob amount 0-1.3
} s_poids;

// The FOV push is added on top of whatever each camera was given: per camera,
// the value written last time and the base it came from. A value someone else
// wrote in between becomes the new base.
static struct { Gfx::Camera *cam; float ecrit, base; } s_fov[4];

static void pousse_fov( Gfx::Camera *c, float plus )
{
	if( !c ) return;
	const float h = c->GetHFOV();
	int k = 0;
	while( k < 4 && s_fov[k].cam != c ) ++k;
	if( k == 4 )
	{
		for( k = 0; k < 3 && s_fov[k].cam; ++k ) {}
		s_fov[k].cam = c;
		s_fov[k].base = h;
	}
	else if( fabsf( h - s_fov[k].ecrit ) > 0.001f )
		s_fov[k].base = h;
	const float n = s_fov[k].base + plus;
	if( fabsf( n - h ) > 0.001f ) c->SetHFOV( n );
	s_fov[k].ecrit = n;
}

static float borne( float v, float a, float b ) { return v < a ? a : v > b ? b : v; }

void DesktopCameraWeight( Gfx::Camera *p_cam, const Mth::Vector &target_vel, bool on_foot, bool on_ground,
						  bool bailing, bool instantly, Mth::Vector &cam_pos, Mth::Matrix &frame )
{
	const float force = desktop_camera_shake(), bob_force = desktop_head_bob();
	const float dt = Tmr::FrameLength();
	const bool  avance = dt > 0.0f && dt < 0.1f;

	if( !s_poids.valide || instantly || !avance )
	{
		if( !s_poids.valide || instantly )
		{
			s_poids.dip = s_poids.dip_vit = 0.0f;
			s_poids.secousse = 0.0f;
			s_poids.bail = bailing;
		}
		s_poids.vel = target_vel;
		s_poids.valide = true;
	}
	else
	{
		// Hard stops: a normal ollie lands at ~400 in/s, big drops at 2000+.
		const float dv = ( target_vel - s_poids.vel ).Length();
		const float choc = borne(( dv - 500.0f ) / 1500.0f, 0.0f, 1.0f ) * force;
		if( choc > 0.0f ) s_poids.dip_vit -= 110.0f * choc;
		s_poids.vel = target_vel;

		if( bailing && !s_poids.bail && force > 0.0f )
		{
			const float s = target_vel.Length();
			s_poids.secousse = ( 0.5f + 0.5f * borne( s / 900.0f, 0.0f, 1.0f )) * force;
			s_poids.secousse_t = 0.0f;
		}
		s_poids.bail = bailing;

		// Spring back, a little under-damped so it settles with a small rebound.
		const float w = 14.0f, z = 0.5f;
		int n = (int)( dt * 240.0f ) + 1;
		const float h = dt / n;
		for( ; n > 0; --n )
		{
			const float a = -w * w * s_poids.dip - 2.0f * z * w * s_poids.dip_vit;
			s_poids.dip_vit += a * h;
			s_poids.dip += s_poids.dip_vit * h;
		}

		if( s_poids.secousse > 0.0f )
		{
			s_poids.secousse_t += dt;
			if( s_poids.secousse_t > 0.45f ) s_poids.secousse = 0.0f;
		}

		// On-foot bob: one bump per step, a slight sway every two.
		Mth::Vector plat = target_vel;
		plat[Y] = 0.0f;
		const float s = plat.Length();
		const float cible = ( on_foot && on_ground ) ? borne( s / 450.0f, 0.0f, 1.3f ) : 0.0f;
		s_poids.bob += ( cible - s_poids.bob ) * ( 1.0f - expf( -8.0f * dt ));
		s_poids.pas += ( 1.6f + 1.3f * borne( s / 450.0f, 0.0f, 1.0f )) * dt;
		if( s_poids.pas > 2.0f ) s_poids.pas -= 2.0f;

		// Top speed: skating ~950-1100 in/s.
		const float cible_fov = on_foot ? 0.0f
			: borne(( target_vel.Length() - 650.0f ) / 500.0f, 0.0f, 1.0f ) * desktop_camera_fov_push();
		s_poids.fov += ( cible_fov - s_poids.fov ) * ( 1.0f - expf( -2.5f * dt ));
	}

	float dx = 0.0f, dy = s_poids.dip, tangage = -s_poids.dip * 0.15f, roulis = 0.0f;

	if( s_poids.secousse > 0.0f )
	{
		const float t = s_poids.secousse_t, fin = 1.0f - t / 0.45f;
		const float a = 2.0f * s_poids.secousse * fin * fin;
		dx += a * sinf( t * 2.0f * Mth::PI * 13.0f );
		dy += a * 0.8f * sinf( t * 2.0f * Mth::PI * 17.0f + 1.0f );
		roulis += 0.5f * s_poids.secousse * fin * fin * sinf( t * 2.0f * Mth::PI * 9.0f );
	}

	if( s_poids.bob > 0.001f && bob_force > 0.0f )
	{
		const float sp = sinf( Mth::PI * s_poids.pas );
		dy += s_poids.bob * bob_force * 1.1f * ( fabsf( sp ) - 0.637f );
		roulis += s_poids.bob * bob_force * 0.3f * sp;
	}

	// Rows: X right (negated), Y up, Z back. Positive pitch looks down.
	if( fabsf( tangage ) > 0.0001f )
	{
		const float a = Mth::DegToRad( tangage ), c = cosf( a ), s = sinf( a );
		const Mth::Vector y = frame[Y], zz = frame[Z];
		frame[Y] = y * c - zz * s;
		frame[Z] = zz * c + y * s;
	}
	if( fabsf( roulis ) > 0.0001f )
	{
		const float a = Mth::DegToRad( roulis ), c = cosf( a ), s = sinf( a );
		const Mth::Vector x = frame[X], y = frame[Y];
		frame[X] = x * c + y * s;
		frame[Y] = y * c - x * s;
	}
	frame[X][W] = frame[Y][W] = frame[Z][W] = 0.0f;
	cam_pos += (Mth::Vector)frame[Y] * dy + (Mth::Vector)frame[X] * dx;
	cam_pos[W] = 1.0f;

	pousse_fov( p_cam, s_poids.fov );
}

}

#endif
