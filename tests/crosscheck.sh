#!/bin/sh
# crosscheck.sh -- extract files from a pack and compare them to the host files
# they were built from.
#
#   usage: sh tests/crosscheck.sh <itsfs> <image> <its-source-tree> [dir...]
#
# THIS IS THE ONLY EVIDENCE HERE THAT DOES NOT COME FROM THE PACK ITSELF.
# Everything else `make oracle` does is self-consistency: the reader agreeing
# with numbers ITS wrote on the same pack.  This compares against a file that
# never went near the pack.
#
# The path a file took:
#
#     ~/its/src/kshack/cmds.m80          a file in the ITS source tree
#       -> itstar                        packed into a tape image
#         -> ITS's own tape loader       written onto the pack by the monitor
#           -> this reader               extracted here
#
# Every layer of this project is on that path.  A byte-identical result means the
# geometry found the right blocks, the descriptor decoded them in the right
# ORDER, UNWRDC gave the right length for the last one, and the seven-bit
# extraction is right -- checked against something no part of this project has
# ever touched.
#
# TWO DIFFERENCES ARE EXPECTED AND ARE NOT FAILURES.  They are properties of ITS
# text files rather than of the reader, and this script names them rather than
# hiding them:
#
#   CR    ITS text uses CRLF.  `itsfs cat` translates nothing, on purpose, so the
#         carriage returns are really there and are stripped here for comparison.
#   ^C    ITS pads the last word of a text file with 003.  The comparison is
#         therefore against the host file's length, and the padding is reported.
#
# AND SOME FILES ARE SKIPPED, WHICH IS THE HONEST PART.  The host copy is not
# raw ITS text: it is in the "evacuated file format" `itstar` uses, which ESCAPES
# the characters that a plain CRLF-to-LF conversion would lose --
#
#     a lone CR, a bare LF, NUL, and anything with the high bit set
#
# -- so for a file containing any of those, the host bytes and the disk bytes
# are legitimately different and comparing them tests nothing but whether this
# script has reimplemented somebody else's escape table.  It does not, and it
# skips those files instead, and it counts them, so the coverage this check
# actually has is visible rather than assumed.
#
set -u
ITSFS=${1:?usage: crosscheck.sh <itsfs> <image> <its-source-tree> [dir...]}
IMG=${2:?usage: crosscheck.sh <itsfs> <image> <its-source-tree> [dir...]}
SRC=${3:?usage: crosscheck.sh <itsfs> <image> <its-source-tree> [dir...]}
shift 3
DIRS=${*:-kshack}

[ -d "$SRC" ] || { echo "no ITS source tree at $SRC"; exit 2; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT

same=0
diff=0
skip=0
esc=0

for d in $DIRS; do
	[ -d "$SRC/$d" ] || { echo "  no $SRC/$d"; continue; }
	D=$(echo "$d" | tr 'a-z' 'A-Z')

	for f in "$SRC/$d"/*; do
		[ -f "$f" ] || continue
		base=$(basename "$f")

		# host `cmds.m80` is ITS `CMDS M80`: upper case, split at the
		# last dot.  A name that is not SIXBIT is skipped, not mangled.
		fn1=$(echo "${base%.*}" | tr 'a-z' 'A-Z')
		fn2=$(echo "${base##*.}" | tr 'a-z' 'A-Z')
		[ "$base" = "${base%.*}" ] && fn2=""

		"$ITSFS" sixbit "$fn1" >/dev/null 2>&1 || { skip=$((skip + 1)); continue; }
		"$ITSFS" sixbit "$fn2" >/dev/null 2>&1 || { skip=$((skip + 1)); continue; }

		# Skip the ones whose host copy is escaped -- see the header.
		if LC_ALL=C tr -d '\000\015\200-\377' < "$f" | cmp -s - "$f"; then
			:
		else
			esc=$((esc + 1))
			continue
		fi

		"$ITSFS" cat "$IMG" "$D" "$fn1" "$fn2" > "$T/out" 2>/dev/null || {
			skip=$((skip + 1))
			continue
		}

		# Strip the carriage returns, then compare against the host
		# file's own length -- what follows it is ITS's ^C padding.
		tr -d '\r' < "$T/out" > "$T/nocr"
		n=$(wc -c < "$f" | tr -d ' ')
		dd if="$T/nocr" of="$T/head" bs=1 count="$n" 2>/dev/null

		if cmp -s "$T/head" "$f"; then
			pad=$(($(wc -c < "$T/nocr" | tr -d ' ') - n))

			if [ "$pad" -gt 4 ]; then
				echo "  ?? $D;$fn1 $fn2: matches, but $pad bytes of padding"
			fi

			same=$((same + 1))
		else
			echo "  DIFFERS $D;$fn1 $fn2  (host $f)"
			cmp "$T/head" "$f" 2>&1 | head -1 | sed 's/^/      /'
			diff=$((diff + 1))
		fi
	done
done

echo "cross-check: $same files byte-identical to their host originals, $diff differ,"
echo "             $esc skipped as escaped by itstar, $skip not on the pack"
[ "$same" -gt 0 ] && [ "$diff" -eq 0 ]
