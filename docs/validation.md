# Validation

How correctness is established here, what has actually been established, and what
has been found by trying.

## The standard

Three levels of evidence, in the order they are worth anything:

1. **Byte-identical** — build something with `itsfs`, build the same thing with
   ITS, `cmp`. Nothing here reaches this level.
2. **Accepted by native tools** — hand what we produce to `NSALV`, ITS's own
   salvager, or to the monitor. **`NSALV` accepts what the writer produces, ITS
   boots on it, and DSKDMP lists the file.** What is still missing is the
   monitor *opening* the file. See below.
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

### A third kind of agreement, found by accident

Halting the emulator abruptly on a pack ITS had been running left 50 blocks whose
allocation-table count was 2 and which only one file claimed. `itsfs check`
reported them:

```
block 2004: the TUT says 2 references, the files make 1
```

`NSALV`, asked about the same pack, answered:

```
TUT #0 50 1_2
```

— 50 blocks whose computed value is 1 and whose stored value is 2. **The same
50 blocks, by number**, on both sides.

That is worth more than it looks. The two earlier comparisons were both of the
same category, "a file holds a block the table calls free". This is a different
one — a *miscount* — and it was not constructed: ITS produced it by being
switched off mid-write, and both checkers independently described it the same
way. It is also the first time this project has seen a reference count above 1,
which `docs/filesystem.md` had listed as an open question for eight phases.

It also cleared `put` of suspicion. A `put` onto that pack reported the same 50
problems and no more, so the writer added none of them.

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

## Level 2 — ITS's own salvager accepts what the writer produced

```console
$ make nsalv IMAGE=~/its/out/simh/rp0.dsk
4. a pack itsfs WROTE to
  ok   itsfs put wrote 2 files
  ok   ...and both read back byte-identical to what went in
  ok   itsfs check: clean
  ok   NSALV: ACCEPTED IT -- salvaged, and had nothing to say
  ok   ...and the files are still there afterwards
```

This is a different and stronger claim than the three sections above it, which
grade a *reading* of a pack ITS wrote. Here ITS's own salvager walks a file
system this project made and finds nothing to say about it — the same silence a
pack it has never touched produces.

**Four checks, in that order, and the order is the argument.** A pack that
passes a salvager and does not return the file is not a success, so the files are
read back first. `check` is asked before `NSALV` because it is the cheap one. And
the pack is checked again *afterwards*, because a salvager that quietly repaired
something would have hidden a disagreement by fixing it.

### What the writer had to get right to earn that

- **The name area is sorted**, and stays sorted. 6,056 entries on the reference
  pack, not one out of order and no gaps; after four `put`s at four different
  sort positions it is 6,060 with the same properties. Insertion shifts the
  entries below the point down by `LUNBLK` and moves `UDNAMP` with them — the
  mirror of `QSQSH`, which is how ITS closes the gap on removal.
- **`UDBLKS` is maintained**, so the number ITS keeps and the number the
  descriptors decode to still agree.
- **The TUT entry is set to 1**, which is what `QGTK2` does, rather than
  incremented.
- **Blocks come from above `QSWAPA`**, which FSDEFS describes as the line new
  files are not written below.

### What put/del does NOT do, deliberately

`del` zeroes the descriptor bytes in place and does not compact the descriptor
area — because `QDEL3` does exactly that and leaves the same hole. So `put` then
`del` is **not byte-identical**; it is identical to a fingerprint, which is the
right test:

```console
$ itsfs manifest w.dsk > before.mf
$ itsfs put w.dsk 'TEST;TEMP FILE' f.txt && itsfs del w.dsk 'TEST;TEMP FILE'
$ itsfs manifest w.dsk | diff before.mf -
$
```

## Level 2, completely — the monitor opens the file and prints it

```console
$ make interop IMAGE=~/its/out/simh/rp0.dsk
5. ...and the monitor opens the file and prints it
  ok   removed the job that owns the console (itsfs del)
  ok   THE MONITOR PRINTED A FILE THIS PROJECT WROTE
       :print KSHACK;ITSFS TXT

       HELLO FROM ITSFS.
       IF ITS LISTS THIS THE ENTRY IS REAL.
```

