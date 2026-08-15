#!/usr/bin/env python3
"""Corruption fuzzer for the itsfs reader.

    python3 tests/fuzz.py --bin bin/itsfs-fuzz --iters 200

Build the binary with the sanitizers first -- `make fuzz` does -- because the
bug class this is looking for is an out-of-bounds READ, which on a normal build
returns whatever was next in memory and exits 0.  Without ASan this program
mostly proves that nothing segfaults, which is the least interesting half.

WHAT IT DOES.  It builds a valid little ITS file system, then damages it: one
random word, in a structure the reader must parse, per iteration.  Every command
is then run over the result and the outcome is graded:

    ok      exit 0 or a plain non-zero error, with a message
    BAD     a signal, a sanitizer abort, or a hang

Nothing is asserted about WHAT the reader says about a damaged pack, only that
it says something and stays inside its own memory.  Every field it reads came
off a disk somebody else wrote, and half of the fields bound a loop or index an
array; a fuzzer is the cheapest way to keep that honest as the reader grows.

This is NOT part of `make test` -- the suite stays sh + coreutils, so it runs
anywhere -- and CI does not run it.
"""

import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

# rp06, from SYSTEM;RP06 DEFS 1.  Duplicated here rather than asked of the
# binary: a fuzzer that got its geometry from the program under test would
# quietly stop reaching the structures as soon as the geometry broke.
NHEDS, NSECS, SECBLK, NBLKSC = 19, 20, 8, 47
NCYLS, XCYLS = 812, 3
WPB = SECBLK * 128
NBLKS = NCYLS * NBLKSC
MFDBLK = NBLKS // 2 - 1
NTUTBL = 4
TUTBLK = MFDBLK - NTUTBL
NUDSL = 500
SECTORS = (NCYLS + XCYLS) * NHEDS * NSECS


def blk_sector(b):
    cyl = b // NBLKSC
    within = (b - cyl * NBLKSC) * SECBLK
    srf = within // NSECS
    return (cyl * NHEDS + srf) * NSECS + (within - srf * NSECS)


def sixbit(s):
    w = 0
    for c in s.ljust(6)[:6]:
        w = (w << 6) | ((ord(c) - 0o40) & 0o77)
    return w


def poke(f, blk, off, words):
    f.seek((blk_sector(blk) * 128 + off) * 8)
    for w in words:
        f.write(struct.pack("<Q", w & 0o777777777777))


def tutword(*vals):
    """Twelve three-bit entries to a word, most significant first."""
    w = 0
    for v in vals:
        w = (w << 3) | v
    return w


