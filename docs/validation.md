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

Everything below is level 3, and the interesting question is how strong a level-3
result can be made. The answer turns out to be: quite strong, if the numbers you
check against were computed by somebody else.

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

**81 checks** in `tests/run.sh`, of which about a third feed the reader a pack
damaged on purpose: an MFD without its check word, an `MDNAMP` outside the block,
a UFD whose `UDNAMP` is zero, a descriptor that takes blocks before loading an
address, one that names a block past the end of the drive, one with no
terminating zero at all, a TUT that ends before it begins, and one that maps more
blocks than its own table can hold. The bar is not that the reader survives — it
is that it **refuses by name** and reads nothing it did not bound first.

The suite builds its own fixture: a complete little ITS file system, poked into a
sparse image one word at a time with `dd`. There is no writer in this project, and
that is exactly why — a suite that used the writer to make its input would be
asking the reader to agree with the writer rather than with ITS.

**`make test-san`** runs the same 81 under AddressSanitizer and UBSan, which is
where those checks have teeth: an out-of-bounds *read* does not fault on a normal
build. It returns whatever was next in memory, and the test passes.

**`make fuzz`** damages one random word of a valid pack, in a structure the reader
must parse, and runs every command over the result. 250 iterations × 7 commands =
1,750 runs against damaged packs under the sanitizers, with no crash, no hang, no
sanitizer report and no silent failure.

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

- **No writer**, so no level-1 or level-2 evidence exists at all.
- **`NSALV` has never been run against anything this produced**, because nothing
  is produced. It is the obvious second opinion and it is phase 8.
- **One pack, one drive, one era.** Everything above is an RP06 built from source
  in 2026. No RP07, no RM03, no multi-pack file system, and no artifact recovered
  from MIT.
- **No ITS magtape has been read**, which is why `core` and `dbd9` are
  `corroborated` here and `confirmed` in `t10fs`.
- **The version span is unknown.** `FSDEFS 43` is one version of one file, and it
  carries two dated format changes in its own comments.
