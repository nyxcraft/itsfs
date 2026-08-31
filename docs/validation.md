# Validation

How correctness is established here, what has actually been established, and what
has been found by trying.

## The standard

Three levels of evidence, in the order they are worth anything:

1. **Byte-identical** — build something with `itsfs`, build the same thing with
   ITS, `cmp`. **Reached, once: a DUMP save set.** ITS's own DUMP writes a tape
   of one directory; `itsfs save` writes the same files; the two files are equal
   over all 2,667,188 bytes. `make itsdump`.
2. **Accepted by native tools** — hand what we produce to `NSALV`, ITS's own
   salvager, or to the monitor. **`NSALV` accepts what the writer produces, ITS
   boots on it, DSKDMP lists the file, and the monitor opens it and prints it**
   — under two unrelated emulators, in two packings.
3. **Self-consistent** — round trips, and cross-checks against numbers the file
   system maintains independently of the thing being checked.

That taxonomy came from the sibling projects and it turned out to be missing an
axis. Levels 1 and 2 both assume the thing being graded is something we *wrote*.
But a native tool can also be asked to grade something we only **read** — point
ITS's own salvager at a pack, ask it what is wrong, and compare its answer with
ours. That needs no writer, and it is not self-consistency either, because the
other opinion is MIT's.

It has its own section below. Most of what follows is level 3, and the
interesting question was how strong a level-3 result can be made. The answer
turned out to be: quite strong, if the numbers you check against were computed by
somebody else.

**And the one level-1 result is the argument for wanting them anyway.** Nothing
below caught the three things `cmp` caught in a single run — a header a word
short, an author field being zeroed, and a date written as zero where ITS writes
all ones. Every one of them had been read back correctly by two independent
readers for nine phases. A round trip cannot see a field that neither end uses,
and a second opinion cannot see one that both readers agree to ignore. Only the
original can.

## Everything, in one table

The sections below appear roughly in the order the evidence was *obtained*, not
by rank — which is why level 1 comes before level 3 and level 2 comes after both.
This is the index.

| what is established | how strong | the command |
|---|---|---|
| a save set we write is byte-identical to one ITS's own DUMP writes, files and links | **level 1** | `make itsdump` |
| ITS's own `LOAD` reads a tape we wrote and restores both a file and a link | level 2 | `make itsload` |
| `NSALV`, ITS's salvager, accepts a pack the writer produced | level 2 | `make nsalv` |
| ITS boots on a pack we wrote to, and its monitor prints a file we put there | level 2 | `make interop` |
| …again on an unrelated emulator, in a different packing | level 2 | `make interop-klh10` |
| a pack built from **nothing** is accepted by `NSALV` and read by `DSKDMP` | level 2 | `make mkfs-test` |
| `NSALV` names the same damaged blocks, and the same files, as our checker | second opinion | `make nsalv` |
| the word layer round-trips 39,641,600 words in three packings, byte for byte | level 3 | `make oracle` |
| the space on a real pack accounts three independent ways | level 3 | `make oracle` |
| a second implementation of the reader agrees over the whole pack | level 3 | `make oracle` |
| 137 extracted files are byte-identical to their host originals | level 3 | `make oracle` |
| `dbd9` matches a pack KLH10's own converter wrote | level 3 | `make klh10` |
| a real ITS tape decodes to the exact words its own program prints | level 3 | `make tape-test` |
| the system's own `tar` reads and extracts an archive `tar c` wrote | second opinion | `make test` |
| a link this project writes is byte-identical to one ITS wrote, in all three encodings | level 1 | by hand, see `link_encode` |
| `UNAUTH` resolves to a directory whose identity fits the file, on all six entries that carry one | level 3 | by hand, see `cmd_shell.c` |
| `UNTIM` is half-seconds since midnight — the monitor says so twice, and 6,019 entries fit the bound | level 3 | by hand, see `its.h` |
| a real pack's 6,303 entries round-trip through a tar archive, contents and block accounting identical | level 3 | by hand |
| a pack read through the kernel gives the bytes `get` gives | level 3 | `make mount-test` |
| 400 checks, a third of them on packs damaged on purpose | level 3 | `make test` |
| the same, under ASan and UBSan | level 3 | `make test-san` |
| 7,000 commands over damaged packs, tapes and tar archives | level 3 | `make fuzz` |
| every constant cited is in the `FSDEFS` it claims | level 3 | `make version-diff` |

What is *not* established has its own section at the end, and is worth reading
first if you are deciding whether to trust any of this.

## Level 1 — a tape ITS wrote, and a tape we wrote, byte for byte

```console
$ make itsdump
2. ITS writes the tape
  ok   ITS booted, ran :dump, and returned to its prompt
  ok   ...leaving 2667188 bytes of tape

3. what is on it
  ok   itsfs saveset reads a tape ITS wrote: 37 files
  ok   all 37 files on it are byte-identical to the same files off the pack

4. and ours, beside it
  ok   the same size, to the byte: 2667188
  ok   IDENTICAL: every byte of a tape ITS's own DUMP wrote
```

ITS boots under KLH10, `:dump` runs ITS's own DUMP program, and it writes a save
set of one directory to a tape the emulator has mounted. Then `itsfs save` writes
the same files from the same pack, and the two files are compared. There is
nothing to interpret in the answer.

Getting a tape out of ITS took more than the documentation admits.
`.INFO.;DUMP INFO` — read off the pack with `itsfs cat`, which is a pleasing way
to find out how to drive the program that made the pack — gives the `_` prompt
and `FILE =` and stops there. DUMP then asks **`TAPE NO=`**, because it keeps a
record per tape on disk in `.TAPE0` and `.TAPE1`. Miss that and the run sits at a
prompt until it is killed, with an empty tape and no error anywhere.

### What `cmp` found that nine phases of round trips had not

The first comparison was **not** byte-identical, and every difference was ours.

**The file header is eight words, not seven.** The eighth is the file's length in
words. This wrote seven, on the strength of `itstar`'s reader, which takes six or
seven and never objected. All 37 headers on the tape ITS wrote are eight, and in
every one the eighth word equals the count of data words that follow it —
`-READ- -THIS-` says `0164` and has 116 words, `BUILD DOC` says `016364` and has
7,412.

**`UNREF` is copied whole.** This masked off its low 18 bits, throwing away
`UNAUTH` — and `its.h` had already recorded, from `FSDEFS` and marked `[v]`, that
`UNAUTH` is *"all ones = none"*. Zeroing it does not say "no author"; it says
**author 0**. The control matters here: before changing anything, the pack's own
`UNREF` for `KSHACK;CMDS M80` was read and it is `176333777000` — exactly the
word ITS put in the header. So DUMP copies it rather than substituting something,
and the fix is to stop masking.

