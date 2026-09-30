#!/usr/bin/env python3
"""Packs a directory into a zip that keeps what a Linux program needs.

    make-zip.py SOURCE_DIR OUTPUT.zip

SOURCE_DIR becomes the single top-level folder of the archive. The archive is for tools that
unpack a zip on another machine (FrameDrop, for one), so it carries Unix permissions explicitly
(the program and the installer are executable, everything else is not) and nothing about who
built it: no owner, no group. Python's zipfile is used because the `zip` command is not on every
machine that builds a release.
"""
import os
import stat
import sys
import zipfile

EXECUTABLE_NAMES = {"frame-notify", "install.sh"}


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    source, output = os.path.abspath(argv[1]), argv[2]
    top = os.path.basename(source.rstrip(os.sep))
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for folder, folders, files in os.walk(source):
            folders.sort()
            relative_folder = os.path.relpath(folder, os.path.dirname(source)).replace(os.sep, "/")
            info = zipfile.ZipInfo(relative_folder + "/", date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (stat.S_IFDIR | 0o755) << 16 | 0x10
            info.create_system = 3   # Unix: only then are the permission bits read when unpacking
            archive.writestr(info, b"")
            for name in sorted(files):
                path = os.path.join(folder, name)
                info = zipfile.ZipInfo.from_file(path, relative_folder + "/" + name)
                info.compress_type = zipfile.ZIP_DEFLATED
                executable = name in EXECUTABLE_NAMES and os.path.dirname(os.path.relpath(path, source)) == ""
                info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
                info.create_system = 3
                with open(path, "rb") as stream:
                    archive.writestr(info, stream.read(), compresslevel=9)
    print(f"Packed {output} ({top}/)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
