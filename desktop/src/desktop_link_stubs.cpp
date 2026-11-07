/*****************************************************************************
**  THUG desktop -- link stubs                                              **
**  desktop/src/desktop_link_stubs.cpp                                      **
**                                                                          **
**  Same symbol set as Code/Sys/Vita/p_link_stubs.S (ARM assembly, Vita     **
**  only), written as C++ with their real signatures so return values are  **
**  well defined on x86. Engine features no backend implements: memory     **
**  viewer, shatter and texture splat effects, the PS2 USB keyboard.       **
*****************************************************************************/

#include <core/defines.h>
#include <gfx/NxMiscFX.h>
#include <sys/timer.h>

void MemViewToggle() {}

namespace Nx
{
void plat_screen_flash_render( sScreenFlashDetails * ) {}
void plat_texture_splat_initialize( void ) {}
void plat_texture_splat_cleanup( void ) {}
void plat_texture_splat_render( void ) {}
void plat_texture_splat_reset_poly( sSplatInstanceDetails *, int ) {}
bool plat_texture_splat( Nx::CSector **, Nx::CCollStatic **, Mth::Vector &, Mth::Vector &, float, float,
                         Nx::CTexture *, Nx::sSplatTrailInstanceDetails * ) { return false; }
void plat_shatter_initialize( void ) {}
void plat_shatter_cleanup( void ) {}
void plat_shatter( CGeom * ) {}
void plat_shatter_update( sShatterInstanceDetails *, float ) {}
void plat_shatter_render( sShatterInstanceDetails * ) {}
}

namespace SIO
{
int  KeyboardRead( char * ) { return 0; }
void KeyboardClear( void ) {}
}

namespace Tmr
{
uint64 GetRenderFrame() { return 0; }
}

bool  gbQuit = false;
bool  gun_fired = false;
float screen_angle = 72.0f;
