# itsfs

Host tools for the **ITS file system** — the disk MIT's Incompatible Timesharing
System ran, read on a machine that has never heard of a 36-bit word.

> **Status: phases 0–7 done — a reader, an independent checker, a writer, and
> ITS's own salvager accepting what the writer produces.** The word layer is proven byte-for-byte
> against a real RP06 pack, the geometry layer finds the master file directory by
> its own check word, and the reader lists directories, decodes the run-length
> descriptors that are ITS's block maps, follows links and extracts files.
> `itsfs check` walks the same pack **sharing no code with the reader**, and
> **`NSALV` — MIT's own salvager, booted from tape — names exactly the same
> damaged blocks and the same files.** Every field offset is transcribed from
> `SYSTEM;FSDEFS 43` with a citation ([sources](docs/sources.md)). `put` and
> `del` are the first destructive commands, and every word they change goes
> through one file. See [the roadmap](docs/roadmap.md),
> [validation](docs/validation.md) and [PLAN.md](PLAN.md).

```console
$ itsfs info rp0.dsk
file          rp0.dsk
size          317132800 bytes
packing       le64 (confirmed) -- one word per 64-bit little-endian container
word          36 bits, 1 per 8 bytes
words         39641600
drive         rp06 (SYSTEM;RP06 DEFS 1)
geometry      812+3 cylinders, 19 surfaces, 20 sectors/track, 8 sectors/block
blocks/cyl    47   (4 sectors per cylinder are unreachable -- 3260 on the pack)
blocks        38164 in the file area, 38305 including the spare cylinders
MFD           block 19081
TUT           blocks 19077..19080
MDCHK         551646164416 = SIXBIT |M.F.D.|  -- an ITS master file directory
directories   247, in an MFD with room for 500 (MDNUDS)

$ itsfs ls -l rp0.dsk KSHACK | head -4
-READ- -THIS-       116      1  30-dec-1985
1PROC  BUGS          23      1   1-jun-1986
AINOTE 8           1676      2  24-oct-1986
DDT    BIN        link          27-jun-2026  -> .;@ DDT
```

## Why a separate project

