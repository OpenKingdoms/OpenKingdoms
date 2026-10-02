#!/usr/bin/env python3
"""Writes web/mods/ok-registry-test-1.0.zip, the registry's test mod.

Everything in it is written here by hand and none of it comes from the
game: one feature that no map places, so the mod changes the data
fingerprint and nothing a player sees. It exists so the registry's
install and join flow can be tried end to end. The zip is built the
same way every time, so its sha256 in web/mods/registry.json holds.

After a change, put the new size and sha256 in the registry, and the
fingerprint `tak-re --mods ok-registry-test --data-report` prints.
"""
import hashlib
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'web', 'mods', 'ok-registry-test-1.0.zip')
STAMP = (2026, 10, 1, 0, 0, 0)

FEATURE = """[OKRegistryMarker]
\t{
\tworld=all worlds;
\tdescription=OpenKingdoms mod registry test marker;
\tcategory=okregistrytest;
\tfootprintx=1;
\tfootprintz=1;
\theight=12;
\tblocking=0;
\tindestructible=1;
\treclaimable=0;
\t}
"""

README = """OK Registry Test 1.0

A test mod for the OpenKingdoms mod registry. It adds one feature that
no map places, so it changes the game data fingerprint and nothing else.
Installing it from the registry puts the Mods folder below into your game
folder. This file is not installed.
"""

FILES = [
    ('Mods/OK Registry Test/features/zz-ok-registry-test/okmarker.tdf', FEATURE),
    ('README.txt', README),
]


def main():
    with zipfile.ZipFile(OUT, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, text in FILES:
            info = zipfile.ZipInfo(name, STAMP)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            info.create_system = 0
            z.writestr(info, text.replace('\r\n', '\n').encode('utf-8'), compresslevel=9)
    data = open(OUT, 'rb').read()
    print('%s\nsize %d\nsha256 %s' % (os.path.normpath(OUT), len(data), hashlib.sha256(data).hexdigest()))


if __name__ == '__main__':
    main()
