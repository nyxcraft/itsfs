#!/bin/sh
# mkfs.sh -- build a file system from nothing, and let ITS grade it.
#
#   usage: sh tests/mkfs.sh <itsfs> <pdp10> <salv.tape> <dskdmp.tape> [scratch]
#
# Run by `make mkfs-test`.  Needs an emulator and two ITS tapes.
#
# THIS IS THE ONE PACK WITH NO ITS IN IT.  Everywhere else in this project the
# starting point is a pack ITS built and the question is whether we read or
# extend it correctly.  Here every word was written by `itsfs mkfs`: the master
# file directory, the allocation table, the directory blocks, the directories,
# the files.  If any of the three structures is wrong in a way that only shows
# up when nothing else is right, this is what finds it.
#
# HOW IT IS GRADED, and why not the obvious way.  A pack this makes DOES NOT
# BOOT, and that is correct rather than a shortcoming: ITS boots from the front
# end's blocks at the very bottom of the disk -- the ones NSALV's ZAP calls the
# "8080 'HOM' sectors" and explicitly refuses to touch -- and then loads a system
# out of a directory.  `mkfs` writes a file system; the boot area and the system
# are somebody else's job and this does not pretend otherwise.
#
# So both graders are booted FROM TAPE and pointed at the pack:
#
#   NSALV    ITS's salvager.  Walks every directory, rebuilds the allocation
#            table from scratch, and compares.  It also prints the pack ID out
#            of the table header, which is our text coming back through ITS.
#   DSKDMP   ITS's standalone loader, a third implementation.  Lists a
#            directory, which exercises the MFD entry, the block its POSITION
#            resolves to, and the UFD header -- all three of them ours.
#
set -u

ITSFS=${1:?usage: mkfs.sh <itsfs> <pdp10> <salv.tape> <dskdmp.tape> [scratch]}
PDP10=${2:?usage: mkfs.sh <itsfs> <pdp10> <salv.tape> <dskdmp.tape> [scratch]}
SALV=${3:?usage: mkfs.sh <itsfs> <pdp10> <salv.tape> <dskdmp.tape> [scratch]}
DSKDMP=${4:?usage: mkfs.sh <itsfs> <pdp10> <salv.tape> <dskdmp.tape> [scratch]}
T=${5:-/tmp/itsfs-mkfs}

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
trap emu_cleanup EXIT INT TERM HUP PIPE


ID=${ID:-ITSFS}

# THE DRIVE, AND THE ONE NUMBER THAT DEPENDS ON IT.  A fresh file system locks
# out the 500 directory slots, the MFD, and the allocation table -- and only the
# table's size varies between drives.  Both values are written out here rather
# than asked of `itsfs drives`, for the same reason the geometry is written out
# in tests/run.sh: a wrong table must not be able to agree with itself.
#
#   rp06   500 + 1 + 4 = 505
#   rm03   500 + 1 + 2 = 503
DRIVE=${ITS_DRIVE:-rp06}

case "$DRIVE" in
rp06) LOCKED=505; NTUT=4 ;;
rm03) LOCKED=503; NTUT=2 ;;
*)
	echo "mkfs.sh: no locked-out count written down for $DRIVE."
	echo "  It is 500 directory slots + 1 MFD + that drive's table blocks."
	exit 2
	;;
esac
EXP=$(dirname "$0")/nsalv.exp

[ -x "$ITSFS" ] || { echo "mkfs.sh: no $ITSFS (build first)"; exit 2; }
[ -f "$SALV" ] || { echo "mkfs.sh: no salvager tape at $SALV"; exit 2; }
[ -f "$DSKDMP" ] || { echo "mkfs.sh: no dskdmp tape at $DSKDMP"; exit 2; }
command -v expect >/dev/null 2>&1 || { echo "mkfs.sh: no expect on PATH"; exit 2; }
[ -x "$PDP10" ] || command -v "$PDP10" >/dev/null 2>&1 ||
	{ echo "mkfs.sh: no emulator at $PDP10"; exit 2; }

