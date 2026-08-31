#!/usr/bin/env python3
"""Corruption fuzzer for itsfs -- readers and writers both.

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

THE WRITERS RUN TOO, and they are the reason this matters most: a reader that
misparses a corrupt descriptor prints nonsense, while a writer that does it
overwrites somebody's file.  `put`, `del` and `mkdir` each get their own fresh
copy of the damaged pack, and each is followed by `check` over the result --
because staying inside its own memory is not enough if what it leaves behind
crashes the reader.

Everything happens in a mkdtemp on packs this builds itself.  It is never
pointed at an image on disk and takes no option that could point it at one.

This is NOT part of `make test` -- the suite stays sh + coreutils, so it runs
anywhere.  CI runs it separately, in the sanitizer job, at a low iteration
count.
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
#
# THE DESCRIPTOR RANGE IS THE WHOLE AREA, not the first twenty words.  A review
# found a write in `del` whose bounds were checked in another file, and the
# reason this had never reached it is that it only ever damaged descriptor bytes
# near the START of the area, where the offsets are small.  A pointer into the
# LAST word of a directory is the interesting one.
TARGETS = [
    ("MFD header", MFDBLK, 0, 8),
    ("MFD names", MFDBLK, 1014, 10),
    ("UFD header", 498, 0, 11),
    ("UFD descriptors", 498, 11, 998),
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
    # `manifest` walks every file AND READS ALL THEIR BLOCKS to checksum them,
    # which no other command does -- a descriptor that decodes to a plausible
    # block list reaches the reader here rather than only the decoder.
    ["manifest", "@"],
]

# The shell is a parser too, and the only command that reads a script.  It gets
# its own list because it takes its input on stdin rather than in argv.
SHELL_SCRIPT = "dirs\ncd TEST\nls -l\nstat HELLO TXT\nblocks HELLO TXT\ntype HELLO TXT\nfree\ninfo\n"

# THE MUTATING COMMANDS, WHICH ARE THE ONES THAT MATTER MOST.
#
# Everything above reads a damaged pack.  These CHANGE one, which is where a bug
# does real harm: a reader that misparses a corrupt descriptor prints nonsense,
# while a writer that does it overwrites somebody's file.  They are run in a
# separate pass because they must not disturb the read pass's input, and each
# gets a pack of its own.
#
# The bar is the same as everywhere else here -- no crash, no hang, no sanitizer
# report, and nothing failing silently -- and NOT that the operation succeeds.
# Refusing a damaged pack is the correct outcome for most of these; the point is
# that it refuses rather than writing somewhere it should not.
WRITE_COMMANDS = [
    # Into the populated directory, whose descriptor area is the damaged one.
    ["put", "@", "TEST;FUZZ TXT", "HOSTFILE"],
    # Into the EMPTY one, which is the case a reader refused for four phases.
    ["put", "@", "EMPTY;FUZZ TXT", "HOSTFILE"],
    # -f READS THE OLD DESCRIPTOR AND FREES WHAT IT DECODES, on a pack whose
    # descriptors are the damaged thing -- so it reaches the decoder and the
    # allocator in one command, which no other writer does.
    ["put", "-f", "@", "TEST;HELLO TXT", "HOSTFILE"],
    ["put", "-f", "@", "TEST;A LINK", "HOSTFILE"],
    # Removal reads a descriptor and then WRITES OVER IT, in place, at offsets
    # it computed from the disk.  This is the path the review was about.
    ["del", "@", "TEST;HELLO TXT"],
    ["del", "@", "TEST;A LINK"],      # a link: three names in the same field
    ["del", "@", "TEST;NUL TXT"],     # the multi-block file
    ["mkdir", "@", "FUZZD"],          # allocates an MFD slot and a block
    # A link WRITES INTO THE DESCRIPTOR AREA at an offset read off the damaged
    # disk (UDESCP), which is the same shape as `put`'s descriptor write with
    # none of the allocation -- so a damaged UDESCP reaches it directly.
    ["ln", "@", "TEST;HELLO TXT", "TEST;FUZZL X"],
    ["ln", "@", "EMPTY;NOSUCH FILE", "TEST;FUZZL Y"],
    # Renaming MOVES an entry, so it memmoves inside the name area using
    # UDNAMP and the entry's own word offset, both off the disk.
    ["mv", "@", "TEST;HELLO TXT", "TEST;ZZZZZZ TXT"],
    ["rmdir", "@", "EMPTY"],          # frees an MFD slot and zeroes its block
]

# `mkfs` is deliberately absent: it overwrites the pack from nothing and reads
# almost none of it, so damage cannot reach it.  It is covered by tests/mkfs.sh,
# which checks the result rather than the survival.


# The blocks that hold anything.  The rest of the 300 MB is holes, and copying
# it per iteration would make this too slow to run a thousand times.
LIVE = [MFDBLK, TUTBLK, TUTBLK + 1, TUTBLK + 2, TUTBLK + 3,
        498, 499, 2000, 2001, 2002, 2100]


# How many times each command ran to completion, filled in by run().  Reported
# at the end for the WRITERS, because a write pass in which every command
# refuses every pack is green and vacuous -- it would exercise the argument
# parser and the refusal, and never once reach the code that writes.
DONE = {}


def fresh_empty(binary, path, timeout):
    """An EMPTY file system for `tar x` to extract into.

    Not a copy of the fixture: the archive was written FROM the fixture, so
    extracting it there finds every name already taken and skips the lot -- which
    looks like a refusal and would make the control below fail for a reason that
    has nothing to do with the archive.  Empty, and made afresh each attempt, so
    a `check` afterwards is answering only for what `tar x` just wrote.

    `mkfs` builds it.  That is the program under test writing the target rather
    than the harness, and if it is broken the control says so immediately."""
    if os.path.exists(path):
        os.remove(path)
    p = subprocess.run([binary, "mkfs", path, "FUZZ"], capture_output=True,
                       timeout=timeout)
    return p.returncode == 0


def copy_damaged(good, dst, blk, word, value):
    """A fresh copy of the fixture with exactly one word changed."""
    with open(good, "rb") as s, open(dst, "wb") as d:
        d.truncate(SECTORS * 1024)
        for b in LIVE:
            s.seek(blk_sector(b) * 128 * 8)
            buf = s.read(WPB * 8)
            d.seek(blk_sector(b) * 128 * 8)
            d.write(buf)
    with open(dst, "r+b") as f:
        poke(f, blk, word, [value])


# ---------------------------------------------------------------------------
# The other untrusted input: a container file.
#
# A pack is something you own.  A .tap is something you DOWNLOADED, which makes
# it the likelier hostile input of the two, and until now nothing damaged one.
# Both readers here parse length fields out of the file and then read that many
# bytes -- `tape` the SIMH record framing, `saveset` the DUMP headers inside it.
#
# The fixture is a save set this project writes itself, with `save`.  For a
# format question that would be circular; for a memory-safety question it is
# not, because what is being asked is whether the READER stays inside its own
# buffers given bytes it did not write, and the damage is what makes them bytes
# it did not write.
TAPE_COMMANDS = [
    ["tape", "@"],
    ["tape", "-x", "XDIR", "@"],
    ["saveset", "@"],
    ["saveset", "-x", "XDIR", "@"],
    # In the wrong packing on purpose: every length in the file then decodes to
    # something else, which is the cheapest way to reach the bounds checks.
    ["saveset", "-p", "le64", "@"],
]


def build_tape(binary, pack, out, timeout):
    """A real save set, written by `save` from the fixture pack."""
    p = subprocess.run(
        [binary, "save", pack, out, "TEST;HELLO TXT", "TEST;NUL TXT", "TEST;A LINK"],
        capture_output=True, timeout=timeout,
    )
    return p.returncode == 0, (p.stderr or p.stdout).decode("utf-8", "replace")


def tap_lengths(b):
    """Offsets of the record-length fields in a SIMH .tap.

    A record is a 4-byte little-endian count, the data padded to even, and then
    the SAME count again.  Those eight bytes are the only ones that tell a reader
    how far to read, which makes them worth far more than an average byte -- and
    random damage finds them about one time in three hundred.  So they are
    enumerated and hit on purpose."""
    out, i = [], 0
    while i + 4 <= len(b):
        n = int.from_bytes(b[i:i + 4], "little")
        out.append(i)
        if n == 0 or n & 0xF0000000:            # tape mark, or an error flag
            i += 4
            continue
        i += 4 + n + (n & 1) + 4
    return out


def damage_file(src, dst, rng):
    """Copy a file with a few bytes changed.  Byte-wise, not word-wise: the
    record framing is 8-bit little-endian counts, so damaging whole 36-bit words
    would leave the container structure itself untouched."""
    b = bytearray(open(src, "rb").read())
    if not b:
        return 0

    # Half the time, go straight for a length field and give it a value chosen
    # to be awkward rather than random: a count longer than the file, the
    # largest a reader might multiply by a word size, or off by one.
    if rng.random() < 0.5:
        offs = tap_lengths(b)
        if offs:
            i = rng.choice(offs)
            v = rng.choice([0xFFFFFFFF, 0x7FFFFFFF, len(b) * 4, len(b) - i,
                            0xFFFFFF, 1, 3, 5, rng.getrandbits(32)])
            b[i:i + 4] = (v & 0xFFFFFFFF).to_bytes(4, "little")
            open(dst, "wb").write(bytes(b))
            return 1

    n = rng.choice([1, 1, 1, 2, 4])
    for _ in range(n):
        # Weighted at the front, where the headers and the first records are.
        i = rng.randrange(min(len(b), 4096)) if rng.random() < 0.7 \
            else rng.randrange(len(b))
        b[i] = rng.randrange(256)
    # And sometimes truncate, which is the commonest real damage of all.
    if rng.random() < 0.25:
        del b[rng.randrange(len(b)):]
    open(dst, "wb").write(bytes(b))
    return n


# ---------------------------------------------------------------------------
# The third untrusted input: a tar archive.
#
# `tar t` and `tar x` parse 512-byte headers with an octal size, an octal
# checksum, a type byte and a 100-byte name, and then read that many bytes --
# the same shape as the .tap framing above and the same reason to damage it.  A
# tar is also the likeliest of the three to arrive from somewhere else: it is
# the format the rest of the world hands you.
#
# AND `tar x` WRITES TO A PACK, which puts it in the same class as `put`: a
# reader that misparses a header prints nonsense, while `tar x` misparsing one
# writes into somebody's file system.  So it gets a fresh pack per attempt and a
# `check` over the result, exactly as the write pass does.
TAR_COMMANDS = [
    ["tar", "t", "ARCHIVE"],
    ["tar", "t", "-v", "ARCHIVE"],
]

# `-m` is required for `tar x`, and both answers reach a different decoder: text
# refuses a byte over 0177, words wants a length that is a multiple of eight.
#
# EACH IS PAIRED WITH AN ARCHIVE WRITTEN THE SAME WAY.  Feeding a text archive
# to `-m words` is not a test, it is a category error: every member's length
# fails the multiple-of-eight rule, so the whole extraction is refused before a
# single header field matters.  That is correct behaviour and it made the first
# version of this control fail on a sound archive.
TAR_WRITE_COMMANDS = [
    (["tar", "x", "-m", "text", "ARCHIVE", "@"], "text"),
    (["tar", "x", "-m", "words", "ARCHIVE", "@"], "words"),
]


def build_tar(binary, pack, out, mode, timeout):
    """A real archive, written by `tar c` from the fixture pack, in one mode.

    THE TWO FILES AND NOT THE LINK, deliberately.  `tar x` cannot create an ITS
    link, so it skips one and says so in its exit status -- which means an
    archive containing a link makes `tar x` exit non-zero no matter how sound it
    is, and the "did anything actually succeed" tally at the end would be
    measuring that instead of the header parser.  The link path is still reached,
    and reached harder: damage_tar sets a member's type byte to '2' at random, so
    what `tar x` meets is a link whose name and size are also damaged."""
    p = subprocess.run(
        [binary, "tar", "c", "-m", mode, out, pack,
         "TEST;HELLO TXT", "TEST;NUL TXT"],
        capture_output=True, timeout=timeout,
    )
    return p.returncode == 0, (p.stderr or p.stdout).decode("utf-8", "replace")


def tar_headers(b):
    """Offsets of the 512-byte headers in a ustar archive.

    Walking it needs the size field, which is the field being damaged -- so this
    walks the UNDAMAGED copy and the offsets are then applied to the damaged one.
    The eight fields below are worth far more than an average byte: everything a
    reader decides is decided from them."""
    out, i = [], 0
    while i + 512 <= len(b):
        if b[i] == 0:                        # the end-of-archive zero blocks
            break
        out.append(i)
        try:
            size = int(b[i + 124:i + 136].split(b"\0")[0].strip() or b"0", 8)
        except ValueError:
            break
        i += 512 + (size + 511) // 512 * 512
    return out


# offset, length, what it is -- ustar, POSIX.1-1988
TAR_FIELDS = [
    (0, 100, "name"),
    (100, 8, "mode"),
    (124, 12, "size"),
    (136, 12, "mtime"),
    (148, 8, "checksum"),
    (156, 1, "typeflag"),
    (157, 100, "linkname"),
    (257, 6, "magic"),
]


def damage_tar(src, dst, rng):
    """Copy an archive with a header field, or a few bytes, changed."""
    b = bytearray(open(src, "rb").read())
    if not b:
        return "empty"

    offs = tar_headers(bytes(b))

    # Two thirds of the time, go for a header field on purpose, with a value
    # chosen to be awkward rather than random.  Random bytes reach a size field
    # about one time in three hundred.
    if offs and rng.random() < 0.67:
        h = rng.choice(offs)
        off, ln, what = rng.choice(TAR_FIELDS)
        if what == "size":
            v = rng.choice([b"77777777777", b"00000000001", b"37777777777",
                            b"99999999999", b"           ", b"\0" * 11,
                            oct(len(b) * 8)[2:].encode().rjust(11, b"0")])
            b[h + off:h + off + 11] = v[:11].ljust(11, b"0")
        elif what == "name":
            v = rng.choice([b"../" * 33, b"/etc/passwd", b"\xff" * 100,
                            b"A" * 100, b".", b"..", b"%2", b"%ZZ/x",
                            b"a/b/c/d", b"\0" * 100])
            b[h:h + 100] = v[:100].ljust(100, b"\0")
        elif what == "linkname":
            v = rng.choice([b"../" * 33, b"\xff" * 100, b"B" * 100, b"\0" * 100])
            b[h + 157:h + 257] = v[:100].ljust(100, b"\0")
        elif what == "typeflag":
            b[h + 156] = rng.choice([ord("0"), ord("5"), ord("2"), ord("1"),
                                     ord("7"), 0, 255, rng.randrange(256)])
        else:
            for k in range(ln):
                b[h + off + k] = rng.randrange(256)
        open(dst, "wb").write(bytes(b))
        return what

    n = rng.choice([1, 1, 1, 2, 4])
    for _ in range(n):
        i = rng.randrange(min(len(b), 4096)) if rng.random() < 0.7 \
            else rng.randrange(len(b))
        b[i] = rng.randrange(256)
    # Truncation, which is the commonest real damage of all -- and for tar it
    # means a member whose header promises more data than the file holds.
    if rng.random() < 0.3:
        del b[rng.randrange(len(b)):]
    open(dst, "wb").write(bytes(b))
    return "bytes"


def run(binary, args, image, timeout, stdin=None, hostfile=None, expect_zero=False,
        xdir=None, archive=None):
    """Substitute the image for '@', run, and grade the outcome."""
    argv = [binary]
    for a in args:
        if a == "@":
            argv.append(image)
        elif a == "HOSTFILE":
            argv.append(hostfile)
        elif a == "XDIR":
            argv.append(xdir)
        elif a == "ARCHIVE":
            argv.append(archive)
        else:
            argv.append(a)
    if "@" not in args:
        argv.append(image)

    try:
        p = subprocess.run(
            argv, capture_output=True, timeout=timeout,
            input=stdin.encode() if stdin else None,
        )
    except subprocess.TimeoutExpired:
        return "hung", " ".join(argv)

    if p.returncode == 0:
        DONE[args[0]] = DONE.get(args[0], 0) + 1

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

    # Only asked of the CONTROL run, on an undamaged pack.  A write command that
    # fails for a boring reason -- a misspelled path here, a changed argument
    # order -- would refuse every damaged pack too, and the whole write pass
    # would report zero failures while testing nothing at all.
    if expect_zero and p.returncode != 0:
        return "control failed (exit %d)" % p.returncode, \
            " ".join(argv) + "\n" + (err or out)[:2000]

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
    wr = os.path.join(tmp, "wr.dsk")
    build(good)

    # Something to `put`.  Two blocks and a bit, so it needs a descriptor with
    # more than one entry and cannot fit in whatever slack a damaged one leaves.
    host = os.path.join(tmp, "host.txt")
    with open(host, "wb") as f:
        f.write(b"fuzz\n" * 300)

    # The pristine pack must pass every command, or nothing below means anything.
    for cmd in COMMANDS:
        what, detail = run(args.bin, cmd, good, args.timeout)
        if what:
            print("the UNDAMAGED fixture already fails: %s\n%s" % (what, detail))
            return 2

    what, detail = run(args.bin, ["shell", "@"], good, args.timeout, SHELL_SCRIPT)
    if what:
        print("the UNDAMAGED fixture already fails in the shell: %s\n%s" % (what, detail))
        return 2

    # And every WRITE command must succeed on an undamaged pack, each on its own
    # copy since they change it.  Without this the write pass below could be
    # green because nothing it runs ever gets as far as writing.
    for cmd in WRITE_COMMANDS:
        copy_damaged(good, wr, MFDBLK, 0, 1)          # 1 == the value already there
        what, detail = run(args.bin, cmd, wr, args.timeout,
                           hostfile=host, expect_zero=True)
        if what:
            print("a write command fails on the UNDAMAGED fixture: %s\n%s" % (what, detail))
            return 2

    # The container fixture, and the same control: every command must work on the
    # UNDAMAGED save set first.  `save` writing an empty or broken tape would
    # otherwise make the whole container pass a test of the "not a tape" path.
    goodtap = os.path.join(tmp, "good.tap")
    worktap = os.path.join(tmp, "work.tap")
    goodtar = {m: os.path.join(tmp, "good-%s.tar" % m) for m in ("text", "words")}
    worktar = {m: os.path.join(tmp, "work-%s.tar" % m) for m in ("text", "words")}
    tarwr = os.path.join(tmp, "tarx.dsk")
    xdir = os.path.join(tmp, "x")
    os.mkdir(xdir)

    ok, msg = build_tape(args.bin, good, goodtap, args.timeout)
    if not ok:
        print("could not build the save-set fixture:\n%s" % msg)
        return 2
    if os.path.getsize(goodtap) < 1024:
        print("the save-set fixture is %d bytes -- too small to be three files"
              % os.path.getsize(goodtap))
        return 2

    for cmd in TAPE_COMMANDS:
        if "-p" in cmd:
            continue            # the wrong-packing one is EXPECTED to fail
        what, detail = run(args.bin, cmd, goodtap, args.timeout,
                           xdir=xdir, expect_zero=True)
        if what:
            print("the UNDAMAGED save set already fails: %s\n%s" % (what, detail))
            return 2

    for m in ("text", "words"):
        ok, msg = build_tar(args.bin, good, goodtar[m], m, args.timeout)
        if not ok:
            print("could not build the %s fixture archive: %s" % (m, msg))
            return 2

    # THE CONTROL, for the same reason the tape has one: if `tar t` cannot read
    # an undamaged archive -- a changed argument order would do it -- then every
    # damaged one below is refused for a boring reason and the pass proves
    # nothing while reporting zero failures.
    for cmd in TAR_COMMANDS:
        for m in ("text", "words"):
            what, detail = run(args.bin, cmd, work, args.timeout,
                               archive=goodtar[m], expect_zero=True)
            if what:
                print("the UNDAMAGED %s archive already fails: %s\n%s"
                      % (m, what, detail))
                return 2

    # `tar x` EXITS 1 ON AN ARCHIVE WITH A LINK IN IT, always: nothing here can
    # create an ITS link, so it reports the skip and says so in its status.  That
    # is correct and permanent, so this control cannot ask for exit 0.  It asks
    # the stronger thing instead -- that the files it COULD write are on the pack
    # afterwards -- because "did not crash" would also be satisfied by a `tar x`
    # that refused the archive at its first header.
    for cmd, m in TAR_WRITE_COMMANDS:
        if not fresh_empty(args.bin, tarwr, args.timeout):
            print("mkfs could not build the extraction target")
            return 2

        what, detail = run(args.bin, cmd, tarwr, args.timeout, archive=goodtar[m])
        if what:
            print("the UNDAMAGED %s archive already fails: %s\n%s" % (m, what, detail))
            return 2

        p = subprocess.run([args.bin, "ls", tarwr, "TEST"], capture_output=True,
                           timeout=args.timeout)
        if b"HELLO" not in p.stdout:
            print("tar x -m %s wrote nothing from an undamaged archive: %s"
                  % (m, (p.stdout or p.stderr).decode("utf-8", "replace")[:400]))
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

        copy_damaged(good, work, blk, word, value)

        for cmd in COMMANDS + [["shell", "@"]]:
            script = SHELL_SCRIPT if cmd[0] == "shell" else None
            what, detail = run(args.bin, cmd, work, args.timeout, script)
            if what:
                bad += 1
                print("BAD  %s: %s word %d := %012o\n     %s" % (what, name, word, value, detail))

        # The write pass.  Each command gets a FRESH copy of the same damaged
        # pack -- otherwise the first `del` to succeed would change what all the
        # rest of them see, and a failure could not be traced back to one word.
        for cmd in WRITE_COMMANDS:
            copy_damaged(good, wr, blk, word, value)
            what, detail = run(args.bin, cmd, wr, args.timeout, hostfile=host)
            if what:
                bad += 1
                print("BAD  %s: %s word %d := %012o\n     %s" % (what, name, word, value, detail))
                continue

            # AND WHAT IT WROTE MUST STILL BE READABLE.  A writer can stay inside
            # its own memory and still leave a pack that crashes the reader --
            # a descriptor whose length disagrees with its contents, a name area
            # pointer past the end.  Running `check` over the result is what
            # turns "the writer did not crash" into "the writer did not produce
            # something that crashes".  It may REPORT anything it likes.
            what, detail = run(args.bin, ["check", "@"], wr, args.timeout)
            if what:
                bad += 1
                print("BAD  %s: after %s -- %s word %d := %012o\n     %s"
                      % (what, cmd[0], name, word, value, detail))

        # ---- containers: a damaged .tap rather than a damaged pack ----
        damage_file(goodtap, worktap, rng)
        for cmd in TAPE_COMMANDS:
            # A fresh extraction directory each time, or the second command
            # finds the first one's output already there and writes nothing.
            for f in os.listdir(xdir):
                os.remove(os.path.join(xdir, f))
            what, detail = run(args.bin, cmd, worktap, args.timeout, xdir=xdir)
            if what:
                bad += 1
                print("BAD  %s: damaged tape\n     %s" % (what, detail))

        # ---- and a damaged tar archive, read and then extracted ----
        fields = {}
        for m in ("text", "words"):
            fields[m] = damage_tar(goodtar[m], worktar[m], rng)

        for cmd in TAR_COMMANDS:
            for m in ("text", "words"):
                what, detail = run(args.bin, cmd, work, args.timeout,
                                   archive=worktar[m])
                if what:
                    bad += 1
                    print("BAD  %s: damaged %s archive (%s)\n     %s"
                          % (what, m, fields[m], detail))

        # `tar x` writes, so it gets a fresh pack and a `check` afterwards --
        # the same treatment as put and del, for the same reason.
        for cmd, m in TAR_WRITE_COMMANDS:
            what_field = fields[m]
            if not fresh_empty(args.bin, tarwr, args.timeout):
                print("mkfs could not build the extraction target")
                return 2
            what, detail = run(args.bin, cmd, tarwr, args.timeout,
                               archive=worktar[m])
            if what:
                bad += 1
                print("BAD  %s: tar x over a damaged archive (%s)\n     %s"
                      % (what, what_field, detail))
                continue

            what, detail = run(args.bin, ["check", "@"], tarwr, args.timeout)
            if what:
                bad += 1
                print("BAD  %s: after tar x over a damaged archive (%s)\n     %s"
                      % (what, what_field, detail))

        if (i + 1) % 25 == 0:
            print("  %d/%d, %d bad" % (i + 1, args.iters, bad), file=sys.stderr)

    ncmd = (len(COMMANDS) + 1 + 2 * len(WRITE_COMMANDS) + len(TAPE_COMMANDS)
            + 2 * len(TAR_COMMANDS) + 2 * len(TAR_WRITE_COMMANDS))
    print("%d iterations x %d commands (%d writers rechecked, %d on damaged "
          "tapes, %d on damaged tar archives), %d bad"
          % (args.iters, ncmd, len(WRITE_COMMANDS), len(TAPE_COMMANDS),
             len(TAR_COMMANDS) + len(TAR_WRITE_COMMANDS), bad))

    # The writers ran on a damaged pack `iters` times each, plus once on the
    # undamaged control.  If these numbers are near zero the pass above proved
    # nothing, however green it looked.
    seen = []
    for c in WRITE_COMMANDS:
        if c[0] not in seen:
            seen.append(c[0])

    for name in seen:
        n = sum(1 for c in WRITE_COMMANDS if c[0] == name)
        print("  %-6s completed %d of %d attempts"
              % (name, DONE.get(name, 0), n * (args.iters + 1)))

    # `tar` covers the readers and `tar x` together, since DONE is keyed on the
    # subcommand name.  If this is zero, every archive above was refused before
    # it reached the header parser and the pass tested nothing.
    ntar = (2 * len(TAR_COMMANDS) + len(TAR_WRITE_COMMANDS)) * (args.iters + 1)
    print("  %-6s completed %d of %d attempts" % ("tar", DONE.get("tar", 0), ntar))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