This is the claim the project was built toward: not a salvager inspecting the
bookkeeping, not a standalone loader listing a directory, but **the operating
system opening a file through its own file-system code and printing what is in
it**.

### The obstacle was never the file system

`^Z` on the console produced nothing for four attempts across two phases, and the
reasons ruled out are recorded in `tests/interop.sh`: not `set cpu idle`, not the
DZ terminal lines over raw sockets with telnet negotiation answered, not the
SYSJOB patch.

**The cause was a job holding the console.** The finished system auto-starts
whatever `SYS;ATSIGN DRAGON` names — an unpatched boot prints `LOGIN TARAKA 0` —
and that job owns the CTY. While it is there, `^Z` does nothing at all. That is
why the ITS build's own scripts drive the console successfully during a *build*,
where no such job exists yet, and why the same sequence failed here.

**`itsfs del` removed it.** The project's own writer cleared the way for its own
reader to be graded — which is a pleasing shape for the last step to have, and
is also the only tool available: there is no console to type `:delete` at until
the file is gone.

### It is reproducible, and it is not reliable

That distinction is worth being exact about. The result has been reproduced by
hand and through `make interop`. But **getting a console is timing-dependent in
a way that has not been pinned down**: the same pack and the same script get one
on one run and not the next, and eight `^Z`s over four minutes sometimes produce
nothing at all.

So the harness reports three outcomes rather than two. The monitor printing the
file is the result; the monitor getting a console and *not* printing it is a
finding and fails the run; **never getting a console is neither** — it is the
harness failing to ask the question, and it says so and does not fail. Reporting
a missed `^Z` as "the monitor cannot read our file" would be reporting a lie
about the file system.

Two more things the harness records, because both cost an afternoon:

- **One `^Z`, after a long settle, and retry with long gaps.** A `^Z` arriving
  while ITS is still starting its jobs is simply lost, and how long that takes
  varies with the host's load.
- **Type blind, about a third of a second per character.** ITS echoes when it
  *processes*, not when it receives, so waiting for each character's echo —
  which is what the ITS build's scripts do — deadlocks against a running system.
  Sending the line in one burst loses it.

## Level 2 — ITS boots on it, and a third implementation reads it

```console
$ make interop IMAGE=~/its/out/simh/rp0.dsk
2. DSKDMP, ITS's standalone loader, reads the directory
  ok   DSKDMP listed our file, reading the directory with its own code
  ok   ...and the whole listing is still in order (40 entries)

3. ...and then the monitor itself boots on it
  ok   the monitor ran its startup salvage over the pack
  ok   ITS CAME UP on a pack itsfs wrote to
  ok   ...and the file is still there, unchanged, after ITS had the pack
  ok   ...and the pack still checks clean
```

`NSALV` inspects a file system for a living. These two *use* one, which is a
different thing to ask of them.

**DSKDMP is a third implementation.** It is ITS's standalone dumper: it boots
from the disk on its own, reads the file system with its own code, and
`U<ESC>dir;` lists a directory. It shares nothing with the monitor or with
`NSALV`. Its listing contains our file — and, more usefully, **the whole listing
is still in sorted order**, which is the check that catches a writer that
appended instead of inserting. A file at the end of the list would be listed
just as happily.

**And it reads a directory this project made from nothing.** That is the stronger
of the two, because of what a directory IS here: the MFD entry's own *position*
is the address of its block, with no pointer anywhere to check it against. So a
`mkdir` that put the entry in the wrong slot would produce a directory this
project reads back perfectly and ITS looks for somewhere else. DSKDMP resolving
it with its own arithmetic is the only way to know.

**The monitor's startup salvage is a gate.** ITS runs a salvage pass over every
directory on the pack before it comes up; reaching `IN OPERATION` means that pass
found nothing worth stopping for. And the pack still holds the file, unchanged,
after ITS has had it — a boot writes.

