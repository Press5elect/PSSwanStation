#!/usr/bin/env bash
# PSSwanStation: compile the FSR 1 shaders (ps5/shaders/fsr_*) into
# ps5/src/fsr_spirv.inc, which is kept in the repository so that a build needs
# no shader compiler. Run it again after changing a shader. Needs
# glslangValidator.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
ps5=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$ps5/src/fsr_spirv.inc"
{
    echo "// Made by ps5/tools/make-fsr-shaders.sh from ps5/shaders/fsr_*: do not edit."
    echo "// SPIR-V of the FSR 1 passes (AMD's ffx_fsr1.h, MIT: ps5/third_party/fsr)."
} > "$out"
for shader in fsr_vertex.vert fsr_easu.frag fsr_rcas.frag; do
    name=${shader%.*}
    glslangValidator -V --quiet -I"$ps5/third_party/fsr" -o "$work/$name.spv" "$ps5/shaders/$shader"
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
