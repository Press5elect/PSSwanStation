#!/usr/bin/env python3
# spvubo.py <file.spv>: member names and offsets of the uniform blocks
#
# SPDX-License-Identifier: GPL-3.0-or-later
import struct, sys
OP_NAME, OP_MEMBERNAME, OP_MEMBERDECORATE, OP_DECORATE, OP_VARIABLE, OP_TYPEPOINTER, OP_TYPESTRUCT = 5, 6, 72, 71, 59, 32, 30
for p in sys.argv[1:]:
    w = open(p,'rb').read(); words = struct.unpack('<%dI' % (len(w)//4), w)
    i = 5; mnames = {}; moff = {}; snames = {}; ptrs = {}; vars_ = []; deco = {}
    while i < len(words):
        op = words[i] & 0xffff; n = words[i] >> 16; a = words[i+1:i+n]
        if op == OP_MEMBERNAME:
            mnames[(a[0], a[1])] = struct.pack('<%dI' % (n-3), *a[2:]).split(b'\0')[0].decode()
        elif op == OP_NAME:
            snames[a[0]] = struct.pack('<%dI' % (n-2), *a[1:]).split(b'\0')[0].decode()
        elif op == OP_MEMBERDECORATE and a[2] == 35:
            moff[(a[0], a[1])] = a[3]
        elif op == OP_TYPEPOINTER:
            ptrs[a[0]] = (a[1], a[2])
        elif op == OP_VARIABLE:
            vars_.append((a[0], a[1], a[2]))
        elif op == OP_DECORATE:
            deco.setdefault(a[0], {})[a[1]] = a[2] if len(a) > 2 else 1
        i += n
    print(p.split('/')[-1])
    for t, vid, sc in vars_:
        if sc != 2: continue
        st = ptrs[t][1]
        print('  block %s binding %s' % (snames.get(st, '?'), deco.get(vid, {}).get(33)))
        for (s, m), off in sorted(moff.items(), key=lambda x: (x[0][0], x[1])):
            if s == st:
                print('    %4d %s' % (off, mnames.get((s, m), '?')))
