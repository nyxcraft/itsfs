#!/bin/sh
# itsdump.sh -- have ITS's OWN DUMP write a tape, and compare ours with it.
#
#   usage: sh tests/itsdump.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]
#
# Run by `make itsdump`.  Needs KLH10 built and about twenty minutes.
#
# THIS IS THE ONLY LEVEL-1 EVIDENCE IN THE PROJECT.
#
# docs/validation.md grades evidence three ways, and the top one is "build
# something with itsfs, build the same thing with ITS, cmp".  For nine phases
# nothing reached it: `itstar` could read what this writes and this could read
# what `itstar` writes, but ITS's own DUMP had never been shown a tape and had
# never written one for comparison.  Everything rested on a third party's
# reader agreeing.
#
# So: boot ITS, run `:dump`, and let ITS write a save set of one directory to a
# tape KLH10 mounts.  Then write the same files with `itsfs save` and `cmp` the
# two files.  There is nothing to interpret in the result.
#
# WHAT IT FOUND, the first time it was run -- because "our tape is byte
# identical" was NOT the first answer:
#
#   THE FILE HEADER IS EIGHT WORDS, NOT SEVEN, and the eighth is the file's
#   length in words.  This wrote seven, on the strength of itstar's reader,
#   which accepts six or seven and never minded.  All 37 headers on the tape
#   ITS wrote are eight, and in every one the eighth word equals the count of
#   data words that follow.
#
#   UNREF IS COPIED WHOLE.  This masked off its low 18 bits, which threw away
#   UNAUTH -- and its.h has said all along that UNAUTH is "all ones = none", so
#   zeroing it does not say "no author", it says AUTHOR 0.  ITS's header holds
#   the pack's UNREF word unchanged.
#
#   AN UNKNOWN DATE IS ALL ONES.  ITS wrote 777777777777, SIXBIT `______`, from
#   a machine whose clock was unset.  This wrote zero, SIXBIT six spaces.  Both
#   read back as `__/__/__`, so nothing depended on it -- but one of the two is
#   what ITS does.
#
# None of the three was visible to any test that existed.  Each needed a tape
# ITS wrote, which is exactly what the roadmap said and why this target exists.
#
# AND A LINK GETS NO HEADER AT ALL, which is not what I expected to find when I
# went looking for its length.  ITS's DUMP omits links:
#
#     KSHACK   37 files, 3 links  ->  37 files, 0 links
#     KMP       3 files, 1 link   ->   3 files, 0 links
#
# (`ITS_DIR=KMP make itsdump` is the second of those, and is much the faster
# run -- four entries against forty.)  So what `itsfs save` writes for a link is
# an EXTENSION that itstar accepts, not a copy of anything ITS produced here.
#
# Note how that surfaced: the run PASSED both times.  The finding is in the
# numbers beside the verdict -- three files from a four-entry directory -- and
# not in the verdict.
#
set -u

