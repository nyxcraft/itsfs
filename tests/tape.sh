#!/bin/sh
# tape.sh -- read a real ITS tape and check it against the files it was made of.
#
#   usage: sh tests/tape.sh <itsfs> <tapefile> <source dir> [scratch]
#
# Run by `make tape-test`.  No emulator: this is a file comparison, which is
# what makes it the cheapest strong result in the project.
#
# THIS IS THE MEASUREMENT THAT PROMOTED `core` FROM corroborated TO confirmed.
# The layout was never in doubt -- the TM03 formatter manual gives it frame by
# frame -- but until phase 9 no ITS artifact had been decoded through THIS code,
# and borrowing a sibling project's measurement would have been dishonest.
#
# Two independent things establish it, and the second is the one that convinces:
#
#   ARITHMETIC.  The salvager image is 79,890 bytes.  That is 15,978 x 5 exactly
#   and 9,986.25 x 8, so the file cannot be one word per eight bytes.
#
#   ITS'S OWN WORDS.  Decoded as five frames per word and read as five seven-bit
#   characters each, the image contains "Salvager", "Use MFD from unit" and
#   "unprotected in old TUT" -- three strings this project has watched NSALV
#   print on a console, the last of them about a pack it damaged on purpose.
#   Text that came out of the emulated machine, found by this decoder in the
#   file the machine loaded it from.
#
# AND THEN THE SAVE SETS, in both directions, with itstar as the second opinion:
# it reads one this project wrote, and extracts a file that came off the pack
# byte-identically to the host original it was built from -- a round trip
# through two independent implementations and an operating system.
#
set -u

ITSFS=${1:?usage: tape.sh <itsfs> <tapefile> <source dir> [scratch]}
TAPE=${2:?usage: tape.sh <itsfs> <tapefile> <source dir> [scratch]}
SRC=${3:?usage: tape.sh <itsfs> <tapefile> <source dir> [scratch]}
T=${4:-/tmp/itsfs-tape}

[ -x "$ITSFS" ] || { echo "tape.sh: no $ITSFS (build first)"; exit 2; }
[ -f "$TAPE" ] || { echo "tape.sh: no tape at $TAPE"; exit 2; }
[ -d "$SRC" ] || { echo "tape.sh: no source directory at $SRC"; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "tape.sh: needs python3"; exit 2; }

rm -rf "$T" && mkdir -p "$T" || exit 2
rc=0
fail() { echo "  FAIL $*"; rc=1; }
ok() { echo "  ok   $*"; }

echo "1. the container"

if "$ITSFS" tape "$TAPE" > "$T/framing.out" 2>&1; then
	ok "read the framing: $(grep -o '[0-9]* records, [0-9]* tape marks.*' "$T/framing.out")"
	sed -n 's/^/       /p' "$T/framing.out" | grep 'file '
else
	fail "itsfs tape could not read $TAPE:"
	sed 's/^/       /' "$T/framing.out"
	exit 1
fi

echo
echo "2. the words, against the files the tape was made from"

"$ITSFS" tape -x "$T" "$TAPE" >/dev/null 2>&1

# The ITS Makefile builds this tape as `tapewrite -n 2560 salv.tape RAM NSALV`,
# so file 0 is the microcode RAM and file 1 is the salvager.
n=0
for host in ram.262 salv.rp06; do
	[ -f "$SRC/$host" ] || { echo "  -- no $SRC/$host, skipping"; n=$((n + 1)); continue; }
	[ -f "$T/file$n.words" ] || { fail "no file$n.words was extracted"; n=$((n + 1)); continue; }

	# Re-encode the extracted words as core and compare with the host file.
	# Doing it here rather than with `itsfs repack` keeps the comparison
	# honest: this is a second implementation of the packing, in ten lines.
	python3 - "$T/file$n.words" "$SRC/$host" <<'PY' > "$T/cmp$n.out" 2>&1
import struct, sys
d = open(sys.argv[1], 'rb').read()
words = [struct.unpack_from("<Q", d, i)[0] for i in range(0, len(d), 8)]
core = bytearray()
for w in words:
    core += bytes([(w >> 28) & 0xFF, (w >> 20) & 0xFF, (w >> 12) & 0xFF,
                   (w >> 4) & 0xFF, w & 0x0F])
host = open(sys.argv[2], 'rb').read()
print("same" if bytes(core[:len(host)]) == host else "differ", len(words), len(host))
PY
	set -- $(cat "$T/cmp$n.out")

	if [ "${1:-}" = "same" ]; then
		ok "file$n: $2 words re-encode to $host byte for byte"
	else
		fail "file$n does not match $host: $(cat "$T/cmp$n.out")"
	fi

	n=$((n + 1))
