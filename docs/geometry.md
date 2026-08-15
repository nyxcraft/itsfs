# Geometry — why a block number is not an offset

This layer has no counterpart in `t10fs` or `s5fs`, and it is the first place ITS
turns out not to be TOPS-10 with different field names.

## The arithmetic

A sector is 128 words on every drive ITS supported. An ITS **block** is `SECBLK`
consecutive sectors, and `SECBLK` is 8 everywhere. So far, so ordinary.

The blocks are then numbered **within a cylinder**:

```
NBLKSC == NHEDS * NSECS / SECBLK        ; MIDAS integer division
NBLKS  == NCYLS * NBLKSC
```

MIDAS truncates. For an RP06, `19 * 20 / 8` is 47.5 and `NBLKSC` is **47**: the
cylinder has 380 sectors, ITS reaches 376, and the last four are addressable by
the hardware and reachable by no block number. Over 815 cylinders that is 3,260
sectors — 3.2 MB of a 300 MB pack that the file system cannot name.

`itsfs drives` prints the remainder for each drive, because it is not the same
everywhere:

```
  rp04   4 sectors per cylinder unused -- 1644 sectors, 1644 KB, on the pack
  rp06   4 sectors per cylinder unused -- 3260 sectors, 3260 KB, on the pack
  rp07   0 sectors per cylinder unused
  rm03   6 sectors per cylinder unused -- 4938 sectors, 4938 KB, on the pack
  rm80   4 sectors per cylinder unused -- 2236 sectors, 2236 KB, on the pack
```

The RP07 divides exactly (`32 * 43 / 8 == 172`), which is worth knowing: a reader
tested only on an RP07 would never notice this layer was needed.

## Block to sector

FSDEFS writes the conversion out for the one block whose address is a constant,
the MFD:

```
MFDCYL==MFDBLK/NBLKSC
MFDSRF==<MFDBLK-MFDCYL*NBLKSC>*SECBLK/NSECS
MFDSEC==<MFDBLK-MFDCYL*NBLKSC>*SECBLK-MFDSRF*NSECS
```

A SIMH or KLH10 image stores sectors in `(cylinder, surface, sector)` order, so
the linear sector is `(cyl * NHEDS + srf) * NSECS + sec`. That is
`its_blk_sector()`, and it is the only place in the project where a block becomes
a sector.

## How this was found, and how it is checked

The MFD is at `NBLKS/2-1`, which for an RP06 is block **19081**. Reading that as
a linear block number — 19081 × 1024 words — lands in the middle of a mail file.
Running it through the formula gives cylinder 405, surface 18, sector 8, which is
linear sector **154268**, and word 5 of that sector is SIXBIT `M.F.D.` — the check
word `MDCHK` exists precisely so that a program can ask this question and get an
answer.

The two numbers differ by 3,244 sectors. There is no way to be *slightly* wrong
about this: either you have the geometry or you have somebody's mail.

The test suite computes block 19081's sector **a second time**, in shell, from the
drive constants — deliberately not by asking `itsfs`, because a wrong mapping
would otherwise agree with itself — and then checks that dumping the block and
dumping that raw sector give the same words. `itsfs dump -s` exists for exactly
that: it addresses sectors and skips this layer entirely.

## A "track" in ITS source is a block

`disk.1228` line 48:

```
;	TUT	TRACK (BLOCK) UTILIZATION TABLE
```

and the file system code uses the two words interchangeably from there on. The
MFD-slot arithmetic in `QFL2` is commented `J <= TRACK ADDR OF USER DIR` and
produces a block number. It is not a surface track, and nothing in the file
system addresses a surface track.

This confuses every reader of the ITS sources once. It is written down here, and
in `itsgeom.h`, so that it confuses the next one less.

## The drive table

Every constant is transcribed from that drive's own parameter file in the ITS
source; the derived quantities are computed rather than transcribed, so a drive
added later cannot get the truncation subtly wrong by hand.

| drive | cyls | spare | surfaces | sectors/track | blocks/cyl | blocks | TUT blocks |
|---|---|---|---|---|---|---|---|
| rp04 | 406 | 5 | 19 | 20 | 47 | 19082 | 2 |
| rp06 | 812 | 3 | 19 | 20 | 47 | 38164 | 4 |
| rp07 | 627 | 3 | 32 | 43 | 172 | 107844 | 9 |
| rm03 | 820 | 3 | 5 | 30 | 18 | 14760 | 2 |
| rm80 | 556 | 3 | 14 | 30 | 52 | 28912 | 3 |

The spare cylinders are `XCYLS`, "extra cylinders for spares, hacks, etc." — they
are inside the image and outside `NBLKS`, which is why `info` reports both totals
and why the MFD, at `NBLKS/2-1`, is not in the middle of the *file*.

The size of a full image identifies the drive on its own: no two of these are the
same number of sectors. That is how `itsfs info` names a drive with nothing but
the file, and why `-d` exists for an image that is not a whole pack.
