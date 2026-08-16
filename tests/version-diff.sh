#!/bin/sh
# version-diff.sh -- how far does this reader's transcription reach?
#
#   usage: sh tests/version-diff.sh [its-source-tree]
#
# Run by hand, and by `make version-diff`.  Not part of `make test`: it needs the
# ITS source tree, including its git history, which is not in this repo.
#
# THE QUESTION IT ANSWERS.  src/its.h is transcribed from ONE file, SYSTEM;FSDEFS
# 43.  ITS ran from 1967 to 1990 and that file carries two dated format changes
# in its own comments, so "which ITS releases does this reader cover?" is a real
# question and the honest answer for a long time was "unknown".
#
# It is answerable, because the PDP-10/its repository preserves an earlier
# version of the same file: SYSENG;FSDEFS 40, three file-versions back, which was
# imported and then deleted in 2016 and is still in the history.  This script
# extracts it and compares every symbol.
#
# WHAT IT DOES NOT ANSWER.  A file version is ITS's own numbering, not a release
# number, and nothing here maps 40 or 43 onto an ITS distribution.  What the
# comparison establishes is that the format did not move between two points; it
# does not establish where those points are.  See docs/sources.md.
#
set -u

ITS=${1:-$HOME/its}
NEW=$ITS/src/system/fsdefs.43

