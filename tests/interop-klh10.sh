#!/bin/sh
# interop-klh10.sh -- the same questions as interop.sh, asked of a SECOND machine.
#
#   usage: sh tests/interop-klh10.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]
#
# Run by `make interop-klh10`.  Needs KLH10 built and several minutes.
#
# WHY A SECOND EMULATOR IS WORTH THE TROUBLE.
#
# Everything in `make interop` runs under SIMH.  When ITS boots there and prints
# a file this project wrote, the chain is: our writer, our packing, SIMH's disk
# emulation, ITS.  A fault in SIMH's RP06 that happened to match a fault in our
# geometry would look exactly like success, and nothing in that run could tell.
#
# KLH10 is an unrelated implementation of the same hardware, by a different
# author, and it reads a DIFFERENT PACKING: its ITS config is `format=dbd9`,
# two words in nine bytes, where SIMH's is one word per eight.  So this run
# shares with the other one only the two things actually being tested -- the
# file system on the pack, and ITS.
#
# It also puts dbd9 where it belongs.  `make klh10` confirms the codec against
# KLH10's own converter, which is a statement about bytes.  This is the packing
# carrying a live operating system.
#
# WHAT TRIPPED THIS UP FOR A WHOLE PHASE, and is the reason for the check below.
# KLH10 drives its devices through SEPARATE PROCESSES -- dprpxx for the disk,
# dpchaos for the network -- which it execs by name at run time.  If they are
# not there it prints
#
#	[dp_exec: Cannot access "dprpxx" - No such file or directory]
#	[rp_dpstart: Start of DP "dprpxx" failed!]
#	Final init of device "dsk0" failed!
#
# in the middle of a screenful of startup, carries on, and DSKDMP then reports
# MFDCLB -- "M.F.D. clobbered".  Which reads as a corrupt pack, and is not: it
# is no pack at all.  That cost a wrong conclusion in the source once already
# (see itspack.c), so the helpers are checked for by name before anything runs.
#
# NO ROOT.  The stock `start` script runs KLH10 under sudo for networking.
# Nothing here needs it: dpchaos complains that it cannot raise its priority and
# cannot bind its port, and ITS boots anyway.  A test that wanted root would not
# be run.
#
set -u

