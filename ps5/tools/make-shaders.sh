#!/usr/bin/env bash
# PSSwanStation: compile the title's own shaders (ps5/shaders: FSR 1, frame
# generation and the picture's look, with NVIDIA Image Scaling) into
# ps5/src/fsr_spirv.inc, fg_spirv.inc and look_spirv.inc, which are kept in
# the repository so that a build needs no shader compiler. Run it again after
# changing a shader. Needs glslangValidator.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# write OUT.inc COMMENT SHADER...
write() {
    local out=$1 about=$2
    shift 2
    {
        echo "// Made by ps5/tools/make-shaders.sh from ps5/shaders: do not edit."
        echo "// $about"
    } > "$out"
    local shader name
    for shader in "$@"; do
        name=${shader%.*}
        glslangValidator -V --quiet -I"$ps5/third_party/fsr" -I"$ps5/third_party/cas" -I"$ps5/third_party/nis" \
            -o "$work/$name.spv" "$ps5/shaders/$shader"
        python3 - "$work/$name.spv" "$name" >> "$out" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
words = struct.unpack("<%dI" % (len(data) // 4), data)
print("static const uint32_t %s_spirv[] = {" % sys.argv[2])
for i in range(0, len(words), 8):
    print("\t" + " ".join("0x%08x," % w for w in words[i:i + 8]))
print("};")
PY
    done
    echo "wrote $out"
}
write "$ps5/src/fsr_spirv.inc" "SPIR-V of the FSR 1 passes (AMD's ffx_fsr1.h, MIT: ps5/third_party/fsr)." \
    fsr_vertex.vert fsr_easu.frag fsr_rcas.frag
write "$ps5/src/fg_spirv.inc" "SPIR-V of the frame generation passes." \
    fg_copy.frag fg_luma.frag fg_search.frag fg_tidy.frag fg_choose.frag fg_blend.frag fg_check.frag
write "$ps5/src/look_spirv.inc" "SPIR-V of the picture's look (with AMD's ffx_cas.h and NVIDIA's NIS, MIT: ps5/third_party)." \
    look_signal.frag look_scale.frag look_cas.frag look_nis.comp
