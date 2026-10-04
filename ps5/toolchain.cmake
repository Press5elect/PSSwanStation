# SwanStation for PS5: cross-compile with the payload SDK PS5_PAYLOAD_SDK names
# (ps5/tools/build.sh sets it). The shape is PSFlyCast's toolchain file.
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-clang")
set(CMAKE_CXX_COMPILER "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-clang++")
set(CMAKE_AR "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-ar")
set(CMAKE_RANLIB "$ENV{PS5_PAYLOAD_SDK}/bin/prospero-ranlib")
set(CMAKE_FIND_ROOT_PATH "$ENV{PS5_PAYLOAD_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(PKG_CONFIG_EXECUTABLE "/bin/false" CACHE FILEPATH "" FORCE)
set(SWANSTATION_PS5 ON CACHE BOOL "" FORCE)

# Link-only probes resolve against the console's libraries; nothing built for
# the console runs on the build host.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -nostartfiles -nodefaultlibs -Wl,-e,0 -lkernel_web -lSceLibcInternal -lScePosixForWebKit")
# The console's processor is a Zen 2. -ffp-contract=off keeps floating point as
# on every other x86-64 build (no multiply and add fused into an FMA), and
# frame pointers are what a crash report on the console walks.
set(PS5_CPU_FLAGS "-march=znver2 -ffp-contract=off -fno-omit-frame-pointer -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_INIT "-O2 ${PS5_CPU_FLAGS} -fPIC -w -DZSTD_TRACE=0")
set(CMAKE_CXX_FLAGS_INIT "-O2 ${PS5_CPU_FLAGS} -fPIC -w -DZSTD_TRACE=0")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