### What this does not establish

**This was the remaining half of phase 8, and it is done** — see the section
above. What it needed was not a file-system fix but the removal of a job that
owned the console.

### A method that looked right and was not

`DSKDMP` has `L<ESC>file` (load a file into core) and `I<ESC>file` (verify a file
against core), which look exactly like a byte-for-byte comparison harness: load
the original ITS wrote, then verify our copy of it against core. It reported
`CMPERR`.

The control run is the only reason that is not written up here as a finding:
**loading the original and verifying it against *itself* also reports `CMPERR`.**
Whatever those two commands compare, it is not what it appears to be, and the
first result said nothing about the writer. Verifying a test against known-good
input is the rule that caught it.

## A file system with no ITS in it

```console
$ make mkfs-test
1. build a file system where nothing was
  ok   made an rp06 file system: pack 0, ID ITSFS, 500 directory slots, 1551 blocks of swapping
   317132800 bytes, 4028 KB actually on disk
  ok   itsfs check: clean, with nothing in it
  ok   ...and 505 blocks locked out: 500 directory slots + 1 MFD + 4 table

3. hand it to NSALV, ITS's own salvager (booted from tape)
  ok   NSALV: ACCEPTED IT -- salvaged, and had nothing to say
  ok   ...and read the pack ID we wrote: ID is ITSFS, Pack #0

4. and to DSKDMP, a third implementation (also from tape)
  ok   DSKDMP listed a directory on a pack with no ITS in it
  ok   ...both files, in order
```

Everywhere else the starting point is a pack ITS built, and the question is
whether this project reads or extends it correctly. **Here every word was written
here**: the master file directory, the allocation table, the directory blocks,
the directories, the files. A structure that is wrong only when nothing else is
right has nowhere to hide.

`mkfs` is a transcription like everything else — NSALV's `MFDINN` for the MFD,
`TUTINI` for the table and `MARK69` for the rest. Three things it takes from
them that would not have been guessed:

- **`MDNAMP` starts at the block size.** `MFDINN` writes `PG$SIZ`, which is an
  empty name area beginning past the end of the block — the same convention an
  empty UFD uses, and the one this project's reader refused for four phases.
- **`QFRSTB` is not always zero.** `TUTINI` computes it as the last block minus
  what the table can physically map, which is negative on an RP06 and clamped to
  zero — but would not be on a drive whose table was too small for it.
- **The MFD block is locked out via `SBTAB`**, NSALV's "special block table",
  and not by the loop that locks out the table itself. Reading only the obvious
  loop leaves the MFD marked free.

**It does not boot, and that is correct.** ITS starts from the front end's blocks
at the very bottom of the disk — the ones `ZAP` calls the "8080 HOM sectors" and
explicitly refuses to touch — and then loads a system out of a directory. `mkfs`
writes a file system; the boot area and the system are somebody else's job. So
both graders are booted from tape and pointed at the pack, and ITS coming up on
one is not claimed anywhere.

## A round trip through two implementations and an operating system

```console
$ make tape-test
4. a save set this project wrote, read by itstar
  ok   wrote a save set of two entries, one of them a link
  ok   ...and reads it back, link and all
  ok   ITSTAR READS IT: 2 entries, and the volume header
  ok   ...and extracts the file
  ok   ROUND TRIP: byte-identical to the host file ITS was given
  ok   ...and the link is a link: -> _/@.ddt
```

`kshack/its.15` has now been all the way around:

```
host file  --itstar-->  tape  --ITS's loader-->  the pack
           --itsfs reader-->  --itsfs save-->  tape
           --itstar's extractor-->  host file
```

and the two host files are the same 460 bytes. Its 1986 creation date survives
the trip — the date word is copied straight from the disk because the two
layouts turn out to be the same one, `year<<9 | month<<5 | day` in a halfword —
and the link comes out a link.

Nothing on that path is this project agreeing with itself: itstar wrote the tape
that went in, ITS put it on the pack, and itstar read the tape that came out.

## Save sets, against itstar

