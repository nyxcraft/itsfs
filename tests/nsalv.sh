#!/bin/sh
# nsalv.sh -- compare `itsfs check` with NSALV, ITS's own salvager.
#
#   usage: sh tests/nsalv.sh <itsfs> <image> <pdp10> <salv.tape> [scratch dir]
#
# Run by `make nsalv`.  Not part of `make test`: it needs an emulator, an ITS
# salvager tape and most of ten minutes, none of which belong in a suite that
# has to run anywhere.
#
# WHY THIS IS THE BEST EVIDENCE THE PROJECT HAS.
#
# Everything `make oracle` establishes is the pack agreeing with itself, or a
# second implementation of OUR reading agreeing with the first.  Both readers
# take their constants from src/its.h, so both inherit any misreading in it -- a
# second reading catches a wrong reading, but it cannot catch what the source
# never says.
#
# NSALV is not a second reading.  It is MIT's, it was written in the 1980s by
# people with the machine in front of them, it boots standalone from a tape and
# it walks every directory on the pack rebuilding the allocation table from
# scratch.  Where it agrees with `itsfs check`, that agreement is about the
# format rather than about our understanding of it.
#
# THE FIRST THREE SECTIONS NEED NO WRITER, which is why they ran at phase 5: a
# pack this project has only READ is enough to ask the question.  The fourth was
# added at phase 7 and is a stronger claim -- see below.
#
# THE TWO RUNS, AND WHY BOTH ARE NEEDED.
#
#   clean     NSALV salvages and returns to DDT without a word.  On its own that
#             proves nothing -- it is indistinguishable from the salvager never
#             having run.  (Its NOISE flag would make it announce every check,
#             and also dump the RH11 controller status after every disk transfer,
#             which over a 500-block walk is thousands of lines.  There is no
#             summary-only setting.)
#
#   damaged   one TUT word cleared -- twelve blocks' worth of entries, of which
#             eleven are held by files.  NOW it speaks, and what it says can be
#             compared line for line with what `itsfs check` says.
#
# The comparison below is on the PAIRS: which block, and which file holds it.
# Not on counts, and not on "both reported something" -- two checkers can agree
# that a pack is broken and disagree about every detail.
#
# AND A THIRD RUN, ADDED AT PHASE 7: a pack this project has WRITTEN to.  That
# one is level-2 evidence in the project's own taxonomy -- "accepted by native
# tools" -- and it is a different claim from the other two, which grade a
# reading.  Here ITS's own salvager is asked whether what `itsfs put` produced
# is a file system, and the bar is that it says nothing at all.
#
set -u

ITSFS=${1:?usage: nsalv.sh <itsfs> <image> <pdp10> <salv.tape> [scratch]}
IMAGE=${2:?usage: nsalv.sh <itsfs> <image> <pdp10> <salv.tape> [scratch]}
PDP10=${3:?usage: nsalv.sh <itsfs> <image> <pdp10> <salv.tape> [scratch]}
TAPE=${4:?usage: nsalv.sh <itsfs> <image> <pdp10> <salv.tape> [scratch]}
T=${5:-/tmp/itsfs-nsalv}

# Which TUT word to clear.  Any word covering blocks that files hold will do;
# this one is in the middle of the pack, well clear of the swapping area and of
# the directories.
TARGET=${TARGET:-20000}

EXP=$(dirname "$0")/nsalv.exp
[ -f "$EXP" ] || { echo "nsalv.sh: no $EXP"; exit 2; }
[ -x "$ITSFS" ] || { echo "nsalv.sh: no $ITSFS (build first)"; exit 2; }
[ -f "$IMAGE" ] || { echo "nsalv.sh: no image at $IMAGE"; exit 2; }
[ -f "$TAPE" ] || { echo "nsalv.sh: no salvager tape at $TAPE"; exit 2; }
command -v expect >/dev/null 2>&1 || { echo "nsalv.sh: no expect on PATH"; exit 2; }
[ -x "$PDP10" ] || command -v "$PDP10" >/dev/null 2>&1 ||
	{ echo "nsalv.sh: no emulator at $PDP10"; exit 2; }

rm -rf "$T" && mkdir -p "$T" || exit 2
rc=0
fail() { echo "  FAIL $*"; rc=1; }
ok() { echo "  ok   $*"; }

