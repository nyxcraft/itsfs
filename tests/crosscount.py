#!/usr/bin/env python3
"""A third reader, in another language, that shares nothing with the other two.

    python3 tests/crosscount.py <image> [le64|dbd9]

Prints the directory and entry counts it finds, for comparison with
`itsfs check`.  Run by `make oracle` when python3 and a real pack are both
available; not part of `make test`, which stays sh + coreutils.

WHY A THIRD ONE.  `structure.c` and `cmd_check.c` are already independent of
each other -- the checker re-derives the geometry and the MFD arithmetic rather
than calling the reader.  But BOTH TAKE THEIR CONSTANTS FROM src/its.h.  A
second reading catches a wrong reading; it cannot catch a wrong TRANSCRIPTION,
because both inherit it.

So the constants below are transcribed again, here, from the same ITS sources
and not from its.h.  Each carries its citation.  If a number here disagrees with
one there, one of the two transcriptions is wrong and the disagreement says so.

It is also a different language, which is not nothing: the C readers share an
integer model, a byte order, and a set of habits about shifts and masks.  Python
has arbitrary-precision integers and no unsigned types, so a 36-bit value that
overflowed or sign-extended in C would come out differently here.

This was written as a throwaway, to answer one question about flag bits on a
pack ITS had run on.  It agreed with `check` to the entry, and that agreement was
worth more than the answer, so it stayed and grew a space audit.

IT EARNED ITS KEEP THE FIRST TIME IT COUNTED BLOCKS, by disagreeing.  Three
constants had been transcribed wrong here:

    UNDSCP is the LOW 13 BITS of UNRNDM, not the top nine
    the map starts at LTIBLK, which is OCTAL 20, not word 0
    a block indexes from QFRSTB, not from zero

Every one produced plausible-looking output -- 6,826 blocks claimed, a table that
still partitioned correctly into 30,792 + 6,864 + 508 -- and every one was caught
by the totals not matching `check`.  That is the argument for a third reader in
one paragraph: two implementations that share a header agree with each other
about a mistake in it.
"""

import struct
import sys

# ---------------------------------------------------------------- the drive
#
# SYSTEM;RP06 DEFS 1.  Transcribed here from the same file src/itsgeom.c reads,
# and deliberately not shared with it.
NHEDS = 19  # surfaces
NSECS = 20  # sectors per track
SECBLK = 8  # sectors per block
NCYLS = 812  # cylinders of file system
XCYLS = 3  # spare cylinders

# DERIVED, and the derivation is the interesting part: blocks per cylinder is a
# TRUNCATING divide, so four sectors of every cylinder are addressable by the
# hardware and reachable by no block number.
NBLKSC = NHEDS * NSECS // SECBLK  # 47, from 47.5
WPB = SECBLK * 128  # words per block
NBLKS = NCYLS * NBLKSC
MFDBLK = NBLKS // 2 - 1  # SYSTEM;FSDEFS 43: MFD is at NBLKS/2-1

# ------------------------------------------------------------ the structures
#
# SYSTEM;FSDEFS 43, transcribed independently of src/its.h.
MD_NAMP = 1  # MDNAMP  origin of the MFD name area
MD_NUDS = 6  # MDNUDS  number of directory slots
LMNBLK = 2  # LMNBLK  words per MFD name entry
UD_NAMP = 1  # UDNAMP  origin of the UFD name area
LUNBLK = 5  # LUNBLK  words per UFD name entry
UN_LNK_P = 18  # UNLINK  the link bit's position
UN_LNK_S = 1
UN_DSCP_P = 0  # UNDSCP==1500,,  byte offset of the descriptor -- the LOW 13
UN_DSCP_S = 13  # bits, and getting that wrong was the first thing this caught
UD_DESC = 11  # UDDESC  first word a descriptor may occupy
UFDBPW = 6  # UFDBPW  six-bit bytes per word

# The descriptor opcodes, SYSTEM;FSDEFS 43, transcribed again.  B is the current
# block, undefined until a load address sets it.
#
#   0            end
#   1..UDTKMX    take N more blocks
#   ..UDWPH-1    skip: B += code - UDTKMX, then take one
#   UDWPH        write place holder
#   UDLOADAD..   load address: this byte's low 5 bits and the next two are B
UD_TKMX = 12
UD_WPH = 31
UD_LOADAD = 0o40

# The allocation table.  Three bits per block, twelve to a word: 0 free,
# 1..5 a reference count, 6 "many", 7 locked out.
TUT_BPE = 3  # TUTBYT  bits per entry
TUT_EPW = 12  # TUTEPW  entries per word
LTIBLK = 0o20  # LTIBLK  OCTAL -- the first word of the map, after the header
Q_FRSTB = 4  # QFRSTB  first block the table maps
Q_LASTB = 5  # QLASTB  last block the table maps
NTUTBL = 4  # RP06: four blocks of table
TUTBLK = MFDBLK - NTUTBL


def words_le64(f, sector, n):
    f.seek(sector * 128 * 8)
    d = f.read(n * 8)
    return [struct.unpack("<Q", d[i : i + 8])[0] & 0o777777777777 for i in range(0, len(d), 8)]


