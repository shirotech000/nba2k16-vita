#!/usr/bin/env python3
"""pulls the two libs the port needs out of YOUR nba 2k16 apk.

usage: python tools/extract_apk.py path/to/nba2k16.apk [out_dir]

copy everything in out_dir to ux0:data/nba2k16/ on the vita afterwards.
the hashes are from the v0.0.29 copy this port was developed against. a
different dump of the same version could differ if that apk was modified.
on a mismatch it still extracts, but game-specific patches check their
bytes first and just get skipped if they don't match.
"""
import hashlib
import os
import sys
import zipfile

WANTED = {
    "lib/armeabi-v7a/libnba2k16_clean_opt.so":
        "2b7fc7de9e04243a148ca842857d689567cd1a4382e8defae7b79c61a186bccf",
    "lib/armeabi-v7a/libgnustl_shared.so":
        "77b7c1db65a68ec4659b9e918fff729413041ac53bb257bf679f9c78ec88b505",
}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    apk = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "nba2k16_vita_files"
    os.makedirs(out, exist_ok=True)

    ok = True
    with zipfile.ZipFile(apk) as z:
        names = set(z.namelist())
        for path, want in WANTED.items():
            if path not in names:
                print(f"missing {path} in the apk, is this the right game?")
                ok = False
                continue
            data = z.read(path)
            got = hashlib.sha256(data).hexdigest()
            name = os.path.basename(path)
            with open(os.path.join(out, name), "wb") as f:
                f.write(data)
            tag = "ok" if got == want else "HASH MISMATCH (not v0.0.29?)"
            print(f"{name}: {len(data)} bytes, {tag}")
            ok &= got == want

    print(f"\ndone -> {out}")
    print("copy those to ux0:data/nba2k16/ and the obb to ux0:data/nba2k16/obb/")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
