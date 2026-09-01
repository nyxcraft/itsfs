# The ITS file system

The format itself: what is known, how it was established, and — at the end — a
register of what is still not known.

Every offset named here is in [`src/its.h`](../src/its.h) with its ITS symbol and
its evidence marker. This document is the prose; that file is the transcription.

## Vocabulary

| ITS calls it | it is |
|---|---|
| a **track** | a block. `disk.1228`: `TUT   TRACK (BLOCK) UTILIZATION TABLE` |
| a **block** | 8 sectors, 1024 words, 2000 octal words |
| the **MFD** | the master file directory: one block listing every user directory |
| a **UFD** | a user file directory: one block, one per directory |
| a **descriptor** | a file's block list, run-length coded in six-bit bytes |
| the **TUT** | the allocation table: three bits per block of the pack |
| a **name block** | a directory entry: five words |

A file is `DIR;FN1 FN2` — three SIXBIT names of at most six characters. There is
no extension in the DEC sense (`FN2` is used like one by convention and is not
one), and there is no nesting: the path depth is exactly one.

## The master file directory

One block, at `NBLKS/2-1` — the middle of the file area, which is a seek-time
decision from when that mattered a great deal.

```
word 0  MDNUM    an ascending directory number
     1  MDNAMP   where the name area starts
     2  MDYEAR   the current year
     3  MPDOFF   the de-Coriolis clock offset
     4  MPDWDK   preferred writing disk
     5  MDCHK    SIXBIT "M.F.D.", and it must be
     6  MDNUDS   how many directories this MFD has room for
```

From `MDNAMP` to the end of the block are two-word entries: a SIXBIT directory
name, and a zero. (FSDEFS notes that the second word being zero is depended on by
a "kludge" in `DECUUO`, and asks to be told before anyone changes it.)

**There are no pointers.** The *position* of an entry is the address of the
directory it names:

```
block = (A - 2000 + LMNBLK*NUDSL) / 2
```

with `A` the word index of the entry, 2000 octal the block size, `LMNBLK` 2 and
`NUDSL` the value in `MDNUDS`. `disk.1228` does it in two instructions:

```
QFL2:	SUBI J,2000-LMNBLK*NUDSL	;J <= TRACK ADDR OF USER DIR
	LSH J,-1
```

So the directories occupy blocks 0 through `NUDSL-1` at the very start of the
pack, and the name area is filled downward from the end of the MFD block: the
last slot is the highest-numbered directory. On the reference pack `MDNAMP` is
1022 octal (530), `MDNUDS` is 500, and 247 directories exist in the room for 500.

The MFD is a directory of directories only. It has no descriptors and no files —
but there is a directory *named* `.` (block 499), which is where the system's own
files live.

## A user file directory

One block, in the block the arithmetic above gives.

```
word 0  UDESCP   free pointer into the descriptor area
     1  UDNAMP   where the name area starts
     2  UDNAME   the directory's own name, for checking
     3  UDBLKS   lh: space allocated;  rh: blocks used
     4  UDALLO   lh: disk number;  rh: allocation
    11  UDDESC   the first word a descriptor may occupy
```

and then **two areas growing towards each other**: descriptor bytes up from word
11, five-word name blocks down from the end of the block, with `UDNAMP` marking
where the names currently start.

**A directory is one block and that is the whole capacity.** No growth, no
chaining, no continuation. 1013 words hold both areas, and the directory is full
when they meet. `t10fs` spends real effort growing a TOPS-10 UFD by a cluster and
matching DEC's convention for it; there is nothing here to match.

`UDBLKS`'s right half is a running total ITS maintains as files are written, and
it is the single most useful number on the pack for grading a reader: it is
computed by the monitor, stored separately from the descriptors, and can be
compared against what the descriptors decode to. On the reference pack all 247
directories agree exactly.

## A name block

Five words, and every one of them is packed.

```
word 0  UNFN1    first name, SIXBIT
     1  UNFN2    second name, SIXBIT
     2  UNRNDM   "all kinds of random info"
     3  UNDATE   creation date and time
     4  UNREF    reference date, author, byte size
```

`UNRNDM` is where the work is. FSDEFS gives each field as a PDP-10 byte pointer,
whose left half is `<position,,size>`:

