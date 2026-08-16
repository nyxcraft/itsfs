# Sources

Where every constant comes from, what has been checked against a real pack, and
two traps worth knowing about before reading the ITS sources yourself.

## The primary source

Every on-disk offset, width and opcode in [`src/its.h`](../src/its.h) is
transcribed from one file:

```
SYSTEM;FSDEFS 43     the file system parameters, "APPLIES TO ALL ITS MACHINES"
```

read in the [PDP-10/its](https://github.com/PDP-10/its) repository, at
`src/system/fsdefs.43`.

## How far the transcription reaches

`FSDEFS 43` is one version of one file, and ITS ran for twenty-three years, so
"which ITS releases does this reader cover?" is a real question. It used to be
answered here with a shrug. It has a better answer now, because the same
repository preserves an **earlier version of the same file** — `SYSENG;FSDEFS
40`, three file-versions back, imported in 2016 and deleted a month later, still
in the history.

```console
$ make version-diff
1. every symbol, both versions
  ok   IDENTICAL: all 71 definitions, same names and same values

2. the symbols src/its.h actually cites
  ok   all 43 symbols its.h cites are defined in FSDEFS 43
  ok   ...and 2 constants that cite ITS code instead, and say so
```

**All 71 `DEFSYM`s are identical** — every name, every value. The 65 lines that
differ are entirely prose: version 43 replaced a terse descriptor comment with a
long one and added a dated note. And that note makes the claim this comparison
checks:

> `; 8/19/90 Due to the larger size of RP07s it was necessary to officially`
> `; flush the DM "funny" bit.  This change only changes the comments in this`
> `; file, but any program that interprets UFD descriptors needs to be fixed`
> `; to not mask that bit out (as most of them currently do).`

The file says its own change was comment-only; the symbol comparison is what
holds it to that.

**One semantic difference survives, with no symbol attached to it.** `FSDEFS 40`
documents the 020 bit of a load address as a `"FUNNY" BLOCK IF DMDSK`; 43 says it
is flushed, so 17 bits are block number. This reader does not mask it, which is
correct for 43 and for any pack FSDEFS calls known — but it is the one place
where reading version 40 instead would have produced different code, and no diff
of symbols would have shown it.

**What this does NOT establish.** A file version is ITS's own numbering, not a
release number, and nothing here maps 40 or 43 onto an ITS distribution. The
comparison establishes that the format did not move between two points; it does
not establish where those points are. Both versions carry `;9/5/79 - tut format
changed!`, so the three-bit TUT entry this reads is the post-1979 one in both —
which puts a floor under the span and no ceiling.

`tests/version-diff.sh` also checks the **citation column**: every symbol
`its.h` names must actually be defined in `FSDEFS`. That is the one kind of wrong
citation a machine can catch, and it found one — two constants whose comments
began with `SIXBIT` and looked like citations to a symbol of that name. They cite
ITS's own code instead now, and say so.

Its own header reads:

```
;;; Copyright (c) 1999 Massachusetts Institute of Technology
;;; See the COPYING file at the top-level directory of this project.
```

Two other files in the same tree supplied facts that FSDEFS does not carry:

| file | what it settled |
|---|---|
| `src/system/disk.1228` | that a "track" is a block (line 48); `QFL2`, the MFD-slot arithmetic |
| `src/system/rp06.defs1` and the four beside it | the drive geometry table, per drive |

one is not a source but a second opinion, and it is now used:

| file | what it settled |
|---|---|
| `src/kshack/nsalv.261` | ITS's own salvager. Run against a pack rather than read for constants — see [validation](validation.md#a-second-opinion-that-is-not-ours). Its `LTYPE` also settled the SIXBIT trap below. |
| `src/midas/midas.458` | that MIDAS's `'X` is SIXBIT, which is what makes `LTYPE` mean what it means |

and one is named on the roadmap rather than used yet:

| file | what it is for |
|---|---|
| `src/system/dskdmp.217` | the standalone dumper and bootstrap: how a pack is written from outside the monitor |

## The licensing line, and where it sits

**The ITS tree is GPL and is never vendored, copied or committed here.** No line
of ITS code and no line of anybody else's C appears in this project.

What is taken is what a file format *is*: word offsets, field widths, opcode
values, and the arithmetic that turns a block number into a disk address. That is
the same thing `t10fs` takes from DEC's `COMMOD.MAC` and `s5fs` from the 2.9BSD
headers, and it is why each constant is cited: a citation is what distinguishes
transcribing a fact from copying an implementation.

The same line applies to two other programs in that tree that read this format,
both of which were read for what they *say* and neither of which contributed
code:

- `tools/dasm/libword/` (GPL) — the container formats a 36-bit word is stored in,
  which is where the name `data8` for what this project calls `le64` comes from.
- `tools/mldev/` — a FUSE client for the ITS *network* file protocol, not the
  disk format. Different problem, listed here so nobody goes looking for disk
  layout in it.

`itsfs` is clean, original C. Disk images and the ITS source tree are usable
locally for validation and are never committed. Test fixtures are built by the
test suite.

## Two traps

**1. MIDAS numbers are octal unless they carry a trailing dot.**

```
DEFSYM	LTIBLK==20		;BYTES MAPPING THE DISK START HERE      <- 16 decimal
DEFSYM	UDDESC==11.	;FIRST LOC AVAIL FOR DESC                   <- 11 decimal
```

Two lines, two bases, and the only difference is a full stop. `its.h` writes each
constant in the base FSDEFS writes it in and says so, so a line can be checked
against the source by eye without converting anything.

**2. FSDEFS's prose gives ASCII codes for characters it stores as SIXBIT.**

The link-target encoding is described as quoting `";" (73)` and `":" (72)`. Those
are the ASCII codes. The bytes on the disk are SIXBIT — **033 and 032** — and
taking the numbers at face value finds no separator anywhere, rendering every
link as one run-on string. The same sentence gives space as `(0)`, which *is* the
SIXBIT value. One sentence, two encodings.

Found by dumping the bytes of a link that was already "working", and settled
empirically. **ITS's own code then confirmed it**, which is worth more than the
measurement: `NSALV`'s link parser compares against character constants —

```
LTYPE:	MOVEI B,6
LTYPE2:	IDPB Z,E		;Z accumulates the link.
	ILDB A,N		;Get a byte.
	JUMPE A,CPOPJ		;Not expecting zeros in the link.
	CAIN A,':		;Quoting character?
```

— and MIDAS assembles `'X` as **SIXBIT**, not ASCII. From MIDAS's own source,
`src/midas/midas.458`:

```
SQUOT9:	JSP F,QOTCON	;SIXBIT SYL
	CAIGE A,40
	 ETR ERRN6B	;NOT SIXBIT
	CAIL A,140
	 SUBI A,40	;CONVERT TO UPPER CASE
	LSH T,6		;SHIFT OVER ACCUMULATED VALUE
	ADDI T,-40(A)	;ADD IN SIXBIT FOR CHARACTER IN A
```

`';` is therefore 073 − 040 = **033**, and `':` is 072 − 040 = **032**: exactly
the bytes on the disk. So FSDEFS's comment and ITS's own code disagree about this
encoding, and the code is right — which is the general lesson worth taking, not
just this instance.

This cost an afternoon here and is written up in `its.h` beside the constants, in
[the file system](filesystem.md#links), and in
[validation](validation.md#what-has-been-found-so-far).

## Evidence markers

`its.h` marks every field:

```
[v]   read in FSDEFS and seen to decode correctly on a real ITS pack
[s]   read in FSDEFS, not yet exercised against a pack
```

A `[s]` field is not less trustworthy as a *transcription* — it is less
trustworthy as an *understanding*, and a reader should not lean on one. The gap
register in [the file system](filesystem.md#gap-register) lists every one that
matters and says what is missing.

What "a real pack" means here is stated plainly because it bounds every `[v]`:
an RP06 image built by the PDP-10/its Makefile, booted, and run —
`out/simh/rp0.dsk`, SHA-256 `27d980f2…`. It is not an artifact recovered from
MIT. A pack built from source in 2026 exercises the format that ITS *writes
today*; a 1980s pack could exercise fields that nothing has written since.
Finding one is on the roadmap.

## Corroborating implementations

Not sources — second opinions, and named as such:

- **SIMH** (`PDP10/pdp10_rp.c`, `sim_disk.c`) — the disk container is
  little-endian per 8-byte word, on any host, because the byte swap is
  conditional on the host being big-endian.
- **KLH10** — defines `dbd9`, two words in nine bytes. DEC never had such a
  format, so KLH10's source is that format's specification rather than a second
  opinion on it.
- **TM03 Magnetic Tape Formatter User Guide**, EK-OTM03-UG-003, table 2-12 — the
  core-dump frame layout, from the hardware that does the packing. The formatter
  splits the word; the operating system only hands it over.

The last two are why `core` and `dbd9` are implemented at all. Both are now
`confirmed`, and neither on the strength of the documents above alone: `core` by
finding three strings ITS prints inside the tape it loads them from, and `dbd9`
by agreeing byte for byte with a pack written by KLH10's own `vdkfmt` — see
[word packing](word-packing.md).