rm -rf "$T" && mkdir -p "$T" || exit 2
rc=0
fail() { echo "  FAIL $*"; rc=1; }
ok() { echo "  ok   $*"; }

echo "1. build a file system where nothing was"

if "$ITSFS" mkfs -d "$DRIVE" "$T/new.dsk" "$ID" > "$T/mkfs.out" 2>&1; then
	ok "$(cat "$T/mkfs.out")"
else
	fail "itsfs mkfs failed:"
	sed 's/^/       /' "$T/mkfs.out"
	exit 1
fi

# IT IS SPARSE, AND THAT IS THE POINT OF SAYING SO: a fresh file system is
# almost entirely zeros, and an image that occupied 300 MB to say that would be
# a waste of everybody's disk.
apparent=$(wc -c < "$T/new.dsk" | tr -d ' ')
actual=$(du -k "$T/new.dsk" | awk '{print $1}')
echo "   $apparent bytes, $actual KB actually on disk"

if "$ITSFS" check -d "$DRIVE" "$T/new.dsk" > "$T/check1.out" 2>&1; then
	ok "itsfs check: clean, with nothing in it"
else
	fail "itsfs check found problems on a fresh file system:"
	sed -n '2,6p' "$T/check1.out" | sed 's/^/       /'
fi

# The locked-out set is the whole structural claim: the directory slots, the
# MFD and the table itself, and nothing else.
if grep -aq "$LOCKED locked out" "$T/check1.out" 2>/dev/null; then
	ok "...and $LOCKED blocks locked out: 500 directory slots + 1 MFD + $NTUT table"
else
	fail "the locked-out count is not 500 + 1 + 4"
	grep "locked out" "$T/check1.out" | sed 's/^/       /'
fi

echo
echo "2. put something in it"

printf 'BUILT BY ITSFS MKFS.\r\nNO PART OF THIS PACK EXISTED BEFORE.\r\n' > "$T/msg.txt"
made=0

for d in SYS KSHACK; do
	"$ITSFS" mkdir -d "$DRIVE" "$T/new.dsk" "$d" >/dev/null 2>&1 && made=$((made + 1))
done

for f in "SYS;HELLO TXT" "SYS;AAA X" "KSHACK;NOTE TXT"; do
	"$ITSFS" put -d "$DRIVE" "$T/new.dsk" "$f" "$T/msg.txt" >/dev/null 2>&1 && made=$((made + 1))
done

if [ "$made" -eq 5 ]; then
	ok "two directories and three files"
else
	fail "only $made of 5 succeeded"
fi

"$ITSFS" cat -d "$DRIVE" "$T/new.dsk" 'SYS;HELLO TXT' > "$T/back.txt" 2>/dev/null

if cmp -s "$T/msg.txt" "$T/back.txt"; then
	ok "...and a file reads back byte-identical"
else
	fail "...and a file reads back byte-identical"
fi

if "$ITSFS" check -d "$DRIVE" "$T/new.dsk" >/dev/null 2>&1; then
	ok "itsfs check: still clean"
else
	fail "itsfs check found problems after populating it"
fi

echo
echo "3. hand it to NSALV, ITS's own salvager (booted from tape)"

# NSALV'S DRIVE IS CHOSEN WHEN IT IS ASSEMBLED, not when it is run.  Its source
# selects one inside a machine block --
#
#     IFCE MCHN,PM,[
#             ...
#             RM03P==1        ;RM03 on RH11 UNIBUS controller.
#
# -- and there are eighteen such blocks in kshack/nsalv.261, between them
# eleven lines setting RP04P/RP06P/RM03P/RM80P.  So a salvager tape grades the
# drive it was built for and no other.
#
# The tape this project has is the one the ITS build produces, and that build is
# an RP06 machine.  Pointed at an rm03 pack it gets as far as
#
#     Salvager 254
#     Active unit numbers? 0Format ran out of arguments.
#     *** ERROR *** THE SYSTEM MAY NOT BE BROUGHT UP
#
# which says nothing about the pack.  The control is that the SAME tape and the
# SAME harness accept an rp06 pack built the same way, minutes earlier: the only
# difference is the drive.
#
# So this stage is skipped for anything but an rp06, and skipped LOUDLY.  A
# grader that cannot read the format it is given is not a grader, and reporting
# its refusal as a failure of the pack would be reporting a lie.
if [ "$DRIVE" != "rp06" ]; then
	echo "  skip NSALV grades the drive it was ASSEMBLED for, and the tape here"
	echo "       is an rp06 build -- see the note in this script.  An $DRIVE"
	echo "       pack needs an $DRIVE salvager, which needs its own ITS build."
	echo
	echo "  skip DSKDMP, for the same reason"
	echo
	echo "an $DRIVE file system built from nothing, checked here but not yet"
	echo "graded by ITS -- itsfs check and the locked-out arithmetic only."
	exit $rc
