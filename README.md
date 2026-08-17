# itsfs

Host tools for the **ITS file system** — the disk MIT's Incompatible Timesharing
System ran, read on a machine that has never heard of a 36-bit word.

> **Status: phases 0–9 done. ITS opens a file this project wrote and prints
> it.** The word layer is proven byte-for-byte
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
| `itsfs mkdir` | make a directory — **destructive** |
| `itsfs mkfs` | create a file system from nothing — **destructive** |
| `itsfs tape` | SIMH `.tap` record framing — the container, not the archive |
| `itsfs saveset` | list or extract an ITS DUMP save set — the archive over it |
| `itsfs save` | write a DUMP save set from files on a pack |

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
$ make test             # the regression suite (sh + coreutils), 264 checks
$ make test-san         # the same suite under ASan + UBSan
$ make fuzz             # optional corruption fuzzer (needs python3)
$ make oracle IMAGE=... # everything above, against a real pack
$ make nsalv  IMAGE=... # ...and hand that pack to ITS's own salvager
$ make interop IMAGE=... # ...and boot ITS on one this wrote
$ make interop-klh10 IMAGE=... # ...and do it again on a second emulator
$ make mkfs-test        # build a pack from nothing; ITS grades it
$ make tape-test        # read a real ITS tape (no emulator needed)
$ make klh10 IMAGE=...  # dbd9 against KLH10's own converter (no emulator either)
$ make itsdump IMAGE=... # ITS's own DUMP writes a tape; cmp it with ours
$ make itsload IMAGE=... # ...and ITS's own LOAD reads one we wrote
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

**A file that went into ITS one way and came out another, unchanged.** `itsfs
save` writes a DUMP save set, and `itstar` — the reader the PDP-10/its project
uses — reads it, extracts it, and gets back the host file ITS was originally
given:

```console
4. a save set this project wrote, read by itstar
  ok   ITSTAR READS IT: 2 entries, and the volume header
  ok   ...and extracts the file
  ok   ROUND TRIP: byte-identical to the host file ITS was given
  ok   ...and the link is a link: -> _/@.ddt
```

`kshack/its.15` went *into* ITS as a host file — itstar packed it onto a tape and
ITS's loader wrote it to the pack. It comes back out by a different road: our
reader, our save-set writer, itstar's extractor. Byte-identical, with its 1986
date intact and its link still a link. That is two independent implementations
and an operating system in the loop, and no part of the path is this project
agreeing with itself.

**And DUMP save sets, checked against `itstar`.** `itsfs saveset` reads the
archive layer over that container — a volume header, a header per file, and the
data up to a tape mark. On a 91 MB ITS source tape it lists **3,795 entries,
name for name and in the same order as `itstar`**, the reader the PDP-10/its
project uses for these.

That comparison earned its keep immediately. The first version listed 3,734 —
exactly one short per link — because a link's target can share its header's
record, and reading a fresh record for it swallowed the *next file's header*. The
listing looked entirely plausible; only an oracle catches that.

**And the magtape packing, confirmed against ITS's own words.** The salvager tape
is 79,890 bytes — a whole multiple of five and *not* of eight, so it cannot be
one word per eight bytes. Decoded as five frames per word and read as seven-bit
characters, it contains `Salvager`, `Use MFD from unit` and `unprotected in old
TUT`: three strings this project has watched NSALV print on a console, the last
of them about a pack it damaged on purpose. Text that came out of the emulated
machine, found by this decoder in the file the machine loaded it from. And
`itsfs tape -x` extracts both files on that tape and re-encodes them to the host
originals byte for byte.

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

**And a file system with no ITS in it at all.** `itsfs mkfs` follows NSALV's own
`MFDINN`, `TUTINI` and `MARK69`: a master file directory, an allocation table
and 500 empty directory blocks. Everywhere else in this project the starting
point is a pack ITS built; here every word is ours, and both graders are booted
from tape and pointed at it:

```console
$ make mkfs-test
1. build a file system where nothing was
  ok   itsfs check: clean, with nothing in it
  ok   ...and 505 blocks locked out: 500 directory slots + 1 MFD + 4 table

3. hand it to NSALV, ITS's own salvager (booted from tape)
  ok   NSALV: ACCEPTED IT -- salvaged, and had nothing to say
  ok   ...and read the pack ID we wrote: ID is ITSFS, Pack #0

4. and to DSKDMP, a third implementation (also from tape)
  ok   DSKDMP listed a directory on a pack with no ITS in it
```

A pack `mkfs` makes **does not boot**, and that is correct rather than a
shortcoming: ITS starts from the front end's blocks at the very bottom of the
disk — the ones NSALV's own `ZAP` calls the "8080 HOM sectors" and refuses to
touch — and then loads a system out of a directory. `mkfs` writes a *file
system*; the boot area and the system are somebody else's job. Which is why both
graders come off tape.

**And ITS opens it and prints it.** The last claim, and the one the whole project
was built toward:

```console
5. ...and the monitor opens the file and prints it
  ok   removed the job that owns the console (itsfs del)
  ok   THE MONITOR PRINTED A FILE THIS PROJECT WROTE
       :print KSHACK;ITSFS TXT

       HELLO FROM ITSFS.
       IF ITS LISTS THIS THE ENTRY IS REAL.
```

