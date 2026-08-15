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
core   corroborated  five frames per word (magtape core-dump mode; TM03 user guide table 2-12)
dbd9   corroborated  two words in nine bytes, no waste (KLH10 disk images; its H36 tape format)
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

## Why `core` and `dbd9` are not `confirmed` here

They are in `t10fs`, on evidence that has not moved: the TM03 formatter manual
gives the core-dump frame table bit by bit, and KLH10's own source defines `dbd9`
because DEC never had a two-words-in-nine-bytes format to be wrong about.

But this project has not yet decoded an **ITS** artifact in either. The round
trip through them in `make oracle` proves the codecs are inverses of each other;
it does not prove that an ITS magtape is laid out that way, because no ITS
magtape has been read. Borrowing a sibling project's measurement and calling it
ours is exactly the thing the status markers exist to prevent.

Reading a DUMP tape is phase 9. When one decodes, `core` gets promoted, and the
line in `itspack.c` that explains why it was not will be replaced by one saying
what settled it.

## What is not here

**SIXBIT is not a packing.** It is an interpretation of a word's contents, not a
way of storing one, and it lives in `itstext.c` one layer up. The same goes for
the seven-bit ASCII that ITS text files are made of, and for the six-bit bytes a
UFD descriptor is read as — which are not characters at all, but the same
division of the word.

Keeping them apart is what lets a manifest taken from an `le64` image verify
against the same pack in `dbd9`: the words are the same, and only the bytes
differ.
