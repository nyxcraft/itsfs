# Handoff

For whoever picks this up next, including me in six months.

## What this is

A read-only toolkit for the ITS file system, third in a family with
[`s5fs`](https://github.com/nyxcraft/s5fs) (UNIX V7/2.xBSD) and
[`t10fs`](https://github.com/nyxcraft/t10fs) (TOPS-10). Same architecture, same
discipline, different file system — and ITS is genuinely different, not TOPS-10
with other field names. See [PLAN.md §5](PLAN.md#5-what-must-be-designed-fresh)
for the four places that matter.

Phases 0–4 are done: word layer, geometry, cited constants, and a complete
read-only reader. **There is no writer.**

## Read these, in this order

1. [PLAN.md](PLAN.md) — written before any code, and still accurate.
2. [docs/geometry.md](docs/geometry.md) — the one layer that is not in either
   sibling. If you read nothing else, read this: block numbers are not offsets,
   and being wrong about it is not subtle.
3. [`src/its.h`](src/its.h) — the transcription. Every constant, its ITS symbol,
   its base, and whether it has been seen to work.
4. [docs/filesystem.md](docs/filesystem.md) — the format in prose, with the gap
   register at the end.
5. [docs/validation.md](docs/validation.md) — what is actually established, and
   what is not.

## The shape of the tree

```
src/itspack.[ch]    36-bit words in 8-bit bytes.  Four packings.
src/itsgeom.[ch]    where a block is.  The drive table.
src/itstext.[ch]    SIXBIT, seven-bit ASCII, six-bit bytes.
src/image.[ch]      an image, addressed in words and blocks.
src/its.h           every on-disk offset, cited to SYSTEM;FSDEFS 43.
src/structure.[ch]  MFD, UFD, name blocks, descriptors, links, TUT.
src/cmd_*.c         the front ends.  cmd_dump.c depends on nothing above image.c.
tests/run.sh        79 checks.  Builds its own ITS file system with dd.
tests/accounting.sh the oracle's real payload: does the space add up?
tests/fuzz.py       one random word damaged per iteration, every command run.
```

## Things not to do

Each of these has already cost somebody time, here or in a sibling.

**Do not treat a block number as an offset.** `blk * 1024` words is wrong from the
second cylinder onward. There is exactly one conversion, `its_blk_sector()`.

**Do not read `UNDSCP` from word 0 of the directory.** It counts six-bit bytes
from `UDDESC`, word 11. Getting this wrong is *silent*: it lands on zero, zero
means "end of description", and every file decodes to zero blocks with no error.
The only thing that catches it is `UDBLKS`.

**Do not believe FSDEFS about the base of a character.** It says the link
separator is `";" (73)`; the disk holds 033. It gives ASCII codes for two
characters and the SIXBIT value for a third, in one sentence.

**Do not add a constant to `its.h` without a citation and a marker.** `[v]` means
you have watched it decode correctly on a real pack. `[s]` means you have only
read it. Writing `[v]` because you are confident is how the markers stop meaning
anything.

**Do not case-fold or truncate a name.** SIXBIT has no lower case. Refusing is
correct; `HELLO` and `hello` are not the same file, and silently making them so
produced duplicate directory entries in `s5fs` on an image its checker called
clean.

**Do not let the test suite build its fixtures with a future writer.** The suite
pokes words in with `dd` on purpose. A fixture built by the writer asks the
reader to agree with the writer rather than with ITS.

**Do not assume a second reference to a block is corruption.** The TUT is a
reference count. On the reference pack nothing is referenced twice, which means
nothing here knows what a legitimate second reference looks like.

## Known gaps, stated plainly

- No writer, therefore no level-1 or level-2 evidence of any kind.
- `NSALV`, ITS's own salvager, has never been pointed at anything this produced.
- One pack, one drive, one era: an RP06 built from source in 2026. No RP07, no
  RM03, no multi-pack file system, no artifact recovered from MIT.
- No ITS magtape has been read, which is why `core` and `dbd9` are `corroborated`
  here and `confirmed` in `t10fs`.
- `UNTIM`'s unit, `UNBYTE`'s other three encodings, `UNDUMP`'s position and the
  flag field's width are all unknown. See
  [the gap register](docs/filesystem.md#gap-register).
- The ITS checkout used is shallow, so this project cannot say when `FSDEFS`
  itself last changed — and therefore cannot say which ITS releases it covers.

## How to get a pack to test against

```console
$ git clone https://github.com/PDP-10/its
$ cd its && make EMULATOR=simh      # or klh10, or pdp10-ka
```

which leaves an RP06 at `out/simh/rp0.dsk`. Then:

```console
$ make oracle IMAGE=~/its/out/simh/rp0.dsk
```

**Work on a copy.** `make oracle` makes one itself; `repack` opens its input
read-only either way; the rule stands regardless, because the first thing a
writer will do is break it.

## If you are starting phase 5

The checker is next, and the rule is that it shares no code with the reader —
which means it re-derives everything from `its.h` and calls neither
`structure.c` nor `image.c`'s block helpers.

The three checks in `tests/accounting.sh` are the specification for its first
version: they already pass on a real pack, in shell, so a `check` that does them
in C has something to be graded against on day one. Then extend it to per-block
reference counts, which is the thing a shell script cannot do.

Start by reading `src/system/salv.317` and `src/kshack/nsalv.*` in the ITS tree.
Not to copy — to find out what ITS itself thought was worth checking, which is a
better list than one derived from the format alone.
