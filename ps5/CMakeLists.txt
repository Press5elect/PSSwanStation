# PSSwanStation: the title's build (ps5/tools/build.sh drives it).
#
# SwanStation is a libretro core. This project compiles the core as it is
# (dep/, src/common, src/core, src/libretro), links it statically into a title
# of its own, and adds the frontend in ps5/src: the display, the library, the
# settings, cheats, audio, the DualSense and the network share.
#
# Two builds come from it:
#   - the title, with ps5/toolchain.cmake (SWANSTATION_PS5 is set there): the
#     objects are handed to ps5/tools/link.sh, which links them with RADV and
#     signs the result;
#   - a host build (no toolchain file), the same frontend on the PC's Vulkan
#     driver with a headless surface, which ps5/tools/host-test.sh runs to
#     check the interface and the emulator without a console.
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)
project(swanstation_ps5 C CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LIBSMB2_DIR "${ROOT}/../deps-src/libsmb2" CACHE PATH "libsmb2")
set(RCHEEVOS_DIR "${ROOT}/../deps-src/rcheevos" CACHE PATH "rcheevos (v12)")
# BlackBearReloaded's ps5-homebrew-ui kit, through mihawk-99's PS5_VKHomebrewUI
# (its Vulkan backend), at the revision ps5/tools/build.sh checks.
set(HUI_DIR "${ROOT}/../PS5_VKHomebrewUI" CACHE PATH "PS5_VKHomebrewUI (the interface kit)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-switch")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-switch")

# What SwanStation's own top-level CMakeLists.txt sets for its subdirectories.
set(CPU_ARCH "x64")
set(USE_EGL OFF)
set(USE_WAYLAND OFF)
set(USE_D3D12 OFF)
set(USE_FBDEV OFF)
set(USE_EVDEV OFF)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(SWANSTATION_PS5)
	# A title's file mappings are charged to its small flexible memory:
	# disc images are read, not mapped.
	set(LIBRETRO_COMMON_NO_MMAP ON CACHE BOOL "" FORCE)
endif()

