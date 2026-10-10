#!/usr/bin/env python3
# spvrefl.py <file.spv>...: the descriptor bindings (set, binding, kind, name), push constants and local size of compute SPIR-V modules.
#
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys

OP_NAME, OP_MEMBERNAME, OP_DECORATE, OP_VARIABLE, OP_TYPEPOINTER = 5, 6, 71, 59, 32
OP_TYPEIMAGE, OP_TYPESAMPLER, OP_TYPESAMPLEDIMAGE, OP_TYPESTRUCT, OP_TYPERUNTIMEARRAY, OP_TYPEARRAY, OP_EXECMODE = 25, 26, 27, 30, 29, 28, 16
OP_TYPEINT, OP_TYPEFLOAT, OP_TYPEVECTOR, OP_CONSTANT = 21, 22, 23, 43
DEC_BINDING, DEC_SET, DEC_BLOCK, DEC_BUFFERBLOCK = 33, 34, 2, 3
SC_UNIFORMCONST, SC_UNIFORM, SC_STORAGEBUFFER, SC_PUSHCONST = 0, 2, 12, 9

def parse(path):
    w = open(path, 'rb').read()
    words = struct.unpack('<%dI' % (len(w) // 4), w)
    assert words[0] == 0x07230203
    i = 5
    names, deco, types, vars_, local = {}, {}, {}, [], None
    while i < len(words):
        op = words[i] & 0xffff; n = words[i] >> 16; a = words[i+1:i+n]
        if op == OP_NAME:
            s = struct.pack('<%dI' % (n - 2), *a[1:]).split(b'\0')[0].decode()
            names[a[0]] = s
        elif op == OP_DECORATE:
            deco.setdefault(a[0], {})[a[1]] = a[2] if len(a) > 2 else True
        elif op in (OP_TYPEIMAGE, OP_TYPESAMPLER, OP_TYPESAMPLEDIMAGE, OP_TYPESTRUCT, OP_TYPERUNTIMEARRAY, OP_TYPEARRAY, OP_TYPEPOINTER, OP_TYPEINT, OP_TYPEFLOAT, OP_TYPEVECTOR):
            types[a[0]] = (op, a[1:])
        elif op == OP_VARIABLE:
            vars_.append((a[1], a[0], a[2]))
        elif op == OP_EXECMODE and a[1] == 17:
            local = tuple(a[2:5])
        i += n
    out = []
    for vid, ptype, sc in vars_:
        if sc not in (SC_UNIFORMCONST, SC_UNIFORM, SC_STORAGEBUFFER, SC_PUSHCONST):
            continue
        t = types[ptype][1][1]
        op, args = types[t]
        while op in (OP_TYPEARRAY, OP_TYPERUNTIMEARRAY):
            t = args[0]; op, args = types[t]
        d = deco.get(vid, {})
        if op == OP_TYPEIMAGE:
            sampled = args[5]
            kind = 'storage_image' if sampled == 2 else 'sampled_image'
            fmt = args[6]
        elif op == OP_TYPESAMPLER:
            kind, fmt = 'sampler', 0
        elif op == OP_TYPESAMPLEDIMAGE:
            kind, fmt = 'combined', 0
        elif op == OP_TYPESTRUCT:
            td = deco.get(t, {})
            if sc == SC_PUSHCONST: kind = 'push_constant'
            elif sc == SC_STORAGEBUFFER or DEC_BUFFERBLOCK in td: kind = 'storage_buffer'
            else: kind = 'uniform_buffer'
            fmt = 0
        else:
            kind, fmt = 'other%d' % op, 0
        out.append((d.get(DEC_SET, 0), d.get(DEC_BINDING, -1), kind, names.get(vid, '?'), fmt))
    return sorted(out), local

if __name__ == '__main__':
  for p in sys.argv[1:]:
    b, local = parse(p)
    print('%s local=%s' % (p.split('/')[-1], local))
    for s, bi, k, nm, fmt in b:
        print('   set %d binding %3d %-15s %s%s' % (s, bi, k, nm, (' fmt%d' % fmt) if fmt else ''))