**An unknown date is all ones.** ITS wrote `777777777777`, SIXBIT `______`, from
a machine whose clock was unset — the same case this writer is in, since today's
date is not available to that layer. This wrote zero, SIXBIT six spaces. Both
render as `__/__/__` in both readers, so nothing depended on it; one of the two
is what ITS does.

With those three, the tapes are equal over all 2,667,188 bytes.

**A third opinion, found afterwards, agrees — and dates the change.** ITS's own
format documentation, `doc/sysdoc/dump.format`, describes the file header as
**six** words: the AOBJN pointer, directory, FN1, FN2, "disk pack number where
file was", "creation date of file". No reference date, no length. That is
`HBLK`…`HDATE` and nothing after, which is exactly the header as it stood before
the source's *"Next two added 7/14/89 by Alan"*. The document and the comment are
independent and agree about when the format grew — and between them they explain
why `itstar` accepts six or seven and why nothing here noticed for nine phases.

**Why none of this was visible before.** `itstar` reads what we write, we read
what `itstar` writes, and both round trips were byte-perfect the whole time. But
a round trip cannot see a field that neither end uses, and a second opinion
cannot see one that both readers agree to ignore. Two readers had been agreeing
about a header that was a word short. Only the original disagrees.

### A link's header, and the switch that produces one

The first tape held 37 files and **no links**, which left an obvious loose end:
what does ITS put in a link header? The pack has directories with links in them,
so this is answerable rather than academic — `KMP` has three files and one link,
`KMP;TS DUMPT -> SYS;TS NT`, and is small enough to dump in a few minutes.

The answer is that there is nothing to measure, because **ITS's DUMP does not
write links at all**:

| directory | on the pack | on the tape ITS wrote |
|---|---|---|
| `KSHACK` | 37 files, 3 links | 37 files, 0 links |
| `KMP` | 3 files, 1 link | 3 files, 0 links |

Every file, no links, twice. This is `DUMP E` on a single directory, which is
the only mode driven here; a full dump may well differ.

That looked like "DUMP does not write links", and it was written down that way.
It is wrong, and the source says so in one line:

```
DMPLNK:	0	;-1 => DUMP LINKS
```

It is a **switch**, default off, and DUMP's own help text lists it —
`LINKS    Dump links as well as files`. The pack's `.INFO.;DUMP INFO`, which is
where the recipe for driving DUMP came from, does not mention it: the program's
built-in help is ahead of the documentation beside it. Both earlier runs used
`DUMP E`, so the tapes had no links because none had been asked for.

`ITS_SWITCHES="E LINKS" ITS_DIR=KMP make itsdump` produces one, and the answer is
that a link's header is the **same eight words as a file's**, with three of them
different — every value named outright in `syseng/dump.449` rather than deduced
from the bytes:

| word | a file | a link | source |
|---|---|---|---|
| `HPKN` | pack number | the target's directory, SIXBIT, whole | `MOVE A,PACKN / MOVEM A,HPKN` |
| `HDATE` | creation date | `400000,,0` | `;CREATION DATE OF LINK IS 400000,,0` |
| `HRDATE` | `UNREF` | `777777,,777000` | `; Unknown author, 36. bit bytes` |
| `HLEN` | length in words | `3` | `; Length of link is always 3` |

The three words that follow are the target as FN1, FN2, SNAME. With those, a
tape this project writes is byte-identical to one ITS wrote, link and all.

**And one of the four was a bug entirely of my own**, which `cmp` caught and
nothing else could have. The target directory was written as `twd << 18`, on the
assumption that it needed shifting into the left half. A SIXBIT name is *already*
left-justified — `SYS` is `637163,,0` — so the shift pushed it off the end of the
word and the mask in `sw_word` ate it, leaving that field zero. The tenth word of
the same record carried the same name, correctly, which is what pointed at the
shift. One word wrong on a 5,016-byte tape, invisible to every reader that has
ever read it, and found only by comparing against the original.

### So ask ITS the other way round

If ITS will not write a link, it will still *read* one. `make itsload` writes a
tape here, mounts it, and runs DUMP's own `LOAD`:

```console
2. ITS loads it
  ok   ITS booted, ran DUMP's LOAD, and returned to its prompt
  ok   ...having read our volume header: TAPE NO      1 CREATION DATE  ______

3. what came back
  ok   the FILE is back: KMP;GOTO 12
  ok   THE LINK IS BACK, AND STILL A LINK: TS DUMPT -> SYS;TS NT
  ok   ...and the file's words are what they were before the round trip
  ok   no block is both free and claimed after ITS wrote to the pack
```

**The control is on the tape.** It carries two entries — an ordinary file and a
link — and both are deleted from the pack before ITS is started, so anything
that comes back came off our tape. Three outcomes were distinguishable in
advance: neither back means `LOAD` never ran and the test says nothing about
links; only the file back means ITS reads our file headers and rejects our link;
both back means ITS accepts both. Without the file beside it, a failure could
not have been told from a broken harness — which is the mistake made earlier
with `MFDCLB` and written up above.

Both came back. `KMP` is restored to the four entries and 953 words it started
with, `GOTO 12` is byte-identical to what was read off the pack before the round
trip, and the link points where it pointed.

One thing fell out of it that was not being tested. ITS echoed the volume header
as `CREATION DATE ______` — reading back, as its own "unknown", the all-ones date
word this writer was changed to produce an hour earlier on the strength of what
ITS *wrote*. Written and read, by ITS, both ways.

So the position on links is now: ITS's DUMP does not write them, ITS's LOAD
accepts ours, and what remains unmeasured is only which ITS program writes the
link entries `itstar` was built to read.

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

### A second kind of damage

Everything above is one damage class. A cleared TUT word makes the table
*under*-claim: blocks a file holds are marked free, and the allocator will hand
them out again. That is the dangerous direction, which is why it came first —
but it is one direction, and `NSALV` had never been shown any other.

Zeroing a directory block is the opposite. The table still claims the blocks its
files held; nothing claims them back. `itsfs check` separates the two by name —
**free but claimed** against **in use but unclaimed** — and that distinction had
no second opinion until `make nsalv` grew a fifth stage.

```console
5. a DIFFERENT kind of damage: a directory, not the table
   zeroed the directory in block 384
  ok   itsfs check: block 384 was reached as |KMP| but its UDNAME is ||
  ok   NSALV: UFD on unit 0 block 384 is KMP, but expected .
  ok   IDENTICAL: the same block, and the same directory, from both
  ok   ...and NSALV calls it fatal, where check calls it five problems
```

