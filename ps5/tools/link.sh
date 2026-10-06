#!/usr/bin/env bash
# PSSwanStation: link the executable CMake built into a signed eboot.
#
#   link.sh TARGET OBJECT... -- LIBRARY...
#
# CMake calls this as the executable's link rule (ps5/CMakeLists.txt). The
# recipe is PS5_Vulkan's for a title that runs RADV on the console, as
# PSFlyCast uses it: the title's own CRT and C++ runtime (ps5/runtime), the
# AGC link stubs, RADV and the payload SDK's libc++ through
# tools/radv-link.sh, then the host tool's link (imports and SCE dynamic
# tables) and the fake-signed SELF. TARGET receives the signed eboot.
#
# PS5_VULKAN_DIR names the PS5_Vulkan checkout, with RADV built
# (tools/build-radv.sh release) and libc.prx and the host tool rebuilt
# (tools/rebuild-libc.sh). RADV_ARCHIVE may name another RADV archive.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
target=$1
shift
objects=()
while (( $# )) && [[ $1 != -- ]]; do
    objects+=("$1")
    shift
done
[[ ${1:-} == -- ]] && shift
archives=()
for item in "$@"; do
    case $item in
        *.a | *.o) archives+=("$item") ;;
        *) ;;  # -l and -pthread flags: the console's libraries come from the SDK below
    esac
done

: "${PS5_VULKAN_DIR:?set PS5_VULKAN_DIR to the PS5_Vulkan checkout}"
vk=$(cd -- "$PS5_VULKAN_DIR" && pwd)
sdk_root=${PS5_PAYLOAD_SDK:-$vk/.deps/native/ps5-payload-sdk}
tool=${PS5_NATIVE_TOOL:-$vk/build/host/ps5-native-tool}
[[ -x $tool ]] || tool="$vk/build/runtime-shim/ps5-native-tool"
archive=${RADV_ARCHIVE:-$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
for file in "$archive" "$tool" "$sdk_root/bin/prospero-lld"; do
    [[ -e $file ]] || { echo "link: missing $file" >&2; exit 2; }
done
export PS5_PAYLOAD_SDK="$sdk_root"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}

work="$(dirname -- "$target")/ps5-link"
mkdir -p "$work/obj" "$work/stubs"
cc() { sh "$vk/tooling/prospero-clang18" "$@"; }
# The title's start (ps5/runtime): ps5_crt.cpp clears the BSS, which the
# console's loader leaves holding old memory, before anything runs.
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
runtime="$here/../runtime"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$runtime/ps5_crt.cpp" -o "$work/obj/ps5_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$runtime/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
# AGC comes from system modules; these host-link stubs only name the imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vk/$source" -o "$work/obj/${library}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$vk/tools/radv-link.sh"
radv_link_recipe "$vk" "$sdk_root" "$archive" || exit 2
# Threads: ps5/src/ps5/threads_ps5.cpp's wrap (2 MiB stacks from libkernel,
# the caller's MXCSR) replaces the SDK's direct-memory stacks.
filtered=()
bound=" "
for flag in "${radv_link_flags[@]}"; do
    case $flag in
        --wrap=pthread_create | --wrap=pthread_join | --wrap=pthread_detach) ;;
        --defsym=*)
            name=${flag#--defsym=}
            bound+="${name%%=*} "
            filtered+=("$flag")
            ;;
        *) filtered+=("$flag") ;;
    esac
done
radv_link_flags=("${filtered[@]}" --wrap=pthread_create)
# fgetpos and fsetpos: the console's fpos_t is larger than the SDK's
# (ps5/src/ps5/stdio_ps5.cpp).
radv_link_flags+=(--wrap=fgetpos --wrap=fsetpos)

# Imports a title cannot use as they are (only libkernel_sys exports them, the
# SDK lists them but the console lacks them, or a title is refused them):
# bound to ps5/src/ps5/libc_ps5.cpp's versions, kept local. A name the RADV
# recipe binds already (the SDK's own localeconv) is left to it.
title_defsyms=()
{
    printf '{\n    local:\n'
    for name in fork link symlink readlink pathconf isatty getcwd realpath mkstemp \
            gai_strerror gethostbyname getnameinfo in6addr_any localeconv; do
        [[ $bound == *" $name "* ]] && continue
        title_defsyms+=("--defsym=$name=ps5_fe_$name")
        printf '        %s;\n' "$name"
    done
    printf '};\n'
} > "$work/title-local.map"

# Mesa leaves the entry points RADV does not implement as weak references that
# resolve to null; they must not become imports. The title's own layout
# (ps5/runtime/ps5-title.ld: with the BSS and code bounds) instead of the
# recipe's.
"$sdk_root/bin/prospero-lld" -T "$runtime/ps5-title.ld" --eh-frame-hdr --error-limit=0 \
    --no-dynamic-linker -z nodynamic-undefined-weak \
    "${radv_link_flags[@]}" "${title_defsyms[@]}" --version-script "$work/title-local.map" \
    --version-script "$runtime/app-symbols.map" --exclude-libs=ALL \
    --Map="$work/swanstation.map" \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/ps5_crt.o" "$work/obj/app_cpp_runtime.o" "${objects[@]}" \
    --start-group "${archives[@]}" --end-group \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so

# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's: an
# import only their stubs define links, and is null at run time, so its first
# call jumps to address 0. Refused here rather than found on the console (the
# check is PS5_VulkanTemplate's).
null_imports=$(comm -23 \
    <("$sdk_root/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" |
        awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk_root"/target/lib/*.so "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so"; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk_root/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "link: imports that no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
    echo "link: bind them in ps5/src/ps5/libc_ps5.cpp (the list above) or the platform layer" >&2
    exit 1
fi
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$target" --magic 0x1D3D154F
"$tool" self --inspect --file "$target" > "$work/eboot.inspect.txt"
# Kept for ps5/tools/symbolize.sh, which turns a crash report's eboot offsets
# into functions.
cp -f -- "$work/llvm-pie.elf" "$(dirname -- "$target")/swanstation-eboot.elf"
printf 'link: %s (%s bytes)\n' "$target" "$(stat -c %s "$target")"
