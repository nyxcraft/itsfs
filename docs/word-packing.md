# Word packing — not a byte-order codec

## Why this is a layer at all

`s5fs` has a byte-order module. It works because a UNIX file system is byte
addressed: the *order* of bytes varies between a PDP-11 and a VAX, and nothing
else does.

A PDP-10 disk is a sequence of **36-bit words**, and 36 does not tile onto 8.
There is no "the" way to lay a word down in bytes — only a set of conventions,
and each emulator and container format picked one. So the bottom layer here is
*word packing*: a pluggable pair of functions with a stride, not a byte swap.

Nothing above this layer ever sees a byte. That is what makes `itsfs repack`
possible: it rewrites every byte of an image and no word of it.

## Bit numbering

DEC's, throughout. **Bit 0 is the most significant** and bit 35 the least, which
is the opposite of the convention most host documentation uses. A word is
conventionally written as two 18-bit halves, `lh,,rh`, which is exactly the first
six and the last six digits of the twelve-digit octal — so `551646164416` is
`551646,,164416`, and `itsfs dump` prints both.

A byte pointer's `P` is the number of bits *to the right* of the byte, which is
why `ITS_FIELD(w, p, s)` is `(w >> p) & ((1 << s) - 1)` and not the other way
round.

## The four packings

```console
$ itsfs packings
name   status        layout
le64   confirmed     one word per 64-bit little-endian container (SIMH disk images; libword calls it data8)
be64   structural    one word per 64-bit big-endian container
core   confirmed     five frames per word (magtape core-dump mode; TM03 user guide table 2-12)
dbd9   confirmed     two words in nine bytes, no waste (KLH10 disk images; its H36 tape format)
```

`le64` is what a SIMH disk image uses, and for ITS it is measured rather than
assumed: every word of a real 300 MB pack decodes and re-encodes byte-for-byte.
Because `get()` masks to 36 bits, that byte identity also proves that no word
anywhere on the pack has a bit set outside the 36 — the invariant everything else
rests on, established by measurement rather than by assertion.

`dbd9` is the only packing with more than one word per group: two words in nine
bytes, sharing byte 4 between them — its high nibble ends the first word and its
low nibble begins the second. It is the reason the interface is group-based
rather than a single stride, and it is the packing that actually exercises the
read-modify-write path in `its_put_words()`.

## How `core` was confirmed

It sat at `corroborated` for eight phases, and the reason is worth keeping: the
layout was never in doubt — the TM03 formatter manual gives the frame table bit
by bit, and `t10fs` had already confirmed it — but no **ITS** artifact had been
decoded through *this* code. Borrowing a sibling project's measurement and
calling it ours is exactly what the status markers exist to prevent.

`make tape-test` settled it, two ways:

**Arithmetic.** `out/simh/salv.tape` carries the salvager as 79,890 bytes. That
is 15,978 × 5 exactly and 9,986.25 × 8 — so the file *cannot* be one word per
eight bytes, and can be one per five.

**ITS's own words.** Decoded as five frames per word and read as five seven-bit
characters each, the image contains:

```
"Salvager"                 word 9259
"Use MFD from unit"        word 9371
"unprotected in old TUT"   word 10026
```

Three strings this project has watched NSALV print on a console — the last of
them about a pack it damaged on purpose. Text that came out of the emulated
machine, found by this decoder in the file the machine loaded it from.

And `itsfs tape -x` extracts both files on that tape into words, which re-encode
to the host originals byte for byte.

**Three sources, and each supplies what the others cannot.** The TM03 formatter
manual gives the frame layout bit by bit, because the *hardware* does the
splitting — the driver hands over words and a mode. The measurement above shows
that ITS's own tapes decode that way. And ITS's tape documentation,
`doc/sysdoc/magtap.101`, supplies the middle term: which mode ITS asks for.

```
3.9=400,,0 Only meaningful for 9 track tapes
    0=> Core dump mode, 36 bit words, any density allowed.
    1=> IBM Character Mode, 32 bit words, doesn't allow 200/556 BPI
```

Core-dump mode is the **zero** case — what a program gets without asking — and
it is named as 36-bit words against a 32-bit alternative. So: the manual says
what core-dump mode *is*, ITS's doc says ITS uses it by default, and the tape
says the bytes agree. None of the three would be enough alone; the manual
describes hardware ITS might not have used that way, the doc names a mode
without laying out a frame, and a measurement of five-byte groups could in
principle be a coincidence of file length.