def words_dbd9(f, sector, n):
    """Two words in nine bytes, sharing byte 4.

    Written out again rather than borrowed, for the same reason as the
    constants: the point is not to agree with itspack.c by construction.
    """
    base = sector * 128
    group, off = divmod(base, 2)
    f.seek(group * 9)
    raw = f.read((n // 2 + 2) * 9)
    out = []

    for i in range(0, len(raw) - 8, 9):
        hi = int.from_bytes(raw[i : i + 5], "big") >> 4
        lo = int.from_bytes(raw[i + 4 : i + 9], "big") & 0xFFFFFFFFF
        out.append(hi)
        out.append(lo)

    return out[off : off + n]


def desc_blocks(u, desc):
    """Count the blocks a descriptor describes.

    THE ORIGIN IS UDDESC, NOT WORD 0 -- UNDSCP counts six-bit bytes from the
    first word a descriptor may occupy.  Reading it from word 0 lands 66 bytes
    early, where a well-formed pack holds zero, so every file looks empty and
    nothing complains.  ITS's own doc/sysdoc/ufd.100 says it in one sentence:
    "a byte address relative to a point 11. words from the beginning".
    """
    n = 0
    b = 0
    loaded = False
    off = desc
    steps = WPB * UFDBPW

    while steps > 0:
        steps -= 1
        word = UD_DESC + off // UFDBPW

        if word >= WPB:
            return n

        c = (u[word] >> (30 - 6 * (off % UFDBPW))) & 0o77
        off += 1

        if c == 0:
            return n

        if c >= UD_LOADAD:
            if UD_DESC + (off + 1) // UFDBPW >= WPB:
                return n
            c2 = (u[UD_DESC + off // UFDBPW] >> (30 - 6 * (off % UFDBPW))) & 0o77
            off += 1
            c3 = (u[UD_DESC + off // UFDBPW] >> (30 - 6 * (off % UFDBPW))) & 0o77
            off += 1
            b = ((c & 0o37) << 12) | (c2 << 6) | c3
            loaded = True
            run = 1
        elif c == UD_WPH:
            continue
        elif c <= UD_TKMX:
            if not loaded:
                return n
            run = c
        else:
            if not loaded:
                return n
            b += c - UD_TKMX
            run = 1

        n += run
        b += run

    return n


def blk_sector(b):
    """A block number is not a linear offset.  Blocks are numbered WITHIN a
    cylinder, so this is the whole geometry in four lines."""
    cyl = b // NBLKSC
    within = (b - cyl * NBLKSC) * SECBLK
    surf = within // NSECS
    return (cyl * NHEDS + surf) * NSECS + (within - surf * NSECS)


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[2].strip())
        return 2

    path = sys.argv[1]
    packing = sys.argv[2] if len(sys.argv) > 2 else "le64"
    rd = words_dbd9 if packing == "dbd9" else words_le64

    with open(path, "rb") as f:
        mfd = rd(f, blk_sector(MFDBLK), WPB)
        namp = mfd[MD_NAMP]
        nuds = mfd[MD_NUDS]

        if not (0 < namp <= WPB) or not (0 < nuds <= WPB):
            print("crosscount: MFD header is not plausible", file=sys.stderr)
            return 1

        # THE POSITION IS THE ADDRESS.  There is no pointer from an MFD entry to
        # its directory; the slot's offset resolves to a block number, and this
        # is that arithmetic written a third time.
        dirs = []
        for w in range(namp, WPB, LMNBLK):
            if mfd[w]:
                dirs.append((w - WPB + LMNBLK * nuds) // LMNBLK)

        nent = nfile = nlink = 0
        claimed = 0

        for b in dirs:
            u = rd(f, blk_sector(b), WPB)
            j = u[UD_NAMP]

            if not (0 < j <= WPB):
                continue

            while j + LUNBLK <= WPB:
                if u[j] or u[j + 1]:
                    nent += 1
                    if (u[j + 2] >> UN_LNK_P) & ((1 << UN_LNK_S) - 1):
                        nlink += 1
                    else:
                        nfile += 1
                        claimed += desc_blocks(u, (u[j + 2] >> UN_DSCP_P) & ((1 << UN_DSCP_S) - 1))
                j += LUNBLK

        # And the table, counted its own way.
        tut = []
        for i in range(NTUTBL):
            tut += rd(f, blk_sector(TUTBLK + i), WPB)

        # THE MAP IS RELATIVE TO QFRSTB AND STARTS AT LTIBLK, which is octal 20.
        # Both were wrong in the first draft of this file, and the disagreement
        # with `check` is what said so -- which is the whole point of it.
        first, last = tut[Q_FRSTB], tut[Q_LASTB]
        free = used = locked = 0

        for blk in range(first, last):
            w, e = divmod(blk - first, TUT_EPW)
            w += LTIBLK
            if w >= len(tut):
                break
            v = (tut[w] >> (TUT_BPE * (TUT_EPW - 1 - e))) & 0o7
            if v == 0:
                free += 1
            elif v == 7:
                locked += 1
            else:
                used += 1

    print("crosscount: %d directories, %d entries (%d files, %d links)" % (len(dirs), nent, nfile, nlink))
    print("crosscount: %d blocks claimed by files, %d in use per the table, %d free, %d locked out"
          % (claimed, used, free, locked))

    if claimed != used:
        print("crosscount: THEY DISAGREE by %d" % (claimed - used))
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
