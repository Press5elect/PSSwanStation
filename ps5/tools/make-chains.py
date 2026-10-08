#!/usr/bin/env python3
"""PSSwanStation - libretro slang shader presets, compiled for the title's chain runner.

  make-chains.py SLANG_SHADERS_DIR OUT_DIR [NAME=PRESET.slangp ...]

Without NAME=PRESET the chains the title offers are made:
  crt-guest-advanced = crt/crt-guest-advanced.slangp

For each preset (github.com/libretro/slang-shaders, the format RetroArch reads):
its passes are read (#include expanded, the #pragma parameter lines collected),
each pass's vertex and fragment stage compiled to SPIR-V with glslangValidator,
and what each stage reads found in the SPIR-V itself (its push constants, its
uniform block and its textures, by name, with their offsets and bindings). The
result goes to OUT_DIR/NAME/: pN.vert.spv, pN.frag.spv, the preset's lookup
pictures, and chain.txt, which the title's chain runner (ps5/src/chain.cpp)
reads:

  chain NAME
  param NAME DEFAULT MIN MAX
  lut NAME FILE LINEAR MIPMAP WRAP
  pass N VERT FRAG FORMAT LINEAR WRAP MIPMAP_INPUT FRAME_COUNT_MOD ALIAS XTYPE X YTYPE Y
  push SIZE                        (of the pass before)
  ubo BINDING SIZE
  member push|ubo OFFSET TYPE NAME (TYPE: float int uint vec4 mat4)
  sampler BINDING NAME

SPDX-License-Identifier: GPL-3.0-or-later
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

DEFAULT_CHAINS = {"crt-guest-advanced": "crt/crt-guest-advanced.slangp"}


def read_preset(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = re.sub(r"(^|\s)(#|//).*$", "", line).strip()
            m = re.match(r'([A-Za-z0-9_]+)\s*=\s*"?([^"]*)"?\s*$', line)
            if m:
                values[m.group(1)] = m.group(2).strip()
    return values


def expand(path, seen=None):
    seen = seen or set()
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'\s*#include\s+"([^"]+)"', line)
            if m:
                inner = os.path.normpath(os.path.join(os.path.dirname(path), m.group(1)))
                if inner in seen:
                    continue
                seen.add(inner)
                out.extend(expand(inner, seen))
            else:
                out.append(line.rstrip("\n"))
    return out


def preprocess(path):
    """The two stages' sources, the parameters, the name and the format a pass declares."""
    lines = expand(path)
    params, name, fmt = [], None, None
    common, stages, current = [], {"vertex": [], "fragment": []}, None
    for line in lines:
        m = re.match(r'\s*#pragma\s+parameter\s+([A-Za-z0-9_]+)\s+"[^"]*"\s+([-0-9.eE+]+)\s+([-0-9.eE+]+)\s+([-0-9.eE+]+)', line)
        if m:
            params.append((m.group(1), float(m.group(2)), float(m.group(3)), float(m.group(4))))
            line = ""
        m = re.match(r"\s*#pragma\s+name\s+(\S+)", line)
        if m:
            name, line = m.group(1), ""
        m = re.match(r"\s*#pragma\s+format\s+(\S+)", line)
        if m:
            fmt, line = m.group(1), ""
        m = re.match(r"\s*#pragma\s+stage\s+(\S+)", line)
        if m:
            current, line = m.group(1), ""
        (stages[current] if current else common).append(line)
    sources = {}
    for stage in ("vertex", "fragment"):
        text = common + stages[stage]
        # The #version line must come first: the rest keeps its line numbers.
        sources[stage] = "\n".join(text) + "\n"
    return sources, params, name, fmt


# ----------------------------------------------------------- SPIR-V reflection

