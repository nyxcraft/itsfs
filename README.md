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
copies files out. It writes an ordinary **tar** archive of a whole pack, and optionally
**mounts** one on a host directory. It also writes to the pack: `put`, `del`, `mv`,
`mkdir`, `rmdir`, and `mkfs` to build a working file system from nothing. It reads and
writes ITS `DUMP` save sets from tape images, and `check` audits a pack for damage using
a second implementation that shares no code with the reader.

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
$ make            # bin/itsfs
$ make test       # the regression suite (sh + coreutils)
$ make FUSE=1     # ...and `mount`, the one optional dependency (libfuse3)
```

C99 and POSIX, nothing else. The binary is self-contained; copy `bin/itsfs` wherever you
like. `mount` is the only thing that needs a library, which is why it is off by default —
`tar`, `get` and the rest do the same work without one, and are the only path on a
machine with no FUSE.

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
| `itsfs ncheck` | which file claims a block — the inverse of a descriptor |
| `itsfs du` | blocks and words per directory |
| `itsfs scavenge` | what is left of deleted files |
| `itsfs shell` | interactive explorer — `cd`, `ls`, `type`, `blocks`, `stat` |
| `itsfs put` | write a host file into a directory — **destructive** |
| `itsfs del` | remove a file — `rm` is the same command — **destructive** |
| `itsfs mv` | rename a file in place — **destructive** |
| `itsfs ln` | make a link — **destructive** |
| `itsfs cp` | copy a file within a pack — **destructive** |
| `itsfs mkdir` | make a directory — **destructive** |
| `itsfs rmdir` | remove an empty directory — **destructive** |
| `itsfs mkfs` | create a file system from nothing — **destructive** |
| `itsfs labelit` | read or set the pack ID and number |
| `itsfs tar` | a pack in and out of an ordinary Unix tar archive |
| `itsfs mount` | mount a pack on a directory, read-only (`make FUSE=1`) |
| `itsfs umount` | unmount one |
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

`ncheck` answers the question a descriptor cannot: given a block, which file holds it?
Nothing on the disk indexes that way, so it is a walk of every descriptor on the pack —
which means asking about twenty blocks costs what asking about one does.

```console
$ itsfs ncheck rp0.dsk 16074 19081
16074    KSHACK;NSALV 261
19081    no file claims it, and the table says locked out
```

`du` totals each directory. ITS has two levels and no nesting, so a directory *is* a
subtree and nothing recurses; a directory's own `UDBLKS` is printed against the sum of
its files, because the two disagreeing is worth seeing.

```console
$ itsfs du rp0.dsk | tail -1
   30940 28497505  5657   399  total, in 247 directories
```

`scavenge` is what is left of deleted files. A delete on ITS destroys the name — `QSQSH`
shifts the entries below it up and zeroes the vacated slot — and the descriptor, which
`QDEL3` writes zeros over. It does **not** touch the data: the blocks are marked free and
nothing else. So contents are recoverable and **names are not**, which the command says
every time it runs.

```console
$ itsfs scavenge rp0.dsk | tail -5
6719 free blocks, 1853 of them holding data, in 296 runs
39 of those runs are below QSWAPA (block 1551) -- paged-out memory,
free because no file holds them rather than because one was deleted.
A NAME CANNOT BE RECOVERED: `del` zeroes the entry and the descriptor,
and leaves only the data.  See cmd_query.c.
```

Runs below `QSWAPA` are labelled `swap` for that reason: they held paged-out program
memory, and are free because no file has them rather than because one was deleted. `-x`
writes each run out; `-t` keeps only the ones that decode as text.

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

`put -f` overwrites a file that is already there. It is **not** a delete followed by a
write: that has a window in which the old file is gone and the new one is not there yet,
and anything stopping the program in between loses it. The entry is replaced in place —
new data and descriptor first, then a single directory-block write to point the entry at
them, and only then are the old blocks freed. Interrupted before that write the old file
is whole; interrupted after it the new one is; there is no instant at which neither is.

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

`mv` renames, and a rename **moves the entry** for the same reason — writing the new name
where the old one sat would leave a directory that reads correctly to anything scanning it
and wrongly to the thing searching it, so the file would be there and ITS would not find
it. It refuses a cross-directory rename: an entry's position in the MFD *is* its
directory, and ITS has no operation that moves a file between them.

`ln` makes a link, whose "block list" *is* its target's name — three components
written into the descriptor area with one bit set in the entry, so a link costs no blocks
at all. The target need not exist: ITS resolves one when the file is opened, and 7 of the
reference pack's 399 links point at nothing.

```console
$ itsfs ln work.dsk 'SYS;TS LISP' 'SYS;TS L'
SYS;TS L -> SYS;TS LISP
$ itsfs cp work.dsk 'KSHACK;BUILD DOC' 'KSHACK;BUILD BAK'
KSHACK;BUILD DOC -> KSHACK;BUILD BAK (7412 words)
```

`cp` may cross directories where `mv` may not, and the difference is what a failure
leaves: a copy reads one entry and writes a second, so an interruption leaves the source
whole and the destination unmade. A link is copied **as a link**, pointing where the
original points — following it would produce a pack with two copies of a file where ITS
had one file and a reference to it.

`rmdir` frees a directory's MFD slot. It frees no blocks, because a directory owns none,
and it refuses a directory that still holds files — that would strand every block they
own, which is exactly the damage `check` exists to report.

```console
$ itsfs mv work.dsk 'KSHACK;OLD TXT' 'KSHACK;NEW TXT'
KSHACK;OLD TXT -> NEW TXT
$ itsfs rmdir work.dsk KSHACK
itsfs: 'KSHACK' still holds 37 entries -- removing its slot would strand every block they own
```

It refuses by name rather than half-doing: a name SIXBIT cannot hold, a file that already
exists, a byte that is not seven-bit (use `-w`), a directory that is full — **a directory
is one block and there is no way to grow one** — and a block whose reference count the
table calls "many or more", which nobody can decrement correctly.

## A whole pack as a tar archive

`tar c` turns a pack into an ordinary Unix tar that any `tar xf` will read:

```console
$ itsfs tar c pack.tar rp0.dsk
5657 files (2473 text, 3184 words), 399 links, 247 directories