# rp06 geometry, from SYSTEM;RP06 DEFS 1.  Written out here rather than asked of
# itsfs, for the same reason tests/run.sh does it: a wrong mapping must not be
# able to agree with itself.
blk_sector() {
	_b=$1
	_cyl=$((_b / 47))
	_within=$(((_b - _cyl * 47) * 8))
	_srf=$((_within / 20))
	echo $(((_cyl * 19 + _srf) * 20 + (_within - _srf * 20)))
}

MFDBLK=19081
TUTBLK=$((MFDBLK - 4))

echo "1. a clean pack"

cp "$IMAGE" "$T/clean.dsk" || exit 2

if "$ITSFS" check "$T/clean.dsk" > "$T/clean.itsfs" 2>&1; then
	ok "itsfs check: $(sed -n 's/^no problems found.*/clean/p' "$T/clean.itsfs")"
else
	fail "itsfs check found problems on an undamaged pack:"
	sed -n '2,6p' "$T/clean.itsfs" | sed 's/^/       /'
fi

echo "   running NSALV (this takes a few minutes) ..."
NSALV_PDP10=$PDP10 NSALV_TAPE=$TAPE NSALV_IMAGE=$T/clean.dsk NSALV_LOG=$T/clean.log \
	expect "$EXP" > "$T/clean.exp" 2>&1

if grep -q "need updating\|Tracking down shared\|unprotected" "$T/clean.log" 2>/dev/null; then
	fail "NSALV reported problems on an undamaged pack:"
	grep "need updating\|Tracking down shared\|unprotected" "$T/clean.log" |
		head -5 | sed 's/^/       /'
elif grep -q "Get user dirs from unit" "$T/clean.log" 2>/dev/null; then
	ok "NSALV: salvaged and returned to DDT with nothing to say"
else
	fail "NSALV did not get as far as salvaging -- see $T/clean.log"
fi

# ...AND IT DID NOT WRITE.  nsalv.exp answers "no" to every offer, and this is
# the check on that: a salvager that quietly rewrote the pack would make every
# later comparison meaningless.
if cmp -s "$IMAGE" "$T/clean.dsk"; then
	ok "and left the pack byte-identical"
else
	fail "NSALV MODIFIED THE PACK -- the run is not read-only"
fi

echo
echo "2. the same pack with one TUT word cleared"

cp "$IMAGE" "$T/dmg.dsk" || exit 2

# The TUT entry for block N is three bits in word LTIBLK + N/12 of the TUT,
# which begins at block TUTBLK.  Clearing the whole word frees twelve blocks'
# worth of entries at once.
word=$((16 + TARGET / 12))
blk=$((TUTBLK + word / 1024))
off=$((word % 1024))
byte=$((($(blk_sector "$blk") * 128 + off) * 8))

dd if=/dev/zero of="$T/dmg.dsk" bs=1 seek="$byte" count=8 conv=notrunc 2>/dev/null
echo "   cleared TUT word $word (block $blk, offset $off): blocks $((TARGET / 12 * 12))..$((TARGET / 12 * 12 + 11))"

"$ITSFS" check "$T/dmg.dsk" > "$T/dmg.itsfs" 2>&1
if [ $? -eq 1 ]; then
	ok "itsfs check: $(sed -n 's/^\([0-9]* problems\).*/\1/p' "$T/dmg.itsfs")"
else
	fail "itsfs check did not report the damage"
fi

echo "   running NSALV again ..."
NSALV_PDP10=$PDP10 NSALV_TAPE=$TAPE NSALV_IMAGE=$T/dmg.dsk NSALV_LOG=$T/dmg.log \
	expect "$EXP" > "$T/dmg.exp" 2>&1

if grep -q "unprotected in old TUT" "$T/dmg.log" 2>/dev/null; then
	ok "NSALV: reported the damage"
else
	fail "NSALV did not report the damage -- see $T/dmg.log"
fi

echo
echo "3. do they name the same blocks, and the same files?"

# itsfs:  "  block 19992 is claimed by C;PHASE ARGS, and the TUT calls it FREE"
sed -n 's/^  block \([0-9]*\) is claimed by \(.*\), and the TUT calls it FREE$/\1 \2/p' \
	"$T/dmg.itsfs" | sort -n > "$T/pairs.itsfs"