`itsfs saveset` reads the DUMP archive layer, and `itstar` — the reader the
PDP-10/its project uses for these tapes — is the oracle. On `sources.tape`,
91 MB and 3,795 entries including 68 links:

```console
$ itsfs saveset sources.tape | ... > mine
$ itstar -tf sources.tape   | ... > theirs
$ diff mine theirs && echo identical
identical
```

Name for name, in the same order.

**It found a bug on the first run, which is the whole reason for having it.**
The listing came to 3,734 — exactly 61 short, one per link. A link's target is
three SIXBIT words that **may share the header's own record**, and reading a
fresh record for it swallowed the next file's header. Nothing about the output
looked wrong: 3,734 plausible names in plausible order. `itstar` disagreeing
about the count is the only thing that noticed.

The same mistake had a second instance, found by fixing the first: a *file's
data* also begins in whatever is left of its header's record. Both come from the
same wrong assumption — one structure per record — and the format simply does not
work that way. `itstar` reads a new record only when the current one is
exhausted, and so does this now.

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

**264 checks** in `tests/run.sh`, of which about a third feed the reader or the
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

**`make test-san`** runs the same 262 under AddressSanitizer and UBSan, which is
where those checks have teeth: an out-of-bounds *read* does not fault on a normal
build. It returns whatever was next in memory, and the test passes.

**`make fuzz`** damages one random word of a valid pack, in a structure the reader
must parse, and runs every command over the result — 200 iterations × 27 commands
= 5,400 runs under the sanitizers. `check` is one of them and reaches every
structure in a single run; `manifest` is another and is the only command that
reads *every block of every file*; the shell is driven with a script.

**It has found one bug so far**, which is what it is for: `manifest` called
`qsort(NULL, 0, …)` on a pack whose `MDNUDS` was zero, because then no MFD slot
resolves and nothing is walked. Passing NULL is undefined behaviour regardless of
the count. Nothing hand-written had reached it — a pack with no directories at
all is not a case anybody thinks to build — and on an ordinary build UBSan only
*prints*, so the regression test for it passes unless `halt_on_error` is set.
Confirmed against the unfixed code under `make test-san`, where it aborts.

### And then it was pointed at the writer

For most of this project the fuzzer only ever *read* damaged packs, which left
the more dangerous half uncovered. A reader that misparses a corrupt descriptor
prints nonsense; a writer that does it overwrites somebody's file. So `put`,
`del` and `mkdir` now run over the damaged pack too — six commands, each on its
own fresh copy, because the first `del` to succeed would otherwise change what
every command after it saw and no failure could be traced back to one word.

Two things had to be added before the pass meant anything.

**A control.** Every write command must first succeed on an *undamaged* pack. A
misspelled path or a changed argument order would make it refuse every damaged
pack as well, and the whole pass would report zero failures while testing the
argument parser and nothing else.

**A completion tally.** The run prints how many of the attempts actually ran to
completion — currently about 85% — because "refused the pack" is a perfectly
correct outcome that also happens to be the outcome in which no writing code
runs. Green is only worth something if that number is not near zero.

And a writer can stay inside its own memory and still leave a pack that crashes
the *reader* — a descriptor whose length disagrees with its contents, a name
pointer past the end of the block. So each write is followed by `check` over the
result, which may report anything it likes and must not crash. That is what turns
"the writer did not crash" into "the writer did not produce something that
crashes".

The descriptor target was widened at the same time, from the first twenty words
of the area to all of it. That was the blind spot the code review had already
found by hand: the fuzzer had never reached the end-of-block case because it only
ever damaged bytes near the *start*, where the offsets involved are small.

### And at the containers, which are what you download

A pack is something you own. A `.tap` is something somebody **sent** you, which
makes it the likelier hostile input of the two, and for nine phases nothing
damaged one. Both readers over it parse a length out of the file and then read
that many bytes: `tape` the SIMH record framing, `saveset` the DUMP headers
inside it.