This is the third in a family — [`s5fs`](https://github.com/nyxcraft/s5fs) reads
UNIX V7/2.xBSD, [`t10fs`](https://github.com/nyxcraft/t10fs) reads TOPS-10 — and
the architecture is deliberately the same. ITS is not, in four ways that shape
the design rather than fitting inside it:

- **A block number is not an offset.** ITS numbers blocks *within a cylinder*,
  and the count per cylinder is an integer division that usually has a
  remainder — so four sectors of every RP06 cylinder are reachable by no block
  number at all. There is a geometry layer here that neither sibling needs.
- **Free space is a reference count**, three bits per block, not a bitmap and not
  a chain. Zero is free, seven is locked out, and everything between is how many
  things point at that block.
- **A directory is exactly one block.** Descriptors grow up from word 11, name
  blocks grow down from the end, and when they meet the directory is full. There
  is no growth and nothing to chain.
- **A file's block list is a program** — a run-length bytecode in six-bit bytes
  with a load-address escape — and a link's "block list" is the target's name in
  the same field, distinguished by one bit.

## Commands

| command | what it does |
|---|---|
| `itsfs info` | describe an image: packing, drive, geometry, and whether the MFD is there |
| `itsfs dump` | print blocks (or raw sectors) as 36-bit words — octal, halfwords, SIXBIT, ASCII |
| `itsfs packings` | the word packings, and how well each one is established |
| `itsfs drives` | the five drives ITS supported, and the geometry each implies |
| `itsfs repack` | rewrite an image word for word in another packing |
| `itsfs sixbit` | encode and decode SIXBIT, with no image involved |
| `itsfs dirs` | list the directories in the MFD — `-l` also opens each one |
| `itsfs ls` | list a directory: `ls rp0.dsk KSHACK` |
| `itsfs cat` | print a file as text |
| `itsfs get` | copy a file out to the host — text, or `-w` for its 36-bit words |
| `itsfs free` | what the TUT says: free, in use, locked out, and the reference counts |
| `itsfs check` | check a pack — **shares no code with the reader** |
| `itsfs manifest` | fingerprint a pack: one line per directory, file and link |
| `itsfs verify` | diff a pack against a manifest |
| `itsfs shell` | interactive explorer — `cd`, `ls`, `type`, `blocks`, `stat` |
| `itsfs put` | write a host file into a directory — **destructive** |
| `itsfs del` | remove a file — `rm` is the same command — **destructive** |

Run any of them with no arguments for its own usage.

### Names

ITS writes a file name as `DIR;FN1 FN2` — a directory and two six-character
SIXBIT names, with no extension in the DEC sense and no path deeper than one.
The space in the middle has to be quoted for the shell, so every command that
takes a name also takes it as three arguments:

```console
$ itsfs cat rp0.dsk 'KSHACK;BUILD DOC'
$ itsfs cat rp0.dsk KSHACK BUILD DOC
```

Both spellings go through one parser. A name that cannot be SIXBIT is refused,
never truncated and never case-folded: SIXBIT has no lower case, and quietly
mapping `hello` to `HELLO` would match a different file.

### Writing

```console
$ itsfs put rp0.dsk 'KSHACK;ITSFS TXT' hello.txt
$ itsfs del rp0.dsk 'KSHACK;ITSFS TXT'
```

**These change the pack. Work on a copy.** `itsfs` refuses to write an image
another process has open — an emulator with the pack attached is writing to it
too and there is no lock to take — comparing by device and inode, overridable
with `ITSFS_IGNORE_INUSE=1`. Reading is never blocked.

Every store goes through [`src/write.c`](src/write.c), which is the only code in
the project that mutates a pack. Three rules it enforces, each of which cost
somebody a pack once in a sibling project:

- **Refuse, do not half-do.** Every check that can fail happens before anything
  is written. A refusal leaves the pack byte-identical, and says the number it
  refused at.
- **The directory entry goes last** — data, then the allocation table, then the
  descriptor, then the name. An interruption before the last step leaves blocks
  marked in use that no file claims, which loses nothing. The other order loses
  a file.
- **Never write a pack somebody else has open.**

`put` inserts the name in **sorted position**, because an ITS name area is
sorted — 6,056 entries on the reference pack, not one out of order — and shifts
the entries below it down, which is the mirror of the `QSQSH` that ITS uses to
close the gap on delete. `del` frees the blocks and zeroes the descriptor bytes
in place, exactly as `QDEL3` does; it does not compact the descriptor area,
because ITS does not either.

What it refuses, by name rather than half-doing: a name SIXBIT cannot hold, a
file that already exists, a byte that is not seven-bit (use `-w`), a directory
that is full — **a UFD is one block and there is no way to grow one** — and a
block the allocation table calls "many or more", whose reference count cannot be
decremented correctly by anybody.

### The shell

```console
$ itsfs shell rp0.dsk
rp0.dsk, rp06, 247 directories.  `help` lists the commands.
- (ro)> cd KSHACK
KSHACK (ro)> blocks NSALV 261
KSHACK;NSALV 261: 43 blocks, 43087 words
  16266..16308 (43)
  1 run
KSHACK (ro)> quit
```

`cd`, `ls`, `pwd`, `type`, `blocks`, `stat`, `free` and `info`. It reads commands
from standard input, so it scripts — which is also what makes it testable.

`blocks` prints **runs** rather than a list of numbers, because a descriptor is
run-length coded and where a file is fragmented is the question worth asking. On
the reference pack 5,431 of 5,650 files are a single run and one is 59 of them.
`stat` prints the raw `UNRNDM`, `UNDATE` and `UNREF` words alongside the decoded
fields, including the ones nothing here interprets — a stat that showed only the
understood fields would hide exactly what somebody investigating an unknown one
needs to see.

**Read-only, and not because of a flag**: there is no writer in this project. The
prompt says `(ro)` so that the day there is one, the difference is visible rather
than assumed.

### Common options

`-p <packing>` selects the word packing (default `le64`, which is what SIMH disk
images use). `-d <drive>` names the drive when the image is not a whole pack —
normally it is identified from the size. A word value on the command line is
**octal by default** — this is a PDP-10 — with `0x` for hex, `d:` for decimal,
`lh,,rh` for the two 18-bit halves, and `sixbit:NAME` for text. A *block number*
is decimal by default, because that is how ITS writes block numbers; `0` or `0o`
in front asks for octal.

## Build

```console
$ make                  # bin/itsfs, no dependencies beyond C99 + POSIX
$ make lint             # 16 warning options, and clang-format
$ make test             # the regression suite (sh + coreutils), 179 checks
$ make test-san         # the same suite under ASan + UBSan
$ make fuzz             # optional corruption fuzzer (needs python3)
$ make oracle IMAGE=... # everything above, against a real pack
$ make nsalv  IMAGE=... # ...and hand that pack to ITS's own salvager
$ make version-diff     # how far the transcription reaches, across two FSDEFS
```

## What is proven, and what is only claimed

Keeping those two apart is the whole discipline of this project.

**Proven.** Every one of the 39,641,600 words of a real 300 MB RP06 ITS pack
decodes and re-encodes byte-for-byte identically, and survives a round trip
through two packings with different strides:

```console
$ make oracle IMAGE=~/its/out/simh/rp0.dsk
IDENTICAL: the word layer round-trips the real pack
IDENTICAL: cross-packing round trip
IDENTICAL: and through the packing that shares a byte
```

That is a stronger result than it looks: because every word is masked to 36 bits
on the way in, byte identity over the whole image also proves that nothing
anywhere on the pack sets one of the 28 bits outside the word.

**Measured, not assumed.** A sector is 128 words and a block is eight of them; an
RP06 cylinder holds 47 blocks and not 47.5; the MFD is at `NBLKS/2-1` and says so
itself in SIXBIT. See [on-disk format](docs/on-disk-format.md), which lists the
evidence for each.

**Sourced, and checked against the artifact.** Every field offset in
[`src/its.h`](src/its.h) is transcribed from `SYSTEM;FSDEFS 43`, in the base the
source writes it in, with ITS's own symbol named and a marker saying whether the
field has actually been seen decoding correctly on a real pack. `make
version-diff` checks that every symbol the header cites is really defined there,
and compares the file against an earlier version preserved in the ITS history:
**all 71 definitions are identical**, so the format did not move across those
three file-versions. See [sources](docs/sources.md#how-far-the-transcription-reaches).

**The space on a real pack accounts for exactly**, three ways, against numbers
this project did not compute:

```console
1. 247 directories walked, 247 whose descriptors agree with their own UDBLKS
2. 30940 blocks described by files == 30940 blocks the TUT calls in use
3. 505 blocks locked out == 500 directories + 1 MFD + 4 TUT blocks
```

Each line is a different structure, maintained by a different part of the
monitor, agreeing with the descriptor decoder. The first is 247 separate
comparisons; the second would catch a single block skipped or double-counted
anywhere on the pack; the third is the only check there is on the MFD-slot
arithmetic, which has no pointer to verify it against. See
[validation](docs/validation.md).

**Written, and ITS's own salvager accepts the result.** This is the level of
evidence the project's own taxonomy calls "accepted by native tools", and it is
a stronger claim than the checkers agreeing about a pack neither of them wrote:

```console
$ make nsalv IMAGE=~/its/out/simh/rp0.dsk
4. a pack itsfs WROTE to
  ok   itsfs put wrote 2 files
  ok   ...and both read back byte-identical to what went in
  ok   itsfs check: clean
  ok   NSALV: ACCEPTED IT -- salvaged, and had nothing to say
  ok   ...and the files are still there afterwards
```

`put` then `del` leaves the file system exactly as it was, verified by
fingerprint rather than by inspection — not byte-identical, because the
descriptor area keeps its hole as it does under ITS, but nothing a reader or a
checker can see has changed.

**Fingerprinted, and the fingerprint is of the file system rather than of the
file.** `itsfs manifest` records every file's name, length and content checksum;
`verify` diffs a pack against one. The checksum is over 36-bit **words**, not
bytes — because `repack` rewrites every byte of an image and no word of it, so a
manifest taken from a 317 MB `le64` pack verifies clean against the same file
system stored as 178 MB of `dbd9`:

```console
$ itsfs manifest rp0.dsk > ref.mf
$ itsfs repack -P dbd9 rp0.dsk rp0.d9
$ itsfs verify -p dbd9 rp0.d9 ref.mf
0 differences
```

`make oracle` does that over all 6,303 directories, files and links of the
reference pack. One flipped bit anywhere in 39.6 million words is reported, by
name:

```console
$ itsfs verify rp0.dsk ref.mf
! SYSTEM;IMPOLD NCP2 (contents differ)
1 difference
```

**And one piece of evidence that is not the pack agreeing with itself.** The ITS
source tree builds its own disk — a host file goes through `itstar`, a tape, and
ITS's own loader before the monitor writes it — so for a file that exists in both
places the host copy is an independent original:

```console
cross-check: 137 files byte-identical to their host originals, 0 differ,
             40 skipped as escaped by itstar, 0 not on the pack
```

Every layer is on that path: the geometry found the right blocks, the descriptor
decoded them in the right order, `UNWRDC` gave the right length for the last one,
and the seven-bit extraction is right. It also found a real bug — `cat` was
dropping NULs from the *middle* of a file, not just the padding at the end, which
nothing the pack knows about itself could ever have caught.

**Checked by something that shares no code with the reader.** `itsfs check`
includes neither `structure.c` nor `itsgeom.c`'s conversion: it re-derives the
block-to-sector arithmetic, the MFD-slot formula, the descriptor bytecode and
the TUT from `its.h` and walks the pack itself. It compares the allocation table
against the files **per block and as a count**, because the TUT is a reference
count rather than a bitmap, and it separates the four ways they can disagree — a
block a file holds that the table calls free loses data, a block marked in use
that no file holds only loses space, and the checker says which:

```console
$ itsfs check rp0.dsk
no problems found (7 notes)

$ itsfs check damaged.dsk          # one TUT word cleared
  block 19992 is claimed by C;PHASE ARGS, and the TUT calls it FREE
  block 19993 is claimed by MAINT;DEKAG SAV, and the TUT calls it FREE
  ...
11 problems, 7 notes
```

The seven notes are links pointing at files that are not there, on a pack that
is otherwise perfect. That is an ordinary state of a live system and not damage,
so it is counted, listed under `-v`, and never changes the exit status.

**And ITS's own salvager agrees with it.** `NSALV` was written at MIT in the
1980s, boots standalone from a tape, and rebuilds the allocation table from the
directories. Handed the same damaged pack, it names the same eleven blocks and
the same eleven files:

```console
$ make nsalv IMAGE=~/its/out/simh/rp0.dsk
   itsfs check names 11, NSALV names 11
  ok   IDENTICAL: the same 11 blocks, each held by the same file

       19992 C;PHASE ARGS
       19993 MAINT;DEKAG SAV
       19994 EMACS;EINIT :EJ
       ...
two checkers with nothing in common but the disk, agreeing block for block
```

This is the check that closes the gap `itsfs check` cannot: both of *our* readers
take their constants from `its.h` and inherit any misreading in it, while `NSALV`
predates this project by forty years. Where it agrees, the agreement is about the
format rather than about our reading of it. It also needs no writer, which is why
it runs now rather than in phase 8 — a pack this project has only read is enough
to ask the question. See [validation](docs/validation.md#a-second-opinion-that-is-not-ours).

**Hostile input is bounded.** 179 checks in the suite, a third of them feeding the
reader a pack damaged on purpose, and a corruption fuzzer that has run 1,750
commands over damaged packs under ASan and UBSan without a finding. The bar is
not that the reader survives — it is that it refuses *by name* and reads nothing
it did not bound first.

**What is NOT proven.** `NSALV` has accepted what the writer produced, but **ITS
itself has never been booted on a pack this project wrote** — a salvager checks
the bookkeeping, and a monitor opening the file is a further claim. `mkdir` and
`mkfs` do not exist, so every write so far has been into a directory ITS made.
`NSALV` was also asked about only one kind of damage, a cleared TUT word. No ITS magtape has been read, which is why `core` and `dbd9` are
`corroborated` rather than `confirmed` here even though `t10fs` confirmed both;
the reference pack is one built from source in 2026, not an artifact recovered
from MIT; and the version span has a floor and no ceiling — both `FSDEFS`
versions compared are after the 1979 TUT change, and nothing here maps a file
version onto an ITS release. Each of those is a phase on
[the roadmap](docs/roadmap.md), and until it is done this file does not claim it.

## Documentation

- [The file system](docs/filesystem.md) — **the format itself**, and a gap register of what is still unknown
- [Design](docs/design.md) — the layers, the invariants, and what is out of scope
- [Geometry](docs/geometry.md) — why a block number is not an offset
- [Word packing](docs/word-packing.md) — why the bottom layer is not a byte-order codec
- [On-disk format](docs/on-disk-format.md) — confirmed facts, observations, and what the source settles
- [Sources](docs/sources.md) — where every constant comes from, and two traps worth knowing
- [Validation](docs/validation.md) — how correctness is established here, and what has been
- [Roadmap](docs/roadmap.md) — the phases, and what ends each one
- [The plan](PLAN.md) — written before any code
- [Handoff](HANDOFF.md) — for whoever picks this up next

## Copyright

`itsfs` is clean, original C. The ITS source tree, the disk images built from it
and the emulators used to validate against it are usable locally and **are never
committed here**. Test fixtures are built by the test suite, never copied off a
pack.

Licensed under the terms in [LICENSE](LICENSE).
