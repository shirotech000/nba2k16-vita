#!/usr/bin/env python3
# tools/shaderpack.py -- build shadercache/pack.bin from a shadercache dir.
#
# the Vita port reads its shader cache (.gxp compiled programs + .g2c
# translated Cg sources) out of ONE file, shadercache/pack.bin, opened once
# at boot and read with sceIoPread (see the pack support in
# src/gles/shader.c). loose files in the directory are still written/used by
# the port, the pack is a read-through layer on top: every entry found in
# the pack saves a slow per-file sceIoOpen on the vita's card.
#
# usage:
#   python tools/shaderpack.py <shadercache-dir>
#       packs *.gxp / *.g2c from that dir (skips junk like *.compiling /
#       *.bad / *.cg markers) into <dir>/pack.bin
#
# pack layout (matches the C side, all fields little endian):
#   header: u32 magic 'NPK1' (0x314B504E), u32 version (1), u32 count,
#           u32 index_size
#   index:  count entries of { u32 name_len, char name[name_len],
#                               u64 data_offset, u32 data_size },
#           strcmp-sorted by name
#   data:   raw contents of the original cache files, data_offset is an
#           absolute byte offset into pack.bin
# names are the cache file names without the directory, e.g.
# "f0123abcd56789ef.gxp" or "t0123abcd56789ef.g2c".

import os
import struct
import sys

PACK_MAGIC = 0x314B504E  # "NPK1"
PACK_VERSION = 1
ENTRY_TYPES = ('.gxp', '.g2c')
NAME_MAX = 80  # must match PACK_NAME_MAX in shader.c


def collect(shadercache_dir):
    entries = []
    for name in os.listdir(shadercache_dir):
        if not name.endswith(ENTRY_TYPES):
            continue
        if len(name) > NAME_MAX:
            print(f'  skip (name too long): {name}')
            continue
        path = os.path.join(shadercache_dir, name)
        if not os.path.isfile(path):
            continue
        data = open(path, 'rb').read()
        if not data:
            print(f'  skip (empty): {name}')
            continue
        entries.append((name, data))
    return entries


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    d = sys.argv[1]
    if not os.path.isdir(d):
        print(f'{d}: not a directory')
        return 1

    entries = collect(d)
    if not entries:
        print(f'no .gxp/.g2c files in {d}, nothing to pack')
        return 1

    entries.sort(key=lambda e: e[0].encode())
    # no duplicate names: they come from one flat directory, but be safe
    names = [n for n, _ in entries]
    dupes = {n for n in names if names.count(n) > 1}
    if dupes:
        print(f'duplicate names in the dir, aborting: {sorted(dupes)[:4]}')
        return 1

    index_size = sum(16 + len(n.encode()) for n, _ in entries)
    out_path = os.path.join(d, 'pack.bin')
    total = 16 + index_size + sum(len(b) for _, b in entries)
    with open(out_path, 'wb') as out:
        out.write(struct.pack('<IIII', PACK_MAGIC, PACK_VERSION,
                              len(entries), index_size))
        off = 16 + index_size
        for name, data in entries:
            nb = name.encode()
            # fixed part of each entry: u32 name_len, u64 offset, u32 size
            out.write(struct.pack('<IQI', len(nb), off, len(data)))
            out.write(nb)
            off += len(data)
        for _, data in entries:
            out.write(data)

    print(f'{out_path}: {len(entries)} entries, {total / (1024 * 1024):.1f} MiB '
          f'({16 + index_size} B index)')
    print('push it to ux0:data/nba2k16/shadercache/ and delete the strip of '
          'loose .gxp/.g2c files ONLY if you also have the pack (the port '
          'reads pack first, loose as fallback, so leaving them also works):')
    return 0


if __name__ == '__main__':
    sys.exit(main())
