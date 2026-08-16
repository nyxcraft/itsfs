# Roadmap

Where each phase ends, and what has to be true before it can be called done.
[PLAN.md](../PLAN.md) has the same table; this is the detail.

Phases 0–7 are done, and phase 8 in part. What follows them is written here so that the next person
does not have to re-derive the order — and the order matters, because each phase
is validated by something the previous one built.

---

## Done

### Phase 0 — the repository

`.clang-format`, CI, the docs site, a `make test` skeleton. Lifted from `t10fs`
and stripped, which is the correct amount of originality for a build system.

**Ended by:** CI green.

### Phase 1 — the word layer

Four packings behind one group-based interface, and SIXBIT one layer above it.

**Ended by:** every word of a real 300 MB pack round-tripping byte-for-byte,
including through a packing with a different stride and one that shares a byte
between two words.

### Phase 2 — geometry

The layer neither sibling project needs: block numbers are cylinder-major and the
blocks-per-cylinder division truncates.

**Ended by:** the MFD being found where the formula says it is, and identifying
itself with its own check word. Also by `itsfs dump -s`, which addresses raw
sectors and lets the two be compared.

### Phase 3 — the constants

`its.h`, transcribed from `SYSTEM;FSDEFS 43` with a citation and an evidence
marker per field.

**Ended by:** the MFD, a UFD and the TUT all decoding into values that are sane
in more than one way at once — dates in two plausible eras, names that match
between the MFD and each UFD's own header.

### Phase 4 — the reader

Directories, name blocks, the descriptor bytecode, links, file extraction, and
the TUT.