[ -f "$NEW" ] || { echo "version-diff: no $NEW"; exit 2; }
command -v git >/dev/null 2>&1 || { echo "version-diff: no git"; exit 2; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
rc=0

# SYSENG;FSDEFS 40, from the commit that added it.  `git log --diff-filter=A`
# finds the addition rather than the deletion, so this keeps working after the
# file is gone from the tree.
add=$(cd "$ITS" && git log --all --format='%h' --diff-filter=A -- src/syseng/fsdefs.40 2>/dev/null | tail -1)

if [ -z "$add" ]; then
	echo "version-diff: SYSENG;FSDEFS 40 is not in this checkout's history."
	echo "              A shallow clone will not have it:  git fetch --unshallow"
	exit 2
fi

(cd "$ITS" && git show "$add:src/syseng/fsdefs.40") > "$T/old" 2>/dev/null ||
	{ echo "version-diff: cannot extract SYSENG;FSDEFS 40 from $add"; exit 2; }

echo "comparing two versions of the file src/its.h is transcribed from:"
echo "  SYSENG;FSDEFS 40   $(cd "$ITS" && git log -1 --format='%h %ad' --date=short "$add") (added, later deleted)"
echo "  SYSTEM;FSDEFS 43   $(cd "$ITS" && git log -1 --format='%h %ad' --date=short -- src/system/fsdefs.43)"
echo

# Every DEFSYM, as `NAME==VALUE` with the whitespace normalised and any trailing
# comment dropped.  MIDAS writes them both at the left margin and indented.
#
# `-a` ON EVERY GREP OVER AN ITS FILE, and it is not decoration.  Some greps --
# ugrep, which a few systems install AS `grep` -- classify a file with any byte
# that is not valid UTF-8 as binary and silently report NO MATCHES, exit 1, no
# warning.  A 1970s MIDAS source is exactly the kind of file that trips it: 54
# of a 400-file sample of the ITS tree here are skipped that way.
#
# The zero-symbol guard below would catch it, because two empty extractions
# compare equal and would otherwise print "IDENTICAL: all 0 definitions".  This
# makes the check WORK rather than merely fail honestly.
syms() {
	grep -aoE 'DEFSYM[[:space:]]+[A-Z0-9$.]+==[^;]*' "$1" |
		sed 's/DEFSYM[[:space:]]*//; s/[[:space:]]*$//' | sort
}

syms "$T/old" > "$T/old.syms"
syms "$NEW" > "$T/new.syms"

nold=$(wc -l < "$T/old.syms" | tr -d ' ')
nnew=$(wc -l < "$T/new.syms" | tr -d ' ')

echo "1. every symbol, both versions"

if [ "$nold" -eq 0 ] || [ "$nnew" -eq 0 ]; then
	echo "  FAIL one of them yielded no symbols -- the extraction is broken"
	rc=1
elif diff -u "$T/old.syms" "$T/new.syms" > "$T/syms.diff" 2>&1; then
	echo "  ok   IDENTICAL: all $nnew definitions, same names and same values"
else
	echo "  --   they differ:"
	sed 's/^/       /' "$T/syms.diff"
	echo
	echo "       That is not a failure -- it is the answer.  Any symbol below"
	echo "       that src/its.h cites must be marked in it as version-dependent."
	rc=1
fi

echo
echo "2. the symbols src/its.h actually cites"

# THE CITATION COLUMN IS CHECKABLE, so check it.  Every offset in its.h is
#
#     #define ITS_MD_NUM	0	/* MDNUM   ascending directory number  [v] */
#
# -- the first word of the comment is the ITS symbol it was transcribed from.  A
# name that is not in FSDEFS at all is a citation to something that does not
# exist, which is the one kind of wrong citation a machine can catch.
HDR=$(dirname "$0")/../src/its.h
[ -f "$HDR" ] || { echo "  FAIL no $HDR"; exit 1; }

sed -n 's|^#define ITS_[A-Z0-9_]*[[:space:]]\+[^[:space:]]*[[:space:]]*/\* \([A-Z][A-Z0-9$.]*\) .*|\1|p' \
	"$HDR" | sort -u > "$T/cited"
ncited=$(wc -l < "$T/cited" | tr -d ' ')

missing=0

while read -r sym; do
	# A DEFSYM, or one of the flag values FSDEFS writes as a bare assignment.
	grep -aqE "DEFSYM[[:space:]]+$sym==|^[[:space:]]*$sym==" "$NEW" || {
		echo "       $sym is cited in its.h and is not defined in FSDEFS 43"
		missing=$((missing + 1))
	}
done < "$T/cited"

if [ "$ncited" -eq 0 ]; then
	echo "  FAIL no citations found -- the extraction is broken, not the header"
	rc=1
elif [ "$missing" -eq 0 ]; then
	echo "  ok   all $ncited symbols its.h cites are defined in FSDEFS 43"
else
	echo "  FAIL $missing of $ncited are not"
	rc=1
fi

# The two constants that deliberately cite no DEFSYM, because FSDEFS defines
# none: the link separators, which are given in prose and got wrong there.  They
# say "no DEFSYM" in their own citation column, and this is what keeps that
# convention from being quietly extended to a constant somebody could not place.
nodefsym=$(grep -ac '/\* no DEFSYM' "$HDR")
echo "  ok   ...and $nodefsym constants that cite ITS code instead, and say so"

echo
echo "3. what DID change between the two"

# Everything else is prose, and FSDEFS 43 says so about itself.
if grep -aq 'This change only changes the comments in this file' "$NEW"; then
	echo "  FSDEFS 43 states its own change was comment-only:"
	echo
	sed -n '/8\/19\/90 Due to the larger size/,/upward compatible/p' "$NEW" |
		sed 's/^/    /'
	echo
	echo "  ...and the symbol comparison above is what checks that claim."
fi

echo "  The 020 bit in a load address:"
echo "    FSDEFS 40:  $(grep -am1 'FUNNY' "$T/old" | sed 's/^[[:space:]]*//')"
echo "    FSDEFS 43:  flushed -- 17 bits of block number, and programs are told"
echo "                not to mask it out.  src/its.h does not."
echo
echo "  The TUT entry width, in BOTH:"
echo "    $(grep -am1 '9/5/79' "$NEW" | sed 's/^[[:space:]]*//')"
echo "    -- so the 3-bit entry this reads is the post-1979 one, in both versions."

exit $rc
