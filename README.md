# itsfs — host tools for the ITS file system

[![CI](https://github.com/nyxcraft/itsfs/actions/workflows/ci.yml/badge.svg)](https://github.com/nyxcraft/itsfs/actions/workflows/ci.yml)
[![Docs](https://github.com/nyxcraft/itsfs/actions/workflows/pages.yml/badge.svg)](https://nyxcraft.github.io/itsfs/)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C99](https://img.shields.io/badge/C-C99%20%2B%20POSIX-blue.svg)](#build)
[![No dependencies](https://img.shields.io/badge/dependencies-none-brightgreen.svg)](#build)

> **Software Architecture, Design & Engineering by Nicholas J. Kisseberth.**

**Read, check and write the disks of MIT's Incompatible Timesharing System — on a
machine that has never heard of a 36-bit word.**

`itsfs` opens a PDP-10 disk image, finds the master file directory, lists directories,
decodes the run-length bytecode that serves as an ITS block map, follows links, and
copies files out. It also writes: `put`, `del`, `mkdir`, and `mkfs` to build a working
file system from nothing. It reads and writes ITS `DUMP` save sets from tape images, and
`check` audits a pack for damage using a second implementation that shares no code with
the reader.

One C99 binary, no dependencies beyond POSIX.

```console
$ itsfs info rp0.dsk
file          rp0.dsk
size          317132800 bytes
packing       le64 (confirmed) -- one word per 64-bit little-endian container (SIMH disk images)
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
BUILD  DOC         7412      8  25-jun-2026
```

## Build

```console
$ make          # bin/itsfs
$ make test     # the regression suite (sh + coreutils)
```

C99 and POSIX, nothing else. The binary is self-contained; copy `bin/itsfs` wherever you
like.

## Quick start

```console
$ itsfs info rp0.dsk                      # what is this image?
$ itsfs dirs rp0.dsk                       # what directories are on it?
$ itsfs ls -l rp0.dsk KSHACK               # what is in one
$ itsfs cat rp0.dsk 'KSHACK;BUILD DOC'     # read a file
$ itsfs get rp0.dsk 'KSHACK;BUILD DOC' build.doc    # copy it out
$ itsfs check rp0.dsk                      # is the pack sound?
```

## Commands

| command | what it does |
|---|---|
| `itsfs info` | describe an image: packing, drive, geometry, and whether the MFD is there |
| `itsfs dirs` | list the directories in the MFD — `-l` also opens each one |
| `itsfs ls` | list a directory |
| `itsfs cat` | print a file as text |
| `itsfs get` | copy a file out to the host — text, or `-w` for its 36-bit words |
| `itsfs free` | what the allocation table says: free, in use, locked out, reference counts |
| `itsfs check` | check a pack — **shares no code with the reader** |
| `itsfs manifest` | fingerprint a pack: one line per directory, file and link |
| `itsfs verify` | diff a pack against a manifest |
| `itsfs shell` | interactive explorer — `cd`, `ls`, `type`, `blocks`, `stat` |
| `itsfs put` | write a host file into a directory — **destructive** |
| `itsfs del` | remove a file — `rm` is the same command — **destructive** |
| `itsfs mkdir` | make a directory — **destructive** |
| `itsfs mkfs` | create a file system from nothing — **destructive** |
| `itsfs saveset` | list or extract an ITS DUMP save set |
| `itsfs save` | write a DUMP save set from files on a pack |
| `itsfs tape` | SIMH `.tap` record framing — the container, not the archive |
| `itsfs dump` | print blocks (or raw sectors) as 36-bit words — octal, halfwords, SIXBIT, ASCII |
| `itsfs repack` | rewrite an image word for word in another packing |
| `itsfs packings` | the word packings, and how well each one is established |
| `itsfs drives` | the five drives ITS supported, and the geometry each implies |
| `itsfs sixbit` | encode and decode SIXBIT, with no image involved |

Run any of them with no arguments for its own usage.

## Naming a file

ITS writes a file name as `DIR;FN1 FN2` — a directory and two six-character SIXBIT
names, with no extension in the DEC sense and no path deeper than one. The space in the
middle has to be quoted for the shell, so every command that takes a name also takes it
as three arguments:

```console
$ itsfs cat rp0.dsk 'KSHACK;BUILD DOC'
$ itsfs cat rp0.dsk KSHACK BUILD DOC
```

Both spellings go through one parser. A name that cannot be SIXBIT is refused, never
truncated and never case-folded: SIXBIT has no lower case, and quietly mapping `hello` to
`HELLO` would match a different file.

## Reading

`get` copies a file out. By default it decodes text; `-w` writes the raw 36-bit words
instead, eight bytes each, for anything that is not ASCII.

```console
$ itsfs get rp0.dsk 'KSHACK;BUILD DOC' build.doc
KSHACK;BUILD DOC -> build.doc (7412 words in 8 blocks)
```

`free` reports the allocation table, which stores a **reference count** per block rather
than a bitmap:

```console
$ itsfs free rp0.dsk
pack          FOOBAR (number 0)
TUT           blocks 0..38164 at block 19077, 4 blocks of table
swap area     blocks 0..1551 (QSWAPA is the first non-swapping block)
search from   block 3102 (QTUTP)

free              6719 blocks   17.6%
in use           30940 blocks   81.1%
locked out         505 blocks    1.3%
```

`check` walks the pack independently of the reader and reconciles what the files claim
against what the table says:

```console
$ itsfs check rp0.dsk
pack           FOOBAR (number 0)
TUT maps       0..38164
blocks         6719 free, 30940 in use, 505 locked out
claimed        30940 blocks, in 5657 files
directories    247, 5657 files, 399 links (7 of them unresolved)

no problems found (7 notes)
```

## Writing

```console
$ cp rp0.dsk work.dsk
$ itsfs put work.dsk 'KSHACK;HELLO TXT' hello.txt
wrote KSHACK;HELLO TXT
$ itsfs del work.dsk 'KSHACK;HELLO TXT'
deleted KSHACK;HELLO TXT
```

**These change the pack. Work on a copy.** `itsfs` refuses to write an image another
process has open — an emulator with the pack attached is writing to it too, and there is
no lock to take — comparing by device and inode, overridable with `ITSFS_IGNORE_INUSE=1`.
Reading is never blocked.

Every store goes through [`src/write.c`](src/write.c), the only code here that mutates a
pack, and it holds to three rules:

- **Refuse, do not half-do.** Every check that can fail happens before anything is
  written. A refusal leaves the pack byte-identical, and says the number it refused at.
- **The directory entry goes last** — data, then the allocation table, then the
  descriptor, then the name. An interruption before the last step leaves blocks marked in
  use that no file claims, which loses nothing. The other order loses a file.
- **Never write a pack somebody else has open.**

`put` inserts the name in **sorted position**, because an ITS name area is sorted and the
monitor binary-searches it. `del` frees the blocks and zeroes the descriptor bytes in
place; it does not compact the descriptor area, because ITS does not either.

It refuses by name rather than half-doing: a name SIXBIT cannot hold, a file that already
exists, a byte that is not seven-bit (use `-w`), a directory that is full — **a directory
is one block and there is no way to grow one** — and a block whose reference count the
table calls "many or more", which nobody can decrement correctly.

## Save sets

An ITS `DUMP` save set is an archive written onto tape. `itsfs` reads and writes them,
and the tape itself is an ordinary SIMH `.tap` file.

```console
$ itsfs save rp0.dsk kshack.tap 'KSHACK;BUILD DOC' 'KSHACK;-READ- -THIS-'
KSHACK;BUILD DOC
KSHACK;-READ- -THIS-
2 files written to kshack.tap

$ itsfs saveset kshack.tap
tape 1, reel 0, created __/__/__, type random
KSHACK;BUILD DOC
KSHACK;-READ- -THIS-
2 files, 0 links

$ itsfs saveset -x out kshack.tap
tape 1, reel 0, created __/__/__, type random
KSHACK;BUILD DOC -> out/KSHACK;BUILD.DOC (7412 words)
KSHACK;-READ- -THIS- -> out/KSHACK;-READ-.-THIS- (116 words)
2 files, 0 links
```

## A file system from nothing

`mkfs` writes a complete, empty ITS file system — master file directory, allocation
table, swap area and all — onto a new image:

```console
$ itsfs mkfs -d rp06 new.dsk ITSFS
made an rp06 file system on new.dsk: pack 0, ID ITSFS, 500 directory slots, 1551 blocks of swapping

$ itsfs mkdir new.dsk KSHACK
made KSHACK
$ itsfs put new.dsk 'KSHACK;HELLO TXT' hello.txt
wrote KSHACK;HELLO TXT
$ itsfs check new.dsk | tail -4
claimed        1 blocks, in 1 files
directories    1, 1 files, 0 links (0 of them unresolved)

no problems found (0 notes)
```

## The shell

```console
$ itsfs shell rp0.dsk
rp0.dsk, rp06, 247 directories.  `help` lists the commands.
- (ro)> cd KSHACK
KSHACK (ro)> blocks NSALV 261
KSHACK;NSALV 261: 43 blocks, 43087 words
  16074..16116 (43)
  1 run
KSHACK (ro)> quit
```

`cd`, `ls`, `pwd`, `type`, `blocks`, `stat`, `free` and `info`. It reads commands from
standard input, so it scripts.

`blocks` prints **runs** rather than a list of numbers, because a descriptor is
run-length coded and where a file is fragmented is the question worth asking. `stat`
prints the raw `UNRNDM`, `UNDATE` and `UNREF` words alongside the decoded fields —
including the ones nothing here interprets, since a stat that showed only the understood
fields would hide exactly what somebody investigating an unknown one needs to see.

## Options

`-p <packing>` selects the word packing (default `le64`, which is what SIMH disk images
use). `-d <drive>` names the drive when the image is not a whole pack — normally it is
identified from the size.

A word value on the command line is **octal by default** — this is a PDP-10 — with `0x`
for hex, `d:` for decimal, `lh,,rh` for the two 18-bit halves, and `sixbit:NAME` for
text. A *block number* is decimal by default, because that is how ITS writes block
numbers; `0` or `0o` in front asks for octal.

```console
$ itsfs packings
name   status        layout
le64   confirmed     one word per 64-bit little-endian container (SIMH disk images)
be64   structural    one word per 64-bit big-endian container
core   confirmed     five frames per word (magtape core-dump mode)
dbd9   confirmed     two words in nine bytes, no waste (KLH10 disk images)

$ itsfs repack -p le64 -P dbd9 rp0.dsk klh10.dsk
```

## Why ITS is not like other file systems

Four things shape the design rather than fitting inside it:

- **A block number is not an offset.** ITS numbers blocks *within a cylinder*, and the
  count per cylinder is an integer division that usually has a remainder — so four
  sectors of every RP06 cylinder are reachable by no block number at all.
- **Free space is a reference count**, three bits per block, not a bitmap and not a
  chain. Zero is free, seven is locked out, and everything between is how many things
  point at that block.
- **A directory is exactly one block.** Descriptors grow up from word 11, name blocks
  grow down from the end, and when they meet the directory is full. There is no growth
  and nothing to chain.
- **A file's block list is a program** — a run-length bytecode in six-bit bytes with a
  load-address escape — and a link's "block list" is the target's name in the same field,
  distinguished by one bit.

Every field offset is transcribed from `SYSTEM;FSDEFS 43` with a citation; see
[sources](docs/sources.md).

`itsfs` is the third in a family: [`s5fs`](https://github.com/nyxcraft/s5fs) reads UNIX
V7/2.xBSD, [`t10fs`](https://github.com/nyxcraft/t10fs) reads TOPS-10.

## Documentation

Full documentation: **[nyxcraft.github.io/itsfs](https://nyxcraft.github.io/itsfs/)**

- [The file system](docs/filesystem.md) — the format itself, and what is still unknown
- [Design](docs/design.md) — the layers, the invariants, and what is out of scope
- [Geometry](docs/geometry.md) — why a block number is not an offset
- [Word packing](docs/word-packing.md) — why the bottom layer is not a byte-order codec
- [On-disk format](docs/on-disk-format.md) — confirmed facts, and what the source settles
- [Sources](docs/sources.md) — where every constant comes from
- [Validation](docs/validation.md) — how correctness is established here, and what has been

## Authorship & attribution

- Software Architecture, Design & Engineering by Nicholas J. Kisseberth.

## License

`itsfs` is clean, original C. The ITS source tree, the disk images built from it and the
emulators used to validate against it are usable locally and **are never committed here**.

Licensed under the [MIT License](LICENSE).