fi

NSALV_PDP10=$PDP10 NSALV_TAPE=$SALV NSALV_IMAGE=$T/new.dsk NSALV_LOG=$T/nsalv.log \
	NSALV_DRIVE=$DRIVE expect "$EXP" > "$T/nsalv.exp.out" 2>&1

if grep -qE "need updating|Tracking down shared|unprotected|Wrong NUDSL|garbaged|Errors in directory" \
	"$T/nsalv.log" 2>/dev/null; then
	fail "NSALV reported problems:"
	grep -E "need updating|Tracking down shared|unprotected|Wrong NUDSL|garbaged|Errors in directory" \
		"$T/nsalv.log" | head -5 | sed 's/^/       /'
elif grep -q "Get user dirs from unit" "$T/nsalv.log" 2>/dev/null; then
	ok "NSALV: ACCEPTED IT -- salvaged, and had nothing to say"
else
	fail "NSALV did not get as far as salvaging -- see $T/nsalv.log"
fi

# It prints the pack ID out of the table header, which is our own text coming
# back through ITS's reading of a structure we invented from a source file.
if grep -q "ID is $ID" "$T/nsalv.log" 2>/dev/null; then
	ok "...and read the pack ID we wrote: $(grep -o "ID is .*" "$T/nsalv.log" | head -1)"
else
	fail "NSALV did not read back the pack ID"
fi

echo
echo "4. and to DSKDMP, a third implementation (also from tape)"

# The pack has no boot blocks -- see the header -- so DSKDMP comes off the tape
# and is then pointed at rp0.
cat > "$T/dd.ini" <<EOF
set console wru=034
set cpu its
set tim y2k
at tu2 $DSKDMP
set rp0 $DRIVE
at rp0 $T/new.dsk
EOF

expect -c "
set timeout 200
log_file -noappend $T/dskdmp.log
spawn $PDP10 $T/dd.ini
expect \"sim>\"
send \"b tu2\r\"
expect \"MTBOOT\"
sleep 2
send \"\033g\"
expect \"DSKDMP\"
sleep 2
send \"u\033SYS;\r\"
sleep 8
send \"\034\"
expect \"sim>\"
send \"q\r\"
expect eof
" > "$T/dskdmp.exp.out" 2>&1

if grep -q "HELLO *TXT" "$T/dskdmp.log" 2>/dev/null; then
	ok "DSKDMP listed a directory on a pack with no ITS in it"
else
	fail "DSKDMP did not list SYS -- see $T/dskdmp.log"
fi

sed -n 's/^ #[0-9]* \(.*\)$/\1/p' "$T/dskdmp.log" | sed 's/ *$//' > "$T/names"
n=$(wc -l < "$T/names" | tr -d ' ')
sort "$T/names" > "$T/names.sorted"

if [ "$n" -eq 2 ] && cmp -s "$T/names" "$T/names.sorted"; then
	ok "...both files, in order"
else
	fail "expected two entries in order, got $n"
	sed 's/^/       /' "$T/names"
fi

echo
if [ $rc -eq 0 ]; then
	echo "a file system with no ITS in it, accepted by ITS's salvager and read"
	echo "by its standalone loader.  Every word of it was written here."
else
	echo "logs are in $T"
fi

exit $rc
