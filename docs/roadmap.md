# Roadmap

Where each phase ends, and what has to be true before it can be called done.
[PLAN.md](../PLAN.md) has the same table; this is the detail.

Phases 0–4 are done. What follows them is written here so that the next person
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

---

## Next

### Phase 5 — `itsfs check`

An independent checker: **it must share no code with the reader**. That rule is
what makes a clean check evidence rather than the reader agreeing with itself,
and it caught real bugs in both sibling projects.

For ITS the checker's shape is different from either sibling's, because the TUT
is a reference count rather than a bitmap or a chain. The job is:

- walk every directory, decode every descriptor, and **count references per
  block**, not merely mark them used;
- compare that count against the TUT's, per block, and report the three kinds of
  disagreement separately: a block files reference that the TUT calls free, a
  block the TUT calls used that no file references, and a count that differs;
- check that the locked-out set is exactly the directories and the tables;
- check each UFD's `UDBLKS` against its own descriptors;
- check that the two areas in each UFD have not overrun each other.

**Ends when:** it is clean on a real pack, and names the right block on a pack
damaged on purpose — and when `tests/accounting.sh`'s three checks are things it
does rather than things a shell script does.

An honest limit to state now: `check` and the reader will share `its.h`, and both
will inherit any misreading in it. `t10fs` documents the same limit and found its
one real instance with the fuzzer rather than the second implementation. A second
reading catches a wrong reading; it cannot catch what the source never says.

### Phase 6 — manifest, verify, and a shell

`manifest` fingerprints a pack — one line per file, with a checksum over **words**
rather than bytes, so that a manifest taken from an `le64` image verifies against
the same pack in `dbd9`. `verify` diffs a pack against one. A shell makes
exploring a pack you do not trust a read-only activity by default.

**Ends when:** a manifest survives a repacking, and `diff` on two manifests is a
useful description of what a monitor did to a pack while it was running.

### Phase 7 — the writer

One file, one mutation path, every front end calling it. In order:
`put`, `del`, `mkdir`, `mkfs`.

Two ITS-specific things to get right, neither of which has an analogue in
`t10fs`:

- **A directory is one block, so it can be full**, and full is a refusal rather
  than an allocation problem. The failure mode is `directory full`, stated with
  the number it refused at, and the directory left exactly as it was.
- **The TUT is a reference count**, so allocation is increment and free is
  decrement — and a block at `TUTMNY` ("many or more") cannot be decremented
  correctly at all, which is a property of the format the writer has to have an
  answer for rather than discover.

**Ends when:** every mutating flow in the suite ends with `check` clean.

### Phase 8 — native-tool interop

The point of the whole exercise, and ITS makes it unusually approachable because
it builds from source.

- **`NSALV`** — ITS's own salvager, a standalone program that walks the file
  system and reports what is wrong with it. It is `DSKRAT`'s counterpart in
  `t10fs`, and `make prove` there is the template: hand it a pack we wrote, and a
  pack we damaged, and require it to agree with `itsfs check` about both.
- **The monitor** — boot ITS on a pack we wrote to, and have it read the file, and
  copy it, and let us read the copy back.
- **A pack built from nothing** — `mkfs`, then `DSKDMP` or the monitor booting on
  it.

**Ends when:** ITS reads what we wrote and `NSALV` finds nothing wrong with it.

### Phase 9 — tapes

The container (SIMH `.tap` framing, or ITS's own), and the archive above it (DUMP
save sets, which `itstar` also reads). This is where `core` gets promoted from
`corroborated` to `confirmed`, because it is where an ITS artifact stored in it
finally gets decoded.

**Ends when:** a tape this project writes and a tape ITS wrote are the same
records.

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
- **A second `FSDEFS`** from another ITS release turns the version-span question
  from an open one into a measured one, and it is cheap: the diff is the whole
  method.
- **A multi-pack file system.** `UNPKN` and `QTRSRV` are read and ignored today.
  `t10fs` found two real bugs the first time it met a genuinely multi-unit
  structure.
