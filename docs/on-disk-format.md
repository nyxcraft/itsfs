# On-disk format — confirmed, observed, sourced

Three columns of knowledge, kept scrupulously apart:

- **Confirmed** — measured on a real pack, by a procedure written down here.
- **Observed** — seen on a real pack before any theory existed to explain it.
- **Sourced** — stated by `SYSTEM;FSDEFS 43`, and separately whether anything
  here has exercised it.

The pack throughout is `out/simh/rp0.dsk`, an RP06 built and booted by the
PDP-10/its Makefile: 317,132,800 bytes, 39,641,600 words.

## Confirmed

**A word is 36 bits, stored one per 8-byte little-endian container.**
`itsfs repack` decodes all 39,641,600 words and re-encodes them; the result is
byte-identical to the original. Because every word is masked to 36 bits on the
way in, that also proves no word on the pack has a bit set above the 36 — 28 bits
× 39.6 million words of the file that are not storage.

**A sector is 128 words; an ITS block is 8 of them, 1024 words.**
Two independent confirmations: `SECBLK==8` in every drive parameter file, and
`UNWRDC` being the last block's word count *mod 2000 octal*, which is 1024
decimal. A ten-bit field would not describe a block of any other size.

**Blocks are numbered within a cylinder, and the count per cylinder truncates.**
`NBLKSC = NHEDS*NSECS/SECBLK` is 47 for an RP06, not 47.5. Confirmed by the MFD:
the formula puts block 19081 at linear sector 154268, and word 5 of that sector
is SIXBIT `M.F.D.`. Linear block numbering puts it 3,244 sectors away. See
[geometry](geometry.md).

**The MFD is at `NBLKS/2-1`, and identifies itself.** `MDCHK` is SIXBIT
`M.F.D.` = `551646164416` octal. This is the one place in the format that exists
purely so a program can ask "is this the thing I think it is", and `itsfs`
refuses to read an MFD without it.

**A directory's block is the position of its MFD entry.** All 247 directories on
the pack resolve through `(A - 2000 + 2*NUDSL)/2`, and every one of them has a
`UDNAME` in its own header matching the name in the MFD slot that addressed it —
247 independent agreements about an arithmetic with no pointer to check.

**The descriptor bytecode.** Decoding every file in every directory produces, per
directory, exactly the block count `UDBLKS` records — a number the monitor
maintains separately as files are written. 247 directories, 247 agreements.

**The TUT is a three-bit reference count per block.** Its map accounts for the
whole pack, and the totals agree with the descriptors: 30,940 blocks in use, and
30,940 blocks described by files. Its 505 locked-out blocks are exactly 500 UFD
blocks + 1 MFD + 4 TUT blocks.

