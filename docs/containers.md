# Containers

Three ways off the pack, and the one question all three have to answer.

A pack is 36-bit words. A tape, a tar member and a byte read through a mount are
bytes. Everything on this page is about that gap, and about the formats that
carry an ITS file somewhere else.

## The question every one of them asks

**A 36-bit word is not a byte, and there is no single right way to make bytes out
of one.** There are two honest ways and they are not interchangeable:

| | what it is | right for | wrong for |
|---|---|---|---|
| **text** | five 7-bit characters per word, most significant first | source, documentation, mail — anything ITS calls text | a binary, which it turns into garbage *silently* |
| **words** | the word in an 8-byte little-endian container | anything at all, losslessly | reading, since it is not text any more |

`cat` and `get` write text; `get -w` writes words. `tar` and `mount` take `-m`,
which selects `text`, `words`, or **`auto`** — the default, which decides per
file by looking at the words.

The test for `auto` is deliberately strict, because a wrong answer is a mangled
file. Five 7-bit characters occupy 35 of a word's 36 bits, so ITS text leaves the
last bit clear in *every* word; that plus a small set of permitted control
characters is the whole test. Anything that fails it is words, which is never
wrong, only unreadable.

Whichever way it goes, **it is reported**: `tar c` prints the tally, and `-v`
names the representation used for every file.

Going the other way, `tar x` **refuses to guess**. Bytes coming back do not say
which they are, and a wrong guess writes a mangled file into a pack, so `-m text`
or `-m words` has to be given.

## Names that are not path components

SIXBIT runs from 040 to 0137, which includes `/`, `.`, `%` and the space. The
reference pack uses three of the four, and one of them matters more than the
others: **there is a directory on it whose name is `.`**, holding `@ ITS`,
`@ DDT` and `@ NSALV` — which is to say the monitor itself. A bare `.` is not a
name a tar member or a mount path can have.

So `tar` and `mount` share one percent-encoding, in `util.c`:

| in an ITS name | becomes | why |
|---|---|---|
| `%` | `%25` | always, or the encoding would not be reversible — ten names on the pack contain one |
| `/` | `%2F` | it is the path separator |
| space | `%20` | it separates `FN1` from `FN2` here |
| `.` or `..` | `%2E`, `%2E%2E` | only when it is the *whole* component |

Nothing else is touched, so an ordinary name is itself: `KSHACK/BUILD DOC` reads
as it should, and the directory named `.` appears as `%2E`.

## `tar` — an ITS pack as an ordinary Unix archive

USTAR (POSIX.1-1988): a 512-byte header, 512-byte blocks, two zero blocks at the
end. No GNU or pax extensions are needed, because an ITS path is at most
`DIR/FN1 FN2` — twenty characters against a 100-character name field — so what
comes out is readable by every tar there is.

- ITS's `DIR;FN1 FN2` is tar's `DIR/FN1 FN2`, with a real directory member per
  ITS directory.
- An ITS **link becomes a relative symlink**, `../DIR/FN1 FN2`.
- Archiving a whole directory writes a directory member for it; archiving *files*
  by name writes only those, as tar does — and `tar x` creates a directory it
  does not find, also as tar does.
- `tar x` restores files, directories **and links**, so a pack round-trips whole.
  A tar *hard* link is refused: ITS has no such thing, an entry names blocks or
  it names another entry.

**Dates.** An ITS date is a *day* — `UNDATE` holds year, month and day, and the
time beside it is in a unit this project has not established. So an mtime here is
**noon UTC** on the day the disk records. Not midnight: an mtime is an instant and
every reader prints it in its own zone, so midnight on the 30th is the 29th to
anybody west of Greenwich, and the date would change silently by extraction. Noon
puts UTC-12 through UTC+11 on the day ITS actually wrote. A date before 1970
becomes the epoch, because a ustar mtime is octal and cannot be negative — the
reference pack has files dated 1969, and writing one as unsigned produced the
year 2241 until `tar xf` said so.

**Links to `>` dangle**, and that is the honest answer rather than a defect. `>`
is ITS's "the highest version there is", resolved when a file is opened; 88 of
the pack's 399 links point at one, and more chain to one through another link. A
symlink is a fixed string, so there is nothing to point it at. The symlink still
carries exactly what the disk records, so `readlink` shows the target ITS wrote.

### What round-tripping proves

The reference pack's **6,303 entries** — 5,657 files, 399 links, 247 directories
— out to a tar archive and back into an empty file system built by `mkfs`, with:

- identical manifests, entry for entry: type, length and checksum
- identical block accounting: 30,940 in use, 6,719 free, 505 locked out
- the same 7 links unresolved
- `check` finding no problems in the result

## `saveset` — ITS's own archive

A `DUMP` save set is what ITS itself writes to tape, and `tape` is the SIMH `.tap`
record framing under it. They are separate commands because they are separate
formats, and because most ITS tapes are not save sets at all.

This is the format to use when the other end is **ITS**: `make itsdump` compares
a tape `save` wrote against one ITS's own DUMP wrote, byte for byte, and `make
itsload` has ITS's own LOAD read one back — the only level-1 evidence in the
project. See [validation](validation.md).

`tar` is the format to use when the other end is `tar xf` and a text editor.

## `mount` — a pack on a directory

Read-only, and built only with `make FUSE=1`. libfuse3 is the project's one
optional dependency; everything else is C99 and POSIX, and the file commands do
the same work without a mount.

```
/                       the master file directory
/KSHACK                 a directory
/KSHACK/BUILD DOC       ITS's KSHACK;BUILD DOC
/%2E/@ ITS              the directory named `.`, and the monitor in it
```

Two levels, because ITS has two. An ITS link is a symlink. `statfs` reports the
allocation table, so `df` works.

**`stat` has to agree with `read`**, which is what makes a mount stricter than an
archive: `st_size` is the length of the bytes `read()` will produce, so `getattr`
decodes the file to measure it. `ls -l` of a directory therefore costs a decode
per file. It is bounded work on a local image and it is the only way the two can
agree.

**Single-threaded**, deliberately: `its_image` holds a file handle and a buffer
and is not thread-safe, so `-s` is forced rather than left to the caller.

### Why read-only is a statement about the writer

Not about FUSE. An ITS write is **whole-file**: `itsw_put` takes a complete file
and writes data, then the allocation table, then the descriptor, then the name,
in that order, so that an interruption strands blocks rather than losing a file.
FUSE hands out byte-range writes at arbitrary offsets against a file that already
exists. Bridging the two means holding a whole file in memory and flushing it on
`release()` — a write-back cache with its own failure modes, in front of a writer
whose first rule is *refuse, do not half-do*. That is a project rather than a
flag, and it is [out of scope](design.md) until it is one.