$ itsfs tar c part.tar rp0.dsk KSHACK      # or just one directory
$ tar tvf part.tar | head -3
drwxr-xr-x 0/0               0 1970-01-01 07:00 KSHACK/
-rw-r--r-- 0/0             580 1985-12-30 07:00 KSHACK/-READ- -THIS-
-rw-r--r-- 0/0             115 1986-06-01 08:00 KSHACK/1PROC BUGS
```

ITS's `DIR;FN1 FN2` becomes `DIR/FN1 FN2`, with a real directory member for each ITS
directory, and an ITS link becomes a relative symlink. Name one or more directories, or
`DIR;FN1 FN2` for a single file, to archive part of a pack. `tar t` lists an archive and
`tar x` reads one back into an image — files, directories and links alike, so a pack
round-trips whole. The reference pack's 6,303 entries go out to a tar and back into an
empty file system with identical contents and identical block accounting.

**A 36-bit word is not a byte, and `-m` says what to do about it.** ITS text is five
7-bit characters per word; anything else is best kept as the word itself in eight bytes,
which is what `get -w` writes. `-m auto`, the default, decides per file by looking at the
words and reports the tally; `-m text` and `-m words` force one for everything.

```console
$ itsfs tar c -v part.tar rp0.dsk KSHACK
KSHACK/                       dir
KSHACK/-READ- -THIS-          580 bytes  text
KSHACK/AINOTE 8             13408 bytes  words
...
37 files (27 text, 10 words), 2 links, 1 directories
```

Two details worth knowing. **Names get percent-encoded where they have to be** — SIXBIT
holds `/`, `.`, `%` and the space, and the reference pack has a directory named `.`
holding the monitor, so it appears as `%2E/`. **Links to `>` dangle**: `>` is ITS's
"latest version", resolved when a file is opened, and 88 of the pack's 399 links point at
one. A symlink is a fixed string, so there is nothing to point it at; `readlink` still
shows exactly what ITS recorded.

## Mounting a pack

With `make FUSE=1`:

```console
$ itsfs mount rp0.dsk /mnt/its
itsfs: rp0.dsk on /mnt/its, 247 directories, read-only
$ ls -l /mnt/its/KSHACK | head -3
-r--r--r-- 1 you you   580 Dec 30  1985 -READ- -THIS-
-r--r--r-- 1 you you   115 Jun  1  1986 1PROC BUGS
lrwxrwxrwx 1 you you    12 Jun 27  2026 DDT BIN -> ../%2E/@ DDT
$ itsfs umount /mnt/its
```

Two levels, because ITS has two: `/DIR/FN1 FN2`. The same `-m` question and the same
percent-encoding as `tar`. **Read-only** — see [design](docs/design.md) for why a
writable mount is a project rather than a flag.

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
- [Containers](docs/containers.md) — tar, save sets and the mount, and words into bytes
- [Sources](docs/sources.md) — where every constant comes from
- [Validation](docs/validation.md) — how correctness is established here, and what has been

## Authorship & attribution

- Software Architecture, Design & Engineering by Nicholas J. Kisseberth.

## License

`itsfs` is clean, original C. The ITS source tree, the disk images built from it and the
emulators used to validate against it are usable locally and **are never committed here**.

Licensed under the [MIT License](LICENSE).