ITSFS=${1:?usage: itsdump.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
IMAGE=${2:?usage: itsdump.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
BIN=${3:?usage: itsdump.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
ITS=${4:?usage: itsdump.sh <itsfs> <image> <klh10 bindir> <its tree> [scratch]}
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
DIR=${ITS_DIR:-KSHACK}
SWITCHES=${ITS_SWITCHES:-E}

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

echo "ITS's own DUMP writes a tape, and ours is compared with it"
echo

echo "1. a pack for ITS to run on"
"$ITSFS" repack -p le64 -P dbd9 "$IMAGE" "$T/run/rp0.dsk" || { no "repack"; exit 1; }
ok "repacked into dbd9, which is what KLH10's ITS config asks for"

if "$ITSFS" del -p dbd9 -d rp06 "$T/run/rp0.dsk" 'SYS;ATSIGN DRAGON' >/dev/null 2>&1; then
	ok "removed SYS;ATSIGN DRAGON so the console can be had"
else
	no "could not remove SYS;ATSIGN DRAGON"
fi

# The tape is mounted from the KLH10 monitor AFTER the ini has been read, not in
# it: a `devmo` in the command file stalls the rest of the file, because the
# mount is asynchronous and the reply comes back through the device process.
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
echo "2. ITS writes the tape"

( cd "$T/run" && KN10=$BIN/kn10-ks-its INI=dskdmp.ini MODE=dump \
	TAPE=out.tap TAPECREATE=1 SWITCHES="$SWITCHES" FILE="$DIR;" \
	expect "$EXP" ) > "$T/dump.log" 2>&1

if grep -aq "DUMP finished" "$T/dump.log"; then
	ok "ITS booted, ran :dump, and returned to its prompt"
else
	no "the dump did not finish (see $T/dump.log)"
	exit 1
fi

if [ -s "$T/run/out.tap" ]; then
	ok "...leaving $(wc -c < "$T/run/out.tap" | tr -d ' ') bytes of tape"
else
	no "the tape is empty"
	exit 1
fi

echo
echo "3. what is on it"

"$ITSFS" saveset "$T/run/out.tap" > "$T/mine.list" 2>&1
n=$(sed -n 's/^\([0-9]*\) files.*/\1/p' "$T/mine.list")

if [ -n "$n" ] && [ "$n" -gt 0 ]; then
	ok "itsfs saveset reads a tape ITS wrote: $n files"
else
	no "could not read the tape ITS wrote"
	sed 's/^/       /' "$T/mine.list" | head -5
	exit 1
fi

# The files must match the pack they came off, word for word.  This is a
# different question from the byte comparison below: it asks whether OUR READER
# of the pack agrees with ITS'S READER of the same pack.
# A LINK IS NOT A FILE, and this list is used two ways below -- to extract and
# compare contents, which only files have, and to hand back to `save`, which
# takes both.  `saveset` prints a link as `DIR;FN1 FN2 -> target`, so the arrow
# separates them.  Getting this wrong is what made the first LINKS run report
# "1 of 4 files differ": the link was being compared as though it had data.
sed -n '2,$p' "$T/mine.list" | grep -av "^[0-9]* file" > "$T/entries"
grep -av -- "->" "$T/entries" > "$T/names"
sed 's/ ->.*//' "$T/entries" > "$T/allnames"
rm -rf "$T/x" && mkdir -p "$T/x"
"$ITSFS" saveset -x "$T/x" "$T/run/out.tap" >/dev/null 2>&1

same=0
bad=0
while read -r f; do
	fn1=$(echo "$f" | sed 's/^[^;]*;//' | awk '{print $1}')
	fn2=$(echo "$f" | sed 's/^[^;]*;//' | awk '{print $2}')
	d=$(echo "$f" | sed 's/;.*//')
	tf="$T/x/$d;$fn1.$fn2"
	[ -f "$tf" ] || { bad=$((bad + 1)); continue; }
	"$ITSFS" get -w -p dbd9 -d rp06 "$T/run/rp0.dsk" "$d;$fn1 $fn2" "$T/one" \
		>/dev/null 2>&1 || { bad=$((bad + 1)); continue; }
	if cmp -s "$tf" "$T/one"; then same=$((same + 1)); else bad=$((bad + 1)); fi
done < "$T/names"

if [ "$same" -gt 0 ] && [ "$bad" -eq 0 ]; then
	nl=$(grep -ac -- "->" "$T/entries" || true)
	ok "all $same files on it are byte-identical to the same files off the pack$(
		[ "$nl" -gt 0 ] && echo " (and $nl link(s), which have no data to compare)")"
else
	no "$bad of $((same + bad)) files differ from the pack"
fi

echo
echo "4. and ours, beside it"

# EVERY entry, in the order ITS dumped them, written by this project -- links
# included, since they are on the tape being compared against.
set --
while read -r f; do set -- "$@" "$f"; done < "$T/allnames"
"$ITSFS" save -p dbd9 -d rp06 "$T/run/rp0.dsk" "$T/ours.tap" "$@" >/dev/null 2>&1 || {
	no "itsfs save failed"
	exit 1
}

a=$(wc -c < "$T/run/out.tap" | tr -d ' ')
b=$(wc -c < "$T/ours.tap" | tr -d ' ')

if [ "$a" = "$b" ]; then
	ok "the same size, to the byte: $a"
else
	no "different sizes: ITS $a, ours $b"
fi

if cmp "$T/run/out.tap" "$T/ours.tap" > "$T/cmp.out" 2>&1; then
	ok "IDENTICAL: every byte of a tape ITS's own DUMP wrote"
else
	no "they differ: $(head -1 "$T/cmp.out")"
fi

echo
if [ "$rc" -eq 0 ]; then
	echo "$pass checks passed -- level 1: built by ITS, built by itsfs, cmp"
else
	echo "FAILED"
fi
exit $rc