execute_process(COMMAND git describe --tags --always WORKING_DIRECTORY ${ROOT}
	OUTPUT_VARIABLE GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${ROOT}
	OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(TIMESTAMP BUILD_TIMESTAMP UTC)
configure_file(${ROOT}/src/libretro/version.h.in ${CMAKE_BINARY_DIR}/gen/version.h @ONLY)

include(CheckSymbolExists)
check_symbol_exists(alloca "alloca.h" HAVE_ALLOCA_H)
if(NOT HAVE_ALLOCA_H)
	check_symbol_exists(alloca "malloc.h" HAVE_MALLOC_H)
endif()
if(NOT SWANSTATION_PS5)
	check_symbol_exists(mkstemp "stdlib.h" HAVE_MKSTEMP)
endif()
configure_file(${ROOT}/src/config.h.cmake ${CMAKE_BINARY_DIR}/gen/config.h)
include_directories(${CMAKE_BINARY_DIR}/gen)

# The title owns the Vulkan device (src/core/gpu_hw_vulkan.cpp keeps it alive
# when a game ends).
add_compile_definitions(SWANSTATION_STANDALONE=1)
# One retro_atomic backend for C and C++ alike, never std::atomic (as SwanStation's own CMakeLists.txt).
add_compile_definitions(RETRO_ATOMIC_FORCE_GCC_NEW)

# The frontend compresses save states and reads the cheat database's ZIP
# archives with the core's miniz (dep/libchdr/deps/miniz-3.1.1/miniz.h keeps
# those APIs when SWANSTATION_STANDALONE is defined).
set(MINIZ_ARCHIVE_APIS ON CACHE BOOL "" FORCE)
set(MINIZ_DEFLATE_APIS ON CACHE BOOL "" FORCE)
add_subdirectory(${ROOT}/dep dep)
add_subdirectory(${ROOT}/src/common common)
add_subdirectory(${ROOT}/src/core core)

# libsmb2 (LGPL-2.1): games on SMB shares. Compiled from a checkout, with
# ps5/smb/config.h; the names the console lacks are bound to ps5/smb/compat.c.
set(LIBSMB2_NAMES
	aes aes_reference aes128ccm alloc asn1-ber compat errors hmac hmac-md5 init libsmb2 md4c md5 ntlmssp pdu
	sha1 sha224-256 sha384-512 smb2-cmd-cancel smb2-cmd-close smb2-cmd-create smb2-cmd-echo smb2-cmd-error
	smb2-cmd-flush smb2-cmd-ioctl smb2-cmd-lock smb2-cmd-logoff smb2-cmd-negotiate smb2-cmd-notify-change
	smb2-cmd-oplock-break smb2-cmd-query-directory smb2-cmd-query-info smb2-cmd-read smb2-cmd-session-setup
	smb2-cmd-set-info smb2-cmd-tree-connect smb2-cmd-tree-disconnect smb2-cmd-write smb2-data-file-info
	smb2-data-filesystem-info smb2-data-security-descriptor smb2-data-reparse-point smb2-share-enum smb3-seal
	smb2-signing socket spnego-wrapper sync timestamps unicode usha)
set(LIBSMB2_SOURCES)
foreach(name ${LIBSMB2_NAMES})
	list(APPEND LIBSMB2_SOURCES "${LIBSMB2_DIR}/lib/${name}.c")
endforeach()
if(SWANSTATION_PS5)
	add_library(smb2 STATIC ${LIBSMB2_SOURCES} smb/compat.c)
	target_include_directories(smb2 BEFORE PRIVATE smb)
	target_compile_definitions(smb2 PRIVATE HAVE_CONFIG_H "_U_=__attribute__((unused))" SOL_TCP=6
		getlogin_r=ps5_smb_getlogin_r gethostname=ps5_smb_gethostname readv=ps5_smb_readv writev=ps5_smb_writev
		getaddrinfo=ps5_smb_getaddrinfo freeaddrinfo=ps5_smb_freeaddrinfo
		fcntl=ps5_smb_fcntl connect=ps5_smb_connect)
else()
	add_library(smb2 STATIC ${LIBSMB2_SOURCES})
	target_include_directories(smb2 BEFORE PRIVATE smb/host)
	target_compile_definitions(smb2 PRIVATE HAVE_CONFIG_H "_U_=__attribute__((unused))" _GNU_SOURCE)
endif()
target_include_directories(smb2 PRIVATE "${LIBSMB2_DIR}/include" "${LIBSMB2_DIR}/include/smb2" "${LIBSMB2_DIR}/lib")
target_include_directories(smb2 INTERFACE "${LIBSMB2_DIR}/include")
target_compile_options(smb2 PRIVATE -w)

# libnfs (LGPL-2.1-or-later): NFS shares, versions 3 and 4, AUTH_SYS. Built
# from its sources as libsmb2 is; on the console with the same socket shims
# (nfs/compat.c, and libsmb2's in smb/compat.c).
set(LIBNFS_DIR "${ROOT}/../deps-src/libnfs" CACHE PATH "libnfs")
set(LIBNFS_SOURCES)
foreach(name lib/init lib/libnfs lib/libnfs-sync lib/libnfs-zdr lib/multithreading lib/nfs_v3 lib/nfs_v4 lib/pdu
		lib/socket mount/mount mount/libnfs-raw-mount nfs/nfs nfs/nfsacl nfs/libnfs-raw-nfs nfs4/nfs4
		nfs4/libnfs-raw-nfs4 nlm/nlm nlm/libnfs-raw-nlm nsm/nsm nsm/libnfs-raw-nsm portmap/portmap
		portmap/libnfs-raw-portmap rquota/rquota rquota/libnfs-raw-rquota)
	list(APPEND LIBNFS_SOURCES "${LIBNFS_DIR}/${name}.c")
endforeach()
if(SWANSTATION_PS5)
	add_library(nfs STATIC ${LIBNFS_SOURCES} nfs/compat.c)
	target_include_directories(nfs BEFORE PRIVATE nfs)
	target_compile_definitions(nfs PRIVATE HAVE_CONFIG_H HAVE_NFS4_2 "_U_=__attribute__((unused))"
		readv=ps5_smb_readv writev=ps5_smb_writev getaddrinfo=ps5_smb_getaddrinfo freeaddrinfo=ps5_smb_freeaddrinfo
		connect=ps5_smb_connect fcntl=ps5_nfs_fcntl ioctl=ps5_nfs_ioctl recv=ps5_nfs_recv
		getservbyport=ps5_nfs_getservbyport getprotobyname=ps5_nfs_getprotobyname
		getuid=ps5_nfs_getuid getgid=ps5_nfs_getgid strndup=ps5_nfs_strndup)
	target_link_libraries(nfs PRIVATE smb2)
else()
	add_library(nfs STATIC ${LIBNFS_SOURCES})
	target_include_directories(nfs BEFORE PRIVATE nfs/host)
	target_compile_definitions(nfs PRIVATE HAVE_CONFIG_H HAVE_NFS4_2 "_U_=__attribute__((unused))" _GNU_SOURCE)
endif()
target_include_directories(nfs PRIVATE "${LIBNFS_DIR}/include" "${LIBNFS_DIR}/include/nfsc" "${LIBNFS_DIR}/mount"
	"${LIBNFS_DIR}/nfs" "${LIBNFS_DIR}/nfs4" "${LIBNFS_DIR}/nlm" "${LIBNFS_DIR}/nsm" "${LIBNFS_DIR}/portmap"
	"${LIBNFS_DIR}/rquota")
target_include_directories(nfs INTERFACE "${LIBNFS_DIR}/include" "${LIBNFS_DIR}/mount" "${LIBNFS_DIR}/nfs")
target_compile_options(nfs PRIVATE -w)

# rcheevos (MIT): RetroAchievements. Its client, the server API, the
# achievement runtime and the hash of a PlayStation disc; nothing for other
# systems. The three definitions are needed by whatever includes its headers
# too (they change a structure's layout).
set(RCHEEVOS_NAMES
	rc_client rc_compat rc_util rc_version
	rapi/rc_api_common rapi/rc_api_info rapi/rc_api_runtime rapi/rc_api_user
	rcheevos/alloc rcheevos/condition rcheevos/condset rcheevos/consoleinfo rcheevos/format rcheevos/lboard
	rcheevos/memref rcheevos/operand rcheevos/richpresence rcheevos/runtime rcheevos/runtime_progress
	rcheevos/trigger rcheevos/value
	rhash/md5 rhash/hash rhash/hash_disc rhash/cdreader)
set(RCHEEVOS_SOURCES)
foreach(name ${RCHEEVOS_NAMES})
	list(APPEND RCHEEVOS_SOURCES "${RCHEEVOS_DIR}/src/${name}.c")
endforeach()
add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
target_include_directories(rcheevos PUBLIC "${RCHEEVOS_DIR}/include")

# glslang: a shader preset brought in from a USB drive is compiled on the
# console (src/slangimport.cpp). Its library alone: GLSL to SPIR-V, no HLSL,
# no optimiser, no programs.
set(GLSLANG_DIR "${ROOT}/../deps-src/glslang" CACHE PATH "glslang (16.6.0)")
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_SPIRV ON CACHE BOOL "" FORCE)
if(SWANSTATION_PS5)
	# The console's threads are in its libc: the probe glslang asks for would
	# link a test program, which a payload cannot be.
	set(CMAKE_HAVE_LIBC_PTHREAD 1 CACHE INTERNAL "")
	set(CMAKE_HAVE_PTHREAD_H 1 CACHE INTERNAL "")
	set(THREADS_PREFER_PTHREAD_FLAG OFF)