**Ended by:** `tests/accounting.sh` — the space on a real pack accounting for
exactly, three ways, against numbers this project did not compute. See
[validation](validation.md#level-3b--the-space-on-a-real-pack-accounts-for-exactly).

### Phase 5 — `itsfs check`

An independent checker: it shares no code with the reader. `cmd_check.c`
includes neither `structure.h` nor `itsgeom.c`'s conversion, and re-derives the
block-to-sector arithmetic, the MFD-slot formula, the UFD layout, the descriptor
bytecode and the TUT from `its.h`.

It counts references **per block**, because the TUT is a reference count rather
than a bitmap, and reports the four kinds of disagreement separately; it checks
each UFD's `UDBLKS` against its own descriptors, that the two areas in a
directory have not overrun each other, and that the locked-out set is exactly
the directories, the MFD and the TUT.

**Ended by:** clean on a real pack, with the same numbers the reader gets;
naming the right blocks and the right files on a pack damaged on purpose; and
`tests/accounting.sh`'s three checks now being things it does in C rather than
things a shell script does with the reader. Both still run in `make oracle`,
because two implementations agreeing is the point and either alone is one
opinion.

One thing it deliberately does **not** call damage: a link pointing at a file
that is not there. Seven on the reference pack do, and a live system is like
that. They are notes, they are listed under `-v`, and they do not change the
exit status.

### Phase 5a — `NSALV`, out of order and on purpose

**This needed no writer**, which is why it ran before phase 6 rather than as part
of phase 8. `NSALV` is ITS's own salvager: it boots standalone from a tape, walks
every directory on a pack, rebuilds the allocation table from scratch and reports
what disagrees. Pointing it at a pack this project has only *read* asks the one
question `itsfs check` cannot answer about itself.

Two runs, because the clean one proves less than it looks: `NSALV` finishes
silently on a good pack, which is indistinguishable from its never having run.
The damaged run is what makes the clean one evidence.

**Ended by:** both checkers naming exactly the same blocks and the same files on
a pack with one TUT word cleared — 11 at one damage site, 8 at another, sorted
and compared as pairs rather than as counts. `make nsalv` does it; see
[validation](validation.md#a-second-opinion-that-is-not-ours).

It also settled a format question as a side effect. `NSALV`'s link parser
compares against `':` and `';`, and MIDAS assembles `'X` as SIXBIT — so ITS's own
code confirms the encoding that FSDEFS's *comment* gets wrong. See
[sources](sources.md#two-traps).

---

## Next

### Phase 6 — manifest, verify, and a shell

`manifest` fingerprints a pack — one line per directory, file and link, with a
checksum over **words** rather than bytes. `verify` diffs a pack against one. The
shell keeps the image path and the current directory so that exploring is three
short lines rather than three long ones.

**Ended by:** a manifest taken from the reference pack in `le64` verifying clean
against the same file system in `dbd9`, over all 6,303 entries — two images
sharing no byte boundary and no differing word. And by the inverse: one flipped
bit in 39.6 million words reported by file name. See
[validation](validation.md#the-fingerprint-is-of-the-file-system-not-of-the-container).

The shell is read-only and has no `-w`, because there is nothing to guard yet.
Its prompt says `(ro)` so that the day there is a writer, the difference is
visible rather than assumed.

One thing worth recording from building it: `blocks` prints runs rather than
block numbers, and that turned out to matter. 5,431 of the reference pack's 5,650
files are a single run — a fresh build writes sequentially — but 219 are not, and
one is 59 runs. A display that printed 1,904 numbers would have hidden that.

### Phase 7 — the writer

One file, one mutation path, every front end calling it. `put` and `del` exist;
`mkdir` and `mkfs` do not yet.

**Ended by:** `NSALV` accepting a file system this project wrote — level 2 in the
oracle's own terms, and the first time anything here has been graded by ITS
rather than compared with it. See
[validation](validation.md#level-2--itss-own-salvager-accepts-what-the-writer-produced).

The two ITS-specific things it had to get right, neither of which has an
analogue in `t10fs`:

- **A directory is one block, so it can be full**, and full is a refusal rather
  than an allocation problem — there is no way to grow a UFD. The message names
  the two numbers that met.
- **The TUT is a reference count**, so a block at `TUTMNY` ("many or more")
  cannot be decremented correctly by anybody. `del` refuses it by name rather
  than guessing.

And one that was not on the list: **the name area is sorted**, and a writer that
appended would produce a directory ITS's own lookup walks wrongly. That was
found by measuring the reference pack rather than by reading the source, and it
is why `put` inserts and shifts.

### Phase 8 — native-tool interop — PARTLY DONE

Three of the four things here exist:

- **`NSALV` accepts a pack we wrote** (phase 7, `make nsalv`).
- **`DSKDMP` lists our file**, with its own reader — a third implementation,
  sharing nothing with the monitor or with `NSALV` — and the listing is still in
  sorted order, which is what catches a writer that appended (`make interop`).
- **`mkdir` exists, and DSKDMP reads what it makes.** The MFD entry's position is
  the address of the directory's block, with no pointer to check it against, so
  a third implementation resolving it is the only way to know it is right.
- **ITS boots on a pack we wrote**, running its startup salvage over every
  directory on the way (`make interop`).

**What is left, and it is the interesting half:** making the monitor OPEN the
file and print it. `:print DIR;FN1 FN2` is the command. The obstacle is not the
file system — it is the console, and what has been ruled out is worth knowing
before trying again:

- `^Z` on the CTY produces nothing, with or without the SYSJOB patch, though
  ITS's own `doc/DDT.md` says that is how you get a terminal.
- `set cpu idle` is not the cause; tried without it.
- Nor are the DZ lines: 0, 5, 6 and 7 over raw sockets with the telnet option
  negotiation answered properly. simh accepts the connection; ITS never speaks.
- **The likely cause, untested because testing it needs a console:** the finished
  system auto-starts a job — an unpatched boot prints `LOGIN TARAKA 0` — and that
  job owns the CTY. The ITS build drives the console successfully during a
  *build*, where no such job exists yet, which fits.

- **`mkfs` exists**, and both NSALV and DSKDMP accept a pack built from nothing
  (`make mkfs-test`). It writes a file system rather than a bootable pack, so
  both are booted from tape; see
  [validation](validation.md#a-file-system-with-no-its-in-it).

**Ends when:** the monitor opens a file we wrote and prints it. Everything else
in this phase is done.

### Phase 9 — tapes

`itsfs tape` reads the SIMH `.tap` container: records with their lengths on both
sides, tape marks, end of medium. It checks the two lengths against each other,
which is the one thing the format itself lets a reader notice, and `-x` extracts
each file as 36-bit words.

**Ended by:** `make tape-test` — the salvager tape's two files extracted and
re-encoded to the host originals byte for byte, and `core` promoted from
`corroborated` to `confirmed` on the strength of finding three strings in the
image that this project has watched NSALV print. See
[word packing](word-packing.md#how-core-was-confirmed).

**And the archive layer over it.** `itsfs saveset` reads a DUMP save set:
volume header, a header per file, data to a tape mark. Graded against `itstar`,
which reads these for the PDP-10/its project — 3,795 entries on a 91 MB tape,
name for name and in the same order. It caught a real bug doing it; see
[validation](validation.md#save-sets-against-itstar).

**And writing one.** `itsfs save` writes a save set from files on a pack, and
itstar reads it, extracts it, and returns the host file ITS was originally given
— byte-identical, with its date and its link intact. See
[validation](validation.md#a-round-trip-through-two-implementations-and-an-operating-system).

Writing follows itstar's own shape, which is not the obvious one: words are
appended to a record buffer and flushed when it fills, so a header shares a
record with the data after it. itstar's `save` has the flush after the header
commented out on purpose. Writing one structure per record would be tidier and
would produce a tape unlike any ITS made.

---

## Not on the roadmap

- **Repair.** A checker that can also fix things is two programs, and the second
  is written after the first is trusted.
- **FUSE, or any kind of mount.** `tools/mldev` in the ITS tree mounts an ITS file
  system over the *network* protocol; that is a different problem and it is
  already solved.
- **DECtape.**
- **Anything that is not ITS.**

## Open questions that could reorder this

- **A pack recovered from MIT** would be worth more than any two phases here, and
  would immediately exercise `UNBYTE` encodings that nothing has written since the
  1980s. If one turns up, read it before writing anything.
- **A KLH10-built ITS pack** would promote `dbd9` the way a tape promoted
  `core`. `make EMULATOR=klh10` in the ITS tree produces one, and it has to be a
  full build: the shortcut of repacking an existing pack and pointing KLH10's
  DSKDMP at it does not work, and the reason is recorded in
  [word packing](word-packing.md#the-attempt-to-promote-it-and-why-it-failed) so
  nobody spends the afternoon on it twice. The codec itself is now checked
  against KLH10's own `cvtfr_dbd9`, exhaustively.
- **A `FSDEFS` from before September 1979.** `make version-diff` already compares
  versions 40 and 43 and finds all 71 symbols identical, but both postdate the
  TUT format change both of them mention, so the span this reader covers has a
  floor and no ceiling. A pre-1979 one would be the interesting artifact.
- **A multi-pack file system.** `UNPKN` and `QTRSRV` are read and ignored today.
  `t10fs` found two real bugs the first time it met a genuinely multi-unit
  structure.
