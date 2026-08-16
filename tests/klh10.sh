#!/bin/sh
#
# klh10.sh -- THE MEASUREMENT THAT PROMOTED `dbd9` FROM corroborated TO confirmed.
#
#     sh tests/klh10.sh bin/itsfs <vdkfmt> <le64 image> [scratch]
#
# `dbd9` is KLH10's disk format, and for nine phases the evidence for it was
# KLH10's SOURCE: cvtfr_dbd9 in vdisk.c builds a word out of nine bytes the way
# itspack.c does, and 200,000 random groups through both formulas disagree
# nowhere.  That is a strong argument and it is still not a measurement -- no
# artifact KLH10 wrote had ever been read.
#
# Building KLH10 settles it without running the emulator, because the build
# ships `vdkfmt`, KLH10's own disk-format converter.  Bytes written by KLH10's
# code, read by ours.
#
# WHAT IS COMPARED, in both directions:
#
#   1. KLH10 converts the pack to dbd9; `itsfs repack` converts the same pack to
#      dbd9; the two files agree byte for byte as far as KLH10 wrote.
#   2. `itsfs check` reads KLH10's file and must reach the SAME ACCOUNTING as it
#      reaches on the le64 original -- same free, same in use, same locked out,
#      same directories, files and links.
#
# 1 alone would only prove the encoders agree.  2 alone would only prove the
# decoder is self-consistent.  Together they close the loop on an artifact
# neither written nor chosen by this project.
#
# NOT part of `make test`: it needs KLH10 built and a 300 MB pack.
#
set -u

ITSFS=${1:?usage: klh10.sh <itsfs> <vdkfmt> <image> [scratch]}
VDKFMT=${2:?usage: klh10.sh <itsfs> <vdkfmt> <image> [scratch]}
IMAGE=${3:?usage: klh10.sh <itsfs> <vdkfmt> <image> [scratch]}
T=${4:-$(mktemp -d)}

[ -x "$ITSFS" ] || { echo "no itsfs at $ITSFS"; exit 1; }
[ -x "$VDKFMT" ] || {
	echo "no vdkfmt at $VDKFMT"
	echo "  build it with, in the PDP-10/its tree:"
	echo "    cd tools/klh10 && ./autogen.sh && mkdir -p tmp && cd tmp &&"
	echo "    ../configure && make -C bld-ks-its"
	exit 1
}
[ -f "$IMAGE" ] || { echo "no image at $IMAGE"; exit 1; }

mkdir -p "$T"
rc=0
pass=0
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
no() { rc=1; printf '  FAIL %s\n' "$1"; }

echo "dbd9 against KLH10's own converter"
echo "  vdkfmt  $VDKFMT"
echo "  image   $IMAGE"
echo

# ---------------------------------------------------------------- 1. the bytes
echo "1. the same pack, converted by both"

"$VDKFMT" ip="$IMAGE" op="$T/klh10.dbd9" ifmt=SIMH ofmt=DBD9 dt=RP06 \
	> "$T/vdkfmt.log" 2>&1 || {
	echo "  FAIL vdkfmt did not run:"
	sed 's/^/       /' "$T/vdkfmt.log"
	exit 1
}
[ -s "$T/klh10.dbd9" ] || { echo "  FAIL vdkfmt wrote nothing"; exit 1; }

"$ITSFS" repack -p le64 -P dbd9 "$IMAGE" "$T/ours.dbd9" || {
	echo "  FAIL itsfs repack failed"
	exit 1
}

nk=$(wc -c < "$T/klh10.dbd9" | tr -d ' ')
no_=$(wc -c < "$T/ours.dbd9" | tr -d ' ')

# KLH10'S FILE IS THE SHORTER ONE, AND THAT IS NOT DAMAGE.  vdkfmt's copy loop
# is `if (!zerosector(wbuff, 128)) devwrite(...)` -- it never writes an all-zero
# sector, so the file stops at the last non-zero one.  Everything it DID write
# is at its true offset, which is what makes the comparison below meaningful.
if [ "$nk" -gt "$no_" ]; then
	no "KLH10's file is LONGER than ours ($nk > $no_) -- unexpected"
else
	ok "vdkfmt wrote $nk bytes, itsfs $no_ (vdkfmt omits trailing zero sectors)"
fi

if cmp -n "$nk" "$T/klh10.dbd9" "$T/ours.dbd9" 2>"$T/cmp.err"; then
	ok "IDENTICAL: all $nk bytes KLH10 wrote"
else
	no "they differ: $(head -1 "$T/cmp.err")"
fi

# And the part KLH10 left out must be exactly the part that is empty, or the
# comparison above was over a prefix that happened to match.
if [ "$(tail -c +$((nk + 1)) "$T/ours.dbd9" | tr -d '\000' | wc -c | tr -d ' ')" = "0" ]; then
	ok "...and every byte beyond that, in ours, is zero"
else
	no "our tail past $nk is not all zeros -- vdkfmt dropped real data"
fi

# ------------------------------------------------------------ 2. the file read
echo
echo "2. what the reader makes of the file KLH10 wrote"

# THE GEOMETRY MUST BE NAMED.  A real KLH10 pack is not its drive's nominal
# size, so the size-based inference refuses it -- correctly, since guessing a
# geometry is the one thing this project will not do.  That refusal is checked
# here rather than merely worked around, because it is the behaviour anybody
# meeting a KLH10 pack will hit first.
out=$("$ITSFS" check -p dbd9 "$T/klh10.dbd9" 2>&1)
case "$out" in
*"no drive geometry"*) ok "a short KLH10 pack is refused rather than guessed at" ;;
*) no "the geometry was inferred from a file that is not nominal size" ;;
esac

# The accounting lines only -- not the header, which names a different file.
acct() { sed -n '/^pack /,/^directories /p'; }

"$ITSFS" check -p dbd9 -d rp06 "$T/klh10.dbd9" 2>&1 | acct > "$T/klh10.acct"
"$ITSFS" check "$IMAGE" 2>&1 | acct > "$T/le64.acct"

if [ ! -s "$T/klh10.acct" ]; then
	no "check said nothing about KLH10's pack"
elif cmp -s "$T/klh10.acct" "$T/le64.acct"; then
	ok "IDENTICAL accounting, from KLH10's dbd9 and from the le64 original"
	sed 's/^/       /' "$T/klh10.acct"
else
	no "the two disagree:"
	diff -u "$T/le64.acct" "$T/klh10.acct" | sed 's/^/       /'
fi

echo
if [ "$rc" -eq 0 ]; then
	echo "$pass checks passed -- dbd9 is confirmed against an artifact KLH10 wrote"
else
	echo "FAILED"
fi
exit $rc
