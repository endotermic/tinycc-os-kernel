#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""firmware-select.py - copy the firmware files a kernel's built-in drivers request into a
staging tree (scripts/build.sh firmware).

Usage: firmware-select.py MODINFO LINUX_FIRMWARE REGDB DEST [--skip PREFIX]...

MODINFO is the kernel build's modules.builtin.modinfo: its "<driver>.firmware=<name>" entries
are the names the drivers may load.  Each one is looked up in the linux-firmware checkout
LINUX_FIRMWARE, by its file name or a "Link:" of WHENCE (the file the link names is copied
under the requested name, as linux-firmware's copy-firmware.sh installs it).  A name that is
not there:
  - a glob ("ath11k/WCN6855/hw2.0/*", "brcm/brcmfmac*-pcie.*.txt"): every match in that
    directory (not in its subdirectories);
  - a versioned name (iwlwifi-so-a0-gf-a0-89.ucode, iwlwifi-bz-a0-fm-c0-c107.ucode,
    rtw89/rtw8852c_fw-2.bin): the highest version at or below it that exists, which the driver
    tries next (it walks the API versions down from the one it names);
  - regulatory.db and regulatory.db.p7s: from the wireless-regdb tree REGDB.
An iwlwifi name also brings every sibling of its family (iwlwifi-bz-a0-fm-b0-c107.ucode:
all iwlwifi-bz-*, e.g. the bz-b0 parts that ship), each at the highest version not above
the requested one, with its .pnvm: the driver builds the name from the steppings it reads
from the card, and lists only one of them per family in MODULE_FIRMWARE.
A file that several names resolve to is stored once; the other names are relative symlinks.
Names starting with a --skip PREFIX are left out (chips of access points, not of PCs).
DEST/lib/firmware gets the files and the licences (WHENCE, LICENCE.*, LICENSE.*, GPL-*, the
regulatory database's LICENSE as LICENSE.wireless-regdb).  Prints a summary; the names found
nowhere go to DEST/../missing.txt, the skipped ones to DEST/../skipped.txt.
"""
import fnmatch
import os
import re
import shutil
import sys


def main(argv):
    if len(argv) < 5 or argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if len(argv) > 1 and argv[1] in ("-h", "--help") else 2
    modinfo, fwsrc, regdb, dest = argv[1:5]
    skips = []
    siblings = "iwlwifi-"
    rest = argv[5:]
    while rest:
        if rest[0] == "--skip" and len(rest) > 1:
            skips.append(rest[1])
            rest = rest[2:]
        else:
            print("firmware-select.py: unknown argument %r" % rest[0], file=sys.stderr)
            return 2

    with open(modinfo, "rb") as f:
        entries = f.read().split(b"\0")
    requested = sorted({e.split(b"=", 1)[1].decode() for e in entries
                        if b".firmware=" in e.split(b"=", 1)[0] + b"="})

    # the index: every file of the checkout, and every Link: of WHENCE whose target is there
    index = {}
    for root, dirs, files in os.walk(fwsrc):
        dirs[:] = [d for d in dirs if d != ".git"]
        for fn in files:
            rel = os.path.relpath(os.path.join(root, fn), fwsrc)
            index[rel] = rel
    with open(os.path.join(fwsrc, "WHENCE"), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^Link:\s*(.+?)\s*->\s*(.+?)\s*$", line)
            if not m:
                continue
            name, target = m.group(1), m.group(2)
            src = os.path.normpath(os.path.join(os.path.dirname(name), target))
            src = index.get(src)            # a link to a link is a link to its file
            if src and name not in index:
                index[name] = src
    for name in ("regulatory.db", "regulatory.db.p7s"):
        if os.path.isfile(os.path.join(regdb, name)):
            index[name] = os.path.abspath(os.path.join(regdb, name))   # join() keeps it absolute

    target_dir = os.path.join(dest, "lib", "firmware")
    os.makedirs(target_dir, exist_ok=True)
    taken, missing, skipped = {}, [], []

    first_of = {}                           # source -> the first name it was taken as

    def take(name, src):
        """name in the image, src its file; a source taken before becomes a relative
        symlink to its first copy (WHENCE's links: ath11k hw2.1 -> hw2.0, ...)"""
        if name in taken:
            return
        out = os.path.join(target_dir, name)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if src in first_of:
            first = os.path.join(target_dir, first_of[src])
            os.symlink(os.path.relpath(first, os.path.dirname(out)), out)
        else:
            shutil.copyfile(os.path.join(fwsrc, src), out)
            first_of[src] = name
        taken[name] = src

    def glob_re(pat):
        """fnmatch without '*' or '?' crossing a '/' (the files of that directory only)"""
        out = ""
        for c in pat:
            out += "[^/]*" if c == "*" else "[^/]" if c == "?" else re.escape(c)
        return re.compile(out + r"\Z")

    versioned = re.compile(r"^(.*-)(c?)(\d+)(\.[A-Za-z0-9_]+)$")

    def best_versions(pre, cflag, ver, ext, keys):
        """per base name, the highest version <= ver (same c-numbering) among keys"""
        best = {}
        for k in keys:
            m = versioned.match(k)
            if not m or m.group(2) != cflag or m.group(4) != ext or not m.group(1).startswith(pre):
                continue
            v = int(m.group(3))
            if v <= ver and (m.group(1) not in best or v > best[m.group(1)][0]):
                best[m.group(1)] = (v, k)
        return best

    for name in requested:
        if any(name.startswith(p) for p in skips):
            skipped.append(name)
            continue
        if any(c in name for c in "*?["):
            rx = glob_re(name)
            hits = sorted(k for k in index if rx.match(k))
            for k in hits:
                take(k, index[k])
            if not hits:
                missing.append(name)
            continue
        m = versioned.match(name)
        if m and name.startswith(siblings):
            # iwlwifi: the driver composes the name from the MAC and RF types and steppings it
            # finds, while its MODULE_FIRMWARE lists one stepping per family (bz-a0, not the
            # bz-b0 of the shipping BE201): every sibling of the family, newest usable version
            fam = name[:name.index("-", len(siblings)) + 1]
            found = best_versions(fam, m.group(2), int(m.group(3)), m.group(4), index)
            for base, (v, k) in sorted(found.items()):
                take(k, index[k])
                pnvm = base[:-1] + ".pnvm"
                if pnvm in index:
                    take(pnvm, index[pnvm])
            if name in index or found:
                if name in index:
                    take(name, index[name])
                continue
        if name in index:
            take(name, index[name])
            continue
        if m:
            found = best_versions(m.group(1), m.group(2), int(m.group(3)), m.group(4), index)
            if m.group(1) in found:
                take(found[m.group(1)][1], index[found[m.group(1)][1]])
                continue
        missing.append(name)

    for fn in sorted(os.listdir(fwsrc)):
        if fn == "WHENCE" or fn.startswith(("LICENCE.", "LICENSE.", "GPL-")):
            shutil.copyfile(os.path.join(fwsrc, fn), os.path.join(target_dir, fn))
    if os.path.isfile(os.path.join(regdb, "LICENSE")):
        shutil.copyfile(os.path.join(regdb, "LICENSE"), os.path.join(target_dir, "LICENSE.wireless-regdb"))

    work = os.path.dirname(os.path.abspath(dest))
    with open(os.path.join(work, "missing.txt"), "w") as f:
        f.write("".join(n + "\n" for n in missing))
    with open(os.path.join(work, "skipped.txt"), "w") as f:
        f.write("".join(n + "\n" for n in skipped))
    size = sum(os.path.getsize(os.path.join(target_dir, n)) for n in first_of.values())
    print("   %d names requested: %d files (%d of them links to another, %.1f MB), %d skipped (--skip), %d not in linux-firmware (%s)"
          % (len(requested), len(taken), len(taken) - len(first_of), size / 1e6, len(skipped), len(missing),
             os.path.join(work, "missing.txt")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
