#!/usr/bin/env python3
"""Build the DiskMap fixture tree: known sizes, a junction (Windows only), a symlink that
points outside the tree, a long path and a hard link. Writes expected.json beside the tree.

Usage: python3 -I make_diskmap_tree.py <empty-target-dir>
The target must not exist or must be empty: the script never deletes or overwrites anything.
"""
import json, os, sys

# (relative path, size in bytes). Contents are deterministic so goldens are stable.
FILES = [
    ("Games/SteamLibrary/steamapps/common/BigGame/data.pak", 5_000_000),
    ("Games/SteamLibrary/steamapps/common/BigGame/game.exe", 400_000),
    ("Games/SteamLibrary/steamapps/common/SmallGame/level1.bin", 1_200_000),
    ("Videos/Captures/clip1.mp4", 3_000_000),
    ("Videos/Captures/clip2.mkv", 2_000_000),
    ("Music/album/track01.flac", 900_000),
    ("Pictures/holiday/img001.jpg", 250_000),
    ("Documents/report.pdf", 120_000),
    ("Documents/notes.txt", 4_096),
    ("dev/project/src/main.cpp", 8_192),
    ("dev/project/node_modules/pkg/index.js", 16_384),
    ("Temp/cache/blob.tmp", 700_000),
    ("Windows/Temp/setup.log", 60_000),
    ("CrashDumps/app.dmp", 1_500_000),
    ("Archives/backup.zip", 2_500_000),
]
LONG_DIR = "/".join(["deep_folder_name_%02d" % i for i in range(14)])   # > 260 chars
LONG_FILE = (LONG_DIR + "/long_path_file.dat", 333_333)
HARDLINK_SRC, HARDLINK_DST = "Documents/report.pdf", "Documents/report-hardlink.pdf"
OUTSIDE_NAME = "outside_target"       # created as a sibling of the tree, never inside it
SYMLINK = "Links/outside-symlink"     # -> ../../outside_target (outside the tree)
JUNCTION = "Links/outside-junction"   # Windows only, -> outside_target


def write(path, size):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        sys.exit("refusing to overwrite " + path)
    block = bytes((i * 31 + 7) & 0xFF for i in range(65536))
    with open(path, "wb") as f:
        left = size
        while left:
            n = min(left, len(block))
            f.write(block[:n]); left -= n


def ext(p):
    # \\?\ form so Windows Python can create the >260-char path without LongPathsEnabled
    if os.name == "nt":
        return "\\\\?\\" + os.path.abspath(p)
    return p


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = os.path.abspath(sys.argv[1])
    tree = os.path.join(root, "tree")
    outside = os.path.join(root, OUTSIDE_NAME)
    if os.path.exists(root) and os.listdir(root):
        sys.exit("target is not empty: " + root)
    os.makedirs(tree); os.makedirs(outside)
    # Sentinel outside the tree: a scanner that follows links will count it. 10 MB so it shows.
    write(os.path.join(outside, "SENTINEL_must_not_be_counted.bin"), 10_000_000)

    for rel, size in FILES + [LONG_FILE]:
        write(ext(os.path.join(tree, rel)), size)

    os.link(os.path.join(tree, HARDLINK_SRC), os.path.join(tree, HARDLINK_DST))
    os.makedirs(os.path.join(tree, "Links"))
    os.symlink(os.path.relpath(outside, os.path.join(tree, "Links")), os.path.join(tree, SYMLINK),
               target_is_directory=True)
    made_junction = False
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(outside, os.path.join(tree, JUNCTION))
        made_junction = True

    unique = sum(s for _, s in FILES) + LONG_FILE[1]
    hl_size = dict(FILES)[HARDLINK_SRC]
    expected = {
        "tree": "tree",
        "file_count_per_link": len(FILES) + 2,          # every directory entry, hard link included
        "bytes_per_link": unique + hl_size,            # legacy DiskMap counts each link
        "bytes_unique_fileid": unique,                 # counting each FileId once
        "hard_link": [HARDLINK_SRC, HARDLINK_DST],
        "symlink_outside": SYMLINK,
        "junction_outside": JUNCTION if made_junction else None,
        "sentinel_bytes": 10_000_000,
        "long_path": LONG_FILE[0],
        "long_path_chars": len(os.path.join(tree, LONG_FILE[0])),
        "note": "A correct scan never includes sentinel_bytes.",
    }
    with open(os.path.join(root, "expected.json"), "w") as f:
        json.dump(expected, f, indent=2)
    print(json.dumps(expected, indent=2))


if __name__ == "__main__":
    main()
