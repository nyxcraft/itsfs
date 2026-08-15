# Validation

How correctness is established here, what has actually been established, and what
has been found by trying.

## The standard

Three levels of evidence, in the order they are worth anything:

1. **Byte-identical** — build something with `itsfs`, build the same thing with
   ITS, `cmp`. Nothing here reaches this level, because there is no writer.
2. **Accepted by native tools** — hand what we produce to `NSALV`, ITS's own
   salvager, or to the monitor. Nothing here reaches this level either, for the
   same reason.
3. **Self-consistent** — round trips, and cross-checks against numbers the file
   system maintains independently of the thing being checked.

That taxonomy came from the sibling projects and it turned out to be missing an
axis. Levels 1 and 2 both assume the thing being graded is something we *wrote*.
But a native tool can also be asked to grade something we only **read** — point
ITS's own salvager at a pack, ask it what is wrong, and compare its answer with
ours. That needs no writer, and it is not self-consistency either, because the
other opinion is MIT's.

It has its own section below. Everything else here is level 3, and the
interesting question is how strong a level-3 result can be made. The answer turns
out to be: quite strong, if the numbers you check against were computed by
somebody else.

## Level 3a — the word layer round-trips a real pack

```console
$ make oracle IMAGE=~/its/out/simh/rp0.dsk
IDENTICAL: the word layer round-trips the real pack
IDENTICAL: cross-packing round trip
IDENTICAL: and through the packing that shares a byte
```

All 39,641,600 words of a 300 MB RP06, decoded and re-encoded, byte for byte;
then the same through `core` (five bytes to the word, a different stride) and
`dbd9` (two words in nine bytes, sharing a byte between them) and back.

This is stronger than "the codec is its own inverse". Because `get()` masks every
word to 36 bits, byte identity over the whole image also proves that **nothing on
the pack sets any of the 28 bits outside the word** — an invariant everything
above depends on, established by measurement rather than assertion.

## Level 3b — the space on a real pack accounts for exactly

```console
1. 247 directories walked, 247 whose descriptors agree with their own UDBLKS
2. 30940 blocks described by files == 30940 blocks the TUT calls in use
3. 505 blocks locked out == 500 directories + 1 MFD + 4 TUT blocks
```

`tests/accounting.sh`, run by `make oracle`. Each line is worth something
different, and the reason to state them separately is that they fail
independently:

**1** compares the descriptor decoder against `UDBLKS`, a running total the
monitor maintains in each UFD header as files are written. Nothing here computes
it and nothing here can influence it. It is 247 separate comparisons, one per
directory, and a decoder that mishandled one opcode would fail some and pass
others.

**2** compares the same sum against the TUT — a different structure, in a
different place, maintained by a different part of the monitor. A block skipped
or double-counted *anywhere on the pack* moves this number. That it lands exactly
on 30,940 is the single strongest statement this project can currently make.

**3** is the only check there is on the MFD-slot arithmetic. There is no pointer
from an MFD entry to its directory — the position *is* the address — so the only
way to know the formula is right is that the blocks it says are directories are
exactly the blocks the TUT refuses to allocate. 500 directories, one MFD, four
TUT blocks, 505 locked out.

## Level 3c — a second implementation, over the same pack

```console
$ itsfs check rp0.dsk
checking rp0.dsk: rp06, 38305 blocks, MFD at 19081, TUT at 19077

pack           FOOBAR (number 0)
TUT maps       0..38164
blocks         6719 free, 30940 in use, 505 locked out
claimed        30940 blocks, in 5657 files
directories    247, 5657 files, 399 links (7 of them unresolved)

no problems found (7 notes)
```

`cmd_check.c` includes neither `structure.h` nor `itsgeom.c`'s conversion. It
re-derives the block-to-sector arithmetic, the MFD-slot formula, the UFD layout,
the descriptor bytecode and the TUT from `its.h`, and walks the pack itself. The
numbers above are the reader's numbers, arrived at separately — and `make oracle`
runs both, one after the other, over the same copy.

**Why the geometry in particular is written out twice.** It is the part of this
format with no check word, no pointer and no redundancy: a block number resolves
to a sector and there is nothing on the disk that says whether it was the right
one. Every other structure announces itself somehow — the MFD has `MDCHK`, a
directory has `UDNAME`, a file has `UDBLKS` — so for those, a second reading adds
less than a second opinion from the pack does. For the geometry there is no
second opinion available, so this is it.

**What it checks that nothing else did.** The TUT is a *reference count*, so
`check` compares it per block **as a count**, not as a flag, and separates the
four ways it and the files can disagree:

| disagreement | what is at risk |
|---|---|
| a file holds a block the TUT calls free | **data**: the allocator will hand it out again |
| the TUT marks a block in use that no file holds | space, and nothing else |
| the counts differ | a reference that was added or dropped without its pair |
| a file holds a locked-out block | a directory or a table is about to be overwritten |

Damaging one TUT word on a copy of the real pack — twelve blocks' worth of
entries — produces exactly the right eleven lines, and names the file that would
have been lost in each:

```console
  block 19992 is claimed by C;PHASE ARGS, and the TUT calls it FREE
  block 19993 is claimed by MAINT;DEKAG SAV, and the TUT calls it FREE
  block 19994 is claimed by EMACS;EINIT :EJ, and the TUT calls it FREE
  ...
11 problems, 7 notes
```

**The limit, stated rather than glossed.** Both readers take their constants from
`its.h`, so both inherit any misreading in it. A second reading catches a wrong
reading; it cannot catch what the source never says. That half is the fuzzer's
job — and in `t10fs` it was indeed the fuzzer, not the second implementation,
that found the one bug both readers shared.

**And the seven notes.** Seven links on the reference pack point at files that
are not there. That is an ordinary state of a live system rather than damage, so
it is counted, listed under `-v`, and never changes the exit status. Getting the
count down to seven took reading the monitor: `>` and `<` are not ordinary names.
`QLOOK` in `disk.1228` treats them specially in either name and walks the
directory for the best numeric part, so a link to `SYSDOC;TV >` resolves against
whatever version is there — and eighty-eight of the ninety-five "dangling" links
the first version reported were that, not breakage.

## A second opinion that is not ours

```console
$ make nsalv IMAGE=~/its/out/simh/rp0.dsk
1. a clean pack
  ok   itsfs check: clean
  ok   NSALV: salvaged and returned to DDT with nothing to say
  ok   and left the pack byte-identical

2. the same pack with one TUT word cleared
   cleared TUT word 1682 (block 19078, offset 658): blocks 19992..20003
  ok   itsfs check: 11 problems
  ok   NSALV: reported the damage

3. do they name the same blocks, and the same files?
   itsfs check names 11, NSALV names 11
  ok   IDENTICAL: the same 11 blocks, each held by the same file

       19992 C;PHASE ARGS
       19993 MAINT;DEKAG SAV
       19994 EMACS;EINIT :EJ
       19995 EMACS;EINIT :EJ
       19996 EMACS;EINIT :EJ
       19997 EMACS;EINIT :EJ
       19998 COMLAP;MK.FAS UNFASL
       19999 MAXDOC;TDCL FASL
       20000 MAXDOC;TDCL FASL
       20001 MAXDMP;RAT3B FASL
       20002 MAXDMP;RAT3B FASL
  ok   and NSALV's own summary agrees: 11 blocks at 1 reference, stored as 0

two checkers with nothing in common but the disk, agreeing block for block
```

**`NSALV` is not a second reading of ours.** It is MIT's, written in the 1980s by
people with the machine in front of them; it boots standalone from a tape, walks
every directory on the pack and rebuilds the allocation table from scratch. It
predates this project by forty years and shares nothing with it.

That matters because it closes the gap `itsfs check` cannot. Both of our readers
take their constants from `src/its.h`, so both inherit any misreading in it — a
second reading catches a wrong reading, but it cannot catch what the source never
says. Where `NSALV` agrees, the agreement is about **the format** rather than
about our understanding of it.

**And it needs no writer**, which is why this runs at phase 5 rather than waiting
for phase 8. A pack this project has only read is enough to ask the question.

### Why both runs are needed

The clean run proves less than it looks. `NSALV` salvages and returns to DDT
without a word, which is indistinguishable from the salvager never having run.
(Its `NOISE` flag makes it announce every check — and also dump the RH11
controller status after *every* disk transfer, then stop to ask whether you want
the UNIBUS registers too. Over a 500-block walk that is thousands of lines and a
blocked script; `SKIPE NOISE` guards both and there is no summary-only setting.)

So the damaged run is what makes the clean one evidence. One TUT word cleared —
twelve blocks' worth of entries, eleven of them held by files — and now it
speaks, in terms that can be compared line for line:

```
File unprotected in old TUT, Block 19992. - C;PHASE ARGS  Pack 0., Unit #0
```

against ours:

```
  block 19992 is claimed by C;PHASE ARGS, and the TUT calls it FREE
```

Same category — the dangerous one, where the allocator would hand the block out
again — same block, same file, and `NSALV` renders the name in the same
`DIR;FN1 FN2` form this project chose from reading the format.

### What the comparison actually compares

The **pairs**: which block, and which file holds it. Not counts, and not "both
reported something" — two checkers can agree that a pack is broken and disagree
about every detail. `NSALV` reports in directory-walk order and `itsfs` in block
order, so both are sorted before `diff`.

