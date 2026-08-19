#!/bin/sh
# itsload.sh -- ITS's own DUMP reads a tape THIS project wrote.
#
#   usage: sh tests/itsload.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]
#
# Run by `make itsload`.  Needs KLH10 built and about twenty minutes.
#
# THE OTHER DIRECTION FROM `make itsdump`.
#
# `itsdump` compares a tape ITS wrote with a tape we wrote: level 1, byte for
# byte, and with `ITS_SWITCHES="E LINKS"` that covers links as well.  But every
# grader there is a READER of ours agreeing with a WRITER of ours.  This asks
# the opposite question -- can ITS actually use what we produce? -- by writing a
# tape here, mounting it, and running DUMP's LOAD command.  If the entries come
# back onto the pack, ITS's own loader understood it.
#
# The two are not redundant.  A tape can be byte-identical and still be one ITS
# would refuse for a reason outside the bytes compared, and a tape ITS accepts
# can differ from what ITS writes.  Both were true here at different points
# today: LOAD accepted a seven-word link header for an hour before `cmp` showed
# that ITS writes eight.
#
# THE CONTROL IS ON THE TAPE.  It carries TWO entries -- an ordinary file and a
# link -- and both are removed from the pack first:
#
#   neither comes back   LOAD did not run, and the test says nothing about links
#   only the file        ITS reads our file headers and rejects our link
#   both                 ITS accepts both
#
# Without the file beside it a failure could not be told from a broken harness,
# which is the mistake this project has already made once and written up.
#
set -u