Getting a console out of a running ITS was the obstacle, not the file system, and
**`itsfs del` is what removed it**: the finished system auto-starts the job
`SYS;ATSIGN DRAGON` names, and that job owns the console — while it is there `^Z`
produces nothing at all, forever. Deleting the link frees the terminal. This
project's own writer clearing the way for its own reader to be graded by ITS.

**And ITS boots on it.** `make interop` writes a file, **makes a directory from
nothing and writes into that**, then hands the pack to the two ITS programs that
*use* a file system rather than inspect one:

```console
2. DSKDMP, ITS's standalone loader, reads the directory
  ok   DSKDMP listed our file, reading the directory with its own code
  ok   ...and the whole listing is still in order (40 entries)

  ok   DSKDMP read a directory THIS PROJECT MADE, and listed its files
  ok   ...all three, in order

4. ...and then the monitor itself boots on it
  ok   the monitor ran its startup salvage over the pack
  ok   ITS CAME UP on a pack itsfs wrote to
  ok   ...and the file is still there, unchanged, after ITS had the pack
  ok   ...and so is the one in the directory we made
```

`DSKDMP` is a **third** implementation of this format — standalone, sharing
nothing with the monitor or with `NSALV` — and it lists our file *in the right
place*, which is the check that catches a writer that appended instead of
inserting. It also reads a directory this project created: the MFD entry, the
block it resolves to — which in this format is the entry's own **position**, with
no pointer anywhere to check it against — and the UFD header in it are all ours,
and DSKDMP resolves them with its own arithmetic. The monitor runs a salvage pass over every directory before it will
come up at all, so reaching `IN OPERATION` means that pass found nothing to stop
for.

**And it does it again on a second machine.** `make interop-klh10` asks the same
questions of KLH10 — an unrelated emulator, by a different author, reading a
**different packing**, since its ITS config is `dbd9` where SIMH's is `le64`. So
the two runs share only the two things being tested: the file system, and ITS.

```
:print KSHACK;K10TST TXT

ITSFS WROTE THIS ON A DBD9 PACK AND KLH10 PRINTED IT
```

That is worth the trouble because a fault in SIMH's RP06 lining up with a fault
in our geometry would look exactly like success under SIMH alone. What it is
*not* is two implementations of ITS — there is only one, and both machines run
the same `SYS;ITS` off the same pack.

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

**Hostile input is bounded.** 264 checks in the suite, a third of them feeding the
reader a pack damaged on purpose, and a corruption fuzzer that has run 5,400
commands over damaged packs and damaged tapes under ASan and UBSan without a
finding. The bar is
not that the reader survives — it is that it refuses *by name* and reads nothing
it did not bound first. Six of those commands per iteration are `put`, `del` and
`mkdir`: the writer is fuzzed on damaged packs too, each on its own copy, and
`check` must still be able to read what it left behind. Five more are `tape` and
`saveset` over a damaged `.tap` — the input most likely to have come from
somewhere else — with its record-length fields corrupted on purpose.

**Byte-identical to what ITS writes.** `make itsdump` boots ITS, runs its own
`DUMP` to write a save set to tape, writes the same files with `itsfs save`, and
compares: equal over all 2,667,188 bytes. It took three fixes to get there, and
none of them was visible to any test that existed — the file header is eight
words and we wrote seven, `UNREF` is copied whole and we were zeroing the author
out of it, and an unknown date is all ones where we wrote zero. Two independent
readers had round-tripped all of it perfectly for nine phases. A round trip
cannot see a field neither end uses; only the original disagrees.

**Links too, and finding them took reading ITS's source.** Two dumps came back
with no link entries at all, which looked like "DUMP does not write links". It
does — `DMPLNK: 0 ;-1 => DUMP LINKS` is a switch, and the program's own help says
`LINKS   Dump links as well as files`. The pack's `.INFO.;DUMP INFO`, where the
recipe came from, never mentions it. With the switch on, a link's header is the
same eight words as a file's with four values changed, every one of them named in
`syseng/dump.449` — and with those, our tape matches ITS's byte for byte, link
included.

**And ITS reads what we write.** `make itsload` hands ITS a tape with a file and
a link on it, both first deleted from the pack, and runs DUMP's own `LOAD`. Both
come back, the link still a link, the file byte-identical. The file beside it is
the control: without it, a failure could not be told from a broken harness.

**What is NOT proven.** A pack `mkfs` builds does not boot, so ITS has never come
up on one — the graders for those are booted from tape. Everything ITS has graded
is an RP06: `mkfs` builds an RM03 as readily, and its arithmetic checks out (503
locked out against the RP06's 505, and a different cylinder truncation), but
NSALV's drive is fixed when the salvager is *assembled*, so the tape here can
only grade the drive it was built for. And the reference pack is one built from
source in 2026, not an artifact recovered from MIT. The version span has a floor and no ceiling — both `FSDEFS` versions
compared are after the 1979 TUT change, and nothing here maps a file system
written by an older monitor. Until each of those is done, this file does not
claim it; what *has* been settled is listed in
[validation](docs/validation.md#what-is-not-established), with the command that
settles it.

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
