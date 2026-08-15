#!/bin/sh
# accounting.sh -- the strongest thing a read-only reader can prove about a real
# pack: that the space adds up, three ways, against numbers it did not compute.
#
#   usage: sh tests/accounting.sh <itsfs> <image>
#
# Run by `make oracle`.  It is not part of `make test`, because it needs a real
# ITS pack and this repository has none.
#
# The three agreements, and why each is worth something on its own:
#
#   1. per directory, the blocks the DESCRIPTORS decode to == UDBLKS
#      UDBLKS is a running total ITS maintains in the UFD header as files are
#      written.  Nothing here computes it and nothing here can influence it, so
#      it is an independent second opinion on the descriptor decoder -- for every
#      directory on the pack, one comparison each.
#
#   2. across the pack, the same sum == the blocks the TUT calls in use
#      The TUT is a different structure, in a different place, maintained by a
#      different part of the monitor.  If the descriptor decoder skipped or
#      double-counted a block anywhere on the pack, this is where it shows.
#
#   3. the blocks the TUT calls LOCKED OUT == the directories and the tables
#      NUDSL user directories, one MFD and NTUTBL TUT blocks.  Getting this
#      exactly right requires the MFD-slot-to-block arithmetic to be right, which
#      is the part of the format with no pointer to check it against.
#
set -u
ITSFS=${1:?usage: accounting.sh <itsfs> <image>}
IMG=${2:?usage: accounting.sh <itsfs> <image>}
rc=0

dirs=$("$ITSFS" dirs "$IMG" | grep -v '^#') || exit 1

total=0
mismatch=0
n=0

for d in $dirs; do
	line=$("$ITSFS" ls -l "$IMG" "$d" | tail -1)
	b=$(echo "$line" | sed -n 's/.*entries, \([0-9]*\) blocks.*/\1/p')
	u=$(echo "$line" | sed -n 's/.*UDBLKS says \([0-9]*\).*/\1/p')

	[ -n "$b" ] && [ -n "$u" ] || { echo "  ?? $d: cannot read its totals"; rc=1; continue; }

	n=$((n + 1))
	total=$((total + b))

	[ "$b" = "$u" ] || {
		echo "  MISMATCH $d: descriptors say $b blocks, UDBLKS says $u"
		mismatch=$((mismatch + 1))
	}
done

echo "1. $n directories walked, $((n - mismatch)) whose descriptors agree with their own UDBLKS"
[ "$mismatch" -eq 0 ] || rc=1

free=$("$ITSFS" free "$IMG")
inuse=$(echo "$free" | sed -n 's/^in use  *\([0-9]*\) blocks.*/\1/p')
locked=$(echo "$free" | sed -n 's/^locked out  *\([0-9]*\) blocks.*/\1/p')

if [ "$total" = "$inuse" ]; then
	echo "2. $total blocks described by files == $inuse blocks the TUT calls in use"
else
	echo "2. MISMATCH: files describe $total blocks, the TUT calls $inuse in use"
	rc=1
fi

# The MFD says how many directory slots the monitor was built for, and the drive
# says how many blocks its TUT occupies.  Locked out should be exactly those,
# plus the MFD itself.
nudsl=$("$ITSFS" info "$IMG" | sed -n 's/.*room for \([0-9]*\).*/\1/p')
ntut=$("$ITSFS" info "$IMG" | sed -n 's/^TUT  *blocks \([0-9]*\)\.\.\([0-9]*\)/\2 \1/p' |
	awk '{print $1 - $2 + 1}')
want=$((nudsl + 1 + ntut))

if [ "$locked" = "$want" ]; then
	echo "3. $locked blocks locked out == $nudsl directories + 1 MFD + $ntut TUT blocks"
else
	echo "3. MISMATCH: $locked blocks locked out, expected $want"
	echo "   ($nudsl directories + 1 MFD + $ntut TUT blocks)"
	rc=1
fi

[ "$rc" -eq 0 ] && echo "the space on this pack is fully accounted for"
exit $rc
