#!/usr/bin/env python3
"""Writes web/atlas/remaster.json, the list of Stars of Darien models the
Asset Atlas shows beside the originals.

Run it by hand from a clean Stars of Darien checkout whose HEAD is on
GitHub, when the remaster has new models:

    python3 scripts/atlas-remaster-index.py ~/Projects/Stars-of-Darien

The page fetches each model from that commit on raw.githubusercontent.com,
which answers with an open CORS header. The list holds names and numbers
read from each file, never a picture or a mesh.
"""
import json
import os
import struct
import subprocess
import sys
import urllib.parse

REPO = "OpenKingdoms/Stars-of-Darien"
FOLDERS = (("unity/Assets/Overrides/Units", "unit"), ("unity/Assets/Overrides/Features", "feature"))
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web", "atlas", "remaster.json")


def git(root, *args):
    return subprocess.run(["git", "-C", root] + list(args), check=True,
                          capture_output=True, text=True).stdout.strip()


def glb_json(path):
    with open(path, "rb") as f:
        head = f.read(20)
        if len(head) < 20 or head[:4] != b"glTF":
            raise ValueError("not a glTF binary")
        length, kind = struct.unpack_from("<I4s", head, 12)
        if kind != b"JSON":
            raise ValueError("no JSON chunk first")
        return json.loads(f.read(length))


def triangles(j):
    n = 0
    accessors = j.get("accessors", [])
    for mesh in j.get("meshes", []):
        for prim in mesh.get("primitives", []):
            if prim.get("mode", 4) != 4:
                continue
            at = prim.get("indices")
            if at is None:
                at = prim.get("attributes", {}).get("POSITION")
            if at is not None and at < len(accessors):
                n += accessors[at].get("count", 0) // 3
    return n


def painted(j):
    return sum(1 for m in j.get("materials", []) if "okPaint" in (m.get("extras") or {}))


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    root = os.path.abspath(sys.argv[1])
    commit = git(root, "rev-parse", "HEAD")
    if git(root, "status", "--porcelain", "--", "unity/Assets/Overrides"):
        print("warning: the checkout has changes under unity/Assets/Overrides that GitHub does not",
              file=sys.stderr)
    models = []
    for folder, kind in FOLDERS:
        tracked = set(git(root, "ls-files", "--", folder).splitlines())
        for name in sorted(os.listdir(os.path.join(root, folder)), key=str.lower):
            rel = folder + "/" + name
            if not name.lower().endswith(".glb") or rel not in tracked:
                continue
            full = os.path.join(root, rel)
            j = glb_json(full)
            entry = {"n": name[:-4], "p": rel, "k": kind, "b": os.path.getsize(full),
                     "t": triangles(j), "i": len(j.get("images", []))}
            if painted(j):
                entry["pt"] = painted(j)
            side = full[:-4] + ".json"
            if kind == "unit" and os.path.exists(side) and rel[:-4] + ".json" in tracked:
                card = json.load(open(side))
                if card.get("replacesPiece"):
                    entry["k"] = "card"
                    entry["rp"] = card["replacesPiece"]
                    entry["rt"] = card.get("replacesTexture", "")
            models.append(entry)
    out = {
        "okAtlasRemaster": 1,
        "repo": REPO,
        "commit": commit,
        "base": "https://raw.githubusercontent.com/" + REPO + "/" + commit + "/",
        "models": models,
    }
    for m in models:
        if urllib.parse.quote(m["p"]) != m["p"]:
            print("warning: %s needs escaping in a URL" % m["p"], file=sys.stderr)
    with open(OUT, "w", newline="\n") as f:
        json.dump(out, f, separators=(",", ":"))
        f.write("\n")
    print("%d models at %s into %s" % (len(models), commit[:12], os.path.relpath(OUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
