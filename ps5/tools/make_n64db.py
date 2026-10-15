#!/usr/bin/env python3
# Mupen64Plus PS5: builds frontend's game-name table (CRC1 <tab> name) from mupen64plus-core's ROM catalog.
#
# N64 cartridges carry two checksums in their header (CRC1/CRC2 at 0x10/0x14), and mupen64plus.ini lists
# them with each ROM's GoodN64 name ("Super Mario 64 (U) [!]"). The shelf names games and finds their
# covers (libretro-thumbnails, No-Intro names) from this table, so the GoodN64 names are turned into
# No-Intro style ones: dump tags ([!] [b1] [t1] ...) dropped, country codes spelled out ((U) -> (USA)),
# (V1.1) -> (Rev 1). It is not a full No-Intro table; files already named the No-Intro way are used as is.
#
# GoodN64 names have no language tags and spell some titles differently, so a converted name often isn't
# libretro-thumbnails' file name ("Diddy Kong Racing (USA) (Rev 1)" is "... (USA) (En,Fr) (Rev 1)" there,
# and every such cover download was a 404). With a list of those file names (tools/libretro-n64-boxarts.txt)
# each name is matched against it, ignoring language tags, case and punctuation, and takes its spelling.
#
#   make_n64db.py mupen64plus.ini out.tsv [boxarts.txt]
#   make_n64db.py --fetch-boxarts boxarts.txt      refresh the list (GitHub's file list of the repository)
#
# SPDX-License-Identifier: MIT
import re
import sys

COUNTRIES = {
    "U": "USA", "E": "Europe", "J": "Japan", "JU": "Japan, USA", "UE": "USA, Europe", "JUE": "World",
    "W": "World", "F": "France", "G": "Germany", "I": "Italy", "S": "Spain", "A": "Australia",
    "B": "Brazil", "K": "Korea", "C": "China", "Ch": "China", "N": "Netherlands", "Sw": "Sweden",
    "FC": "Canada", "GR": "Greece", "HK": "Hong Kong", "D": "Netherlands", "PD": "PD",
}


def convert(good):
    name = re.sub(r"\s*\[[^\]]*\]", "", good)  # [!] [b1] [t1] [T+Spa] ...
    name = re.sub(r"\s*\(M\d+\)", "", name)  # (M3): number of languages
    name = re.sub(r"\s*\((?:PAL|NTSC|PAL-NTSC|V64-PAL)[^)]*\)", "", name)

    def country(m):
        code = m.group(1)
        return " (%s)" % COUNTRIES[code] if code in COUNTRIES else m.group(0)

    name = re.sub(r" \(([A-Za-z]{1,3})\)", country, name)
    name = re.sub(r" \(V1\.0\)", "", name)
    name = re.sub(r" \(V1\.(\d)\)", lambda m: " (Rev %s)" % m.group(1), name)
    return re.sub(r"\s+", " ", name).strip()


# ---- libretro-thumbnails' names ------------------------------------------------------------------------------
BOXARTS_API = "https://api.github.com/repos/libretro-thumbnails/Nintendo_-_Nintendo_64/git/trees/master?recursive=1"
# characters a thumbnail's file name has as "_" (fe_covers.cpp ThumbnailName)
UNSAFE = '&*/:`<>?\\|"'
LANG = re.compile(r"\s*\((?:[A-Z][a-z](?:-[A-Z][a-z]+)?)(?:,[A-Z][a-z](?:-[A-Z][a-z]+)?)*\)")  # (En,Fr) (Fr,De,Nl)
# GoodN64's own language tags: (Ita-Spa) (Eng-Spa-Fre-Ger)
GOOD_LANG = re.compile(r"\s*\((?:Eng|Fre|Ger|Spa|Ita|Dut|Swe|Jap|Por|Dan|Nor|Fin)(?:-(?:Eng|Fre|Ger|Spa|Ita|Dut|Swe|Jap|Por|Dan|Nor|Fin))*\)")


def fetch_boxarts(dst):
    import datetime
    import json
    import urllib.request

    req = urllib.request.Request(BOXARTS_API, headers={"User-Agent": "mupen64plus-ps5 make_n64db.py"})
    tree = json.load(urllib.request.urlopen(req, timeout=60))
    if tree.get("truncated"):
        sys.exit("GitHub's file list came back truncated")
    names = sorted(e["path"][len("Named_Boxarts/"):-4] for e in tree["tree"]
                   if e["path"].startswith("Named_Boxarts/") and e["path"].endswith(".png"))
    with open(dst, "w", encoding="utf-8", newline="\n") as out:
        out.write("# Named_Boxarts of github.com/libretro-thumbnails/Nintendo_-_Nintendo_64 (master, %s), for "
                  "tools/make_n64db.py\n" % datetime.date.today().isoformat())
        out.write("".join(n + "\n" for n in names))
    print("%s: %d box art names" % (dst, len(names)))


def thumb(name):
    return "".join("_" if c in UNSAFE else c for c in name)


def loose(name):
    # the same game whatever the language tags, case, punctuation or a (Z64) dump tag
    n = LANG.sub("", GOOD_LANG.sub("", thumb(name)))
    n = re.sub(r"\s*\((?:Z64|V64)\)", "", n)
    n = n.lower().replace("_", " ")
    n = re.sub(r"[^a-z0-9()]+", " ", n)
    return re.sub(r"\s+", " ", n).strip()


class Boxarts:
    def __init__(self, path):
        names = [l.rstrip("\n") for l in open(path, encoding="utf-8") if l.strip() and not l.startswith("#")]
        self.exact = set(names)
        self.by_key = {}
        for n in sorted(names, key=len):  # the plainest variant first
            self.by_key.setdefault(loose(n), n)

    def match(self, name):
        if thumb(name) in self.exact:
            return name
        found = self.by_key.get(loose(name))
        if not found:
            return name
        # keep our readable characters where the file name has "_" ("Command & Conquer")
        ours = [c for c in name if c in UNSAFE]
        if found.count("_") == len(ours):
            it = iter(ours)
            found = "".join(next(it) if c == "_" else c for c in found)
        return found


def rank(good):
    # verified dumps first, then plain names, then bad/overdumps/hacks/trainers/translations
    if "[!]" in good:
        return 0
    if "[" not in good:
        return 1
    return 2


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--fetch-boxarts":
        fetch_boxarts(sys.argv[2])
        return
    src, dst = sys.argv[1], sys.argv[2]
    boxarts = Boxarts(sys.argv[3]) if len(sys.argv) > 3 else None
    best = {}
    good = crc = None
    with open(src, encoding="latin-1") as f:
        for line in f:
            line = line.strip()
            if line.startswith("["):
                good = crc = None
            elif line.startswith("GoodName="):
                good = line[9:]
            elif line.startswith("CRC="):
                crc = line[4:].split()[0].upper()
            if good and crc and re.fullmatch(r"[0-9A-F]{8}", crc):
                r = rank(good)
                if crc not in best or r < best[crc][0]:
                    best[crc] = (r, convert(good))
                good = crc = None
    renamed = 0
    if boxarts:
        for crc, (r, name) in best.items():
            m = boxarts.match(name)
            if m != name:
                best[crc] = (r, m)
                renamed += 1
    with open(dst, "w", encoding="utf-8", newline="\n") as out:
        out.write("# CRC1\tname (generated by tools/make_n64db.py from mupen64plus.ini)\n")
        for crc in sorted(best):
            out.write("%s\t%s\n" % (crc, best[crc][1]))
    print("%s: %d games, %d named as in libretro-thumbnails" % (dst, len(best), renamed))


if __name__ == "__main__":
    main()
