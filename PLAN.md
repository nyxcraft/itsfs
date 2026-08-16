# itsfs — plan

A host-side toolkit for the **ITS file system**, in the shape of
[`s5fs`](https://github.com/nyxcraft/s5fs) and
[`t10fs`](https://github.com/nyxcraft/t10fs): one dependency-free C99 binary,
git-style subcommands, one mutation path, an independent checker, and a
byte-fidelity correctness bar.

This is the planning document. It records what has already been **measured on a
real ITS pack**, what is still assumption, what the two sibling projects give us
for free, and — most importantly — the four places where ITS differs enough that
copying `t10fs` would be a mistake.

---

## 1. The name

`itsfs`. Short prefix + `fs`, matching `s5fs` and `t10fs`.

ITS had no name for its file system; it was simply *the disk*, and the monitor
source calls the pieces by their initials — MFD, UFD, TUT. There is no period
short name to inherit, so this is a coinage in the same way `t10fs` is.

The distinction from `t10fs` is not cosmetic. ITS and TOPS-10 ran on the same
hardware, addressed the same drives, and share the 36-bit word and SIXBIT — and
below that they have almost nothing in common. Directories, allocation, block
addressing and file identity are different designs, not dialects.

---

## 2. What is already confirmed

Not assumed — **measured**, against `~/its/out/simh/rp0.dsk`, an RP06 built and
booted by the [PDP-10/its](https://github.com/PDP-10/its) Makefile.

| fact | evidence |
|---|---|
| Words are **36-bit**, one per **8-byte little-endian** container | all 39,641,600 words round-trip byte-for-byte; nothing on the pack sets a bit above 36 |
| A sector is **128 words**; an ITS **block is 8 sectors = 1024 words** | `SECBLK==8`, and `UNWRDC` is the last block's word count *mod 2000 octal* = 1024 |
| Blocks are numbered **within a cylinder**, 47 to an RP06 cylinder | `NBLKSC==NHEDS*NSECS/SECBLK` = 47.5 **truncated**; four sectors of every cylinder are unreachable |
| The **MFD is at `NBLKS/2-1`** = block 19081 | word 5 of it is SIXBIT `M.F.D.`, which is exactly what `MDCHK` is for |
| A directory's **block is the position of its MFD entry** | `(A - 2000 + 2*NUDSL)/2`; 247 directories resolve, and each one's `UDNAME` matches the name in the MFD |
| A **UFD is one block**: header, descriptors up from word 11, five-word name blocks down from the end | reading it that way lists 3,000+ files whose names, dates and lengths are sane |
| A file's blocks are a **run-length program in six-bit bytes** | decoding it gives, for every one of 247 directories, exactly the block count `UDBLKS` independently records |
| The **TUT is a 3-bit reference count per block**, not a bitmap | 30,940 blocks in use — the same number the descriptors account for — and 505 locked out |
| Those 505 are **500 UFDs + 1 MFD + 4 TUT blocks** | `MDNUDS` says 500, `NTUTBL` says 4 |

That is enough to have written the word layer, the geometry layer, a read-only
reader and an independent checker — which is what exists today. See
[validation](docs/validation.md).

---

## 3. What is still assumption

Everything below is read in the source and **not yet exercised against a pack**,
or read and not understood. Each is marked `[s]` in [`src/its.h`](src/its.h) and
listed in the gap register in [the file system](docs/filesystem.md).

- **`UNTIM`**, the "compacted time of creation". 18 bits, and the values seen are
  too large for seconds since midnight. Half-seconds fits; that is a guess and is
  not implemented as anything but a raw number.
- **`UNBYTE`**, the byte size and odd-byte count. FSDEFS gives four ranges with
  different encodings, and every file on the reference pack has zero (which means
  36-bit bytes), so nothing has ever exercised the other three.
- **`UNDUMP`** — FSDEFS gives it as `400000`, which does not sit where the other
  flags in that word sit. Not implemented.
- **The width of the flag field** holding `UNLINK`/`UNREAP`/`UNWRIT`/`UNMARK`/
  `UNCDEL`. Six bits is deduced from the values, not transcribed.
- **`UNAUTH`**, "MFD index of author". The reference pack has all-ones (no
  directory) everywhere, so the index has never been resolved to a name.
- **Multi-pack file systems.** `UNPKN` is a pack number in every name block and
  `QTRSRV` names a "secondary" pack, so a file system spanning several drives is
  clearly provided for. One pack is all that has been read.
- **Version drift — now partly measured.** `make version-diff` compares `FSDEFS
  43` with `SYSENG;FSDEFS 40`, preserved in the PDP-10/its history: all 71
  symbols identical, the 65 differing lines all prose. So the format did not move
  across those three file-versions. What is still open is where they sit — both
  postdate the `9/5/79` TUT change they both mention, so the span has a floor and
  no ceiling, and a file version is not a release number.

**Do not extend `its.h` from this document.** Extend it from the source, and cite
the source per constant.

---

## 4. What ports over unchanged

Most of the value of the two siblings is architecture, and it transfers:

- **Exactly one mutation path**, when there is one. One writer core, every
  front-end calling it. This is the rule that keeps front-ends from inventing
  subtly different file systems.
- **An independent checker.** A checker sharing no logic with the reader is what
  makes "clean" evidence instead of a tautology. ITS has its own — `SALV`, and
  the KS10-era `NSALV` — which raises the bar further: there is a second opinion
  that is not ours.
- **Structure/encoding split.** Field offsets in one header that knows nothing
  about encoding; encoding in one module that knows nothing about file systems.
- **The test discipline.** Self-contained fixtures, `sh`+coreutils only, every
  mutating flow ending in a clean check, a regression test per bug **verified to
  fail against the code before the fix**, `make test-san` because an
  out-of-bounds read does not fault on a normal build, and a corruption fuzzer.
- **CI shape.** gcc + clang + macOS + sanitizers, `-Werror`, and the docs site.
- **Front-end families.** `ls/cat/get/put/cp/mv/rm`, a shell, a manifest/verify
  pair, analysis tools.
- **The evidence markers.** `confirmed` / `corroborated` / `structural` /
  `unverified` on packings, `[v]` / `[s]` on fields. Both were lifted verbatim,
  and both immediately earned their place: `core` and `dbd9` are `corroborated`
  here where `t10fs` has them `confirmed`, because no *ITS* artifact in either
  packing has been read yet.

`.clang-format`, the workflow files and `gh-pages/` were lifted essentially
as-is.

---

## 5. What must be designed fresh

Four differences are deep enough that copying `t10fs` would produce something
wrong. The first has no analogue in either sibling.

### 5.1 Block numbers are not offsets — there is a geometry layer

`t10fs` needs no such layer: a TOPS-10 block is `n * 128` words into the image
and the pack's geometry is bookkeeping the monitor keeps but the addressing does
not use.

ITS numbers blocks **within a cylinder**:

```
NBLKSC == NHEDS * NSECS / SECBLK      ; MIDAS integer division
```

For an RP06 that is `19*20/8 = 47.5`, truncated to **47**. A cylinder holds 380
sectors, ITS reaches 376, and **four sectors per cylinder are addressable by the
hardware and reachable by no block number** — 3,260 sectors, 3.2 MB, on a 300 MB
pack. On an RM03 it is six sectors per cylinder; on an RP07 the division is exact
and there is no remainder at all.

So there is a third layer between the packing and the structure, and it is not
optional: linear block numbering puts the MFD 3,244 sectors from where it is.
See [`src/itsgeom.h`](src/itsgeom.h) and [docs/geometry.md](docs/geometry.md).

### 5.2 Free space is a reference count, not a bitmap or a chain

The TUT is **three bits per block**: 0 free, 1 through 5 that many references, 6
"many or more", 7 locked out.

- The checker's job is to compare a computed count against a stored one, per
  block — a stronger statement than "allocated or not", and the reason the
  accounting on a real pack comes out exact rather than approximately.
- A block can legitimately be referenced twice. Do **not** design a checker that
  treats a second reference as corruption until it is known what makes one.
- Locked-out blocks are a third category, and on a real pack they are exactly the
  directories and the tables. That is a checkable invariant and
  `tests/accounting.sh` checks it.

### 5.3 A directory is one block, and that is the whole capacity

No growth, no chaining, no second cluster. 1013 words hold **both** the
descriptor area (growing up from word 11) and the five-word name blocks (growing
down from the end), and the directory is full when they meet.

`t10fs` spends real effort growing a UFD by a cluster and matching TOPS-10's
convention for doing so. Here there is nothing to match: the capacity is fixed
when the block is, and a writer's failure mode is `directory full`, which is a
refusal rather than an allocation problem.

### 5.4 A file's block list is a program, and a link's is a string

The same 13-bit `UNDSCP` field points at either, and one bit in the name block
says which. The block list is run-length coded with a load-address escape; the
link is the target's name in SIXBIT with its own quoting rules. Consequences:

- **Every descriptor is untrusted input in the most literal way**: it is a
  bytecode read out of a forty-year-old disk, and the interpreter must bound the
  opcode, the block number and the number of steps. It does.
- The two decoders share their byte reader and nothing else.
- File identity is `DIR;FN1 FN2` — three SIXBIT names, no extension in the DEC
  sense, and no `namei`-style path walk. Depth is exactly one.
- **Refuse a name the format cannot represent; never truncate.** Silent
  truncation in `s5fs` produced duplicate directory entries on an image `fsck`
  called clean. SIXBIT has no lower case, so this bites immediately.

---

## 6. The oracle

Better than either sibling had at this stage, because ITS builds from source.

[PDP-10/its](https://github.com/PDP-10/its) assembles the monitor, boots it under
SIMH, KLH10 or `pdp10-ka`, and leaves a working RP06. That gives all three levels
of evidence:

1. **Byte-identical** — build a file system with `itsfs`, build the same one under
   the real monitor, `cmp`.
2. **Accepted by native tools** — hand what we write to `SALV`/`NSALV`, ITS's own
   salvager, and to the monitor itself.
3. **Self-consistent** — round trips, and an internal check.

Level 3 and a read-only reader can be validated against a real pack *today*, and
have been. Level 2 has a specific target: `NSALV` is a standalone program that
walks the whole file system and reports what is wrong with it, which is exactly
`DSKRAT`'s role in `t10fs` — and `make prove` there is the template for what to
do with it.

**Copyright:** the ITS source tree is GPL and is never vendored, copied or
committed here. What is taken from it is what a file format *is* — offsets,
widths and opcodes — which is the same thing `t10fs` takes from DEC's
`COMMOD.MAC`. `itsfs` is clean, original C. Fixtures are built by the test suite,
never copied off a pack. See [docs/sources.md](docs/sources.md).

---

## 7. Phasing

Each phase ends somewhere useful, and nothing is written before it can be
validated.

| phase | deliverable | validated by | state |
|---|---|---|---|
| **0** | repo, `.clang-format`, CI, docs site, `make test` skeleton | CI green | **done** |
| **1** | word packing + SIXBIT | round-trip every word of a real pack byte-for-byte | **done** |
| **2** | the geometry layer | the MFD is found where the formula says, by its own check word | **done** |
| **3** | structure constants from **FSDEFS**, cited per field | the MFD, a UFD and the TUT decode | **done** |
| **4** | read-only reader: directories, descriptors, links, files | `make oracle`: the space on a real pack accounts for exactly | **done** |
| **5** | `itsfs check` — an independent checker | clean on a real pack, agreeing with the reader block for block; names the right file on a damaged one | **done** |
| **5a** | ITS's own salvager, against a pack we only READ | `NSALV` names the same blocks and files as `itsfs check`, at two damage sites | **done** |
| **6** | manifest / verify, and an interactive shell | a manifest from `le64` verifies against the same file system in `dbd9`, over all 6,303 entries | **done** |
| **7** | writer core, then the mutation engine | every mutating flow ends `check` clean, and `NSALV` accepts the result | **done** |
| **8** | native-tool interop: `NSALV`, then the monitor | ITS mounts and reads what we wrote | **all but one: NSALV, DSKDMP, a boot, and a pack built from nothing. The monitor OPENING a file is blocked on a console, not on the format** |
| **9** | tapes: DUMP save sets, and `.tape` containers | round-trips compare bytes | **done: both layers read, save sets written, and a file round-tripped through itstar and ITS unchanged** |

Phase 5a was added after the fact, and the lesson is worth keeping: the original
phasing assumed native-tool evidence had to wait for a writer, because levels 1
and 2 of the oracle both grade something we PRODUCED. They do not have to. A
native tool can grade a pack we only read, and doing that at phase 5 rather than
phase 8 cost one afternoon and produced the strongest result in the project.

Phases 1–6 are entirely read-only and carry no risk to a reference pack.
**Work on a copy regardless.**

---

## 8. Scope boundaries (decide now, revisit never)

- **ITS only.** TOPS-10 is `t10fs` and TOPS-20 is neither.
- **Disk packs**, not DECtape and not tape archives — those are front-ends to add
  in phase 9, not the core.
- **One pack per image to begin with.** Multi-pack file systems are clearly
  provided for by `UNPKN` and `QTRSRV`; defer deliberately, and say so.
- **The version span is an open question** (§3), not an assumed "covers
  everything".

---

## 9. First moves

Done, and recorded here because the order mattered:

1. `git init`; lift `.clang-format`, `.github/workflows/`, `gh-pages/` from
   `t10fs` and strip them to a skeleton.
2. Write the word layer and prove it: read every word of a **copy** of a real
   pack, re-encode, `cmp`. If that is not byte-identical, nothing above it can be
   trusted. (`make oracle`.)
3. Write `dump`, the tool every later phase is debugged with.
4. *Then* read FSDEFS, and only then write `its.h`.

Step 2 found nothing, which is the point of doing it first. Step 4 found the
geometry — which is to say, step 4 is where this project stopped being a copy of
`t10fs`.

---

## 10. The lessons worth carrying, not just the code

From the review history of both siblings — every one of these was a real bug,
found late:

- **Refuse what the format cannot represent.** Never truncate a name, a path or a
  count. Truncation produced duplicate entries, unreachable files, and a "clean"
  checker.
- **Bound every value that comes off the disk before using it as an index.** Here
  that is `MDNAMP`, `UDNAMP`, `UNDSCP`, `QFRSTB`, `QLASTB` and every descriptor
  opcode — all of which are bounded, and all of which the fuzzer attacks.
- **One definition of any layout arithmetic.** The block-map ladder in `s5fs` was
  open-coded in four readers and three had it wrong. `its_blk_sector()` is the
  only place a block becomes a sector, and `ITS_FIELD()` the only place a byte
  pointer becomes a value.
- **A tool that reports success while destroying data is worse than one that
  crashes.**
- **Verify a test against the broken code**, or it is decoration.
- **An out-of-bounds read does not fault on a normal build.** Sanitizers are not
  optional for a parser of untrusted input.
- **Re-check a source you wrote off.** `t10fs` promoted a whole packing from
  `corroborated` to `confirmed` by re-opening a scan it had recorded as
  unreadable. The equivalent here is FSDEFS's own prose: it says the skip opcodes
  have been unreachable "for years", and it says `";" (73)` when the byte on the
  disk is `033`. Read the comments, then check them.
