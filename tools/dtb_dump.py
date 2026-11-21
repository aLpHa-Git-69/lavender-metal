#!/usr/bin/env python3
"""Minimal flattened-device-tree reader (no dtc needed).

  dtb_dump.py file.dtb NODE_SUBSTRING [--depth N]   print every node whose path contains the substring
  dtb_dump.py file.dtb --phandle 0x1a2              print the node that has that phandle

Values are shown as strings when printable, otherwise as 32-bit cells.
"""
import struct
import sys

FDT_BEGIN, FDT_END, FDT_PROP, FDT_NOP, FDT_END_FDT = 1, 2, 3, 4, 9


def parse(d):
    magic, total, off_struct, off_str, _, ver, _, _, sz_str, sz_struct = struct.unpack('>10I', d[:40])
    assert magic == 0xD00DFEED, 'not a dtb (is it a concatenated multi-dtb? split it first)'
    strs = d[off_str:off_str + sz_str]
    p = off_struct
    path = []
    nodes = {}
    cur = None
    while True:
        tag, = struct.unpack('>I', d[p:p + 4])
        p += 4
        if tag == FDT_BEGIN:
            end = d.index(b'\0', p)
            name = d[p:end].decode()
            p = (end + 4) & ~3
            path.append(name)
            full = '/'.join(path) or '/'
            cur = nodes.setdefault(full, {})
        elif tag == FDT_PROP:
            ln, nameoff = struct.unpack('>II', d[p:p + 8])
            p += 8
            val = d[p:p + ln]
            p = (p + ln + 3) & ~3
            pname = strs[nameoff:strs.index(b'\0', nameoff)].decode()
            cur[pname] = val
        elif tag == FDT_END:
            path.pop()
            cur = nodes.get('/'.join(path) or '/')
        elif tag == FDT_NOP:
            continue
        elif tag == FDT_END_FDT:
            break
    return nodes


def show(v):
    if not v:
        return '<empty>'
    if v[-1] == 0 and all(32 <= b < 127 or b == 0 for b in v):
        return '"' + '", "'.join(s.decode() for s in v[:-1].split(b'\0')) + '"'
    if len(v) % 4 == 0:
        return '<' + ' '.join('0x%x' % c for c in struct.unpack('>%dI' % (len(v) // 4), v)) + '>'
    return v.hex()


def main():
    d = open(sys.argv[1], 'rb').read()
    nodes = parse(d)
    if sys.argv[2] == '--phandle':
        want = int(sys.argv[3], 0)
        for path, props in nodes.items():
            for k in ('phandle', 'linux,phandle'):
                if k in props and struct.unpack('>I', props[k])[0] == want:
                    print(path)
                    for pk, pv in props.items():
                        print('   %s = %s' % (pk, show(pv)))
        return
    pat = sys.argv[2]
    for path, props in nodes.items():
        if pat in path:
            print(path)
            for pk, pv in props.items():
                print('   %s = %s' % (pk, show(pv)))


if __name__ == '__main__':
    main()