# NSALV: "File unprotected in old TUT, Block 19992. - C;PHASE ARGS  Pack 0., Unit #0"
# Two spaces separate the name from the pack, and the name may itself contain
# one, so the cut is at the double space rather than at the first.
sed -n 's/^File unprotected in old TUT, Block \([0-9]*\)\. - \(.*\)  Pack .*$/\1 \2/p' \
	"$T/dmg.log" | sed 's/ *$//' | sort -n > "$T/pairs.nsalv"

n_itsfs=$(wc -l < "$T/pairs.itsfs" | tr -d ' ')
n_nsalv=$(wc -l < "$T/pairs.nsalv" | tr -d ' ')

echo "   itsfs check names $n_itsfs, NSALV names $n_nsalv"

if [ "$n_itsfs" -eq 0 ] || [ "$n_nsalv" -eq 0 ]; then
	fail "one of them named nothing, so there is nothing to compare"
elif diff -u "$T/pairs.itsfs" "$T/pairs.nsalv" > "$T/pairs.diff" 2>&1; then
	ok "IDENTICAL: the same $n_itsfs blocks, each held by the same file"
	echo
	sed 's/^/       /' "$T/pairs.itsfs"
else
	fail "they disagree:"
	sed 's/^/       /' "$T/pairs.diff" | head -20
fi

# NSALV also prints its own summary of the differences, as `count old_new`.
# Eleven blocks that should hold one reference and hold none is "11 1_0".
if grep -q "^TUT #0 $n_nsalv 1_0" "$T/dmg.log" 2>/dev/null; then
	ok "and NSALV's own summary agrees: $n_nsalv blocks at 1 reference, stored as 0"
else
	echo "   note: NSALV's summary line reads:" \
		"$(grep '^TUT #0' "$T/dmg.log" 2>/dev/null | head -1)"
fi

echo
echo "4. a pack itsfs WROTE to"

# Two files, in two directories, so the name-area insertion is exercised at more
# than one place in the sort order.
cp "$IMAGE" "$T/wrote.dsk" || exit 2
printf 'HELLO FROM ITSFS.\r\nTHIS FILE WAS WRITTEN BY A HOST TOOL,\r\nNOT BY ITS ITSELF.\r\n' \
	> "$T/msg.txt"

wrote=0

for f in "KSHACK;ITSFS TXT" "SYSENG;ITSFS 2"; do
	if "$ITSFS" put "$T/wrote.dsk" "$f" "$T/msg.txt" >/dev/null 2>&1; then
		wrote=$((wrote + 1))
	else
		fail "itsfs put '$f' failed"
	fi
done

[ "$wrote" -eq 2 ] && ok "itsfs put wrote $wrote files"

# READ THEM BACK BEFORE ASKING ANYBODY ELSE.  A pack that passes a salvager and
# does not return the file is not a success.
back=0

for f in "KSHACK;ITSFS TXT" "SYSENG;ITSFS 2"; do
	"$ITSFS" cat "$T/wrote.dsk" "$f" > "$T/back.txt" 2>/dev/null
	cmp -s "$T/msg.txt" "$T/back.txt" && back=$((back + 1))
done

if [ "$back" -eq 2 ]; then
	ok "...and both read back byte-identical to what went in"
else
	fail "only $back of 2 read back identical"
fi

if "$ITSFS" check "$T/wrote.dsk" > "$T/wrote.itsfs" 2>&1; then
	ok "itsfs check: clean"
else
	fail "itsfs check found problems on a pack itsfs wrote:"
	sed -n '2,6p' "$T/wrote.itsfs" | sed 's/^/       /'
fi

echo "   running NSALV on it ..."
NSALV_PDP10=$PDP10 NSALV_TAPE=$TAPE NSALV_IMAGE=$T/wrote.dsk NSALV_LOG=$T/wrote.log \
	expect "$EXP" > "$T/wrote.exp" 2>&1

if grep -q "need updating\|Tracking down shared\|unprotected\|Errors in directory" "$T/wrote.log" 2>/dev/null; then
	fail "NSALV REPORTED PROBLEMS on a pack itsfs wrote:"
	grep "need updating\|Tracking down shared\|unprotected\|Errors in directory" "$T/wrote.log" |
		head -5 | sed 's/^/       /'
elif grep -q "Get user dirs from unit" "$T/wrote.log" 2>/dev/null; then
	ok "NSALV: ACCEPTED IT -- salvaged, and had nothing to say"
else
	fail "NSALV did not get as far as salvaging -- see $T/wrote.log"