`KMP` is chosen because it is small — three files, one link, three blocks — so
the expected result is *enumerable* rather than approximate. "They agree" here
means the same block number and the same directory name, not merely that both
complained.

**And they disagree about what it means, which is worth recording rather than
smoothing over.** Told not to repair it, `NSALV` declares `*** ERROR *** THE
SYSTEM MAY NOT BE BROUGHT UP` and stops. `itsfs check` reports its five problems
and goes on to give the whole account, including the three orphaned blocks —
37507, 37559, 37604 — that `NSALV` never reaches, because it has already decided
the pack will not boot. That is not a conflict. One is deciding whether to bring
a machine up; the other is diagnosing. A test that recorded only the agreement
would have hidden something true about both.

### And the MFD, which everything else depends on

A damaged directory is one directory. A damaged MFD is *every* directory,
because the MFD is how they are found at all — so both programs stop rather than
guess, and what each says when it stops is the comparison.

```console
6. and the MFD itself, which everything else depends on
   cleared MDCHK, word 5 of the MFD in block 19081
  ok   itsfs check: block 19081 is not an MFD: MDCHK is |      | (000000000000), not |M.F.D.|
  ok   ...and stops there rather than reporting counts it cannot stand behind
  ok   NSALV: 'MFD check word garbaged?' -- the same word, named the same way
```

The second line is the one worth defending. `check` reports its counts only when
the structure they are derived from verified; a tool that answered "5,657 files"
from a pack whose index did not check would be confidently wrong, which is the
failure this document keeps returning to.

`nsalv.sh` had `garbaged` in its list of messages to watch for from the day it
was written — taken from reading NSALV's source, and never actually seen until
this stage existed.

**It also closes a loop from the start of the day.** `MFDCLB` — "M.F.D.
clobbered" — is what KLH10's DSKDMP said at a `dbd9` pack, and it is recorded
further up as a false alarm with an invented cause. The real reason was a device
process that never started: no disk, so the loader read zeros where the MFD
should be and reported the MFD clobbered. Which, from where it stood, it was.
Here the MFD is genuinely clobbered, and both programs say so by name.

### And the file side: two files holding one block

Three directions were covered by then — the table under-claiming, the table
over-claiming, and the index itself gone. The fourth is the file side: a
descriptor that points where it should not.

The damage is a single field. `KMP;GOTO 12`'s `UNDSCP` is set to
`KMP;BABYL 19`'s, so both files read the same descriptor: BABYL's block is
claimed twice and GOTO's own is claimed by nobody.

```console
7. a broken descriptor: two files holding one block
   KMP;GOTO 12 now shares KMP;BABYL 19's descriptor
  ok   itsfs check: block 37507 is claimed by KMP;GOTO 12 and already by KMP;BABYL 19
  ok   NSALV: 'Tracking down shared blocks.'
  ok   ...naming both files, the same two itsfs check names
```

NSALV shows it a different way: it prints both files' descriptors, one under each
name, and they are **identical** — `51 12 03 (JUMP 111203)` under both. The
shared block made visible.

**Two things about getting there are worth more than the result.**

The first attempt wrote to the entry's first word instead of its third — an
entry is FN1, FN2, RNDM, DATE, REF — and so renamed a file rather than damaging
it. `check` reported **no problems**, correctly, because a renamed file is not
damage. Stopping there would have produced a finding that `check` misses shared
blocks: a fabricated defect in working code, from one wrong offset. The stage now
verifies the word reads `467700000045` before writing and refuses otherwise.

And `Tracking down shared blocks` had been in this harness's watch-list since the
day it was written, taken from NSALV's source and never once observed. It turns
out to be exactly right — it simply comes *after* `UFD needs update - Write`, a
prompt nothing here answered, so every run stopped before reaching it. The
anticipated message was correct; the thing in front of it was missing. It was
briefly written up here as "still unseen", which was wrong and is corrected.

### A check ITS's own salvager does not make

The four damage classes above all end in agreement. This one began as a question
and ended in a **missing check**.

The name area is sorted — measured early, 6,056 entries on the reference pack
with none out of place. But "always observed sorted" and "required to be sorted"
are different claims, and only the second makes an unsorted pack *damaged*. So
two entries were swapped and the pack handed to `NSALV`.

**It said nothing.** Walked the pack, returned to DDT, no complaint. And
`itsfs check` said nothing either — which looked like a match, and was the wrong
conclusion to draw from it.

`QLOOK` in `disk.1228` does not scan the name area. It calls `QLGLK`, which is a
binary search:

```
	ADDI J,600	;128. NAME BLOCKS FROM END
REPEAT 7,[		;THIS CODE DELIBERATELY NOT INDENTED.
	...
	CAML A,D
	ADDI J,<1_<7-.RPCNT>>*LUNBLK
	SUBI J,<1_<6-.RPCNT>>*LUNBLK
]
```

Seven halving steps over 128 name blocks. **The order is load-bearing.** Two
entries in the wrong places make a file unfindable by the monitor while every
block on the pack is still accounted for — and the salvager will not tell you,
because it is checking the allocation, not the ordering.

So this is the one kind of damage where the second opinion is silent and the pack
is broken anyway, which makes it exactly the kind worth checking here. `check`
now reports it:

```
KMP;BABYL 19 follows |GOTO 12| in the name area, out of order
  -- QLOOK binary-searches it, so this file is unfindable
```

The reference pack is still clean under the new check, which is the earlier
measurement made again by a different route: all 247 directories, all 6,056
entries, in order.

**The check is deliberately stricter than the monitor requires.** `QLGLK`
searches on FN1 alone; `QLOOK` then walks *backwards* through the run of equal
FN1s comparing both names — `SUBI Q,LUNBLK / CAML Q,J / JRST QLK1`, commented
"SEARCH THROUGH * FILES". So two entries sharing an FN1 could be in any FN2
order and still be found. They never are: on the reference pack **1,913 adjacent
pairs share an FN1 and not one has its FN2 out of order**, because `QRELOC` sorts
on the whole name. Checking what ITS *writes* rather than the minimum ITS can
*read* is the more useful of the two for catching a writer that has gone wrong —
but a pack failing only on FN2 order would still work, and the message deserves
to be read in that light.

**And it closes a loop on the writer.** That `put` inserts in the right place has
been checked since phase 8 by watching `DSKDMP` list a directory under an
emulator — a strong test that costs ten minutes and a PDP-10. Now that `check`
knows the invariant, the same property is testable in the ordinary suite: write
`ZULU`, `MIKE`, `ALPHA`, `TANGO`, `BRAVO` in that order, read back
`ALPHA BRAVO MIKE TANGO ZULU`, and have `check` agree. Appending, prepending, or
stopping the duplicate search early would each show up in that sequence.