def reflect(words):
    """Push constant members, the uniform block (binding, members) and the textures."""
    names, member_names, decorations, member_offsets = {}, {}, {}, {}
    types, variables = {}, []
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        args = words[i + 1:i + count]
        if op == 5:  # OpName
            names[args[0]] = string(args[1:])
        elif op == 6:  # OpMemberName
            member_names[(args[0], args[1])] = string(args[2:])
        elif op == 71:  # OpDecorate
            decorations.setdefault(args[0], {})[args[1]] = args[2] if len(args) > 2 else True
        elif op == 72 and args[2] == 35:  # OpMemberDecorate Offset
            member_offsets[(args[0], args[1])] = args[3]
        elif op == 21:
            types[args[0]] = ("int" if args[2] else "uint", 4)
        elif op == 22:
            types[args[0]] = ("float", 4)
        elif op == 23:
            types[args[0]] = ("vec%d" % args[2], 4 * args[2], args[1])
        elif op == 24:
            types[args[0]] = ("mat%d" % args[2], 16 * args[2], args[1])
        elif op in (25, 26, 27):
            types[args[0]] = ("texture", 0)
        elif op == 30:
            types[args[0]] = ("struct", 0, list(args[1:]))
        elif op == 32:
            types[args[0]] = ("pointer", 0, args[1], args[2])
        elif op == 59:
            variables.append((args[1], args[0], args[2]))
        i += count
    push, ubo, samplers = None, None, []

    def members(struct_id):
        out = []
        for index, member_type in enumerate(types[struct_id][2]):
            kind = types[member_type][0]
            if kind not in ("float", "int", "uint", "vec4", "mat4"):
                sys.exit("make-chains: a member of type %s is not handled" % kind)
            out.append((member_offsets[(struct_id, index)], kind, member_names.get((struct_id, index), "")))
        size = max((o + (64 if k == "mat4" else 16 if k == "vec4" else 4) for o, k, _ in out), default=0)
        return out, size

    for var, pointer, storage in variables:
        target = types[pointer][3]
        if storage == 9:
            push = members(target)
        elif storage == 2 and types[target][0] == "struct":
            ubo = (decorations.get(var, {}).get(33, 0),) + members(target)
        elif storage == 0 and types[target][0] == "texture":
            samplers.append((decorations.get(var, {}).get(33, 0), names.get(var, "")))
    return push, ubo, samplers


def string(words):
    data = b"".join(struct.pack("<I", w) for w in words)
    return data.split(b"\0", 1)[0].decode("utf-8")


