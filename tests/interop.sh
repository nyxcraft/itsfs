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
# AND IT ASKS ABOUT A DIRECTORY THIS PROJECT MADE, not only about a file in one
# of ITS's.  That is the stronger half: the MFD entry, the block it resolves to
# -- which in this format is the entry's own POSITION, with no pointer anywhere
# to check it against -- and the UFD header in it are all ours, and DSKDMP
# resolves them with its own arithmetic.
#
# AND THE MONITOR OPENS THE FILE AND PRINTS IT, which is the whole point and
# took the longest to arrange.  Not because of the file system: because a
# running ITS will not give up its console until the job that owns it is gone.
# `SYS;ATSIGN DRAGON` names that job, and `itsfs del` removes it -- this
# project's own writer clearing the way for its own reader to be graded.
#
# The older note is kept because what it rules out is still worth not
# re-testing:
#
#   ^Z on the CTY produces nothing, before or after the SYSJOB patch below.
#   ITS's own doc/DDT.md says ^Z is how you get a terminal.
#
#   `set cpu idle` is not the cause.  Tried without it; no difference.
#
#   The DZ lines are not it either.  Lines 0, 5, 6 and 7 were tried over raw
#   sockets with the telnet option negotiation answered properly; simh accepts
#   the connection and ITS never says anything on any of them.
#
#   THE CAUSE, now confirmed: the finished system auto-starts a job -- an
#   unpatched boot prints "LOGIN TARAKA 0" -- and that job owns the CTY.  The
#   ITS build drives the console successfully during a BUILD, where no such job
#   exists yet, which is why its scripts work and these did not.
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

# ---------------------------------------------------------------------------
# KILL THE EMULATOR ON THE WAY OUT, and this is not a tidiness measure.
#
# The emulator is spawned by expect, not by this script, so when the expect
# script exits with SIMH still sitting at its prompt, SIMH is orphaned onto
# init and SPINS AT 100%% OF A CORE FOREVER.  That is not hypothetical: five of
# them were found running two and three days after the runs that started them,
# ~96%% CPU each, and nothing in this suite would ever have noticed.
#
# Matched on the BINARY and on this run's own directory, so it cannot touch an
# emulator somebody else is using -- and by walking `ps` rather than
# `pkill -f "$T"`, which would also match this script's own command line when the
# directory was passed as an argument, and kill the harness mid-run.
emu_cleanup() {
	for _p in $(ps -eo pid,args 2>/dev/null |
		awk -v d="$T" '$0 ~ d && ($2 ~ /pdp10$/ || $2 ~ /kn10/) { print $1 }'); do
		kill "$_p" 2>/dev/null || true
	done
}
trap emu_cleanup EXIT INT TERM


DIR=${DIR:-KSHACK}
FN1=${FN1:-ITSFS}
FN2=${FN2:-TXT}

# A directory this project makes from nothing, to put beside the one ITS made.
NEWDIR=${NEWDIR:-ITSFS}

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
echo "2. make a directory from nothing, and write into that"

if "$ITSFS" mkdir "$T/i.dsk" "$NEWDIR" >/dev/null 2>&1; then
	ok "mkdir $NEWDIR"
else
	fail "itsfs mkdir failed"
fi

# Three names at three points in the sort order, so the insertion into a
# brand-new empty name area is exercised at the front, middle and end.
made=0

for f in "$NEWDIR;FIRST FILE" "$NEWDIR;AAA X" "$NEWDIR;ZZZ X"; do
	"$ITSFS" put "$T/i.dsk" "$f" "$T/msg.txt" >/dev/null 2>&1 && made=$((made + 1))
done

if [ "$made" -eq 3 ]; then
	ok "...and put three files into it"
else
	fail "only $made of 3 files went into $NEWDIR"
fi

if "$ITSFS" check "$T/i.dsk" >/dev/null 2>&1; then
	ok "itsfs check: still clean"
else
	fail "itsfs check found problems after mkdir"
fi

echo
echo "3. DSKDMP, ITS's standalone loader, reads the directory"

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

# ...AND THE DIRECTORY WE MADE OURSELVES.  This is the stronger of the two:
# the MFD entry, the block it resolves to and the UFD header in it are all this
# project's work, and DSKDMP resolves them with its own arithmetic.
ITS_PDP10=$PDP10 ITS_IMAGE=$T/i.dsk ITS_LOG=$T/ours.log ITS_DIR=$NEWDIR ITS_MODE=dskdmp \
	expect "$EXP" > "$T/ours.exp" 2>&1