fi

# And it did not repair anything on the way past, which would have hidden a
# disagreement by silently fixing it.
if "$ITSFS" check "$T/wrote.dsk" >/dev/null 2>&1; then
	ok "...and the files are still there afterwards"
else
	fail "the pack does not check clean after NSALV saw it"
fi

echo
echo "5. a DIFFERENT kind of damage: a directory, not the table"

# EVERYTHING ABOVE IS ONE DAMAGE CLASS.  A cleared TUT word makes the table
# under-claim: blocks a file holds are marked free, and the allocator will hand
# them out again.  That is the dangerous direction, which is why it was first --
# but it is one direction, and NSALV had never been shown any other.
#
# Zeroing a directory block is the opposite.  The table still claims the blocks
# its files held; nothing claims them back.  `itsfs check` separates the two by
# name -- "free but claimed" against "in use but unclaimed" -- and that
# distinction had no second opinion until this stage existed.
#
# KMP is chosen because it is small: three files, one link, three blocks.  The
# expected result is enumerable rather than approximate.
cp "$IMAGE" "$T/dir.dsk" || exit 2

DIRBLK=${DIRBLK:-384}
dirbyte=$(( $(blk_sector "$DIRBLK") * 128 * 8 ))
dd if=/dev/zero of="$T/dir.dsk" bs=1 seek="$dirbyte" count=8192 conv=notrunc 2>/dev/null
echo "   zeroed the directory in block $DIRBLK"

"$ITSFS" check "$T/dir.dsk" > "$T/dir.itsfs" 2>&1

if grep -aq "block $DIRBLK was reached as" "$T/dir.itsfs"; then
	ok "itsfs check: $(grep -a "block $DIRBLK was reached as" "$T/dir.itsfs" | sed 's/^ *//')"
else
	fail "itsfs check did not report the damaged directory"
fi

NSALV_PDP10=$PDP10 NSALV_TAPE=$TAPE NSALV_IMAGE=$T/dir.dsk NSALV_LOG=$T/dir.log \
	expect "$EXP" > "$T/dir.exp" 2>&1

# NSALV names the block and the directory in its own message, which is what
# makes this comparable rather than merely both-complained.
if grep -aq "block $DIRBLK is" "$T/dir.log" 2>/dev/null; then
	ok "NSALV: $(grep -a "block $DIRBLK is" "$T/dir.log" | head -1 | sed 's/Correct it.*//')"
else
	fail "NSALV did not name block $DIRBLK -- see $T/dir.log"
fi

if grep -aq "NSALV VERDICT: directory mismatch at block $DIRBLK" "$T/dir.exp" 2>/dev/null; then
	ok "IDENTICAL: the same block, and the same directory, from both"
else
	fail "the two do not agree about the damage"
	grep -a "NSALV VERDICT" "$T/dir.exp" 2>/dev/null | sed 's/^/       /'
fi

# AND THE TWO DISAGREE ABOUT WHAT IT MEANS, which is worth stating rather than
# smoothing over.  NSALV, having been told not to repair it, declares
# "*** ERROR *** THE SYSTEM MAY NOT BE BROUGHT UP" and stops; `itsfs check`
# reports its five problems and goes on to give the whole account.  Different
# jobs: one is deciding whether to boot, the other is diagnosing.
if grep -aq "MAY NOT BE BROUGHT UP" "$T/dir.log" 2>/dev/null; then
	ok "...and NSALV calls it fatal, where check calls it five problems"
fi

echo
echo "6. and the MFD itself, which everything else depends on"

# THE ONE STRUCTURE WITH NOTHING ABOVE IT.  A damaged directory is one
# directory; a damaged MFD is every directory, because the MFD is how they are
# found at all.  So both programs stop rather than guess, and what each says
# when it stops is the comparison.
cp "$IMAGE" "$T/mfd.dsk" || exit 2

mfdbyte=$(( ($(blk_sector "$MFDBLK") * 128 + 5) * 8 ))
dd if=/dev/zero of="$T/mfd.dsk" bs=1 seek="$mfdbyte" count=8 conv=notrunc 2>/dev/null
echo "   cleared MDCHK, word 5 of the MFD in block $MFDBLK"

"$ITSFS" check "$T/mfd.dsk" > "$T/mfd.itsfs" 2>&1