The check is not vacuous: changing one file name in one of the two lists makes it
fail, and if either extraction pattern stopped matching, that list would be empty
and the comparison would fail rather than pass quietly.

`TARGET=` moves the damage. At block 9000 instead of 20000 it is a different
eight blocks and eight different files — `SYS;TS @`, `EJS;TS UPTINI`,
`2500;ZAP FASL`, `MUNFAS;RESIDU UNFASL` — and the two agree there too. One
matching site could be luck; two disjoint ones are the format.

### What this establishes, and what it does not

Agreeing about *which block, held by which file* means agreeing about the whole
chain that produces that answer: the block-to-sector geometry, the MFD-slot
arithmetic that finds each directory, the UFD layout, the descriptor bytecode,
and the TUT's three-bit entries. A single wrong step anywhere in it moves a block
number or a name.

What it does **not** establish is anything `NSALV` never looks at. It rebuilds
the allocation table, so it exercises descriptors and names; it does not care
about `UNTIM`, `UNBYTE`, creation dates, or where a link points. Those stay in
[the gap register](filesystem.md#gap-register).

And it grades a **reading**. Nothing here has been written by this project, so it
is not the level-2 result — that still needs a writer, and it is still phase 8.

## The fingerprint is of the file system, not of the container

```console
$ make oracle IMAGE=~/its/out/simh/rp0.dsk
fingerprinting it, and verifying that through a different packing ...
manifest: 6303 directories, files and links
0 differences
IDENTICAL: the same file system, four and a half bytes per word instead of eight
```

A manifest taken from the 317 MB `le64` pack verifies clean against the same file
system stored as 178 MB of `dbd9`. **Not one byte boundary is shared** between
those two images — `dbd9` puts two words in nine bytes and splits one of them
down the middle — and every word is identical, which is exactly the distinction
the checksum has to make.

It makes it by feeding each word to the CRC as five bytes in the `core`
convention rather than as whatever the container happens to hold. A byte-wise
checksum would have made a manifest a fingerprint of the *container*: the same
pack in two packings would disagree about every file, which is precisely
backwards.

**What is compared, and what is only recorded.** The type, the path, and — for a
file — the length and the checksum. Not the block count: which blocks a file
occupies is a property of the pack's allocation history rather than of the file.
A fingerprint that reports differences nobody cares about gets ignored, and then
it reports nothing at all.

A link is recorded **by its target**, not by what the target contains. Following
it would checksum the same file twice under two names, and — since a link may
point at a file that is not there, which the reference pack does seven times —
would make an unrelated deletion look like damage to the link.

**And it detects one bit.** Flipping a single bit of one word in 39,641,600
names the file it was in:

```console
! SYSTEM;IMPOLD NCP2 (contents differ)
1 difference
```

## The one check that is not the pack agreeing with itself

Everything above compares the reader against numbers ITS wrote **on the same
pack**. `tests/crosscheck.sh` compares it against files that never went near one.

The ITS source tree builds its own disk: a host file is packed into a tape image
by `itstar`, the tape is read by ITS's own loader, and the monitor writes the
file onto the pack. So for a file that exists in both places, the host copy is an
independent original:

```console
$ make oracle IMAGE=~/its/out/simh/rp0.dsk
cross-check: 137 files byte-identical to their host originals, 0 differ,
             40 skipped as escaped by itstar, 0 not on the pack
```

Every layer of this project is on that path. A byte-identical result means the
geometry found the right blocks, the descriptor decoded them **in the right
order**, `UNWRDC` gave the right length for the last one, and the seven-bit
extraction is right — checked against something this project cannot influence.

Two differences are expected and are the file's rather than the reader's: ITS
text uses CRLF (`itsfs cat` translates nothing, so the CRs are really there) and
ITS pads the last word with `^C`.

**Forty files are skipped, and saying why is the point.** The host copy is not
raw ITS text — it is in the "evacuated file format" `itstar` uses, which escapes
exactly the characters a plain CRLF-to-LF conversion would lose: a lone CR, a
bare LF, NUL, and anything with the high bit set. Comparing those files would
test whether this script had reimplemented somebody else's escape table, so it
does not, and it counts them instead. What the check actually covers is visible
rather than assumed.

## Hostile input

The reader's whole job is parsing a file nobody here wrote, most of whose fields
bound a loop or index an array.

**151 checks** in `tests/run.sh`, of which about a third feed the reader or the
checker a pack damaged on purpose: an MFD without its check word, an `MDNAMP` outside the block,
a UFD whose `UDNAMP` is zero, a descriptor that takes blocks before loading an
address, one that names a block past the end of the drive, one with no
terminating zero at all, a TUT that ends before it begins, and one that maps more
blocks than its own table can hold. The bar is not that the reader survives — it
is that it **refuses by name** and reads nothing it did not bound first.

The suite builds its own fixture: a complete little ITS file system, poked into a
sparse image one word at a time with `dd`. There is no writer in this project, and
that is exactly why — a suite that used the writer to make its input would be
asking the reader to agree with the writer rather than with ITS.

**`make test-san`** runs the same 151 under AddressSanitizer and UBSan, which is
where those checks have teeth: an out-of-bounds *read* does not fault on a normal
build. It returns whatever was next in memory, and the test passes.

**`make fuzz`** damages one random word of a valid pack, in a structure the reader
must parse, and runs every command over the result — 200 iterations × 10 commands
= 2,000 runs under the sanitizers. `check` is one of the ten and reaches every
structure in a single run; `manifest` is another and is the only command that
reads *every block of every file*; the shell is driven with a script.

**It has found one bug so far**, which is what it is for: `manifest` called
`qsort(NULL, 0, …)` on a pack whose `MDNUDS` was zero, because then no MFD slot
resolves and nothing is walked. Passing NULL is undefined behaviour regardless of
the count. Nothing hand-written had reached it — a pack with no directories at
all is not a case anybody thinks to build — and on an ordinary build UBSan only
*prints*, so the regression test for it passes unless `halt_on_error` is set.
Confirmed against the unfixed code under `make test-san`, where it aborts.

## Lint

`make lint` compiles the tree under **16 warning options** — including
`-Wconversion` and `-Wsign-conversion`, which are the two that catch a 36-bit
value quietly becoming an `int` — and requires zero. It also requires the tree to
be `clang-format` clean under a pinned version, with the hand-aligned tables
(`its.h` is one large one) fenced so the tool cannot collapse the columns that
are the documentation.

CI keeps `-Werror` on the portable three (`-Wall -Wextra -pedantic`) across gcc,
clang and macOS; the larger set stays local, because a build that fails on
somebody else's compiler version is worse than a warning nobody sees.

## What has been found so far

The point of writing down a method is that it finds things. So far:

**The link separator is SIXBIT, not ASCII.** FSDEFS documents the quoting
characters as `";" (73)` and `":" (72)`, which are ASCII codes; the disk holds
033 and 032. Implemented from the documentation, the link decoder found no
separator in anything and rendered `.;@ DDT` as `.;@;DDT`. Found by dumping the
bytes of a link that was already "working". Written up in
[sources](sources.md#two-traps).

**A descriptor pointer counts from `UDDESC`, not from word 0.** `UNDSCP` is a
six-bit-byte offset from word 11 of the directory block. Read from word 0, it
lands 66 bytes early — where a well-formed directory holds zero, which means "end
of description". So every file decoded to **zero blocks and no error at all**.
The check that caught it was `UDBLKS`: 37 files, 0 blocks, and a header saying
540. A cross-check against a number somebody else computed is the only thing that
catches a bug whose symptom is silence.

**Two documented numbers in the MFD are not what they look like.** `MDNUDS` is
the number of directory slots the monitor was *built* for, not the number in use;
the number in use follows from where `MDNAMP` points. The first version of `info`
printed them the other way round.

**`cat` dropped interior NULs, not just trailing padding.** A file's length is
known in words, so the last word may hold up to four characters that are not part
of the file; dropping every NUL is the same code with one state fewer, and it is
wrong. Two files on the pack have NULs in the middle — `ASCII \^@E^@A` in a macro
argument — and lost them silently. **The cross-check above is what found it**,
which is exactly the argument for having a piece of evidence that does not come
from the pack: nothing the pack knows about itself could have caught this, because
the pack has no opinion about what a character is.

## What is not established

Stated plainly, because a validation document that only lists successes is
marketing:

- **No writer**, so no level-1 or level-2 evidence exists at all. `NSALV` has
  graded this project's *reading* of a pack, not anything it produced.
- **`NSALV` was asked about one kind of damage.** A cleared TUT word, at two
  sites. It has not been shown a broken descriptor, a damaged directory header or
  a corrupt MFD — all of which `itsfs check` reports and none of which has been
  put to ITS for a second opinion.
- **`NSALV` has never been run against anything this produced**, because nothing
  is produced. It is the obvious second opinion and it is phase 8.
- **One pack, one drive, one era.** Everything above is an RP06 built from source
  in 2026. No RP07, no RM03, no multi-pack file system, and no artifact recovered
  from MIT.
- **No ITS magtape has been read**, which is why `core` and `dbd9` are
  `corroborated` here and `confirmed` in `t10fs`.
- **The version span is unknown.** `FSDEFS 43` is one version of one file, and it
  carries two dated format changes in its own comments.
