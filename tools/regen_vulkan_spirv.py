#!/usr/bin/env python3
# Regenerate embedded Vulkan SPIR-V blobs from GLSL sources.
#
# This script is NOT invoked by the core build. It is run by contributors who
# edit shader sources under data/shaders/vulkan/. The build itself only sees
# the resulting .inc files (checked into src/common/vulkan/embedded_spirv/),
# so the libretro core has zero glslangValidator dependency.
#
# Usage:
#     python3 tools/regen_vulkan_spirv.py [--glslang PATH]
#
# Requires glslangValidator from the Vulkan SDK or the `glslang-tools`
# package on Debian / Ubuntu.

import argparse
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GLSL_DIR = REPO_ROOT / "data" / "shaders" / "vulkan"
INC_DIR = REPO_ROOT / "src" / "common" / "vulkan" / "embedded_spirv"

# Map glsl file suffix to glslangValidator stage flag. Anything not in this
# table is skipped with a warning.
STAGE_FROM_SUFFIX = {
    ".vert.glsl": ("vert", "vs"),
    ".frag.glsl": ("frag", "fs"),
    ".comp.glsl": ("comp", "cs"),
    ".geom.glsl": ("geom", "gs"),
}


# Templates: shader sources that produce more than one .inc output, each
# with a different combination of -D preprocessor defines. Index ordering
# below MUST match the helper in src/common/vulkan/embedded_shaders.cpp
# that picks the right blob at runtime - if you add a variant here, add
# the matching entry there too.
#
# For each entry: filename -> [(variant_suffix, [-D defines]), ...]
def _batch_vs_variants():
    out = []
    # Attribute layout: untextured / textured (the latter always with
    # UV limits since the UV_LIMITS-routing commit lifted the axis to
    # the FS-side u_uv_limits cbuffer scalar). 2 layouts now.
    attr_axes = [
        ("untextured",       []),
        ("textured",         ["TEXTURED"]),
    ]
    # Output interpolation: standard / centroid (MSAA) / sample (SSAA).
    interp_axes = [
        ("none",     []),
        ("centroid", ["INTERP_CENTROID"]),
        ("sample",   ["INTERP_SAMPLE"]),
    ]
    # Color perspective: standard / noperspective.
    persp_axes = [
        ("persp",   []),
        ("noperp",  ["NOPERSP"]),
    ]
    for a_name, a_defs in attr_axes:
        for i_name, i_defs in interp_axes:
            for p_name, p_defs in persp_axes:
                suffix = f"{a_name}_{i_name}_{p_name}"
                out.append((suffix, a_defs + i_defs + p_defs))
    return out


def _batch_fs_untextured_variants():
    out = []
    # Output interpolation: standard / centroid (MSAA) / sample (SSAA).
    # MUST match the batch VS chosen by the C++ side.
    interp_axes = [
        ("none",     []),
        ("centroid", ["INTERP_CENTROID"]),
        ("sample",   ["INTERP_SAMPLE"]),
    ]
    # Color perspective: standard / noperspective. MUST match batch VS.
    persp_axes = [
        ("persp",   []),
        ("noperp",  ["NOPERSP"]),
    ]
    # Dual-source colour output: 1 attachment vs 2 (location 0
    # index 0/1). Pipeline blend state references SRC1_* or it does
    # not - per-call decision driven by transparency_mode AND
    # m_supports_dual_source_blend.
    dual_axes = [
        ("nodual",  []),
        ("dual",    ["DUAL_SOURCE"]),
    ]
    # PGXP_DEPTH used to be a fourth axis (gating gl_FragDepth
    # declaration) - collapsed to a runtime branch on the u_pgxp_depth
    # cbuffer scalar so the FS body unconditionally writes
    # gl_FragDepth with a ternary on u_pgxp_depth.
    for i_name, i_defs in interp_axes:
        for p_name, p_defs in persp_axes:
            for d_name, d_defs in dual_axes:
                suffix = f"{i_name}_{p_name}_{d_name}"
                out.append((suffix, i_defs + p_defs + d_defs))
    return out