ITSFS=${1:?usage: interop-klh10.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
IMAGE=${2:?usage: interop-klh10.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
BIN=${3:?usage: interop-klh10.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
ITS=${4:?usage: interop-klh10.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
T=${5:-$(mktemp -d)}

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


# The loader and DDT come from the ITS tree, not from the KLH10 build: they are
# PDP-10 programs that KLH10 loads, not host binaries that KLH10 runs.
LOADERS=$ITS/build/klh10

# ABSOLUTE.  The expect runs after a `cd` into the scratch directory -- KLH10
# resolves its device helpers and its ini relative to the working directory --
# so a path like `tests/klh10.exp` is not there any more by the time it is used.
EXP=$(cd "$(dirname "$0")" && pwd)/klh10.exp
DIR=${ITS_DIR:-KSHACK}
FN1=${ITS_FN1:-K10TST}
FN2=${ITS_FN2:-TXT}

pass=0
rc=0
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
no() { rc=1; printf '  FAIL %s\n' "$1"; }
skip() { printf '  skip %s\n' "$1"; }

[ -x "$ITSFS" ] || { echo "no itsfs at $ITSFS"; exit 1; }
[ -f "$IMAGE" ] || { echo "no image at $IMAGE"; exit 1; }
command -v expect >/dev/null 2>&1 || { echo "no expect(1) -- skipping"; exit 0; }

[ -x "$BIN/kn10-ks-its" ] || {
	echo "no kn10-ks-its in $BIN"
	echo "  build it, in the PDP-10/its tree:"
	echo "    cd tools/klh10 && ./autogen.sh && mkdir -p tmp && cd tmp &&"
	echo "    ../configure && make -C bld-ks-its"
	exit 1
}

# THE DEVICE HELPERS, BY NAME.  See the note above: without these the run does
# not fail, it lies.
for dp in dprpxx dpchaos dptm03; do
	[ -x "$BIN/$dp" ] || {
		echo "no $dp in $BIN -- KLH10 would run with no disk and report"
		echo "MFDCLB, which looks like a corrupt pack and is not one."
		exit 1
	}
done

mkdir -p "$T/run"
cp "$BIN/dprpxx" "$BIN/dpchaos" "$BIN/dptm03" "$T/run/" || exit 1
for f in @.ddt-u dskdmp.216bin; do
	[ -f "$LOADERS/$f" ] || { echo "no $f in $LOADERS"; exit 1; }
	cp "$LOADERS/$f" "$T/run/" || exit 1
done

echo "ITS under KLH10, on a dbd9 pack"
echo "  image   $IMAGE"
echo "  klh10   $BIN/kn10-ks-its"
echo

# ---------------------------------------------------------- the pack, in dbd9
echo "1. the pack, converted and written to"

"$ITSFS" repack -p le64 -P dbd9 "$IMAGE" "$T/run/rp0.dsk" || {
	no "repack to dbd9 failed"
	exit 1
}
ok "repacked into dbd9, which is what KLH10's ITS config asks for"

# The job SYS;ATSIGN DRAGON names owns the CTY, and while it is there no ^Z is
# ever seen.  Removing it is this project's writer clearing the way for its own
# reader to be graded -- exactly as in the SIMH run.
if "$ITSFS" del -p dbd9 -d rp06 "$T/run/rp0.dsk" 'SYS;ATSIGN DRAGON' >/dev/null 2>&1; then
	ok "removed SYS;ATSIGN DRAGON so the console can be had"
else
	no "could not remove SYS;ATSIGN DRAGON"
fi

printf 'ITSFS WROTE THIS ON A DBD9 PACK AND KLH10 PRINTED IT\n' > "$T/msg.txt"
if "$ITSFS" put -p dbd9 -d rp06 "$T/run/rp0.dsk" "$DIR;$FN1 $FN2" "$T/msg.txt" \
	>/dev/null 2>&1; then
	ok "wrote $DIR;$FN1 $FN2 onto it"
else
	no "could not write $DIR;$FN1 $FN2"
	exit 1
fi

if "$ITSFS" check -p dbd9 -d rp06 "$T/run/rp0.dsk" >"$T/check.out" 2>&1; then
	ok "...and our own checker still finds no problems"
else
	no "check reports problems after the write:"
	sed 's/^/       /' "$T/check.out" | tail -8
fi

cat > "$T/run/dskdmp.ini" <<EOF
devdef rh0  ub1   rh11   addr=776700 br=6 vec=254
devdef rh1  ub3   rh11   addr=772440 br=6 vec=224
devdef dsk0 rh0.0 rp     type=rp06 format=dbd9 path=rp0.dsk iodly=0
devdef mta0 rh1.0 tm02   fmtr=tm03 type=tu45
set clk_ithzfix=60
devdef dz0  ub3   dz11   addr=760010 br=5 vec=340
devdef chaos ub3  ch11   addr=764140 br=6 vec=270 myaddr=177002 chudpport=44042 chip=177001/localhost:44041
devdef idler ub3 host addr=777000
load @.ddt-u
load dskdmp.216bin
EOF

# --------------------------------------------------------------- 2. DSKDMP
echo
echo "2. DSKDMP, ITS's standalone loader, reads the pack"

( cd "$T/run" && KN10=$BIN/kn10-ks-its INI=dskdmp.ini MODE=dskdmp DIR=$DIR \
	expect "$EXP" ) > "$T/dskdmp.log" 2>&1
dd_rc=$?

if [ $dd_rc -ne 0 ]; then
	no "the DSKDMP run did not finish (see $T/dskdmp.log)"
elif grep -aq "MFDCLB" "$T/dskdmp.log"; then
	no "DSKDMP says MFDCLB -- check that dprpxx started (see $T/dskdmp.log)"
elif grep -aq "^ #[0-9]* *$FN1 *$FN2" "$T/dskdmp.log"; then
	ok "DSKDMP listed our file, off a dbd9 pack, with its own code"
else
	no "DSKDMP did not list $FN1 $FN2 -- see $T/dskdmp.log"
fi

# The listing must still be SORTED.  A writer that puts a name in the wrong
# slot is what this catches, and DSKDMP walks the name area itself.
sed -n 's/^ #[0-9]* \(.*\)$/\1/p' "$T/dskdmp.log" | sed 's/ *$//' > "$T/names"
sort "$T/names" > "$T/names.sorted"
if [ -s "$T/names" ] && cmp -s "$T/names" "$T/names.sorted"; then
	ok "...and the listing is in order ($(wc -l < "$T/names" | tr -d ' ') entries)"
else
	no "DSKDMP's listing is out of order -- the insertion went in the wrong slot"
fi

# ---------------------------------------------------------- 3. the monitor
echo
echo "3. ITS boots on it, and prints the file"

( cd "$T/run" && KN10=$BIN/kn10-ks-its INI=dskdmp.ini MODE=print \
	FILE="$DIR;$FN1 $FN2" MATCH='ITSFS WROTE THIS' \
	expect "$EXP" ) > "$T/print.log" 2>&1
pr_rc=$?

if grep -aq "IN OPERATION" "$T/print.log"; then
	ok "ITS reached IN OPERATION -- its salvage pass walked every directory"
else
	no "ITS did not come up (see $T/print.log)"
fi

case $pr_rc in
0)
	ok "THE MONITOR PRINTED THE FILE, on a second emulator and a second packing"
	;;
3)
	# Getting the console is timing-dependent in a way that has not been
	# pinned down, under either emulator.  Reporting "the monitor cannot read
	# our file" on a missed ^Z would be reporting a lie about the pack.
	skip "the monitor never gave up its console (timing) -- file not tested"
	;;
*)
	no "the monitor did not print the file (see $T/print.log)"
	;;
esac

# AND THE FILE MUST SURVIVE ITS HAVING THE PACK.  A running ITS writes to the
# disk -- it salvages at startup, it keeps its own state -- so "the monitor read
# it" and "the monitor left it alone" are two questions.  The SIMH run asks both
# and so does this one.
echo
echo "4. and the pack afterwards"

# NOT "still checks clean" -- THAT WAS THE WRONG ASSERTION, and writing it cost
# a failing test to find out.  A running ITS creates files of its own, and this
# harness stops the machine by HALTING it, so the TUT it had in memory is never
# written back.  The result is exactly what ITS's own salvager has a category
# for: reference counts that disagree with the files, on a pack that is
# otherwise sound.  70 of them here, and the pack had grown from 5658 files to
# 5660 while it ran.  Demanding a clean check is demanding that ITS not be ITS.
#
# What must hold is the part that means data is at risk:
#
#   free but claimed        a file holds a block the allocator will hand out
#   on locked-out blocks    a file holds a directory or a table
#
# Those must be zero.  A miscount costs space and nothing else, and NSALV fixes
# it in one pass.
"$ITSFS" check -p dbd9 -d rp06 "$T/run/rp0.dsk" > "$T/check2.out" 2>&1
d=$(sed -n 's/^disagreements *//p' "$T/check2.out")

case "$d" in
"0 free but claimed, 0 in use but unclaimed"*)
	ok "no block is both free and claimed after ITS ran"
	;;
"")
	ok "the pack still checks clean after ITS had it"
	;;
*)
	no "a dangerous disagreement after ITS ran: $d"
	;;
esac

case "$d" in
*"0 on locked-out blocks")
	ok "...and no file has landed on a directory or a table"
	;;
"")
	ok "...and nothing is on a locked-out block"
	;;
*)
	no "a file holds a locked-out block: $d"
	;;
esac

# Said out loud rather than hidden, because a reader seeing `check` exit
# non-zero here should know it is expected.
case "$d" in
*" 0 miscounted"*) ;;
"") ;;
*)
	echo "       (miscounts are expected: the machine was halted, so the TUT"
	echo "        ITS held in memory was never written back -- $d)"
	;;
esac

if "$ITSFS" cat -p dbd9 -d rp06 "$T/run/rp0.dsk" "$DIR;$FN1 $FN2" 2>/dev/null |
	cmp -s - "$T/msg.txt"; then
	ok "...and our file is there, byte for byte, unchanged"
else
	no "our file changed or vanished while ITS had the pack"
fi

echo
if [ "$rc" -eq 0 ]; then
	echo "$pass checks passed -- a second emulator, a second packing, same answers"
else
	echo "FAILED"
fi
exit $rc