## How `dbd9` was confirmed

KLH10's source defines it, and DEC never had a two-words-in-nine-bytes format
for it to be wrong about — so that source is the specification rather than a
second opinion on one.

That source has now been read **in this tree, and checked rather than trusted**.
KLH10 declares the format in `vdisk.h`:

```
vdk_fmt(VDK_FMT_DBD9, "DBD9", "Disk_BigEnd_Double (9/2) (H36)",
                                9, cvtfr_dbd9, cvtto_dbd9)
```

and `cvtfr_dbd9` in `vdisk.c` assembles each word as `LRHSET(w, lh, rh)` out of
the same nine bytes. Running 200,000 random 9-byte groups through both that
formula and `itspack.c`'s gives **no disagreement** — they are the same function,
not merely the same intention.

### The attempt that failed, and why — which took two goes to get right

KLH10's DSKDMP was pointed at a pack repacked into `dbd9`. It answered `MFDCLB`
— "M.F.D. clobbered".

The control saved that from being reported as a finding: the *same* KLH10 and
the *same* DSKDMP, pointed at the **untouched `le64` pack** through KLH10's own
`SIMH` format, answer `MFDCLB` as well. A setup that cannot tell a good pack
from a bad one says nothing about the packing.

That much was right. The *cause* recorded beside it — that `build/klh10`'s
DSKDMP is build 216 against the pack's 217, and "probably expects a machine this
is not" — was a guess, and it was **wrong**.

KLH10 drives its disk through a separate process, `dprpxx`, which has to be
findable at run time:

```
[dp_exec: Cannot access "dprpxx" - No such file or directory]
[rp_dpstart: Start of DP "dprpxx" failed!]
Final init of device "dsk0" failed!
```

Three lines in the middle of the startup noise, after which there is no disk at
all and DSKDMP is reading nothing. Hence `MFDCLB`, for the `dbd9` pack and for
the control alike. With `dprpxx` present, DSKDMP 216 reads a `dbd9` pack and
lists a directory off it; take `dprpxx` away again and `MFDCLB` comes straight
back. The version difference was never involved.

The lesson is not "run the control" — the control worked exactly as intended. It
is that a control tells you your setup cannot answer the question, and **not
why**. The why still has to be found, and until it is, it is a guess and should
be written down as one.

### What settled it

Building KLH10 from the tree, which is worth doing for a reason that was not
obvious: it ships **`vdkfmt`**, KLH10's own disk-format converter. That means an
artifact written by KLH10's code without running the emulator at all.

```console
$ vdkfmt ip=rp0.dsk op=klh10.dbd9 ifmt=SIMH ofmt=DBD9 dt=RP06
$ itsfs repack -p le64 -P dbd9 rp0.dsk ours.dbd9
$ cmp -n 177776640 klh10.dbd9 ours.dbd9
$ itsfs check -p dbd9 -d rp06 klh10.dbd9
blocks         6719 free, 30940 in use, 505 locked out
directories    247, 5657 files, 399 links (7 of them unresolved)
no problems found (7 notes)
```

Two directions over one artifact, and neither of them ours. The repack is
byte-identical to KLH10's output over every byte KLH10 wrote, and the reader
takes KLH10's file to the same accounting — to the block — as the `le64`
original.

### The size difference is not damage

KLH10's file is 177,776,640 bytes and ours is 178,387,200, which looks exactly
like truncation. It is not. `vdkfmt`'s copy loop is

```c
if (!zerosector(wbuff, 128))
	err = devwrite(&dvo, nsect, wbuff);	/* Write a sector  */
```

— it never writes an all-zero sector, so the file simply **stops** at the last
non-zero one, 1,060 sectors short of the drive. Every sector it did write is at
its true offset, and our extra 610,560 bytes are all zero.

One practical consequence: a real KLH10 pack does not have its drive's nominal
size, so the size-based geometry inference refuses it and `-d rp06` must be
given. Refusing is the right behaviour — guessing a geometry is the one thing
this project will not do — but it is worth knowing before you meet it.

## What is not here

**SIXBIT is not a packing.** It is an interpretation of a word's contents, not a
way of storing one, and it lives in `itstext.c` one layer up. The same goes for
the seven-bit ASCII that ITS text files are made of, and for the six-bit bytes a
UFD descriptor is read as — which are not characters at all, but the same
division of the word.

Keeping them apart is what lets a manifest taken from an `le64` image verify
against the same pack in `dbd9`: the words are the same, and only the bytes
differ.