| field | pointer | bits | meaning |
|---|---|---|---|
| `UNDSCP` | `1500,,` | 0–12 | where this entry's descriptor starts |
| `UNPKN` | `150500,,` | 13–17 | pack number |
| `UNLNKB` | `220100,,` | 18 | this entry is a link |
| `UNWRDC` | `301200,,` | 24–33 | words in the last block, **mod 2000 octal** |

`UNWRDC` being modular is what makes a file's length computable: a file of *n*
blocks has at least `(n-1)*1024` words, so a last block that is exactly full
recording zero is unambiguous.

`UNDATE` carries the year less 1900, the month and the day in its left half and a
"compacted time" in its right. The dates decode: files on the reference pack are
dated 1985 and 1986 where they came off MIT tapes, and 2026 where the build
wrote them.

## Descriptors — a file's block list is a program

`UNDSCP` is an offset **in six-bit bytes from word `UDDESC`** of the directory
block — not from word 0. That distinction is worth a paragraph because getting it
wrong is silent: 66 bytes early, in a well-formed directory, is zero, and zero
means "end of description", so every file appears to be empty and nothing
complains.

The bytecode, with `B` the current block number, undefined until loaded:

| byte | meaning |
|---|---|
| `0` | end of the description |
| `1`–`UDTKMX` (12) | take N blocks: `B` … `B+N-1`, then `B += N` |
| `13`–`30` | skip `N-UDTKMX` blocks and take one |
| `UDWPH` (31) | write place holder: a no-op |
| `040`–`077` | load address: `B = (N&037)<<12 \| N2<<6 \| N3`, take `B`, `B += 1` |

A load address is three bytes and carries **17 bits** of block number — 131,072
blocks, against an RP07's 108,360. A zero-length file is `UDWPH` then `0`, and a
legal description must load an address before it may use anything else.

Two things FSDEFS says about its own format that are worth repeating:

- The skip codes have been unreachable for years: *"ITS has been broken for years
  such that it never uses this UFD descriptor code!"* They are implemented
  anyway. A reader's job is to read what is there rather than what the writer
  meant.
- The "funny" bit that used to sit in a load address was removed on 19 August
  1990 to make room for RP07 block numbers, and FSDEFS says outright that any
  program interpreting descriptors *"needs to be fixed to not mask that bit out
  (as most of them currently do)"*. Nothing here masks it.

## Links

The same `UNDSCP` field, with the link bit set, points at the target's name
instead of a block list: three SIXBIT components — directory, first name, second
name — ended by a zero byte. A component shorter than six characters is ended by
`;`, and `;`, `:` and space are each quoted by a preceding `:`.

FSDEFS's own examples are the test cases, and it observes that the encoding
admits a lot of illegal or wasteful spellings of the same name, "which is
surprising since somebody was clearly trying to compress them into as few bytes
as possible".

**The trap:** FSDEFS writes those characters as `";" (73)` and `":" (72)`, which
are their **ASCII** codes, while the bytes on the disk are **SIXBIT** — 033 and
032. Taking the numbers at face value finds no separator anywhere and renders
every link as one run-on string. The real bytes, from a real pack:

```
16 33 40 33 44 44 64 00      ".;@;DDT"  ->  the file  .;@ DDT
```

Note that the same sentence in FSDEFS gives space as `(0)`, which *is* its SIXBIT
value. One sentence, two encodings.

## The TUT

`NTUTBL` blocks ending just below the MFD — so the two things a salvager needs
first are next to each other.

```
word 0  QPKNUM   pack number
     1  QPAKID   SIXBIT pack ID
     2  QTUTP    free-space search pointer
     3  QSWAPA   first block of the non-swapping area
     4  QFRSTB   first block mapped
     5  QLASTB   last block mapped
     6  QTRSRV   -1: only allocated directories may have files here;
                 otherwise the SIXBIT name of a "secondary" pack
    20  LTIBLK   (octal) where the map itself begins
```

Then **three bits per block**, twelve to a word, most significant first:

| value | meaning |
|---|---|
| 0 | free |
| 1 … `TUTMNY`-1 (5) | that many references |
| `TUTMNY` (6) | many or more references |
| `TUTLK` (7) | locked out |