**Link targets are stored in SIXBIT, with SIXBIT separators.** Confirmed by the
bytes: `16 33 40 33 44 44 64 00` is `.;@;DDT`, the link `.;@ DDT`. See the trap in
[sources](sources.md#two-traps).

## Observed

Things seen on the pack, recorded before they were understood, and some of them
still not:

- The TUT's reference counts on this pack are only 0, 1 and 7. **No block is
  referenced twice**, so the "many or more" value and the counts between have
  never been seen in the wild here. A reader must still handle them; a *checker*
  must not assume a second reference is corruption until it is known what makes
  one.
- 72 blocks *inside* the swapping area (below `QSWAPA`) are in use by files.
  `QSWAPA` is documented as "new files will not be written lower than this", which
  is a rule about writing rather than a rule about what is there.
- Every `UNAUTH` on the pack is all ones — "no directory" — so nothing has ever
  resolved an author index.
- Every `UNBYTE` on the pack is zero, meaning 36-bit bytes, including on files
  that are plainly seven-bit text. Three of the four `UNBYTE` encodings have
  therefore never been exercised.
- File dates split cleanly into two eras: 1985–1987 for files that came off MIT
  tapes, and 2026 for files the build wrote. That is what makes the date decoding
  self-evidently right; both halves are sane at once.
- The directory named `.` (block 499) holds the system's own files. It is a
  perfectly ordinary UFD.

## Sourced

Stated by FSDEFS. The full transcription with evidence markers is
[`src/its.h`](../src/its.h); what follows is which parts have been *exercised*.

| area | sourced | exercised |
|---|---|---|
| MFD header words | all seven | five of seven; `MPDOFF`, `MPDWDK` read but not interpreted |
| MFD name area | yes | yes |
| UFD header words | all five | four of five; `UDALLO` read but not interpreted |
| name block words | all five | yes |
| `UNRNDM` fields | four byte pointers | all four |
| `UNDATE` fields | four byte pointers | three; `UNTIM`'s unit is unknown |
| `UNREF` fields | three byte pointers | one; `UNAUTH` all-ones everywhere, `UNBYTE` zero everywhere |
| flag bits | five values | one (`UNLINK`); the field's width is deduced |
| descriptor opcodes | five kinds | four; the skip codes are unreachable in ITS itself |
| link encoding | with five examples | yes, and its documentation is wrong about the base |
| TUT header words | all seven | five of seven; `QTRSRV` and the secondary-pack idea untouched |
| TUT map | yes | yes |

## The DUMP save set, which is a tape rather than a disk

Not a disk format at all, but it belongs beside one, and it is the best-attested
thing in this project: `itsfs save` writes a tape byte-identical to one ITS's own
DUMP writes (`make itsdump`).

A save set is a SIMH `.tap` of records in `core` packing — five frames to the
word. It opens with a four-word volume header, and then each entry is a header
followed by its data. **The header carries its own length** as an AOBJN pointer
in its first word: `1000000 - n` in the left half. That is what lets tapes of
different ages be read by one reader, and it is the reason a wrong length is
invisible — every reader believes the pointer.

The layout is `HBLK` in `syseng/dump.449`, transcribed with its own comments:

| word | name | a file | a link |
|---|---|---|---|
| 0 | — | AOBJN `1000000-8` | AOBJN `1000000-8` |
| 1 | `HSNM` | sys name (the directory) | same |
| 2 | `HFN1` | first name | same |
| 3 | `HFN2` | second name | same |
| 4 | `HPKN` | *link flag,,pack number* — zero | the target's directory, SIXBIT, whole |
| 5 | `HDATE` | creation date, in disk format | `400000,,0` |
| 6 | `HRDATE` | `UNREF` entire: reference date, author, byte size | `777777,,777000` |
| 7 | `HLEN` | length of the file in words | `3` |

The data that follows is the file's words, or for a link the three words of its
target as FN1, FN2, SNAME.

Two things about this table are worth knowing rather than deducing.

**Words 6 and 7 are younger than the rest.** The source marks them: *"Next two
added 7/14/89 by Alan"*. So a pre-1989 tape has a six-word header, which is why
`itstar` accepts six or seven and why writing seven here went unremarked for nine
phases — every reader took the pointer at its word. Only comparing against a tape
ITS wrote showed it.

ITS's own format documentation confirms that, by being older than the change.
`doc/sysdoc/dump.format` in the PDP-10/its tree describes the header as
**six words** — the AOBJN pointer, directory, FN1, FN2, "disk pack number where
file was", and "creation date of file" — and stops. That is `HBLK` through
`HDATE` exactly, with `HRDATE` and `HLEN` simply not yet existing. Two
independent artifacts, the document and the source comment, agreeing about when
this format grew.

It says the same thing about links from the other side: *"the left half of the
pack number is non-zero and the data of the file consists of three words,
containing the sixbit file name the link points to"* — which is `HPKN`'s left
half as the flag, and the three words that `HLEN` counts in the modern form.

**A link's four odd values are constants, and the source names each one.**
`MOVE A,[SETZ] / MOVEM A,HDATE ;CREATION DATE OF LINK IS 400000,,0` — `SETZ` is
opcode 400, so the literal's address *is* that word. `HRROI A,777000` builds
`777777,,777000`, commented *"Unknown reference date / Unknown author, 36. bit
bytes"*. And `MOVEI A,3 / MOVEM A,HLEN ; Length of link is always 3`.

**And DUMP does not write links unless asked.** `DMPLNK: 0 ;-1 => DUMP LINKS`,
reachable as the `LINKS` switch — which the program's own help lists and the
pack's `.INFO.;DUMP INFO` does not.

## The home block, which is not this format

Blocks 0 and 1 of a bootable pack are not part of the ITS file system. They hold
the KS10 home block, and the reason to write it down here is that anyone reading
a pack will meet it before anything else:

| word | contents |
|---|---|
| 0 | SIXBIT `HOM` |
| 0103 (67.) | the front-end file system's directory address |

— that 128-word sector replicated across the whole block, and the whole block
written twice, to blocks 0 and 1. Sixteen copies in all. Written by `NSALV`'s
`FESET` command, and matched exactly on the reference pack, where word 67 reads
`007700,,000004`.

It explains a constant in the writer that would otherwise look arbitrary:
`mkdir` will not place a directory below block 2.

What the address points at is a *different file system*, for the KS10's 8080
console, with its own format and its own tool (`KSFEDR`). This project neither
reads nor writes it, and `mkfs` writes no home block either — a pack that
advertised a front-end file system it did not have would be worse than one that
makes no claim.

## What would change these columns

- **A pack recovered from MIT**, rather than built from source, would exercise
  fields nothing has written since the 1980s — `UNBYTE`'s other three encodings
  above all.
- **An ITS magtape** promoted `core` from `corroborated` to `confirmed`, and
  KLH10's own `vdkfmt` did the same for `dbd9`.
- **A multi-pack file system** would exercise `UNPKN` and `QTRSRV`, which are
  currently read and ignored.
- **A second FSDEFS**, from another ITS release, would turn the version-span
  question from an open one into a measured one.
