#!/usr/bin/env python3
"""Convert an Android sparse image (magic 0xED26FF3A) to a raw image.

  simg2raw.py in.img out.raw

Handles RAW, FILL and DONT_CARE chunks (CRC chunks are skipped). No external tools.
"""
import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE, CRC32 = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f, open(dst, 'wb') as o:
        hdr = f.read(28)
        magic, major, minor, fhsz, chsz, blksz, total_blks, total_chunks, _ = struct.unpack('<IHHHHIIII', hdr)
        if magic != SPARSE_MAGIC:
            sys.exit('not a sparse image')
        f.seek(fhsz)
        out_blocks = 0
        for _ in range(total_chunks):
            ctype, _, chunk_blks, total_sz = struct.unpack('<HHII', f.read(12))
            data_sz = total_sz - chsz
            if ctype == RAW:
                remaining = data_sz
                while remaining:
                    n = min(remaining, 1 << 24)
                    o.write(f.read(n))
                    remaining -= n
            elif ctype == FILL:
                fill = f.read(4)
                block = fill * (blksz // 4)
                for _ in range(chunk_blks):
                    o.write(block)
            elif ctype == DONT_CARE:
                o.seek(chunk_blks * blksz, 1)
            elif ctype == CRC32:
                f.read(data_sz)
            else:
                sys.exit('unknown chunk type 0x%x' % ctype)
            out_blocks += chunk_blks
        o.truncate(total_blks * blksz)
    print('blocks', out_blocks, 'of', total_blks, 'blksz', blksz)


if __name__ == '__main__':
    main()
