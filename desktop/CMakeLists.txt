# THUG desktop -- Windows / Linux build of the Vita port.
#
# Builds the same engine sources as the Vita target (vita/engine.cmake): the
# portable game code plus the Code/*/Vita backends. Instead of the Vita SDK
# and vitaGL, desktop/shim provides those headers on top of SDL2 and desktop
# OpenGL (compatibility profile), and desktop/src implements them.
#
# 32-bit only: the 2003 engine stores pointers in 32-bit fields.
#
#   Windows (MSYS2 MINGW32 shell, or a MinGW i686 cross compiler):
#     cmake -S desktop -B build-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-desktop
#   Linux (gcc -m32 + 32-bit SDL2): see desktop/README.md

cmake_minimum_required(VERSION 3.16)
project(thug_desktop C CXX)

get_filename_component(THUG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
# Source tree to compile (a case-fixed copy can be passed on Linux).
set(THUG_SRC_ROOT "${THUG_ROOT}" CACHE PATH "Root holding Code/ and vita/")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "THUG must be built as 32-bit (got ${CMAKE_SIZEOF_VOID_P}-byte pointers). "
                      "Use an i686 toolchain or -m32.")
endif()

if(NOT SDL2_INCLUDE_DIRS)
  find_package(SDL2 REQUIRED)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive -fno-exceptions -fno-rtti")
add_compile_options(
  -Wno-write-strings -Wno-narrowing -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-multichar -Wno-deprecated-declarations -Wno-address-of-packed-member
  -fno-strict-aliasing -msse2 -mfpmath=sse
)
add_compile_definitions(
  __PLAT_WN32__
  __PLAT_VITA__
  VITA
  THUG_DESKTOP
  # Player build: no debug overlay or shortcuts, no dev tools, short log.
  THUG_RELEASE
)
if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED)
endif()

# Engine sources: same selection as the Vita target.
set(THUG_ROOT_SAVED "${THUG_ROOT}")
set(THUG_ROOT "${THUG_SRC_ROOT}")
include("${THUG_SRC_ROOT}/vita/engine.cmake")
set(THUG_ROOT "${THUG_ROOT_SAVED}")
# ARM assembly stubs: replaced by desktop/src/desktop_link_stubs.cpp.
list(FILTER THUG_SOURCES EXCLUDE REGEX "p_link_stubs\\.S$")

set(DESKTOP_SOURCES
  src/shim_gl.cpp
  src/shim_psp2.cpp
  src/shim_input_audio.cpp
  src/cg_to_glsl.cpp
  src/desktop_runtime.cpp
  src/desktop_link_stubs.cpp
  src/crash_handler.cpp
  "${THUG_SRC_ROOT}/vita/src/vita_log.c"
)

if(WIN32)
  list(APPEND DESKTOP_SOURCES src/win_main.cpp)
endif()

add_executable(thug ${THUG_SOURCES} ${DESKTOP_SOURCES})

# The 2003 engine code: gnu++98, and the Vita build's optimisation policy
# (vita/CMakeLists.txt): the engine as a whole is NOT optimised (it relies on
# undefined behaviour, e.g. methods called on NULL that test 'this', which
# the optimiser deletes), and a list of hot files tested one by one is -O2.
# -funsigned-char: char is unsigned on ARM, which the Vita build was tested
# with. -fno-delete-null-pointer-checks keeps the 'this == NULL' tests even
# in optimised files.
set(THUG_ENGINE_OPTS "-std=gnu++98;-funsigned-char;-fno-strict-aliasing;-fno-delete-null-pointer-checks")
set_source_files_properties(${THUG_SOURCES} PROPERTIES COMPILE_OPTIONS "${THUG_ENGINE_OPTS};-O0")
set(R "${THUG_SRC_ROOT}")
set(THUG_HOT_SOURCES
    "${R}/Code/Gfx/Vita/p_NxModel.cpp"
    "${R}/Code/Gfx/Vita/p_world_render.cpp"
    "${R}/Code/Gfx/Vita/p_scene_load.cpp"
    "${R}/Code/Gfx/Vita/p_NxTexture.cpp"
    "${R}/Code/Core/Vita/adpcm.cpp"
    "${R}/Code/Gel/Music/Vita/p_music.cpp"
    "${R}/Code/Gel/SoundFX/Vita/p_sfx.cpp"
    "${R}/Code/Gfx/Vita/p_shader_decor.cpp"
    "${R}/Code/Gfx/Vita/p_NxSprite.cpp"
    "${R}/Code/Gfx/Vita/p_NxFont.cpp"
    "${R}/Code/Gfx/Vita/p_NxLight.cpp"
    "${R}/Code/Gfx/Vita/p_occlusion.cpp"
    "${R}/Code/Gfx/Vita/p_nx.cpp"
    "${R}/Code/Gfx/Vita/p_ombre.cpp"
    "${R}/Code/Gfx/Vita/p_gamma.cpp"
    "${R}/Code/Gfx/Vita/p_NxNewParticle.cpp"
    "${R}/Code/Gfx/Vita/p_NxParticle.cpp")
file(GLOB THUG_HOT_GLOBS
     "${R}/Code/Core/Math/*.cpp" "${R}/Code/Gel/Collision/*.cpp"
     "${R}/Code/Gfx/*.cpp" "${R}/Code/Gel/Components/*.cpp" "${R}/Code/Sk/Objects/*.cpp"
     "${R}/Code/Gel/Scripting/*.cpp" "${R}/Code/Gel/Object/*.cpp" "${R}/Code/Sk/Components/*.cpp"
     "${R}/Code/Sys/File/*.cpp")
list(APPEND THUG_HOT_SOURCES ${THUG_HOT_GLOBS})
foreach(f ${THUG_HOT_SOURCES})
  list(FIND THUG_SOURCES "${f}" idx)
  if(NOT idx EQUAL -1)
    set_source_files_properties("${f}" PROPERTIES COMPILE_OPTIONS "${THUG_ENGINE_OPTS};-O2")
  endif()
endforeach()
foreach(f ${DESKTOP_SOURCES})
  if(f MATCHES "\\.cpp$")
    set_source_files_properties(${f} PROPERTIES COMPILE_OPTIONS "-std=gnu++11")
  endif()
endforeach()

# Same per-file optimisation policy as the Vita build would be ideal; on x86
# the whole engine is built -O1 by default, with -O0 available for debugging.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O1 -g")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O1 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O1")
set(CMAKE_C_FLAGS_RELEASE "-O1")

target_include_directories(thug PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/shim"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${THUG_SRC_ROOT}/vita/src"
  "${THUG_SRC_ROOT}/Code"
  "${THUG_SRC_ROOT}/Code/Sk"
  "${THUG_SRC_ROOT}/Code/Gfx/Vita"
)
if(THUG_CASE_SHADOW)
  target_include_directories(thug BEFORE PRIVATE "${THUG_CASE_SHADOW}")
endif()

if(TARGET SDL2::SDL2)
  target_link_libraries(thug PRIVATE SDL2::SDL2)
else()
  target_include_directories(thug PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(thug PRIVATE ${SDL2_LIBRARIES})
endif()

if(WIN32)
  target_link_libraries(thug PRIVATE opengl32 winmm ws2_32)
  # Static runtime: the .exe only needs SDL2.dll beside it.
  target_link_options(thug PRIVATE -mwindows -static-libgcc -static-libstdc++)
else()
  find_package(OpenGL REQUIRED)
  target_link_libraries(thug PRIVATE OpenGL::GL pthread m)
endif()