ITSFS=${1:?usage: itsload.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
IMAGE=${2:?usage: itsload.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
BIN=${3:?usage: itsload.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
ITS=${4:?usage: itsload.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
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


LOADERS=$ITS/build/klh10
EXP=$(cd "$(dirname "$0")" && pwd)/klh10.exp

# KMP is four entries -- three files and one link -- which makes it the cheapest
# directory on the pack that can answer the question.
DIR=${ITS_DIR:-KMP}
FILE=${ITS_FILE:-GOTO 12}
LINK=${ITS_LINK:-TS DUMPT}

pass=0
rc=0
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
no() { rc=1; printf '  FAIL %s\n' "$1"; }

[ -x "$ITSFS" ] || { echo "no itsfs at $ITSFS"; exit 1; }
[ -f "$IMAGE" ] || { echo "no image at $IMAGE"; exit 1; }
command -v expect >/dev/null 2>&1 || { echo "no expect(1) -- skipping"; exit 0; }
[ -x "$BIN/kn10-ks-its" ] || { echo "no kn10-ks-its in $BIN -- see tests/klh10.sh"; exit 1; }

for dp in dprpxx dpchaos dptm03; do
	[ -x "$BIN/$dp" ] || { echo "no $dp in $BIN"; exit 1; }
done

mkdir -p "$T/run"
cp "$BIN/dprpxx" "$BIN/dpchaos" "$BIN/dptm03" "$T/run/" || exit 1
for f in @.ddt-u dskdmp.216bin; do
	[ -f "$LOADERS/$f" ] || { echo "no $f in $LOADERS"; exit 1; }
	cp "$LOADERS/$f" "$T/run/" || exit 1
done

echo "ITS's own DUMP reads a tape this project wrote"
echo

echo "1. a tape, and a hole on the pack to load it into"

"$ITSFS" repack -p le64 -P dbd9 "$IMAGE" "$T/run/rp0.dsk" || { no "repack"; exit 1; }

if "$ITSFS" save -p dbd9 -d rp06 "$T/run/rp0.dsk" "$T/run/in.tap" \
	"$DIR;$FILE" "$DIR;$LINK" >/dev/null 2>&1; then
	ok "wrote a tape with a file and a link on it"
else
	no "itsfs save failed"
	exit 1
fi

# What it should look like when ITS gives it back.
"$ITSFS" get -w -p dbd9 -d rp06 "$T/run/rp0.dsk" "$DIR;$FILE" "$T/before.words" \
	>/dev/null 2>&1 || { no "could not read $DIR;$FILE"; exit 1; }

for e in "$FILE" "$LINK"; do
	"$ITSFS" del -p dbd9 -d rp06 "$T/run/rp0.dsk" "$DIR;$e" >/dev/null 2>&1 || {
		no "could not remove $DIR;$e"
		exit 1
	}
done
ok "removed both from the pack, so what comes back came off the tape"

"$ITSFS" del -p dbd9 -d rp06 "$T/run/rp0.dsk" 'SYS;ATSIGN DRAGON' >/dev/null 2>&1
n=$("$ITSFS" ls -p dbd9 -d rp06 "$T/run/rp0.dsk" "$DIR" 2>/dev/null | grep -ac "^[A-Z0-9.]")
ok "$DIR now holds $n entries"

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

echo
echo "2. ITS loads it"

# No TAPECREATE: the tape exists, and `create` refuses a file that is already
# there -- printed once, in passing, after which the machine runs with no tape.
( cd "$T/run" && KN10=$BIN/kn10-ks-its INI=dskdmp.ini MODE=load \
	TAPE=in.tap FILE="$DIR;" expect "$EXP" ) > "$T/load.log" 2>&1

if grep -aq "LOAD finished" "$T/load.log"; then
	ok "ITS booted, ran DUMP's LOAD, and returned to its prompt"
else
	no "the load did not finish (see $T/load.log)"
	exit 1
fi

# ITS echoes the volume header it read.  Ours says the date is unknown, and ITS
# renders that `______` -- which is the encoding `save` was changed to write
# after seeing what ITS's own DUMP puts there.
if grep -aq "CREATION DATE" "$T/load.log"; then
	ok "...having read our volume header: $(sed -n 's/.*\(TAPE NO.*\)/\1/p' "$T/load.log" | head -1)"
fi

echo
echo "3. what came back"

"$ITSFS" ls -p dbd9 -d rp06 "$T/run/rp0.dsk" "$DIR" > "$T/after" 2>&1

f1=$(echo "$FILE" | awk '{print $1}')
l1=$(echo "$LINK" | awk '{print $1}')

if grep -aq "^$f1" "$T/after"; then
	ok "the FILE is back: $DIR;$FILE"
else
	no "the file did not come back"
	sed 's/^/       /' "$T/after"
fi

# THE ONE THAT MATTERS.  It must be back AND still be a link.
if grep -a "^$l1" "$T/after" | grep -aq -- "->"; then
	ok "THE LINK IS BACK, AND STILL A LINK: $(grep -a "^$l1" "$T/after" | sed 's/  */ /g')"
elif grep -aq "^$l1" "$T/after"; then
	no "$l1 came back but not as a link"
else
	no "the link did not come back -- ITS did not accept what save wrote"
fi

if "$ITSFS" get -w -p dbd9 -d rp06 "$T/run/rp0.dsk" "$DIR;$FILE" "$T/after.words" \
	>/dev/null 2>&1 && cmp -s "$T/before.words" "$T/after.words"; then
	ok "...and the file's words are what they were before the round trip"
else
	no "the file came back changed"
fi

# A running ITS leaves miscounts behind, because the machine is HALTED and the
# allocation table it held in memory is never written back.  What must be zero
# is the two categories that mean data is at risk.
"$ITSFS" check -p dbd9 -d rp06 "$T/run/rp0.dsk" > "$T/check.out" 2>&1
d=$(sed -n 's/^disagreements *//p' "$T/check.out")

case "$d" in
"0 free but claimed, 0 in use but unclaimed"*)
	ok "no block is both free and claimed after ITS wrote to the pack"
	;;
"")
	ok "the pack checks clean after ITS wrote to it"
	;;
*)
	no "a dangerous disagreement: $d"
	;;
esac

echo
if [ "$rc" -eq 0 ]; then
	echo "$pass checks passed -- ITS's own loader accepts what itsfs save writes,"
	echo "link included"
else
	echo "FAILED"
fi
exit $rc