def _batch_fs_textured_nearest_variants():
    out = []
    interp_axes = [
        ("none",     []),
        ("centroid", ["INTERP_CENTROID"]),
        ("sample",   ["INTERP_SAMPLE"]),
    ]
    persp_axes = [("persp", []), ("noperp", ["NOPERSP"])]
    dual_axes  = [("nodual", []), ("dual", ["DUAL_SOURCE"])]
    # UV_LIMITS used to be a fifth axis here, PGXP_DEPTH a fourth.
    # Both have been collapsed to runtime branches on the
    # u_uv_limits / u_pgxp_depth cbuffer scalars - v_uv_limits is
    # always declared (the batch VS always emits it when textured)
    # and gl_FragDepth is always written. Brings the Nearest FS cube
    # into parity shape with the Bilinear / JINC2 / xBR families.
    for i_name, i_defs in interp_axes:
        for p_name, p_defs in persp_axes:
            for d_name, d_defs in dual_axes:
                suffix = f"{i_name}_{p_name}_{d_name}"
                out.append((suffix, i_defs + p_defs + d_defs))
    return out


def _batch_fs_textured_filter_variants():
    """Shared structural cube for non-Nearest filter templates.

    UV_LIMITS is implicit (ShouldUseUVLimits() forces it true for any
    non-Nearest filter); PGXP_DEPTH used to be a fourth axis but has
    been collapsed to a runtime branch on u_pgxp_depth. Cube
    collapses to 3 x 2 x 2 = 12 blobs.
    """
    out = []
    interp_axes = [
        ("none",     []),
        ("centroid", ["INTERP_CENTROID"]),
        ("sample",   ["INTERP_SAMPLE"]),
    ]
    persp_axes = [("persp", []), ("noperp", ["NOPERSP"])]
    dual_axes  = [("nodual", []), ("dual", ["DUAL_SOURCE"])]
    for i_name, i_defs in interp_axes:
        for p_name, p_defs in persp_axes:
            for d_name, d_defs in dual_axes:
                suffix = f"{i_name}_{p_name}_{d_name}"
                out.append((suffix, i_defs + p_defs + d_defs))
    return out


def _with_shader_blend(variants):
    """Adds the accurate-blending blobs (data/shaders/vulkan/
    batch_shader_blend.glslinc): no MSAA, so no interpolation qualifier,
    and no colour output at all, so no dual-source axis; colour
    perspective as the batch VS has it. Suffixes none_persp_blend and
    none_noperp_blend, after the cube above."""
    return variants + [
        ("none_persp_blend",  ["SHADER_BLEND"]),
        ("none_noperp_blend", ["NOPERSP", "SHADER_BLEND"]),
    ]


TEMPLATE_VARIANTS = {
    "batch.vert.glsl":                      _batch_vs_variants(),
    "batch_untextured.frag.glsl":           _with_shader_blend(_batch_fs_untextured_variants()),
    "batch_textured_nearest.frag.glsl":     _with_shader_blend(_batch_fs_textured_nearest_variants()),
    "batch_textured_bilinear.frag.glsl":    _with_shader_blend(_batch_fs_textured_filter_variants()),
    "batch_textured_jinc2.frag.glsl":       _with_shader_blend(_batch_fs_textured_filter_variants()),
    "batch_textured_xbr.frag.glsl":         _with_shader_blend(_batch_fs_textured_filter_variants()),
}


def find_glslang(explicit):
    if explicit:
        return explicit
    for name in ("glslangValidator", "glslangValidator.exe"):
        from shutil import which
        path = which(name)
        if path:
            return path
    sys.stderr.write(
        "error: glslangValidator not found on PATH. Install the Vulkan SDK\n"
        "       or the glslang-tools package, or pass --glslang PATH.\n")
    sys.exit(1)


def sanitize_identifier(stem):
    # Map "screen_quad.vert" -> "screen_quad_vs"; only [A-Za-z0-9_] survive.
    name = re.sub(r"[^A-Za-z0-9_]", "_", stem)
    return name


