# Design

What the layers are, which rules are not negotiable, and what is deliberately
out of scope.

## The layers

```
   cmd_*.c        front ends: info, dump, dirs, ls, cat, get, free, ...
   ---------------------------------------------------------------------
   structure.c    the file system: MFD, UFD, name blocks, descriptors, TUT
   its.h          every on-disk offset, transcribed from FSDEFS with a citation
   ---------------------------------------------------------------------
   image.c        an image, addressed in words and in ITS blocks
   itsgeom.c      WHERE A BLOCK IS: cylinder-major numbering, and its remainder
   ---------------------------------------------------------------------
   itspack.c      how a 36-bit word is stored in 8-bit bytes
   itstext.c      what the bits mean when they are characters
```

Each layer knows nothing about the one above it. `itspack.c` has never heard of a
block; `itsgeom.c` has never heard of a directory; `structure.c` has never heard
of a byte.

The middle layer is the one that is not in `t10fs` or `s5fs`, and it is there
because ITS block numbers are not offsets. [Geometry](geometry.md) is the whole
of that argument.

## The invariants

Each of these cost somebody a bug once, in this project or in a sibling.

**1. Never lay a host integer over image bytes.** Every word enters through
`its_pack`'s `get()` and leaves through `put()`, masked to 36 bits. A `uint64_t`
carrying a word always has its top 28 bits clear. This is what makes `repack`
possible and what makes the byte-for-byte round trip mean something.

**2. Nothing in `its.h` is a C struct, and nothing ever will be.** A PDP-10 word
is 36 bits; there is no host type that is one. Every field is fetched by word
index out of a `uint64_t[]`.

**3. One definition of any layout arithmetic.** `its_blk_sector()` is the only
place a block becomes a sector. `ITS_FIELD()` is the only place a byte pointer
becomes a value. `its_pack_bytes()` is the only place a word count becomes a byte
count. `s5fs` learned this by open-coding a block-map ladder in four readers and
getting it wrong in three.

**4. Bound every value that comes off the disk before using it as an index.**
`MDNAMP`, `UDNAMP`, `UNDSCP`, `QFRSTB`, `QLASTB`, every descriptor opcode and
every block number a descriptor produces. All of them are checked at the point
they are read, not at the point they are used, and the fuzzer attacks each.

**5. Refuse what the format cannot represent.** A name that is not SIXBIT is an
error, not something to truncate or case-fold. Silent truncation in `s5fs`
produced two directory entries that looked identical and named different files,
on an image the checker called clean.

**6. A refusal names the thing it refused.** `no directory named 'NOSUCH' in the
MFD`, not `error 2`. Half the test suite asserts on those strings, which is also
what keeps them from rotting.

**7. Evidence markers are part of the code.** A packing says whether it is
`confirmed` or `corroborated`; a field says `[v]` or `[s]`. "We implemented it"
and "we know ITS did it this way" are different claims, and a project that stops
distinguishing them starts quietly inventing a file system.

Two more are held in reserve for code that does not exist yet:

**8. Exactly one mutation path.** When there is a writer, it is one file, and
every front end calls it.

**9. The checker shares no code with the reader.** A clean check has to be
evidence rather than the reader agreeing with itself.

## What is deliberately out of scope

- **Anything that is not ITS.** TOPS-10 is `t10fs`; TOPS-20 is neither.
- **Writing, for now.** Phase 7. Until then `img_write_words()` exists, is
  unreachable from any command, and is there so that the read-modify-write shape
  a shared-byte packing needs is settled before anything depends on it.
- **Interpreting file contents.** `cat` writes the seven-bit characters that are
  on the disk and translates nothing — not line endings, not control characters.
  An extraction tool that quietly changes a file is worse than no extraction
  tool. (ITS text uses CRLF; that is the file's, not ours.)
- **Repairing.** A checker that can also fix things is two programs, and the
  second one has to be written after the first is trusted.

## Why the front ends are separate files

`cmd_dump.c` is the tool every other phase is debugged with, and it must keep
working when the structure layer is broken — including when the structure layer
is broken *on purpose*, which is what the fuzzer does. So it depends on nothing
above `image.c`, and `itsfs dump -s` bypasses even the geometry, addressing raw
128-word sectors. When a block does not decode, that is how you find out whether
the block, the geometry or the reader is at fault.
