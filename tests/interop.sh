#!/bin/sh
# interop.sh -- write a file with itsfs, then let ITS itself look at the pack.
#
#   usage: sh tests/interop.sh <itsfs> <image> <pdp10> [scratch dir]
#
# Run by `make interop`.  Needs an emulator and a few minutes, so it is not part
# of `make test`.
#
# WHAT THIS ESTABLISHES, and how it differs from `make nsalv`.
#
# `nsalv` hands a pack to ITS's SALVAGER -- a program whose job is to inspect a
# file system.  This hands it to the two programs that USE one:
#
#   DSKDMP    ITS's standalone dumper.  It boots from the disk, reads the file
#             system with its own code, and lists a directory.  A third
#             implementation, sharing nothing with the monitor or with NSALV.
#             If our name block is in the wrong place -- or in the wrong ORDER,
#             which is the mistake a writer makes -- this is what notices.
#
#   the monitor  ITS runs a salvage pass at startup, over every directory on the
#             pack, before it will come up.  Reaching "IN OPERATION" means that
#             pass found nothing to stop for.
#
# WHAT IT DOES NOT ESTABLISH.  The monitor has not been made to OPEN our file
# and print it.  ITS's console stops accepting input once the system is up: ^Z,
# which its own doc/DDT.md says gets you a terminal, produces nothing on the
# CTY, and the DZ lines this machine profile exposes are not configured for
# login.  So the monitor's file-opening path -- as opposed to its salvage path
# -- is untested here, and the roadmap says so rather than this pretending
# otherwise.
#
# ONE METHOD THAT LOOKED RIGHT AND WAS NOT, recorded so nobody spends the
# afternoon again: DSKDMP has `L<ESC>file` (load a file into core) and
# `I<ESC>file` (verify a file against core), which look like a byte-for-byte
# comparison harness -- load ITS's original, verify our copy against it.  It
# reports CMPERR for a copy this project made AND for the original against
# ITSELF.  Whatever those two commands compare, it is not what it appears to be,
# and the control run is the only reason that was not written up as a finding.
#
set -u

ITSFS=${1:?usage: interop.sh <itsfs> <image> <pdp10> [scratch]}
IMAGE=${2:?usage: interop.sh <itsfs> <image> <pdp10> [scratch]}
PDP10=${3:?usage: interop.sh <itsfs> <image> <pdp10> [scratch]}
T=${4:-/tmp/itsfs-interop}

DIR=${DIR:-KSHACK}
FN1=${FN1:-ITSFS}
FN2=${FN2:-TXT}

EXP=$(dirname "$0")/interop.exp
[ -f "$EXP" ] || { echo "interop.sh: no $EXP"; exit 2; }
[ -x "$ITSFS" ] || { echo "interop.sh: no $ITSFS (build first)"; exit 2; }
[ -f "$IMAGE" ] || { echo "interop.sh: no image at $IMAGE"; exit 2; }
command -v expect >/dev/null 2>&1 || { echo "interop.sh: no expect on PATH"; exit 2; }
[ -x "$PDP10" ] || command -v "$PDP10" >/dev/null 2>&1 ||
	{ echo "interop.sh: no emulator at $PDP10"; exit 2; }

rm -rf "$T" && mkdir -p "$T" || exit 2
rc=0
fail() { echo "  FAIL $*"; rc=1; }
ok() { echo "  ok   $*"; }

cp "$IMAGE" "$T/i.dsk" || exit 2
printf 'HELLO FROM ITSFS.\r\nIF ITS LISTS THIS THE ENTRY IS REAL.\r\n' > "$T/msg.txt"

echo "1. write a file with itsfs"

if "$ITSFS" put "$T/i.dsk" "$DIR;$FN1 $FN2" "$T/msg.txt" >/dev/null 2>&1; then
	ok "put $DIR;$FN1 $FN2"
else
	fail "itsfs put failed"
	exit 1
fi

"$ITSFS" cat "$T/i.dsk" "$DIR;$FN1 $FN2" > "$T/back.txt" 2>/dev/null