The emulator test still earns its place — it is ITS's own code doing the reading
— but the invariant no longer goes unchecked between one run of it and the next.

**And the check was put to ITS's own writing before being trusted.** Every pack
it had been validated against was written by ITS *before* today and read cold;
none exercised an insertion made by a running monitor into a directory already in
use. `make interop-klh10` does exactly that — ITS creates `.BATCH;BATCHN LOG` and
`.BATCH;BATCHN NEXTUP` while it is up, `QRELOC` places them, and stage 4 then
runs `check` over the result. Eleven checks, all passing, the new one included.

So the invariant holds for the writer that defines it, not only for artifacts
that happen to satisfy it. That distinction is the same one that cost a
retraction two hours earlier: a claim verified only against things that already
existed is weaker than one tested against the process that makes them.

**The general point is worth more than the check.** Agreement with a second
implementation is the strongest evidence this project has, and it is still
bounded by what that implementation looks at. `NSALV` and `itsfs check` agreed
here — both silent — and both were wrong. Only the source of the program that
*uses* the structure settled it.

### And the same question one level up, where I got it wrong

If the monitor binary-searches a directory's name area, what does it do with the
MFD's? `QFL` in `disk.1228`:

```
QFL1:	LDB J,[1200,,Q]
	JUMPE J,QFL3	;give up
	CAMN C,MNUNAM(Q)
	 JRST QFL2	;found
	ADDI Q,LMNBLK
	JRST QFL1
```

A **linear scan**, not a search — so the MFD's name area need not be sorted, and
it is not: `itsfs dirs` lists `.TAPE0`, `BACKUP`, `(INIT)`, `SHARE2` in that
order. That much is right, and the asymmetry is real.

Then I read `JUMPE J,QFL3` as "stop at the first zero slot", concluded that a gap
in the MFD hides every directory beyond it, added a check for it, wrote a test,
and committed the lot. **It is wrong.**

`Q=10` in `its.1652`. **Q is an accumulator**, so `[1200,,Q]` addresses the
*register*, and the `LDB` takes the low ten bits of the entry's **address**, not
of the entry. That is exactly why `QFL2` can then use `J` as the address — it is
the same "position is the address" this project relies on everywhere else. The
loop ends when the offset walks off the block; an empty slot simply fails to
match.

**The measurement said so before the source did, and I nearly overrode it.** 105
of the reference pack's 247 directory names have zero in their low ten bits —
names shorter than six characters, NUL-padded — and the first is the *eleventh*
entry. A scan that stopped there would leave ITS with eleven directories on a
pack that demonstrably has 247. The contradiction was on the screen before the
check was written; what saved it was going back to the source rather than
explaining the measurement away.

The check and its two tests are gone. What remains is this note, and the
`Q=10` that makes the difference, so the next reader of `QFL1` does not spend
the afternoon the same way.

**And the pattern across the day is worth having**, because this was not the
only claim taken from reading MIDAS. Every other one turned out to be anchored to
something that could contradict it:

| claim, from reading assembly | what could have contradicted it |
|---|---|
| `DUMP` writes links under a `LINKS` switch | ran it — a link appeared on the tape |
| a link header is 8 words, `HDATE` `400000,,0`, `HLEN` 3 | `cmp` against a tape ITS wrote |
| `NSALV`'s drive is fixed at assembly | the same tape accepted an RP06 and refused an RM03 |
| `FESET` writes `HOM` at word 0, address at `0103` | the bytes on the reference pack |
| `UNAUTH` all-ones means "none" | every file on the pack has `777` there |
| `QLGLK` binary-searches the name area | 6,056 entries, none out of order — and the search would fail if they were |
| **`QFL1` stops at an empty slot** | **nothing — and it was wrong** |

The one claim with no measurement behind it is the one that failed. That is not
a coincidence and it is not a rule about assembly: it is that a reading which
*cannot* be contradicted by an artifact will not be, however carefully it is
done. Five times today a plausible mechanism turned out to be invented, and
every time what settled it was making the system produce something.

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

**400 checks** in `tests/run.sh`, of which about a third feed the reader or the
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

## What a maximally damaged pack prints

`cmd_check.c` caps how much it will print and never caps what it counts:

```c
#define CK_MAXPRINT 100
```

The distinction is the point. A checker that answers a wrecked pack with a
hundred thousand lines is one nobody reads twice; one that also under-reports
the total is lying about the extent of the damage.

Clearing `QLASTB` is the cheapest way to make everything wrong at once — the TUT
then maps the range `0..0`, so every block on the pack falls outside it. On the
reference pack:

```
30942 problems, 7 notes
```

in **110 lines of output**. Every problem counted, a hundred printed.

The suite checks the same property at fixture scale, and cannot check all of it:
two directories and four file blocks give six problems, nowhere near the cap. So
it verifies the output stays short and the count is exact — six, one per claimed
block — and the cap itself is the measurement above. A test that claimed to
exercise `CK_MAXPRINT` on a six-problem pack would pass without testing anything.

## A path traversal, found by reviewing rather than by testing

The one security defect in this tree, and it was not found by the fuzzer, the
sanitizers, or any of the emulators. It was found by asking where data from
outside the program becomes a filename.

**SIXBIT decodes to ASCII 040..0137, and that range contains `/` and `.`.**
`saveset -x` built a host path out of three names taken off the tape:

```c
snprintf(path, sizeof path, "%s/%s;%s", extract, dir, name);
```

A save set whose directory is `../..` therefore escapes:

```console
$ itsfs saveset -x out/sub/deep evil.tap
../..;OWNED TXT -> out/sub/deep/../..;OWNED.TXT
$ find out -type f
out/sub/..;OWNED.TXT          <- one level above where -x pointed
```

Demonstrated, not deduced — the tape was hand-built from the format documented
above, and `tests/run.sh` now builds the same one and checks the refusal.

The reach is bounded: six characters a component, so a few levels at most. But
bounded escape is still escape, and **a tape is the one artifact in this project
that arrives from somewhere else** — a real one was downloaded from
`pdp-10.trailing-edge.com` earlier the same day to test the reader against.

**Refused by name rather than sanitised.** Rewriting `/` to something else would
silently produce a file whose name is not the one on the tape, and the point of
`-x` is to get out what is in there. Listing is untouched: refusing to *write* a
name is not refusing to *read* it, and `saveset` without `-x` still shows the
entry.

`tape -x` was checked at the same time and is safe — it names its output
`fileN.words` from a counter, never from the container.

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

### And one more, on the code the day changed most