def compile_stage(source, stage, out_path, work):
    src = os.path.join(work, "pass." + ("vert" if stage == "vertex" else "frag"))
    with open(src, "w") as f:
        f.write(source)
    result = subprocess.run(["glslangValidator", "-V", "--quiet", "--target-env", "vulkan1.0", "-o", out_path, src],
                            capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("make-chains: %s did not compile:\n%s%s" % (out_path, result.stdout, result.stderr))
    with open(out_path, "rb") as f:
        data = f.read()
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


FORMATS = {"R8G8B8A8_UNORM", "R8G8B8A8_SRGB", "R16G16B16A16_SFLOAT", "R32G32B32A32_SFLOAT", "A2B10G10R10_UNORM_PACK32",
           "R16G16_SFLOAT", "R32_SFLOAT", "R8_UNORM", "R16_SFLOAT", "R32G32_SFLOAT"}


def make_chain(name, preset_path, out_root):
    preset = read_preset(preset_path)
    base = os.path.dirname(preset_path)
    out = os.path.join(out_root, name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    lines = ["chain " + name]
    params = {}
    pass_lines = []
    with tempfile.TemporaryDirectory() as work:
        for n in range(int(preset["shaders"])):
            path = os.path.normpath(os.path.join(base, preset["shader%d" % n]))
            sources, pass_params, declared, fmt = preprocess(path)
            for p in pass_params:
                params.setdefault(p[0], p)
            reflected = {}
            for stage in ("vertex", "fragment"):
                spv = "p%d.%s.spv" % (n, "vert" if stage == "vertex" else "frag")
                reflected[stage] = reflect(compile_stage(sources[stage], stage, os.path.join(out, spv), work))
            if preset.get("float_framebuffer%d" % n) == "true":
                fmt = "R16G16B16A16_SFLOAT"
            elif preset.get("srgb_framebuffer%d" % n) == "true":
                fmt = "R8G8B8A8_SRGB"
            fmt = fmt or "R8G8B8A8_UNORM"
            if fmt not in FORMATS:
                sys.exit("make-chains: pass %d asks for %s" % (n, fmt))
            last = n == int(preset["shaders"]) - 1
            scale = {}
            for axis in ("x", "y"):
                kind = preset.get("scale_type_%s%d" % (axis, n), preset.get("scale_type%d" % n))
                value = preset.get("scale_%s%d" % (axis, n), preset.get("scale%d" % n))
                if kind is None:
                    kind, value = ("viewport", "1.0") if last else ("source", "1.0")
                scale[axis] = (kind, float(value or 1.0))
            alias = preset.get("alias%d" % n) or declared or "-"
            pass_lines.append("pass %d p%d.vert.spv p%d.frag.spv %s %d %s %d %d %s %s %g %s %g" % (
                n, n, n, fmt, 1 if preset.get("filter_linear%d" % n) == "true" else 0,
                preset.get("wrap_mode%d" % n, "clamp_to_border"), 1 if preset.get("mipmap_input%d" % n) == "true" else 0,
                int(preset.get("frame_count_mod%d" % n, "0")), alias, scale["x"][0], scale["x"][1], scale["y"][0], scale["y"][1]))
            # The two stages together: a member either reads is set for both.
            push_members, push_size, ubo_members, ubo_binding, ubo_size, samplers = {}, 0, {}, None, 0, {}
            for stage in ("vertex", "fragment"):
                push, ubo, stage_samplers = reflected[stage]
                if push:
                    push_size = max(push_size, push[1])
                    for offset, kind, member in push[0]:
                        push_members[member] = (offset, kind)
                if ubo:
                    ubo_binding = ubo[0]
                    ubo_size = max(ubo_size, ubo[2])
                    for offset, kind, member in ubo[1]:
                        ubo_members[member] = (offset, kind)
                for binding, sampler in stage_samplers:
                    samplers[binding] = sampler
            if push_size:
                pass_lines.append("push %d" % push_size)
            if ubo_binding is not None:
                pass_lines.append("ubo %d %d" % (ubo_binding, ubo_size))
            for where, table in (("push", push_members), ("ubo", ubo_members)):
                for member, (offset, kind) in sorted(table.items(), key=lambda kv: kv[1][0]):
                    pass_lines.append("member %s %d %s %s" % (where, offset, kind, member))
            for binding, sampler in sorted(samplers.items()):
                pass_lines.append("sampler %d %s" % (binding, sampler))
    overrides = {}
    for key in preset.get("parameters", "").split(";"):
        key = key.strip()
        if key and key in preset:
            overrides[key] = float(preset[key])
    for pname, default, low, high in params.values():
        lines.append("param %s %g %g %g" % (pname, overrides.get(pname, default), low, high))
    for i, lut in enumerate(t.strip() for t in preset.get("textures", "").split(";") if t.strip()):
        source = os.path.normpath(os.path.join(base, preset[lut]))
        file = "lut%d%s" % (i, os.path.splitext(source)[1])
        shutil.copyfile(source, os.path.join(out, file))
        lines.append("lut %s %s %d %d %s" % (lut, file, 1 if preset.get(lut + "_linear") == "true" else 0,
                                             1 if preset.get(lut + "_mipmap") == "true" else 0,
                                             preset.get(lut + "_wrap_mode", "clamp_to_border")))
    lines.extend(pass_lines)
    with open(os.path.join(out, "chain.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d passes, %d parameters" % (name, int(preset["shaders"]), len(params)))


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    shaders, out = sys.argv[1], sys.argv[2]
    chains = dict(a.split("=", 1) for a in sys.argv[3:]) or DEFAULT_CHAINS
    os.makedirs(out, exist_ok=True)
    for name, preset in chains.items():
        make_chain(name, os.path.join(shaders, preset), out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