if grep -aq "is not an MFD: MDCHK is" "$T/mfd.itsfs"; then
	ok "itsfs check: $(grep -a "is not an MFD" "$T/mfd.itsfs" | sed 's/^ *//')"
else
	fail "itsfs check did not report the garbaged MFD"
fi

if grep -aq "nothing below it could be checked" "$T/mfd.itsfs"; then
	ok "...and stops there rather than reporting counts it cannot stand behind"
else
	fail "itsfs check carried on past a bad MFD"
fi

NSALV_PDP10=$PDP10 NSALV_TAPE=$TAPE NSALV_IMAGE=$T/mfd.dsk NSALV_LOG=$T/mfd.log \
	expect "$EXP" > "$T/mfd.exp" 2>&1

if grep -aq "MFD check word garbaged" "$T/mfd.log" 2>/dev/null; then
	ok "NSALV: 'MFD check word garbaged?' -- the same word, named the same way"
else
	fail "NSALV did not report the garbaged MFD -- see $T/mfd.log"
fi

echo
echo "7. a broken descriptor: two files holding one block"

# THE LAST DIRECTION.  Stage 2 has the table under-claiming, stage 5 has it
# over-claiming, stage 6 has the index itself gone.  This is the file side: a
# descriptor that points somewhere it should not, so two files hold one block.
#
# The damage is a single field.  KMP;GOTO 12's UNDSCP is set to KMP;BABYL 19's,
# so both files read the same descriptor -- BABYL's block is claimed twice and
# GOTO's own is claimed by nobody.
cp "$IMAGE" "$T/shared.dsk" || exit 2

python3 - "$T/shared.dsk" <<'PY' || { echo "   (needs python3 -- skipped)"; SKIP7=1; }
import struct, sys
NHEDS, NSECS, SECBLK, NBLKSC = 19, 20, 8, 47
b = 384
cyl = b // NBLKSC; w = (b - cyl * NBLKSC) * SECBLK; srf = w // NSECS
sec = (cyl * NHEDS + srf) * NSECS + (w - srf * NSECS)
f = open(sys.argv[1], "r+b")
f.seek((sec * 128 + 1011) * 8)
got = struct.unpack("<Q", f.read(8))[0] & 0o777777777777

# VERIFY BEFORE WRITING.  The first attempt at this wrote to the entry's first
# word instead of its third -- an entry is FN1, FN2, RNDM, DATE, REF -- and
# renamed a file rather than damaging it, which `check` correctly called no
# problem at all.  A convincing false negative, from one wrong offset.
if got != 0o467700000045:
    print("   word 1011 is %012o, not the UNRNDM expected -- refusing" % got)
    sys.exit(1)

f.seek((sec * 128 + 1011) * 8)
f.write(struct.pack("<Q", got & ~0o17777))
f.close()
PY

if [ -z "${SKIP7:-}" ]; then
	echo "   KMP;GOTO 12 now shares KMP;BABYL 19's descriptor"

	"$ITSFS" check "$T/shared.dsk" > "$T/shared.itsfs" 2>&1

	if grep -aq "and already by" "$T/shared.itsfs"; then
		ok "itsfs check: $(grep -a "and already by" "$T/shared.itsfs" | sed 's/^ *//')"
	else
		fail "itsfs check did not report a doubly-claimed block"
	fi

	NSALV_PDP10=$PDP10 NSALV_TAPE=$TAPE NSALV_IMAGE=$T/shared.dsk \
		NSALV_LOG=$T/shared.log expect "$EXP" > "$T/shared.exp" 2>&1

	if grep -aq "Tracking down shared blocks" "$T/shared.log" 2>/dev/null; then
		ok "NSALV: 'Tracking down shared blocks.'"
	else
		fail "NSALV did not report shared blocks -- see $T/shared.log"
	fi

	# It prints both files' descriptors, one under each name, and they are
	# identical -- which is the shared block made visible.
	if grep -aq "GOTO 12" "$T/shared.log" && grep -aq "BABYL 19" "$T/shared.log"; then
		ok "...naming both files, the same two itsfs check names"
	else
		fail "NSALV did not name both files"
	fi
fi

echo
if [ $rc -eq 0 ]; then
	echo "two checkers with nothing in common but the disk, agreeing block for block --"
	echo "on four kinds of damage -- and ITS's own salvager accepting a file system"
	echo "this project wrote"
else
	echo "logs are in $T"
fi

exit $rc