The fixture is a save set this project writes itself with `save` — three files
and a link, about 10 KB. For a question about the *format* that would be
circular. For a question about memory safety it is not: what is being asked is
whether the reader stays inside its own buffers given bytes it did not write, and
the damage is precisely what makes them bytes it did not write.

Random bytes reach a length field about one time in three hundred, and the length
field is the entire point — so half the iterations enumerate the record framing
and hit one on purpose, with a value chosen to be awkward rather than random:
longer than the file, `0x7FFFFFFF`, `0xFFFFFFFF`, off by one. A quarter of the
iterations truncate the file instead, which is the commonest real damage there
is.

That pass confirmed a property worth stating outright, since "refused rather than
allocated" is easy to claim and easy to get wrong: **a record length is checked
before it is believed.** A `.tap` whose first record announces 4 GB is refused
with `a record longer than a megabyte`, and peak RSS never leaves 3.5 MB. The
suite now measures that rather than asserting it — the same file is read again
under `ulimit -v 65536`, where refusing still works and reserving the space
could not. (Skipped, and *reported* as skipped, under the sanitizers, which
reserve a shadow mapping too large to run under any useful limit. A check that
quietly weakens itself to keep passing is worse than one that says it did not
run.)

One difference between the two layers is deliberate and worth recording, because
it looks like an inconsistency: a leading `0xFFFFFFFF` makes `tape` exit 0 and
`saveset` exit 1. End of medium at position zero is a valid, empty tape — and an
empty *save set* is an error. The container layer and the archive layer are
allowed to disagree about that, and the fact that they do is the separation
working.

200 iterations × 27 commands = 5,400 runs under the sanitizers, clean.

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

**The test fixture was never a legal ITS directory, for three phases.** Its name
area was in reverse SIXBIT order, which no ITS pack is — 6,056 entries on the
reference pack are sorted and `QRELOC` in `disk.1228` is what keeps them that
way. Nothing noticed until `put` was written, because every reader walks the area
linearly and does not care. What noticed was `put`'s own test: a writer looking
for an existing name stopped early on an unsorted area and created a **duplicate
entry** — precisely the bug s5fs shipped, on an image its checker called clean.

Two things came out of it. The fixture is sorted now, and `name_slot` no longer
stops early: the duplicate search scans the whole area while the insertion point
stops at the first greater entry. On a sorted area those are the same scan; on a
damaged one, only the full scan is safe.

**The reader could not read an empty directory.** `UDNAMP == wpb` means the name
area is empty — it starts past the end of the block — and it is exactly what ITS
writes when it creates one (`QSKON`: `MOVEI A,2000 / MOVEM A,UDNAMP-1(B)`). Both
the reader and the checker refused it as "outside the block" for four phases, and
nothing noticed, because **every directory on the reference pack has at least one
file in it** — the lowest `UDNAMP` there is 1019, which is one entry. Writing
`mkdir` is what found it: the first directory this project created was one its
own reader would not open.

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

## What a review found, after the phases were done

Reading `write.c` back with the whole thing finished turned up two things worth
recording, and neither was a live bug — which is itself the useful part of
saying so.

**A comment that lied about the order of operations.** It claimed the
directory-full check happened *before* the blocks were allocated, "so a full
directory does not leave the allocation table marked for a file that was never
made". The code allocates first. It has to: how many descriptor bytes a file
needs depends on how fragmented its blocks turn out to be, so the question
cannot be asked until they are chosen. The invariant does hold — every path out
calls `itsw_free` and the table ends up byte-identical — but a comment that
describes a safety property in terms of an order the code does not follow is
worse than none, because the next person will rely on it.

**A write whose bounds were checked in a different file.** `del` zeroes a
descriptor in place, and the two bytes after a load address were written without
a bounds check of their own. They were safe: `its_desc_blocks` in `structure.c`
walks the same bytes first with a checked reader and refuses anything that runs
off the block, so `del` never reaches them with a bad offset. Verified by trying
— a descriptor pointer put in the last word of a directory is refused with
`truncated load address`, and the file is left alone.