if cmp -s "$T/msg.txt" "$T/back.txt"; then
	ok "...and it reads back byte-identical"
else
	fail "...and it reads back byte-identical"
fi

if "$ITSFS" check "$T/i.dsk" >/dev/null 2>&1; then
	ok "itsfs check: clean"
else
	fail "itsfs check found problems"
fi

echo
echo "2. DSKDMP, ITS's standalone loader, reads the directory"

ITS_PDP10=$PDP10 ITS_IMAGE=$T/i.dsk ITS_LOG=$T/its.log ITS_DIR=$DIR ITS_MODE=dskdmp \
	expect "$EXP" > "$T/dskdmp.exp" 2>&1

if [ $? -ne 0 ]; then
	fail "the DSKDMP run did not finish (see $T/its.log)"
	tail -5 "$T/its.log" 2>/dev/null | sed 's/^/       /'
	exit 1
fi

# DSKDMP prints one line per entry, as ` #NN FN1    FN2   `.
if grep -q "$FN1 *$FN2" "$T/its.log" 2>/dev/null; then
	ok "DSKDMP listed our file, reading the directory with its own code"
else
	fail "DSKDMP did not list $FN1 $FN2 -- see $T/its.log"
fi

# AND IN THE RIGHT PLACE.  A writer that appended instead of inserting would
# still be listed -- at the end.  The check that matters is that DSKDMP's own
# reading of the directory comes out sorted, which is what ITS maintains.
sed -n 's/^ #[0-9]* \(.*\)$/\1/p' "$T/its.log" | sed 's/ *$//' > "$T/dskdmp.names"
sort "$T/dskdmp.names" > "$T/dskdmp.sorted"

if [ -s "$T/dskdmp.names" ] && cmp -s "$T/dskdmp.names" "$T/dskdmp.sorted"; then
	ok "...and the whole listing is still in order ($(wc -l < "$T/dskdmp.names" | tr -d ' ') entries)"
else
	fail "DSKDMP's listing is not in order -- the insertion put it in the wrong place"
	diff "$T/dskdmp.names" "$T/dskdmp.sorted" 2>&1 | head -6 | sed 's/^/       /'
fi

echo
echo "3. ...and then the monitor itself boots on it"

ITS_PDP10=$PDP10 ITS_IMAGE=$T/i.dsk ITS_LOG=$T/boot.log ITS_DIR=$DIR ITS_MODE=boot \
	expect "$EXP" > "$T/boot.exp" 2>&1

if [ $? -ne 0 ]; then
	fail "the boot run did not finish (see $T/boot.log)"
	tail -5 "$T/boot.log" 2>/dev/null | sed 's/^/       /'
fi

if grep -q "Salvager" "$T/boot.log" 2>/dev/null; then
	ok "the monitor ran its startup salvage over the pack"
else
	fail "no startup salvage in the log"
fi

if grep -q "IN OPERATION" "$T/boot.log" 2>/dev/null; then
	ok "ITS CAME UP on a pack itsfs wrote to"
else
	fail "ITS did not come up"
fi

# The boot writes to the pack -- a salvage pass is a write -- so the file has to
# still be there and still be right afterwards.
"$ITSFS" cat "$T/i.dsk" "$DIR;$FN1 $FN2" > "$T/back2.txt" 2>/dev/null

if cmp -s "$T/msg.txt" "$T/back2.txt"; then
	ok "...and the file is still there, unchanged, after ITS had the pack"
else
	fail "the file changed while ITS had the pack"
fi

if "$ITSFS" check "$T/i.dsk" >/dev/null 2>&1; then
	ok "...and the pack still checks clean"
else
	fail "the pack does not check clean after ITS had it"
fi

echo
if [ $rc -eq 0 ]; then
	echo "ITS booted on a file system this project wrote, and its standalone"
	echo "loader listed the file -- in the right place in the directory."
else
	echo "logs are in $T"
fi

exit $rc