def compile_one(glslang, glsl_path, stage_flag, defines=None):
    # Write SPIR-V to a temp file under INC_DIR so we don't pollute /tmp on
    # restricted-filesystem hosts.
    spv_path = INC_DIR / (glsl_path.stem + ".spv.tmp")
    cmd = [
        glslang,
        "--target-env", "vulkan1.0",
        "-S", stage_flag,
        "-V", str(glsl_path),
        "-o", str(spv_path),
    ]
    for d in defines or ():
        cmd.append(f"-D{d}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(f"glslangValidator failed for {glsl_path.name}:\n")
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        if spv_path.exists():
            spv_path.unlink()
        sys.exit(1)
    data = spv_path.read_bytes()
    spv_path.unlink()
    if len(data) % 4 != 0:
        sys.stderr.write(
            f"error: {glsl_path.name} produced {len(data)} bytes "
            f"of SPIR-V (not a multiple of 4)\n")
        sys.exit(1)
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if not words or words[0] != 0x07230203:
        sys.stderr.write(
            f"error: {glsl_path.name} produced invalid SPIR-V (bad magic)\n")
        sys.exit(1)
    return words


def emit_inc(identifier, glsl_name, words, out_path):
    # 8 words per line keeps the file diff-friendly without being too tall.
    PER_LINE = 8
    lines = []
    lines.append(
        f"// Autogenerated from data/shaders/vulkan/{glsl_name}.\n"
        f"// DO NOT EDIT. Regenerate with tools/regen_vulkan_spirv.py.\n"
        f"// glslangValidator --target-env vulkan1.0\n"
        f"//\n"
        f"// Included inside namespace Vulkan::EmbeddedShaders in\n"
        f"// embedded_shaders.cpp. The matching extern declaration lives in\n"
        f"// embedded_shaders.h.\n"
        f"\n"
        f"const uint32_t k_{identifier}[] = {{\n")
    for i in range(0, len(words), PER_LINE):
        chunk = ", ".join(f"0x{w:08x}u" for w in words[i:i + PER_LINE])
        lines.append(f"  {chunk},\n")
    lines.append(f"}};\n"
                 f"const size_t k_{identifier}_size_bytes = "
                 f"sizeof(k_{identifier});\n")
    out_path.write_text("".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glslang", help="path to glslangValidator binary")
    args = ap.parse_args()
    glslang = find_glslang(args.glslang)
    INC_DIR.mkdir(parents=True, exist_ok=True)

    glsl_files = sorted(GLSL_DIR.glob("*.glsl"))
    if not glsl_files:
        sys.stderr.write(f"warning: no .glsl files under {GLSL_DIR}\n")
        return

    total = 0
    for glsl_path in glsl_files:
        # glsl_path.name looks like "screen_quad.vert.glsl".
        suffix = None
        for k in STAGE_FROM_SUFFIX:
            if glsl_path.name.endswith(k):
                suffix = k
                break
        if suffix is None:
            sys.stderr.write(
                f"warning: {glsl_path.name} has no recognised stage suffix; "
                f"skipping\n")
            continue
        stage_flag, ident_suffix = STAGE_FROM_SUFFIX[suffix]

        # "screen_quad.vert.glsl" -> stem "screen_quad.vert" -> "screen_quad"
        base = glsl_path.name[:-len(suffix)]

        variants = TEMPLATE_VARIANTS.get(glsl_path.name)
        if variants is None:
            # Single-variant shader: one .inc, no -D flags.
            identifier = sanitize_identifier(base) + "_" + ident_suffix
            out_path = INC_DIR / (identifier + ".inc")
            words = compile_one(glslang, glsl_path, stage_flag)
            emit_inc(identifier, glsl_path.name, words, out_path)
            size_kb = (len(words) * 4) / 1024.0
            print(f"  {glsl_path.name:<40} -> {out_path.name}  "
                  f"({len(words)} words, {size_kb:.1f} KiB)")
            total += 1
        else:
            # Template: emit one .inc per variant, named with a suffix
            # appended after the stage identifier.
            for variant_suffix, defines in variants:
                identifier = (sanitize_identifier(base) + "_" + ident_suffix +
                              "_" + variant_suffix)
                out_path = INC_DIR / (identifier + ".inc")
                words = compile_one(glslang, glsl_path, stage_flag, defines)
                emit_inc(identifier, glsl_path.name, words, out_path)
                size_kb = (len(words) * 4) / 1024.0
                print(f"  {glsl_path.name:<40} -> {out_path.name}  "
                      f"({len(words)} words, {size_kb:.1f} KiB)")
                total += 1

    print(f"regenerated {total} SPIR-V blob(s) in {INC_DIR}")


if __name__ == "__main__":
    main()