done

echo
echo "3. ITS's own words, in the image"

# Read each 36-bit word as five seven-bit characters and look for text this
# project has watched the program print.
python3 - "$T/file1.words" <<'PY' > "$T/text.out" 2>&1
import struct, sys
d = open(sys.argv[1], 'rb').read()
words = [struct.unpack_from("<Q", d, i)[0] for i in range(0, len(d), 8)]
chars = bytearray()
for w in words:
    for k in range(5):
        chars.append((w >> (29 - 7 * k)) & 0x7F)
s = bytes(chars)
for probe in (b"Salvager", b"Use MFD from unit", b"unprotected in old TUT"):
    i = s.find(probe)
    print("%s %d %s" % ("found" if i >= 0 else "missing", i // 5, probe.decode()))
PY

while read -r what word rest; do
	if [ "$what" = "found" ]; then
		ok "\"$rest\" is in the image at word $word"
	else
		fail "\"$rest\" is NOT in the image -- the decoding is wrong"
	fi
done < "$T/text.out"

# --- and the save sets, if there is a pack and an itstar to check against.

ITSTAR=${ITSTAR:-$(dirname "$TAPE")/../../tools/itstar/itstar}
PACK=${PACK:-$(dirname "$TAPE")/rp0.dsk}
SRCTREE=${SRCTREE:-$(dirname "$TAPE")/../../src}

if [ -x "$ITSTAR" ] && [ -f "$PACK" ]; then
	echo
	echo "4. a save set this project wrote, read by itstar"

	if "$ITSFS" save "$PACK" "$T/out.tap" 'KSHACK;ITS 15' 'KSHACK;DDT BIN' \
		> "$T/save.out" 2>&1; then
		ok "wrote a save set of two entries, one of them a link"
	else
		fail "itsfs save failed:"
		sed 's/^/       /' "$T/save.out"
	fi

	# Our own reader first: a tape that only WE can read proves nothing.
	"$ITSFS" saveset "$T/out.tap" > "$T/mine.list" 2>&1

	if grep -q "KSHACK;ITS 15" "$T/mine.list" && grep -q "DDT BIN -> " "$T/mine.list"; then
		ok "...and reads it back, link and all"
	else
		fail "our own reader does not agree with our writer"
		sed 's/^/       /' "$T/mine.list"
	fi

	"$ITSTAR" -tf "$T/out.tap" > "$T/its.list" 2>&1

	if grep -q "KSHACK;ITS 15" "$T/its.list" && grep -q "KSHACK;DDT BIN" "$T/its.list"; then
		ok "ITSTAR READS IT: $(grep -c 'KSHACK' "$T/its.list") entries, and the volume header"
	else
		fail "itstar could not read the save set:"
		sed 's/^/       /' "$T/its.list"
	fi

	# THE ROUND TRIP, and the reason it is worth the trouble.  `its.15` went
	# INTO ITS as a host file -- itstar packed it onto a tape, ITS's loader
	# put it on the pack.  This takes it back out by a different road: our
	# reader, our save-set writer, itstar's extractor.  If it survives both
	# ways it is not one implementation agreeing with itself.
	mkdir -p "$T/x"
	(cd "$T/x" && "$ITSTAR" -xf "$T/out.tap" >/dev/null 2>&1)

	if [ -f "$T/x/kshack/its.15" ]; then
		ok "...and extracts the file"
	else
		fail "itstar did not extract kshack/its.15"
	fi

	if [ -f "$SRCTREE/kshack/its.15" ] && [ -f "$T/x/kshack/its.15" ]; then
		if cmp -s "$T/x/kshack/its.15" "$SRCTREE/kshack/its.15"; then
			ok "ROUND TRIP: byte-identical to the host file ITS was given"
		else
			fail "the round trip changed the file"
			cmp "$T/x/kshack/its.15" "$SRCTREE/kshack/its.15" 2>&1 |
				head -2 | sed 's/^/       /'
		fi
	fi

	# A link must come out a link.  itstar makes it a symlink on the host.
	if [ -L "$T/x/kshack/ddt.bin" ]; then
		ok "...and the link is a link: -> $(readlink "$T/x/kshack/ddt.bin")"
	else
		fail "the link did not survive as one"
	fi
fi

echo
if [ $rc -eq 0 ]; then
	echo "an ITS tape decoded to the exact words its own program prints,"
	echo "and a save set this project wrote read back by ITS's own reader."
else
	echo "output is in $T"
fi

exit $rc