endif()
add_subdirectory(${GLSLANG_DIR} glslang EXCLUDE_FROM_ALL)
target_compile_definitions(rcheevos PUBLIC RC_HASH_NO_ROM RC_HASH_NO_ZIP RC_HASH_NO_ENCRYPTED)
target_compile_options(rcheevos PRIVATE -w)

set(frontend_sources
	src/main.cpp
	src/storage.cpp
	src/rewind.cpp
	src/recorder.cpp
	src/memsearch.cpp
	src/speedrun.cpp
	src/web.cpp
	src/slangimport.cpp
	third_party/qrcodegen/qrcodegen.cpp
	src/music.cpp
	src/memcard.cpp
	src/netplay.cpp
	src/achievements.cpp
	src/update.cpp
	src/ui_more.cpp
	src/ui_flow.cpp
	src/display.cpp
	src/chain.cpp
	src/host.cpp
	src/options.cpp
	src/ui.cpp
	src/ui_widgets.cpp
	src/swan.cpp
	src/library.cpp
	src/cheats.cpp
	src/audio.cpp
	src/sound.cpp
	src/vfs.cpp
	src/smb.cpp
	src/ftp.cpp
	src/nfs.cpp
	src/netfiles.cpp
	src/covers.cpp
	src/gamedb.cpp
	src/history.cpp
	src/diag.cpp
	src/texture.cpp
	src/hui_platform.cpp)
if(SWANSTATION_PS5)
	list(APPEND frontend_sources
		src/ps5/pad_ps5.cpp
		src/ps5/audio_ps5.cpp
		src/ps5/http_ps5.cpp
		src/ps5/libc_ps5.cpp
		src/ps5/threads_ps5.cpp
		src/ps5/stdio_ps5.cpp
		src/ps5/platform_ps5.cpp
		src/ps5/elevation/elevation.cpp)