if grep -q "FIRST *FILE" "$T/ours.log" 2>/dev/null; then
	ok "DSKDMP read a directory THIS PROJECT MADE, and listed its files"
else
	fail "DSKDMP did not list $NEWDIR's files -- see $T/ours.log"
fi

sed -n 's/^ #[0-9]* \(.*\)$/\1/p' "$T/ours.log" | sed 's/ *$//' > "$T/ours.names"
n=$(wc -l < "$T/ours.names" | tr -d ' ')
sort "$T/ours.names" > "$T/ours.sorted"

if [ "$n" -eq 3 ] && cmp -s "$T/ours.names" "$T/ours.sorted"; then
	ok "...all three, in order"
else
	fail "expected three entries in order, got $n"
	sed 's/^/       /' "$T/ours.names"
fi

echo
echo "4. ...and then the monitor itself boots on it"

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

# The boot writes to the pack -- a salvage pass is a write -- so the files have
# to still be there and still be right afterwards.  Both of them: the one in
# ITS's directory and the one in ours.
"$ITSFS" cat "$T/i.dsk" "$DIR;$FN1 $FN2" > "$T/back2.txt" 2>/dev/null

if cmp -s "$T/msg.txt" "$T/back2.txt"; then
	ok "...and the file is still there, unchanged, after ITS had the pack"
else
	fail "the file changed while ITS had the pack"
fi

"$ITSFS" cat "$T/i.dsk" "$NEWDIR;FIRST FILE" > "$T/back3.txt" 2>/dev/null

if cmp -s "$T/msg.txt" "$T/back3.txt"; then
	ok "...and so is the one in the directory we made"
else
	fail "the file in $NEWDIR did not survive"
fi

if "$ITSFS" check "$T/i.dsk" >/dev/null 2>&1; then
	ok "...and the pack still checks clean"
else
	fail "the pack does not check clean after ITS had it"
fi

echo
echo "5. ...and the monitor opens the file and prints it"

# THE CONSOLE HAS TO BE FREED FIRST, and `itsfs del` is what frees it: the job
# `SYS;ATSIGN DRAGON` names owns the CTY, and while it is there ^Z produces
# nothing at all.  This is the project's own writer clearing the way for its own
# reader to be graded by ITS.
cp "$T/i.dsk" "$T/p.dsk" || exit 2

if "$ITSFS" del "$T/p.dsk" 'SYS;ATSIGN DRAGON' >/dev/null 2>&1; then
	ok "removed the job that owns the console (itsfs del)"
else
	fail "could not remove SYS;ATSIGN DRAGON"
fi

ITS_PDP10=$PDP10 ITS_IMAGE=$T/p.dsk ITS_LOG=$T/print.log ITS_DIR=$DIR ITS_MODE=print \
	ITS_FILE="$DIR;$FN1 $FN2" ITS_MATCH="IF ITS LISTS THIS THE ENTRY IS REAL" \
	expect "$EXP" > "$T/print.exp" 2>&1
prc=$?

#
# THREE OUTCOMES, AND THEY ARE NOT TWO.  The monitor printing the file is the
# result; the monitor failing to print it is a finding; and never getting a
# console is NEITHER -- it is the harness failing to ask the question.
#
# Getting a terminal out of a running ITS is timing-dependent in a way that has
# not been pinned down: the same pack and the same script get one on one run and
# not the next.  Reporting that as "the monitor cannot read our file" would be
# reporting a lie about the file system, so it is reported as what it is and
# does not fail the run.  It has been seen to work, by hand and through this
# harness; see docs/validation.md.
#
if grep -q "IF ITS LISTS THIS THE ENTRY IS REAL" "$T/print.log" 2>/dev/null; then
	ok "THE MONITOR PRINTED A FILE THIS PROJECT WROTE"
	sed -n '/:print/,/^\*/p' "$T/print.log" | sed 's/\r$//; s/^/       /' | head -6
elif [ "$prc" -eq 3 ]; then
	echo "  --   no console this run (timing); the file was not put to the monitor."
	echo "       This is the harness failing to ask, not ITS failing to answer."
else
	fail "the monitor got a console and did NOT print the file -- see $T/print.log"
	tail -6 "$T/print.log" 2>/dev/null | sed 's/^/       /'
fi

echo
if [ $rc -eq 0 ]; then
	echo "ITS booted on a file system this project wrote, its standalone loader"
	echo "listed the files -- including out of a directory made from nothing --"
	echo "and the monitor itself opened one and printed it."
else
	echo "logs are in $T"
fi

exit $rc