`cmd_saveset.c` was read back last, because it is what today rewrote: an
eight-word header where it wrote seven, `UNREF` copied whole, a link header
built from four constants, and a computation hoisted above the header to make
any of that possible.

**One finding, and it is the same shape as the two above.** The link target is
taken apart with `strchr` for the `;` and the space, and the three name buffers
are zeroed first. If either separator were missing, or a component too long, the
old code simply skipped the copy — and `its_sixbit_make` succeeds on an empty
string. So a target that failed to parse would have been written as an **empty
target, with no complaint**.

It could not happen: `its_link_target` formats `"%s;%s %s"` from three components
of at most six characters, so the separators are always present and the lengths
always fit. But that guarantee lives in `structure.c`, and a silently-zeroed
field is precisely the bug that cost an afternoon today — a SIXBIT name shifted
off the end of its own word, which no reader noticed and only `cmp` caught. It
refuses now, by name.

Verified after the change: the tape is still byte-identical to the one ITS's own
DUMP wrote, all 5,016 bytes of it, link included.

### And then the same question asked of every file at once

Three reviews had found three instances of one shape — a write whose bound was
established in another file — so the fourth pass was a search rather than a
reading: every `memcpy`, `strcpy`, `strcat` and `sprintf` in the tree, and where
each one's bound comes from.

Fourteen sites. **Thirteen carry their own bound**, most of them immediately
above the copy:

| site | bound |
|---|---|
| `util.c` ×2 | `n >= sizeof lh` / `n >= sizeof head`, with an error |
| `image.c` ×2 | `take` clamped to `n`; `stage[8192]` against at most 1,820 words for any packing |
| `cmd_fs.c`, `cmd_write.c` ×4 | `n >= ITS_NAME_MAX`, with an error |
| `cmd_manifest.c` ×2 | `malloc(strlen + 1)`; `n >= sizeof out->path` |
| `cmd_check.c` | `malloc(strlen + 1)` |
| `cmd_saveset.c` argv ×2 | `goto toolong` |
| `write.c` | `n` clamped to `wpb`, `left` decremented so the source stays in range |

The fourteenth was the link target above, and it is fixed. `cmd_fs.c` is worth
singling out as the pattern the rest should match: it checks, it errors by name,
and the check is on the line before the copy.

There is no `strcpy` into a fixed buffer anywhere, no `strcat` at all, and no
`sprintf` — the two `strcpy`s that exist follow a `malloc` of the measured
length. That is not an accident of style; it is the property that makes a
fourteen-site audit take twenty minutes instead of a day.

### One more class, chosen because no tool here covers it

The sanitizers catch out-of-bounds and leaks on paths the tests reach,
`-Wconversion` catches truncation, and the fuzzer covers damaged input. **An
ignored I/O return value is invisible to all three**, so that was the next
search: every `fread`, `fwrite`, `fseeko`, `ftruncate`, `fflush` and `fclose` in
the tree — 53 call sites.

Two were unchecked, and one of them mattered. `verify` writes a manifest to a
temporary file and reads it straight back to compare against. It did

```c
fflush(tmp);
fclose(tmp);
```

with neither result examined. A write that failed part way — a full disk is the
ordinary way — would not produce an error there. It would produce a **shorter
manifest**, and every file missing from the truncated end would be reported as a
difference. `verify` would answer the question wrongly rather than decline to
answer it.

`fclose` is where that surfaces, because a deferred write error is reported when
the stream is flushed and not when `fprintf` returned; the `fflush` was doing the
same job twice and checking neither. It errors by name now. (The other unchecked
call is `fflush(stdout)` before reading a line in the shell, where a failure
means the prompt did not appear on a stream that is going nowhere anyway.)

### Two clean ones, which are also results

`structure.c` and `cmd_check.c` got the same reading afterwards — the reader
every command depends on, and the independent checker. **Nothing was found in
either**, and that is worth a paragraph rather than none, so the next person
knows they have been looked at and where.

What was checked: the descriptor decoder's opcode ranges against `FSDEFS`
(0 ends, 1–12 take, 13–30 skip, 31 write-placeholder, 32+ load address) and the
branch order that distinguishes them — get that wrong and a load address is
silently read as a "take"; the `run` count each branch contributes; the hard step
cap that stops a corrupt descriptor looping; and `its_link_target`'s
`comp[ncomp][n] = '\0'`, which writes at index `ITS_SIXBIT_CHARS` into a buffer
of `ITS_NAME_MAX` — 6 into 7, safe, but only because those two constants are
defined in terms of each other in different headers. Its `goto done` path leaves
a component unterminated and relies on the opening `memset` for that, which
holds.

In `cmd_check.c` the risk is different, because it allocates from what it reads.
Three sites: the growable name list (doubles, and survives either allocation
failing without corrupting its state), the MFD slot table, and the two per-block
arrays. The slot count is `(wpb - namp) / LMNBLK`, an unsigned subtraction that
would underflow into an enormous `calloc` if `namp` exceeded `wpb` — and `namp`
is range-checked against `wpb` immediately above it, with a `return -1`. The
per-block arrays are sized from the **drive table**, not from the disk:
`ntutbl`, `ncyls`, `nblksc`. So nothing an image controls can inflate an
allocation, which is the property that matters for a program whose whole job is
to be pointed at damaged packs.

## A second machine, and a second packing

Everything in `make interop` runs under SIMH. When ITS boots there and prints a
file this project wrote, the chain is: our writer, our packing, SIMH's disk
emulation, ITS. A fault in SIMH's RP06 that happened to line up with a fault in
our geometry would look exactly like success, and nothing in that run could tell
the difference.

`make interop-klh10` asks the same questions of KLH10 — an unrelated
implementation of the same hardware, by a different author — and it reads a
**different packing**, because KLH10's ITS config is `format=dbd9`, two words in
nine bytes, where SIMH's is one word per eight. The two runs share only the two
things actually being tested: the file system on the pack, and ITS.

```console
1. the pack, converted and written to
  ok   repacked into dbd9, which is what KLH10's ITS config asks for
  ok   removed SYS;ATSIGN DRAGON so the console can be had
  ok   wrote KSHACK;K10TST TXT onto it
  ok   ...and our own checker still finds no problems

2. DSKDMP, ITS's standalone loader, reads the pack
  ok   DSKDMP listed our file, off a dbd9 pack, with its own code
  ok   ...and the listing is in order (40 entries)

3. ITS boots on it, and prints the file
  ok   ITS reached IN OPERATION -- its salvage pass walked every directory
  ok   THE MONITOR PRINTED THE FILE, on a second emulator and a second packing
```

and on the console:

```
:print KSHACK;K10TST TXT

ITSFS WROTE THIS ON A DBD9 PACK AND KLH10 PRINTED IT
```

This also puts `dbd9` where it belongs. `make klh10` confirms the codec against
KLH10's own converter, which is a statement about bytes. This is the same packing
carrying a live operating system.

### Two things that cost time, both worth writing down

**KLH10 drives its devices through separate processes.** `dprpxx` for the disk,
`dpchaos` for the network, `dptm03` for the tape — exec'd by name at run time.
If they are not in the working directory it prints

```
[dp_exec: Cannot access "dprpxx" - No such file or directory]
[rp_dpstart: Start of DP "dprpxx" failed!]
Final init of device "dsk0" failed!
```

in the middle of a screenful of startup, carries on, and DSKDMP then says
`MFDCLB` — "M.F.D. clobbered". Which reads as a corrupt pack and is nothing of
the kind: it is *no pack at all*. That had already cost one wrong conclusion in
the source, so the test checks for the helpers by name before it runs anything.

**There are two escape characters, and they are not the same one.** ESC belongs
to the PDP-10 software — DSKDMP's `U<ESC>dir;`, DDT's `<ESC>G`. KLH10's own
command escape is `^\` (`doc/usage.txt`: "the command escape character (^\,
CONTROL-\)"). SIMH uses ESC for both jobs, which is why the SIMH script gets away
with one. Sending ESC where `^\` was meant does not produce an error: DSKDMP
takes it, answers `FNF`, and the `quit` typed after it goes in as a file name —
so the run hangs at a timeout with a perfectly correct listing on the screen.

### A wrong assertion, found by running the test

The KLH10 run gained a fourth stage — does the pack survive ITS *having* it? —
and the obvious form of that was **"it still checks clean"**. That test failed
the first time it ran, which is the whole argument for running a test before
believing it:

```
disagreements  0 free but claimed, 0 in use but unclaimed, 70 miscounted, 0 on locked-out blocks
directories    247, 5660 files, 398 links
70 problems
```

Zero in both categories that mean data is at risk, seventy **miscounts** — and
the pack had grown from the 5,658 files we left it with to 5,660. Neither is
damage, and rather than assert that, it can be measured. Diffing a manifest of
the pack before and after names the two files exactly:

```
.BATCH;BATCHN LOG
.BATCH;BATCHN NEXTUP
```

— the batch daemon writing its log and its next-up file, and `.BATCH` growing
from 248 blocks to 249 to hold them. The arithmetic closes with nothing left
over: 5,657 on the original pack, **+1** for the file we wrote, **+2** for ITS's,
= 5,660; and 399 links down to 398 because `SYS;ATSIGN DRAGON` — which turns out
to point at `CHANNA;ATSIGN TARAKA` — is the one we removed to get the console.

The miscounts follow from the same fact: this harness stops the machine by
*halting* it, so the allocation table ITS held in memory is never written back.
What is left is precisely the category ITS's own salvager has a name for.
Demanding a clean check here is demanding that ITS not be ITS.

So the assertion is now the one that means something:

- **free but claimed** must be zero — otherwise the allocator will hand out a
  block a file is using.
- **on locked-out blocks** must be zero — otherwise a file is sitting on a
  directory or a table.
- **miscounted** may be anything, is reported rather than hidden, and `NSALV`
  fixes it in one pass.

Both dangerous categories are zero, and our file comes back byte for byte. The
weaker claim is the true one, and it is worth more than a green tick that was
asserting the wrong thing.

### What it does not prove

The same `SYS;ITS` binary, off the same pack, under both emulators. Two machines
agreeing is evidence about the *machines* and about our geometry; it is not two
independent implementations of ITS, because there is only one ITS.

## A second drive, and why ITS cannot grade one

`mkfs` builds an `rm03` file system as readily as an `rp06`, and the arithmetic
comes out right on both: 500 directory slots + 1 MFD + the table, which is **505
locked out** on an RP06 with its four table blocks and **503** on an RM03 with
two. The geometry differs in the way that matters, too — blocks per cylinder is
truncated, and it truncates differently: 19×20/8 = 47.5 → **47** on an RP06,
leaving 4 sectors per cylinder unreachable, against 5×30/8 = 18.75 → **18** and 6
unreachable on an RM03.

So `make mkfs-test ITS_DRIVE=rm03` should be a second, independent exercise of
the one layer with no check word and no redundancy. It gets as far as building
the pack and checking it, and then stops:

```
Salvager 254
Active unit numbers? 0Format ran out of arguments.
*** ERROR *** THE SYSTEM MAY NOT BE BROUGHT UP
```

**That says nothing about the pack.** NSALV's drive is chosen when it is
*assembled*, not when it is run — its source selects one inside a machine block:

```
 IFCE MCHN,PM,[
	...
	RM03P==1	;RM03 on RH11 UNIBUS controller.
```

There are eighteen such blocks in `kshack/nsalv.261` and eleven lines between
them setting `RP04P`/`RP06P`/`RM03P`/`RM80P`. A salvager tape grades the drive it
was built for and no other, and the tape here is the one the ITS build produces,
for an RP06 machine.

**The control had already run.** The same tape and the same harness accepted an
RP06 pack built the same way, minutes earlier; the only difference is the drive.
Without that, "the salvager rejected it" and "the salvager cannot read this drive
at all" would look identical — which is the error this document has had to
correct twice already.

So the stage is skipped for any drive but `rp06`, and skipped *loudly*: a grader
that cannot read the format it is handed is not a grader, and reporting its
refusal as a fault in the pack would be reporting a lie. Grading an RM03 needs an
RM03 salvager, which needs its own ITS build — a bigger undertaking than a test,
and one that would change the reference environment everything else here depends
on.

## A prose description of the directory, found afterwards

The on-disk transcription rests on `FSDEFS`, which is a file of assembler
symbols. Every offset in `its.h` cites one, and two readers were written from it
so a misreading could not agree with itself — but both still read the same
source. `doc/sysdoc/ufd.100`, in the PDP-10/its tree, describes the same
structure in English, and it was read only after the format had been implemented
and measured:

| `ufd.100` | `its.h` |
|---|---|
| "the first word of the file directory contains a pointer to the beginning of the descriptor area" | `ITS_UD_ESCP 0` |
| "the second word points to the beginning of the name area" | `ITS_UD_NAMP 1` |
| "the third word of the directory contains the sixbit user name" | `ITS_UD_NAME 2` |
| "5 words of information for every file" | `ITS_LUNBLK 5` |

**And the one that mattered most.** The descriptor pointer, it says, "is actually
a byte address relative to a point **11. words** from the beginning of the
directory." That is the single worst bug this project has had: reading `UNDSCP`
from word 0 instead of word 11 lands 66 bytes early, where a well-formed pack
holds zero, so every file decodes as empty and *nothing complains*. It was found
by the space not adding up, and fixed by reading `QFL2` in the monitor. Here it
is in one sentence of documentation, which would have saved an afternoon — and
which nobody would have thought to doubt if the code had happened to agree with a
wrong reading of it.

Links too: "Each name is terminated by a semicolon unless it is a full six
characters long. Colon quotes the character that follows it." That is the rule
`its.h` carries with a `[v]` and a note that `FSDEFS` defines no symbol for
either character — the project had to take them from `NSALV`'s `LTYPE` and from
what MIDAS assembles `';` as. The prose names the characters; the code gives the
encoding, which is SIXBIT `033` and `032` rather than ASCII. Consistent, and
neither alone would do.

**The order was lucky rather than careful, and it is worth being honest about
that.** These documents were found by searching the web after the work was
finished, not before. Read first, they would have been believed — and
`dump.format` in particular describes a save-set header two words shorter than
the one ITS writes today. It is corroboration precisely because it came second.

## What an `[s]` costs to check

The per-field markers in `its.h` are the finest-grained claims here: `[v]` means
a field has been exercised against a real pack, `[s]` that it is read in `FSDEFS`
and has not. Fifteen fields are still `[s]`, and the temptation on finding ITS's
own prose documentation was to promote some of them, because a second source
feels like stronger evidence.

It is not the same thing. `doc/sysdoc/ufd.100` says "Colon quotes the character
that follows it", which corroborates the *rule*; `[s]` is a claim about whether
any artifact has ever made the code run.

So that one was checked properly instead. All 296 distinct link targets on the
reference pack, and the character set across every one of them is

```
!%*-.0123456789;>?@ABCDEFGHIJKLMNOPQRSTUVWXY[]_
```

— no colon, and no space inside a component. **Nothing on this pack needs
quoting.** The marker stays, and now says why: not for want of looking, but
because a pack that exercises it has not been read. The decoder implements the
rule, `FSDEFS`'s own four examples drive it in the suite, and ITS's documentation
agrees with all of it — and none of that is an artifact.

### The rest of them, counted

Having done one properly, the others were worth the same scan of the reference
pack — 247 directories and 6,056 entries, which are the numbers `check`
independently reports:

| field | on the pack | so |
|---|---|---|
| `MPDOFF` | `771107,,352672` | present, but nothing here interprets a clock offset |
| `MPDWDK` | zero | not exercised |
| `QTRSRV` | zero | not exercised — it is `-1` only for "allocated dirs only" |
| `UDALLO` | zero in **0 of 247** directories | not exercised |
| `UNREAP` | set on **1** entry | exercised, once, out of 6,056 |
| `UNWRIT` | **0** | not exercised |
| `UNMARK` | **0** | not exercised |
| `UNCDEL` | **0** | not exercised |
| `UNBYTE` | **0 on all 5,657 files** | 36-bit bytes; the other encodings are unexercised, as the roadmap says |

**One of those matters more than the others.** `UNIGFL` — the bits that mean
"ignore this file" — is `UNWRIT | UNCDEL`, and both are zero everywhere on this
pack. So the reader's `deleted` flag is **never true** on the reference pack, and
the branch that skips such an entry has never been taken against a real artifact.
It is driven by the suite's own fixture, and that is all.

`UNWRIT` means "open for writing", so an abruptly halted machine looked like the
way to produce one — and these interop runs halt ITS with the console escape,
which is as abrupt as it gets. So a pack ITS had actually run on was scanned too:
6,058 entries, including the two the batch daemon created while it was up. Still
none. Whatever ITS does with that bit, it does not leave it set to be found.

That is not a defect; it is the sort of thing worth knowing before trusting a
count. Every "5,657 files" this project prints is a count of entries none of
which were ignorable.

That scan was written from scratch to answer the flag question — its own `dbd9`
decode, its own MFD-slot arithmetic, no `itsfs` code — and it agreed with `check`
to the entry. The agreement was worth more than the answer, so it stayed:
`tests/crosscount.py`, run by `make oracle`.

**Why a third reader is not redundant.** `structure.c` and `cmd_check.c` are
already independent of each other — the checker re-derives the geometry and the
MFD arithmetic rather than calling the reader. But *both take their constants
from `src/its.h`*. A second reading catches a wrong reading; it cannot catch a
wrong **transcription**, because both inherit it. So `crosscount.py` transcribes
`NHEDS`, `NSECS`, `SECBLK`, `MDNAMP`, `MDNUDS`, `LMNBLK`, `LUNBLK` and `UNLINK`
again, from the same ITS sources, with citations. If a number disagrees, one of
the two transcriptions is wrong and the disagreement says so.

It is also a different language, which is not nothing: the C readers share an
integer model, a byte order, and a set of habits about shifts and masks. Python
has arbitrary-precision integers and no unsigned types, so a 36-bit value that
overflowed or sign-extended in C would come out differently here.

```console
crosscount: 247 directories, 6056 entries (5657 files, 399 links)
crosscount: 30940 blocks claimed by files, 30940 in use per the table, 6719 free, 505 locked out
itsfs check: 6719 free, 30940 in use, 505 locked out / claimed 30940 blocks, in 5657 files
```

Two packs, two packings, exact — including **30,940 == 30,940**, which is the
strongest single claim this project makes and which had until now been checked
only by two implementations sharing a header.

**It earned its keep the first time it counted blocks, by disagreeing.** Three
constants had been transcribed wrong in it:

- `UNDSCP` is the **low 13 bits** of `UNRNDM`, not the top nine
- the map starts at `LTIBLK`, which is **octal** 20, not word 0
- a block indexes from `QFRSTB`, not from zero

Every one produced plausible output. The block count came out 6,826 — a number
with no obvious flaw in it — and the table still partitioned exactly, into
30,792 + 6,864 + 508 = 38,164. Nothing about either result looked wrong on its
own. They were caught by the totals not matching `check`, which is the entire
argument for a third reader: two implementations that share a header agree with
each other about a mistake in it.

(The search turned up three targets worth knowing about for a different reason:
`EMACS;[PRFY] >`, `EMACS;[PURE] >`, `EMACS;[RMAI] >`. Bracketed names, and `>`
as the second name — the version wildcard that `QLOOK` resolves, which is why
the dangling-link count on this pack is seven rather than ninety-five.)

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

### It happened three times, which makes it a pattern rather than an anecdote

Within one working session, three separate tools this project verifies *with*
silently discarded the thing they were asked about and then reported an answer
with no hint anything was missing:

| tool | what it silently dropped | what it reported |
|---|---|---|
| `grep` (ugrep) | any file with a non-UTF-8 byte — 13.5% of the ITS tree | "no matches", exit 1 |
| `check_links.py` | every `#fragment` — it stripped them before resolving | "no broken internal links" |
| `expect` | the console banner it was waiting for, somehow | "NO CONSOLE" |

The `expect` one is the sharpest, and it comes with a correction attached. The
script waits for ITS's console banner; the log shows `Happy hacking!` printed,
then eleven more retries, then `NO CONSOLE` — the evidence and the denial four
lines apart.

Two explanations were written down before either was tested, and **both were
wrong**, which is the real story here.

*First:* `match_max`, which defaults to **2000 bytes** and discards the oldest
data on overflow without saying so — and this script `sleep`s while ITS is
producing output, so the pty backs up. Plausible. Measured: only 312 bytes
separate `IN OPERATION` — the previous successful match, which clears the buffer
— from the banner. Nowhere near the limit.

*Second:* that the banner prints **once**, so a retry loop keyed on it can only
win on the attempt that happens to be listening. Also plausible; the fix was to
also accept `??`, ITS's answer to a ^Z at the DDT prompt, which every retry
produces. The next run sent eleven more ^Z and still reported `NO CONSOLE`, with
`^Z?? ^Z?? ^Z??…` in the log. So `??` was there, being read, and not matched
either.

At that point the thing to do is stop reasoning about the emulator and reproduce
it in ten lines:

```tcl
spawn sh -c "while :; do sleep 1; printf 'Happy hacking!\n'; done"
expect { "Happy" {} timeout { exit 1 } }
set got 0
expect { "Happy hacking" { set got 1 } timeout {} }   ;# got=0
expect {
	"Happy hacking" { set got 1 }
	timeout {}
}                                                     ;# got=1
```

Same build (`expect 5.45.4`), same spawned process, same pattern. The one-line
pattern list does not match; the multi-line one does. `tests/klh10.exp` was
written multi-line and has always worked — which is exactly why the interop runs
succeeded while the tape script, written one-line, never got past the console.

The mechanism is *still* not established, and this document is not going to guess
at it a third time. What is established is reproducible, which is enough to fix
the code and enough to warn the next person. Two wrong mechanisms in one
afternoon, on top of the `MFDCLB` note further up, is a good argument for a rule:
**a cause you have not made fail on demand is a hypothesis, and belongs in the
notes labelled as one.**

None of these produced a wrong *answer*. Each produced a **confident empty one**,
which is worse, because an empty result reads as a fact about the world: "there
is no DEFSYM for this", "no link is broken", "the console never came". A tool
that cannot distinguish *absence* from *its own failure to look* will
occasionally hand you the second while you record the first.

The defence is the same each time and it is cheap: **make the check fail on
purpose.** Point a link at a heading that does not exist. Grep for a string you
know is on line 1. If the check still passes, it was never checking. Every guard
added in this session was verified that way before being believed.

## A passing test that ran for two days

Five SIMH processes were found spinning at ~96% of a core each, two and three
days after the runs that started them. Every one belonged to a harness here, and
every one of those harnesses had reported **ok**.

The emulator is spawned by `expect`, not by the shell script around it, so when
the expect script exits with SIMH still sitting at its prompt, SIMH is orphaned
onto `init` and never stops. Nothing in the suite looked, because nothing in the
suite had any reason to: the assertions were all about what ITS *said*, and it
had said the right things.

What makes this worth a section is not the waste. It is that **a green result was
not the whole result**, in a project whose entire method is refusing to accept a
green result at face value. The check that would have caught it is trivial and
was never written, because the harnesses were graded on what they *asserted*
rather than on what they *left behind*.

All six emulator harnesses now kill anything they started, matched on the binary
and on their own run directory. Two details of that are load-bearing:

- The match walks `ps` rather than using `pkill -f "$T"`. The obvious form also
  matches the harness's own command line, whenever the directory was passed as an
  argument — so it would kill the harness mid-run, which is a worse bug than the
  one being fixed.
- The trap fires on `INT` and `TERM` as well as `EXIT`, since an interrupted run
  is exactly the case that produced the orphans.

`make emu-clean` sweeps any that survive, because a harness killed with `-9`
cannot run its own trap.

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

  That was a vague limitation until it was looked at, and it is worth scoping
  precisely, because most of it is not a file-system problem at all. A KS10 boots
  in two steps, and `mkfs` writes neither:

  **The home block**, in blocks 0 and 1, which is entirely specified and would
  take an afternoon. It is written by `NSALV`'s own `FESET` command
  (`kshack/nsalv.261`), and the code is short enough to quote whole:

  ```
  	SETZM FDBUF
  	MOVE TT,[FDBUF,,FDBUF+1]
  	BLT TT,FDBUF+177	; one 128-word sector, zeroed
  	MOVSI TT,(SIXBIT /HOM/)
  	MOVEM TT,FDBUF+0	; word 0
  	MOVEM A,FDBUF+103	; word 0103 -- the FE directory address
  	MOVE TT,[FDBUF,,FDBUF+200]
  	BLT TT,FDBUF+1777	; that sector, replicated across the block
  	MOVEI J,0 ... CALL WRITE
  	MOVEI J,1 ... CALL WRITE
  ```

  The reference pack matches it exactly: `HOM` at word 0 and `007700,,000004` at
  word 67 (= 0103), repeating every 128 words through blocks 0 and 1. Sixteen
  copies, because the sector is replicated eight times per block and the block is
  written twice. It also explains a constant the writer already had: `mkdir`'s
  floor is block 2, and blocks 0 and 1 are why.

  **The front-end file system it points at**, which is the actual work. The
  address in word 0103 is not a bootstrap — it locates a separate file system for
  the KS10's 8080 console, with its own format, manipulated by `KSFEDR`
  (`kshack/ksfedr.146`, about a thousand lines). The programs the console loads live in
  *that*, not in the ITS file system.

  So writing the home block alone would produce a pack that *claims* to be
  bootable and is not — a worse artifact than one that makes no claim. The gap is
  the FE file system, and it is a different format that happens to share a disk
  with this one.
- **One pack, one drive, one era.** Everything ITS has graded is an RP06 built
  from source in 2026. No multi-pack file system, and no artifact recovered from
  MIT. The *drive* half of that turns out to be blocked rather than merely
  undone, and the reason is worth writing down — see below.
- ~~Which ITS program writes link entries~~ — DUMP does, under its `LINKS`
  switch, and what this writes now matches it byte for byte.
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