But that is the shape an out-of-bounds write has *before* somebody changes one
of the two functions. `desc_put` bounds itself now, which costs one comparison
and turns "unreachable" into "impossible". The corruption fuzzer had never found
it because it damages descriptor bytes at the *start* of the area, where the
offsets involved are small.

## The tool you verify with can lie by omission

This one is not about ITS, and it is the most portable lesson here.

Every negative claim in `src/its.h` — every `no DEFSYM`, every "FSDEFS does not
define this" — was established by grepping the ITS source tree. While chasing
something else, a `grep` for `DBD9` in KLH10's `vdisk.h` came back with **no
matches**, and `sed -n '68p'` on the same file printed the line containing it.

The cause: `grep` on this machine is **ugrep**, which some systems install under
that name, and it classifies a file containing any byte that is not valid UTF-8
as *binary* — then reports no matches, exits 1, and says nothing. `vdisk.h` has
a Latin-1 copyright sign on line 5. That is the whole trigger.

It is not a rare edge. Of a 400-file sample of the ITS source tree here, **54
are skipped that way** — 13.5%, and skewed exactly toward the oldest files,
which are the ones worth reading.

The failure mode that matters is not a wrong answer. It is a *confident empty
one*: "there is no DEFSYM for this" and "grep could not read the file" are the
same output. A verification method built on grepping old source has to know
this, or it will keep confirming absences that were never checked.

**What was done about it.** Every negative claim in `its.h` was re-checked with
`grep -a`. All of them held: `LTYPE` is at `kshack/nsalv.261:2152` exactly as
cited, comments and all, and no `DEFSYM` for a link separator or quote exists
anywhere in the tree. `tests/version-diff.sh` — the only script here that greps
ITS sources — now passes `-a` on all six of its greps.

Its zero-symbol guard would have caught a silent skip anyway, and that is worth
noting because it is the reason nothing was actually wrong: two empty extractions
compare *equal*, so without that guard the check would have printed `IDENTICAL:
all 0 definitions` and passed. A comparison that cannot tell "the same" from
"nothing at all" is not a comparison. The guard was already there; the `-a` makes
the check work rather than merely fail honestly.

`tests/crosscheck.sh` was never exposed — it compares with `cmp` and `tr`, and
already forces `LC_ALL=C`.

## What is not established

Stated plainly, because a validation document that only lists successes is
marketing:

*This list was written when there was no writer, and most of it has since been
struck through by work rather than by editing. What is left is what is still
true.*

- **`NSALV` was asked about two kinds of damage.** A cleared TUT word at two
  sites, and a miscount found by accident after an abrupt halt. It has not been
  shown a broken descriptor, a damaged directory header or a corrupt MFD — all of
  which `itsfs check` reports and none of which has been put to ITS for a second
  opinion.
- **A pack `mkfs` builds does not boot.** Nothing writes a boot area; the graders
  are booted from tape instead. ITS comes up on packs this project has *written
  files to*, which is a weaker statement than it first sounds.
- **One pack, one drive, one era.** Everything above is an RP06 built from source
  in 2026. No RP07, no RM03, no multi-pack file system, and no artifact recovered
  from MIT.
- **No tape written here has been compared with one ITS wrote.** `itstar` reads
  ours and ours reads `itstar`'s, but ITS's own DUMP has never been shown one.
- **The version span has a floor and no ceiling.** Both `FSDEFS` versions
  compared are after the 1979 TUT change, and nothing here maps a file system
  written by a monitor older than that.

Struck off since, and where the evidence is:

| was | now |
|---|---|
| no writer, so no level-1 or level-2 evidence at all | `NSALV` accepts packs this writes, DSKDMP lists their files, and ITS boots and prints one — [phases 8–10](roadmap.md) |
| `NSALV` never run against anything this produced | `make nsalv` does exactly that |
| no ITS magtape read, so `core` is unconfirmed | three strings ITS prints, found in the tape it loads them from — `make tape-test` |
| `dbd9` unconfirmed, no KLH10 artifact read | byte-identical to KLH10's own `vdkfmt` — `make klh10` |