It is a **reference count, not a bitmap**, which is a different thing to check:
the question is not "is this block allocated" but "is it referenced as many times
as something says". FSDEFS notes the entry used to be four bits (`;9/5/79 - tut
format changed!`), which is one of the two dated format changes in the file.

On the reference pack the counts are 0, 1 and 7 and nothing else — no block is
referenced twice. **A count of 2 has since been seen**, though not on a healthy
pack: halting ITS abruptly mid-write left 50 blocks at 2 that only one file
claimed, and NSALV described them exactly as `itsfs check` did (see
[validation](validation.md#a-third-kind-of-agreement-found-by-accident)). So a
count above 1 is reachable, and on that evidence it is a state to repair rather
than a feature to support — and the 505 locked-out blocks are exactly the 500 UFD blocks,
the MFD and the four TUT blocks.

## Reading a file

Decode the descriptor to a block list; the length in words is
`(n-1) * 1024 + (UNWRDC ? UNWRDC : 1024)`. Text is five seven-bit characters to a
word, most significant first, with bit 35 left over. ITS text uses CRLF line
endings; `itsfs cat` does not translate them, or anything else.

---

## Gap register

What is not known. Each is `[s]` in `its.h`, and none of it is guessed at in code.

**`UNBYTE`, byte size and odd-byte count.** FSDEFS gives four encodings by range:

```
400+100xS+C    S=1 to 3      C=0 to 35.
200+20xS+C     S=4 to 7      C=0 to 8
44+4xS+C       S=8 to 18.    C=0 to 3
44-S           S=19. to 36.  C=0
```

with `UNBYTE==0` meaning 36-bit bytes, and the note that "old files have
UNBYTE=0". Every file on the reference pack has zero, so three of the four
encodings have never been exercised and the field is read but not decoded.

**`UNDUMP`.** FSDEFS gives it as `400000`, which does not sit where the other
flags in that word sit. Not implemented, and not guessed at.

**The width of the flag field.** `UNLINK`, `UNREAP`, `UNWRIT`, `UNMARK` and
`UNCDEL` are given as values relative to the bit `UNLNKB` addresses. Their width
is not stated anywhere; six bits is what the values need and does not collide
with `UNWRDC` above them. Deduced, and marked as deduced.

**Padding in the last word is `^C`.** A file's length is kept in words, so a
character count that is not a multiple of five leaves slots at the end of the
last word. ITS fills them with 003, not with NUL — 100 of 131 text files sampled
on the reference pack pad that way, 2 with NUL, and the rest end exactly on a
word boundary and pad with nothing. It matters because *a reader stops at* `^C`:
MDL opens a NUL-padded file, prints its name and never evaluates it. `put` writes
`^C`; `cat` and `get` pass whatever is there through, since translating an
extraction is how an extraction tool lies.

**`UNAUTH`, the author.** "MFD index of author, all 1 => no directory" — and
the phrase "MFD index" is the ambiguity, because a directory has both a slot in
the MFD name area and a block its UFD occupies. **It is the block.** 6,050 of the
reference pack's 6,056 entries carry all ones and settle nothing, but six carry a
real value, and those six decide it: as slot indices they point at empty slots,
while as block numbers each names a directory whose identity fits the file --
`EMACS;TSTCH 1` authored by `TEACH`, `EMACS;TSINFO 63` by `INFO`, `SYS3;TS VIEW`
by `KMP`. `stat` resolves an author to its directory name.

**Multi-pack file systems.** `UNPKN` is a pack number in every name block, and
`QTRSRV` names a secondary pack. Clearly provided for; entirely unexercised here.
`t10fs` found two real bugs the first time it read a genuinely multi-unit
structure, and there is no reason to expect better.

**Version drift — narrowed, not closed.** `make version-diff` compares `FSDEFS
43` against `SYSENG;FSDEFS 40`, an earlier version preserved in the PDP-10/its
history: **all 71 symbols are identical**, and the 65 differing lines are prose.
So the format did not move across those three file-versions.

What is still open is where they sit. A file version is ITS's own numbering, not
a release number, and nothing maps 40 or 43 onto an ITS distribution. Both carry
`;9/5/79 - tut format changed!`, which puts a floor under the span — the 3-bit
TUT entry read here is the post-1979 one — and nothing puts a ceiling on it. A
`FSDEFS` from before 1979 would be the interesting one, and none is in the tree.

One semantic difference survives with no symbol attached: version 40 documents
the 020 bit of a load address as a `"FUNNY" BLOCK IF DMDSK`, and 43 says it is
flushed. This reader does not mask it, per 43. See
[sources](sources.md#how-far-the-transcription-reaches).

**The `DM` disks.** FSDEFS's removed "funny" bit was conditional on `DMDSK`, a
disk type this project has never seen. Whether such a pack would read is unknown.