def tutpoke(f, word, value):
    """The TUT is NTUTBL blocks long, so a word index runs off the first one."""
    poke(f, TUTBLK + word // WPB, word % WPB, [value])


def build(path):
    """A small but complete file system: MFD, two UFDs, two files, a link, a TUT.

    IT MUST CHECK CLEAN.  `check` compares the TUT against the files block by
    block, so a fixture with a blank allocation table is not a file system with
    nothing in it -- it is a file system with 505 problems, and every one of them
    would be reported on every iteration, drowning whatever the damage did.

    This is the same fixture tests/run.sh builds, in the same words.  Two
    implementations of one fixture is a maintenance cost paid on purpose: the
    shell one proves the suite needs no python, and this one runs a thousand
    times a minute.
    """
    with open(path, "wb") as f:
        f.truncate(SECTORS * 1024)

        # MFD: header, then two directory names at the top of the block.  The
        # POSITION of a name is the address of its directory.
        poke(f, MFDBLK, 0, [1, 1020, 2026, 0, 0, sixbit("M.F.D."), NUDSL])
        poke(f, MFDBLK, 1020, [sixbit("TEST"), 0, sixbit("EMPTY"), 0])

        # UFD for TEST in block 498, and an empty one in 499.
        poke(f, 498, 0, [18, 1009, sixbit("TEST"), 4, 0])
        poke(f, 499, 0, [0, 1019, sixbit("EMPTY"), 0, 0])

        #   40 37 20  load address -> block 2000;  02  take two more;  00 end
        #   at byte 6:   a link, ".;@;DDT"
        #   at byte 14:  40 40 64 00, one block at 2100
        poke(f, 498, 11, [0o403720020000, 0o163340334444, 0o640040406400])
        poke(f, 498, 1009, [sixbit("NUL"), sixbit("TXT"), (1 << 24) | 14, 0, 0])
        poke(f, 498, 1014, [sixbit("HELLO"), sixbit("TXT"), 3 << 24, 0, 0])
        poke(f, 498, 1019, [sixbit("A"), sixbit("LINK"), (1 << 18) | 6, 0, 0])

        # The files' blocks, and the TUT header.
        poke(f, 2000, 0, [(0o110 << 29) | (0o111 << 22)])
        poke(f, 2002, 0, [(0o110 << 29) | (0o111 << 22)])
        poke(f, 2100, 0, [(0o101 << 29) | (0o102 << 15)])
        poke(f, TUTBLK, 0, [0, sixbit("TESTPK"), 3102, 1551, 0, NBLKS, 0])

        # Locked out: 500 directory blocks, the MFD, four TUT blocks.
        for i in range(41):                              # blocks 0..491
            tutpoke(f, 0o20 + i, tutword(*([7] * 12)))
        tutpoke(f, 57, tutword(7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 0, 0))
        tutpoke(f, 1605, tutword(0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 7))
        tutpoke(f, 1606, tutword(7, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))

        # In use: the four blocks the two files hold, once each.
        tutpoke(f, 182, tutword(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0))
        tutpoke(f, 191, tutword(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))


# The structures worth damaging, and where they are.  A random word anywhere on
# a 300 MB pack would almost always land in unused space and prove nothing.
TARGETS = [
    ("MFD header", MFDBLK, 0, 8),
    ("MFD names", MFDBLK, 1014, 10),
    ("UFD header", 498, 0, 11),
    ("UFD descriptors", 498, 11, 20),
    ("UFD names", 498, 1009, 15),
    ("TUT header", TUTBLK, 0, 8),
    ("TUT map", TUTBLK, 0o20, 40),
]

COMMANDS = [
    ["info"],
    ["dirs"],
    ["dirs", "-l"],
    ["ls", "-l", "@", "TEST"],
    ["cat", "@", "TEST;HELLO TXT"],
    ["free"],
    ["dump", "@", str(MFDBLK)],
    # `check` is a parser like the rest, and it is the one that walks EVERY
    # structure on the pack in one run -- so a damaged word anywhere reaches it.
    # It is also the one that allocates from numbers it read off the disk, which
    # is the failure mode the sanitizers are here for.
    ["check", "@"],
]


def run(binary, args, image, timeout):
    """Substitute the image for '@', run, and grade the outcome."""
    argv = [binary]
    for a in args:
        argv.append(image if a == "@" else a)
    if "@" not in args:
        argv.append(image)

    try:
        p = subprocess.run(argv, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return "hung", " ".join(argv)

    if p.returncode < 0:
        return "signal %d" % -p.returncode, " ".join(argv)

    # A sanitizer abort exits 1 like an ordinary error, so it is recognised by
    # what it prints rather than by its status.
    err = p.stderr.decode("utf-8", "replace")
    for marker in ("AddressSanitizer", "runtime error:", "LeakSanitizer"):
        if marker in err:
            return "sanitizer", " ".join(argv) + "\n" + err[:2000]

    # A non-zero exit with nothing said anywhere is a failure this cannot tell
    # from a crash.  `check` reports on stdout and exits 1, which IS saying
    # something -- so both streams count.
    out = p.stdout.decode("utf-8", "replace")
    if p.returncode != 0 and not err.strip() and not out.strip():
        return "silent failure", " ".join(argv)

    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="bin/itsfs")
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    tmp = tempfile.mkdtemp(prefix="itsfs-fuzz-")
    good = os.path.join(tmp, "good.dsk")
    work = os.path.join(tmp, "work.dsk")
    build(good)

    # The pristine pack must pass every command, or nothing below means anything.
    for cmd in COMMANDS:
        what, detail = run(args.bin, cmd, good, args.timeout)
        if what:
            print("the UNDAMAGED fixture already fails: %s\n%s" % (what, detail))
            return 2

    bad = 0
    for i in range(args.iters):
        name, blk, off, span = rng.choice(TARGETS)
        word = off + rng.randrange(span)
        value = rng.choice(
            [
                0,
                0o777777777777,
                rng.getrandbits(36),
                rng.getrandbits(18),
                (1 << 35) | rng.getrandbits(18),
            ]
        )

        with open(good, "rb") as s, open(work, "wb") as d:
            d.truncate(SECTORS * 1024)
            s.seek(0)
            # Copy only the blocks that hold anything; the rest is holes.
            for b in (MFDBLK, TUTBLK, TUTBLK + 1, TUTBLK + 2, TUTBLK + 3,
                      498, 499, 2000, 2001, 2002, 2100):
                s.seek(blk_sector(b) * 128 * 8)
                buf = s.read(WPB * 8)
                d.seek(blk_sector(b) * 128 * 8)
                d.write(buf)

        with open(work, "r+b") as f:
            poke(f, blk, word, [value])

        for cmd in COMMANDS:
            what, detail = run(args.bin, cmd, work, args.timeout)
            if what:
                bad += 1
                print("BAD  %s: %s word %d := %012o\n     %s" % (what, name, word, value, detail))

        if (i + 1) % 25 == 0:
            print("  %d/%d, %d bad" % (i + 1, args.iters, bad), file=sys.stderr)

    print("%d iterations x %d commands, %d bad" % (args.iters, len(COMMANDS), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