else()
	list(APPEND frontend_sources src/host/platform_host.cpp)
endif()

# With the core's C++17: its disc header does not compile as C++20.
set(frontend_cxx17_sources src/discid.cpp)

set(core_glue_sources
	${ROOT}/src/libretro/libretro_audio_stream.cpp
	${ROOT}/src/libretro/libretro_game_settings.cpp
	${ROOT}/src/libretro/libretro_host_display.cpp
	${ROOT}/src/libretro/libretro_host_interface.cpp
	${ROOT}/src/libretro/libretro_settings_interface.cpp)

# The interface kit: shapes, SDF text, themes, components, springs, the mixer
# and its cues. Not its OpenGL backend, its own console layer or its app (the
# designs, the shell, the demo content), and not its stb_vorbis: music.cpp's
# copy serves both. Its Vulkan backend draws through the same function table as
# the rest of the title (dep/vulkan-loader), forced in ahead of its headers.
file(GLOB hui_sources
	${HUI_DIR}/src/gfx/draw_list.cpp ${HUI_DIR}/src/gfx/font.cpp ${HUI_DIR}/src/gfx/renderer.cpp
	${HUI_DIR}/src/gfx/triangulate.cpp ${HUI_DIR}/src/gfx/vk/vk_renderer.cpp
	${HUI_DIR}/src/ui/*.cpp ${HUI_DIR}/src/ui/components/*.cpp
	${HUI_DIR}/src/core/*.cpp ${HUI_DIR}/src/audio/*.cpp)
add_library(hui STATIC ${hui_sources})
target_include_directories(hui PUBLIC ${HUI_DIR}/src)
target_include_directories(hui PRIVATE ${ROOT}/dep/vulkan-loader/include)
target_compile_options(hui PRIVATE -std=c++20 -w -fno-exceptions)
set_source_files_properties(${HUI_DIR}/src/gfx/vk/vk_renderer.cpp PROPERTIES COMPILE_OPTIONS
	"-include;${ROOT}/dep/vulkan-loader/include/vulkan_loader.h")

add_executable(swanstation ${frontend_sources} ${frontend_cxx17_sources} ${core_glue_sources})
target_link_libraries(swanstation PRIVATE hui core common glad vulkan-loader libretro-common stb xxhash miniz smb2 nfs rcheevos
	glslang glslang-default-resource-limits)
target_include_directories(swanstation PRIVATE src ${ROOT}/src/libretro
	${ROOT}/dep/stb/include ${CMAKE_BINARY_DIR}/gen ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nis
	${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen)
# Vulkan is reached through the core's function table (dep/vulkan-loader),
# which is filled from the RADV linked into the title.
target_compile_definitions(swanstation PRIVATE VK_NO_PROTOTYPES
	FE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
# The fonts and the logo are assembled into ui_widgets.cpp's object.
file(GLOB hui_fonts ${CMAKE_CURRENT_SOURCE_DIR}/assets/hui/fonts/*.huifont)
set_source_files_properties(src/ui_widgets.cpp PROPERTIES OBJECT_DEPENDS
	"${hui_fonts};${CMAKE_CURRENT_SOURCE_DIR}/assets/logo-box.png")
set_source_files_properties(${frontend_sources} PROPERTIES COMPILE_OPTIONS "-std=c++20")
# The two decoders in it are other people's C, compiled as they are.
set_source_files_properties(src/music.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20;-w")
# The build's date, in main.cpp alone so a new day recompiles one file.
string(TIMESTAMP FE_BUILD_DATE "%Y-%m-%d" UTC)
set_source_files_properties(src/main.cpp PROPERTIES COMPILE_DEFINITIONS "FE_BUILD_DATE=\"${FE_BUILD_DATE}\"")
if(SWANSTATION_PS5)
	target_compile_definitions(swanstation PRIVATE SWANSTATION_PS5=1)
	# CMake gives the objects and archives, in link order, to link.sh, which
	# links them with RADV and the console's runtime, then converts and signs.
	set(CMAKE_CXX_LINK_EXECUTABLE
		"bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/link.sh <TARGET> <OBJECTS> -- <LINK_LIBRARIES>")
else()
	target_compile_definitions(swanstation PRIVATE SWANSTATION_HOST=1)
	target_link_libraries(swanstation PRIVATE dl pthread)
endif()
