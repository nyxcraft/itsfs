#!/bin/sh
# itsfs regression tests -- self-contained (every fixture is built here) and
# dependency-free (sh + coreutils).  Run:  make test
#
# NOTHING IN THIS SUITE READS A REAL ITS PACK.  A pack is not in this repo and
# never will be, so a suite that needed one would be a suite only its author can
# run.  The proof against the real thing is a separate target, `make oracle`,
# and what it establishes is written up in docs/validation.md.
#
# The fixture below is instead a REAL ITS FILE SYSTEM BUILT BY HAND, one word at
# a time, into a sparse image: an MFD with a check word and two directories, a
# UFD with a descriptor and a name block, three blocks of text, a link, and a
# TUT.  It costs a few kilobytes on disk and it exercises every layer -- the
# packing, the block-to-sector geometry, the descriptor decoder and the reader.
#
# There is no writer in this project yet, which is exactly why the fixture is
# built with dd: a suite that used the writer to make its input would be asking
# the reader to agree with the writer rather than with ITS.

set -u
ITSFS=${ITSFS:-./bin/itsfs}
[ -x "$ITSFS" ] || ITSFS=../bin/itsfs
[ -x "$ITSFS" ] || { echo "run.sh: cannot find bin/itsfs (build first)"; exit 2; }
ITSFS=$(cd "$(dirname "$ITSFS")" && pwd)/$(basename "$ITSFS")

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

#
# A CHECK'S FAILURE MESSAGE MUST BEGIN WITH ITS SUCCESS MESSAGE, so that a
# harness looking for "FAIL <name>" can find the check it meant.  `chk` gets
# this right for free; a hand-written if/else has to do it on purpose.
#
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
no() { fail=$((fail + 1)); printf '  FAIL %s\n' "$1"; }
chk() { if [ "$2" = "$3" ]; then ok "$1"; else no "$1 (got '$2', want '$3')"; fi; }

# Does the output contain this?
has() { case "$2" in *"$3"*) ok "$1";; *) no "$1 (output was: $(echo "$2" | head -3))";; esac; }
hasnt() { case "$2" in *"$3"*) no "$1 (output contained '$3')";; *) ok "$1";; esac; }

# Did it exit with a plain error rather than dying?  A signal (>128) or a
# sanitizer abort is a failure even though both are "non-zero".
died() { [ "$1" -gt 128 ] || [ "$1" -eq 124 ]; }

# ------------------------------------------------------------------ fixtures

# rp06: 19 surfaces, 20 sectors, 8 sectors per block, so 47 blocks per cylinder
# and four sectors of every cylinder unreachable.  This is the arithmetic under
# test, written out a second time here ON PURPOSE -- if the suite computed block
# addresses by calling itsfs, a wrong mapping would agree with itself.
RP06_SECTORS=309700
blk_sector() {
	_b=$1
	_cyl=$((_b / 47))
	_within=$(((_b - _cyl * 47) * 8))
	_srf=$((_within / 20))
	_sec=$((_within - _srf * 20))
	echo $(((_cyl * 19 + _srf) * 20 + _sec))
}

# One 36-bit word as eight little-endian bytes.
emit_word() {
	_w=$1; _i=0

	while [ $_i -lt 8 ]; do
		_b=$(((_w >> (8 * _i)) & 255))
		printf "\\$(printf '%03o' "$_b")"
		_i=$((_i + 1))
	done
}

# poke <image> <block> <word offset> <word>...   -- words are shell arithmetic,
# so `0551646164416` is octal exactly as ITS would write it.
poke() {
	_img=$1; _blk=$2; _off=$3; shift 3
	_byte=$((($(blk_sector "$_blk") * 128 + _off) * 8))
	for _w in "$@"; do emit_word "$((_w))"; done |
		dd of="$_img" bs=1 seek="$_byte" conv=notrunc 2>/dev/null
}

# The SIXBIT of a name, as an octal literal for the arithmetic above.  This uses
# the tool to build the tool's input, which is safe here only because check 1
# pins `sixbit` against a value measured off a real pack.
sb() { echo "0$("$ITSFS" sixbit "$1" | awk '{print $1}')"; }

# A sparse rp06-sized image: 309,700 sectors of 1024 bytes.  Sparse, so the file
# is 302 MB and the disk cost is the few kilobytes actually written.
mkpack() { dd if=/dev/zero of="$1" bs=1 count=0 seek=$((RP06_SECTORS * 1024)) 2>/dev/null; }

echo "itsfs regression tests ($ITSFS)"

# ---------------------------------------------------------------- encodings

# 1. SIXBIT, against a word measured off a real ITS pack: MDCHK in the MFD.
out=$("$ITSFS" sixbit M.F.D. | awk '{print $1}')
chk "SIXBIT of M.F.D. is the MDCHK word on a real pack" "$out" "551646164416"

out=$("$ITSFS" sixbit -d 551646164416)
has "and it decodes back" "$out" "|M.F.D.|"

# 2. Refusal, not truncation, and not case folding.
out=$("$ITSFS" sixbit hello 2>&1); rc=$?
chk "lower case is refused (SIXBIT has none)" "$rc" "1"
has "...and says why" "$out" "no lower case"

out=$("$ITSFS" sixbit SEVENCH 2>&1); rc=$?
chk "a seven-character name is refused, not truncated" "$rc" "1"

out=$("$ITSFS" sixbit '' 2>&1 | awk '{print $1}')
chk "the empty name is six spaces" "$out" "000000000000"

# 3. The halfword and value syntax.
out=$("$ITSFS" sixbit -d 777777,,0 | awk '{print $1}')
chk "lh,,rh parses as two 18-bit halves" "$out" "777777000000"

out=$("$ITSFS" sixbit -d 1000000,,0 2>&1); rc=$?
chk "a half wider than 18 bits is refused" "$rc" "1"

out=$("$ITSFS" sixbit -d 7777777777777 2>&1); rc=$?
chk "a value wider than 36 bits is refused" "$rc" "1"

out=$("$ITSFS" sixbit -d d:255 | awk '{print $1}')
chk "d: means decimal" "$out" "000000000377"

out=$("$ITSFS" sixbit -d 0xff | awk '{print $1}')
chk "0x means hex" "$out" "000000000377"

out=$("$ITSFS" sixbit -d 99 2>&1); rc=$?
chk "9 is not an octal digit" "$rc" "1"

# ----------------------------------------------------------------- packings

out=$("$ITSFS" packings)
has "packings lists le64" "$out" "le64"
has "...with its status" "$out" "confirmed"
has "...and dbd9, the one that shares a byte" "$out" "dbd9"

# A small image of pseudo-random WORDS.
#
# Random BYTES are not a PDP-10 image: three and a half bytes of every eight are
# not storage, and a word read out of random bytes comes back masked.  So the
# fixture is made by putting random bytes through one repack, which is exactly
# what masking to 36 bits means -- and that first pass is itself the check that
# nothing above bit 36 survives a read.
dd if=/dev/urandom of="$T/raw.img" bs=8192 count=1 2>/dev/null
"$ITSFS" repack -f "$T/raw.img" "$T/rand.img" 2>/dev/null

if cmp -s "$T/raw.img" "$T/rand.img"; then
	no "random bytes are not a word image: the top 28 bits are dropped"
else
	ok "random bytes are not a word image: the top 28 bits are dropped"
fi

"$ITSFS" repack -f "$T/rand.img" "$T/rand2.img" 2>/dev/null

if cmp -s "$T/rand.img" "$T/rand2.img"; then
	ok "...and a second pass changes nothing, so the masking is idempotent"
else
	no "...and a second pass changes nothing, so the masking is idempotent"
fi

for p in le64 be64 core dbd9; do
	"$ITSFS" repack -f -P "$p" "$T/rand.img" "$T/x.$p" 2>/dev/null &&
		"$ITSFS" repack -f -p "$p" "$T/x.$p" "$T/back.$p" 2>/dev/null
	rc=$?

	if [ $rc -eq 0 ] && cmp -s "$T/rand.img" "$T/back.$p"; then
		ok "le64 -> $p -> le64 round-trips byte-for-byte"
	else
		no "le64 -> $p -> le64 round-trips byte-for-byte (rc=$rc)"
	fi
done

# THE dbd9 CODEC IS KLH10'S OWN FUNCTION, and this checks that rather than
# trusting the comment that says so.  KLH10's cvtfr_dbd9 builds each word as
# LRHSET(w, lh, rh) out of nine bytes; the same nine bytes through `dump -s`
# must give the same two words.  The expected values below were computed from
# KLH10's formula, not from this project's.
printf '\001\002\003\004\125\006\007\010\011' > "$T/d9.img"
out=$("$ITSFS" dump -s -z -w 2 -p dbd9 "$T/d9.img" 0 | tail -2 | awk '{print $2}' | tr '\n' ' ')
chk "the dbd9 codec agrees with KLH10's cvtfr_dbd9" "$out" "002010030105 240601604011 "

# The top 28 bits of a word are not storage.  An le64 container with them set
# comes back with them clear, which is the invariant the whole project rests on.
printf '\377\377\377\377\377\377\377\377' > "$T/big.img"
out=$("$ITSFS" dump -s -z -w 1 "$T/big.img" 0 | tail -1 | awk '{print $2}')
chk "bits above 36 are masked off on the way in" "$out" "777777777777"

out=$("$ITSFS" repack -f -P nonesuch "$T/rand.img" "$T/no" 2>&1); rc=$?
chk "an unknown packing is refused" "$rc" "2"
has "...and lists the known ones" "$out" "le64"

"$ITSFS" repack "$T/rand.img" "$T/x.le64" >/dev/null 2>&1; rc=$?
chk "repack will not overwrite without -f" "$rc" "1"

# ----------------------------------------------------------------- geometry

out=$("$ITSFS" drives)
has "drives lists the rp06" "$out" "rp06"
has "...and the rp07, whose cylinders divide exactly" "$out" "rp07"
has "...and says which sectors no block reaches" "$out" "reachable by no block number"

out=$("$ITSFS" drives | awk 'NF>=10 && $1=="rp06" {print $6; exit}')
chk "an rp06 cylinder holds 47 blocks, not 47.5" "$out" "47"

out=$("$ITSFS" drives | awk 'NF>=10 && $1=="rp07" {print $6; exit}')
chk "an rp07 cylinder holds 172, and divides exactly" "$out" "172"

out=$("$ITSFS" drives | awk 'NF>=10 && $1=="rp06" {print $7; exit}')
chk "so an rp06 has 38164 blocks in its file area" "$out" "38164"

# ------------------------------------------------------- an ITS file system

# Twelve three-bit entries to a word, most significant first.
tutword() {
	_w=0

	for _v in "$@"; do _w=$(((_w << 3) | _v)); done
	echo $_w
}

# The TUT is NTUTBL blocks long, so a word index runs off the end of the first
# one; poking by (block, offset) is what poke() takes, so convert here.
tutpoke() { # tutpoke <image> <word index within the TUT> <word>
	poke "$1" $((TUTBLK + $2 / 1024)) $(($2 % 1024)) "$3"
}


#
# THE FIXTURE, AS A FUNCTION, so that every damage case below starts from a
# fresh one rather than from a copy.  A copy would work, but this pack is a
# sparse 302 MB file and whether `cp` keeps the holes is a property of the
# host's coreutils -- building it again is thirty dd invocations and no
# assumptions.
#
mkfixture() {
	# Build one.  Every number here is the value ITS itself would write; see its.h
	# for the symbol each belongs to.
	mkpack "$1"

	# UDNAMP is 1009, so the name area holds three entries.
	# The MFD: header, then two directory names at the top of the block.  The
	# POSITION of a name is the address of its directory:
	#   word 1020 -> block (1020 - 1024 + 2*500)/2 = 498
	#   word 1022 -> block (1022 - 1024 + 2*500)/2 = 499
	poke "$1" $MFDBLK 0 1 1020 0 0 0 0551646164416 $NUDSL
	poke "$1" $MFDBLK 1020 "$(sb TEST)" 0 "$(sb EMPTY)" 0

	# The UFD for TEST, in block 498.  UDESCP is the free pointer into the
	# descriptor area (18 six-bit bytes are used below) and UDNAMP puts the name
	# area at word 1009, which leaves room for three entries.  UDBLKS is the
	# directory's own count of the blocks its files hold.
	poke "$1" 498 0 18 1009 "$(sb TEST)" 4 0

	# ...and an EMPTY one in block 499, which is a legal directory with nothing in
	# it: no descriptors, and a name area of one unused slot.
	poke "$1" 499 0 0 1019 "$(sb EMPTY)" 0 0

	# Its descriptor area, at word 11 (UDDESC).  Six-bit bytes:
	#   40 37 20   load address: (0<<12) | (037<<6) | 020 = block 2000
	#   02         take 2 more, so blocks 2000, 2001, 2002
	#   00         end
	#   at byte 6:  a link, ".;@;DDT" in SIXBIT, ending in 0
	#   at byte 14: 40 40 64 00, a one-block file at block 2100
	poke "$1" 498 11 0403720020000 0163340334444 0640040406400

	# Three name blocks, IN SIXBIT ORDER, because a real ITS name area is
	# sorted -- 6,056 entries on the reference pack and not one out of place,
	# which is `QRELOC` in disk.1228 keeping it that way.  The first version
	# of this fixture was in the reverse order, and it was `put`'s own tests
	# that noticed: a writer looking for an existing name stopped early on an
	# unsorted area and happily created a duplicate.
	#
	#   A LINK      the link bit set, descriptor at byte 6
	#   HELLO TXT   descriptor at byte 0, 3 words in the last block, so
	#               2*1024 + 3 = 2051 words
	#   NUL TXT     one block, one word, at byte 14
	poke "$1" 498 1009 "$(sb A)" "$(sb LINK)" $(((1 << 18) | 6)) 0 0
	poke "$1" 498 1014 "$(sb HELLO)" "$(sb TXT)" $((3 << 24)) 0 0
	poke "$1" 498 1019 "$(sb NUL)" "$(sb TXT)" $(((1 << 24) | 14)) 0 0

	# Three blocks of text.  "HI" is 0110 0111 in seven-bit ASCII, five to a word.
	poke "$1" 2000 0 $(((0110 << 29) | (0111 << 22)))
	poke "$1" 2002 0 $(((0110 << 29) | (0111 << 22))) 0 0

	# ...and one word holding  A NUL B NUL NUL, for the trailing-versus-interior
	# question below.
	poke "$1" 2100 0 $(((0101 << 29) | (0102 << 15)))

	# The TUT: a pack name, and blocks 0..38164 mapped.  It is filled in
	# properly rather than left blank, because `check` compares it against the
	# files BLOCK BY BLOCK -- a fixture with an empty allocation table is not a
	# file system, it is a file system with 505 problems.
	poke "$1" $TUTBLK 0 0 "$(sb TESTPK)" 3102 1551 0 38164 0

	# LOCKED OUT: the 500 directory blocks, the MFD, and the four TUT blocks.  The
	# map starts at word LTIBLK (20 octal), so block b's entry is in word 16 + b/12.
	i=0
	while [ $i -lt 41 ]; do                     # blocks 0..491
		tutpoke "$1" $((16 + i)) "$(tutword 7 7 7 7 7 7 7 7 7 7 7 7)"
		i=$((i + 1))
	done
	tutpoke "$1" 57 "$(tutword 7 7 7 7 7 7 7 7 0 0 0 0)"   # 492..503
	tutpoke "$1" 1605 "$(tutword 0 0 0 0 0 0 0 0 0 7 7 7)" # 19077..19079
	tutpoke "$1" 1606 "$(tutword 7 7 0 0 0 0 0 0 0 0 0 0)" # 19080, 19081

	# IN USE: the four blocks the two files hold, each referenced once.
	tutpoke "$1" 182 "$(tutword 0 0 0 0 0 0 0 0 1 1 1 0)"  # 2000..2002
	tutpoke "$1" 191 "$(tutword 1 0 0 0 0 0 0 0 0 0 0 0)"  # 2100
}

MFDBLK=19081
TUTBLK=19077
NUDSL=500
mkfixture "$T/pack.dsk"

out=$("$ITSFS" info "$T/pack.dsk")
has "info identifies the drive from the size alone" "$out" "rp06"
has "...and finds the MFD where the geometry says it is" "$out" "an ITS master file directory"
has "...and reads MDNUDS" "$out" "room for 500"

# THE GEOMETRY, CHECKED AGAINST ITSELF FROM THE OTHER SIDE.  Block 19081 lands
# at a sector this suite computes independently; dumping that raw sector must
# give the same words as dumping the block.
sec=$(blk_sector $MFDBLK)
chk "block 19081 is at sector 154268" "$sec" "154268"
a=$("$ITSFS" dump -z -w 8 "$T/pack.dsk" $MFDBLK | tail -8)
b=$("$ITSFS" dump -z -w 8 -s "$T/pack.dsk" "$sec" | tail -8)
chk "the block and its raw sector hold the same words" "$a" "$b"

out=$("$ITSFS" dirs "$T/pack.dsk")
has "dirs lists TEST" "$out" "TEST"
has "...and EMPTY" "$out" "EMPTY"

out=$("$ITSFS" ls "$T/pack.dsk" TEST)
has "ls finds the file" "$out" "HELLO  TXT"
has "...and the link, rendered as ITS writes a name" "$out" "-> .;@ DDT"

out=$("$ITSFS" ls -l "$T/pack.dsk" TEST | grep '^HELLO')
chk "the descriptor decodes to 3 blocks and 2051 words" \
	"$(echo "$out" | awk '{print $3, $4}')" "2051 3"

out=$("$ITSFS" ls -l "$T/pack.dsk" TEST | tail -1)
has "and the total agrees with the directory's own UDBLKS" "$out" "4 blocks"
has "...which is what UDBLKS says" "$out" "UDBLKS says 4"

out=$("$ITSFS" ls "$T/pack.dsk" NOSUCH 2>&1); rc=$?
chk "a directory that is not in the MFD is refused" "$rc" "1"
has "...by name" "$out" "no directory named 'NOSUCH'"

out=$("$ITSFS" cat "$T/pack.dsk" 'TEST;HELLO TXT')
chk "cat reads the file's blocks in descriptor order" "$out" "HIHI"

out=$("$ITSFS" cat "$T/pack.dsk" TEST HELLO TXT)
chk "...and the three-argument spelling is the same file" "$out" "HIHI"

out=$("$ITSFS" cat "$T/pack.dsk" 'TEST;A LINK' 2>&1); rc=$?
chk "cat of a link refuses rather than reading the link itself" "$rc" "1"
has "...and says what it points at" "$out" ".;@ DDT"

"$ITSFS" get "$T/pack.dsk" 'TEST;HELLO TXT' "$T/out.txt" >/dev/null 2>&1
chk "get writes the same bytes cat prints" "$(cat "$T/out.txt")" "HIHI"

"$ITSFS" get -w "$T/pack.dsk" 'TEST;HELLO TXT' "$T/out.words" >/dev/null 2>&1
chk "get -w writes 2051 words of eight bytes" "$(wc -c < "$T/out.words" | tr -d ' ')" "16408"

#
# INTERIOR NULs ARE DATA; A TRAILING RUN OF THEM IS PADDING.
#
# NUL TXT is one word holding  A NUL B NUL NUL.  A file's length is known in
# words, so the last word may carry characters that are not part of the file --
# but only the ones at the END of it.  The first version of this dropped every
# NUL, which is the same code and one state fewer, and it silently lost two
# characters out of the middle of a real file on a real pack.  Found by
# comparing an extracted file against the host file it was built from
# (tests/crosscheck.sh); this is the regression test for it.
#
"$ITSFS" cat "$T/pack.dsk" 'TEST;NUL TXT' > "$T/nul.txt" 2>/dev/null
chk "an interior NUL survives extraction" "$(wc -c < "$T/nul.txt" | tr -d ' ')" "3"
chk "...and the trailing pair does not" "$(od -An -c < "$T/nul.txt" | tr -s ' ')" " A \\0 B"

out=$("$ITSFS" free "$T/pack.dsk")
has "free reads the pack name out of the TUT" "$out" "TESTPK"
has "...and the swap boundary" "$out" "1551"

# ------------------------------------------------- refusing malformed input
#
# Everything below is a pack that has been damaged on purpose.  The bar is not
# "it survives" -- it is that it refuses BY NAME and does not read past the end
# of anything.  Under `make test-san` these are the checks with teeth.

mkfixture "$T/bad.dsk"
poke "$T/bad.dsk" $MFDBLK 5 0123456701234
out=$("$ITSFS" dirs "$T/bad.dsk" 2>&1); rc=$?
chk "an MFD without its check word is refused" "$rc" "1"
has "...naming MDCHK" "$out" "not an MFD"
out=$("$ITSFS" info "$T/bad.dsk" 2>&1)
has "and info says so rather than pretending" "$out" "NOT |M.F.D.|"

mkfixture "$T/bad2.dsk"
poke "$T/bad2.dsk" $MFDBLK 1 9999
out=$("$ITSFS" dirs "$T/bad2.dsk" 2>&1); rc=$?
chk "an MDNAMP outside the block is refused" "$rc" "1"
has "...with the value" "$out" "9999"

mkfixture "$T/bad3.dsk"
poke "$T/bad3.dsk" 498 1 0
out=$("$ITSFS" ls "$T/bad3.dsk" TEST 2>&1); rc=$?
chk "a UFD with a zero UDNAMP is refused" "$rc" "1"
has "...as not a UFD" "$out" "not a UFD"

# A descriptor that takes blocks before any load address has set one.
mkfixture "$T/bad4.dsk"
poke "$T/bad4.dsk" 498 11 0020000000000
out=$("$ITSFS" ls -l "$T/bad4.dsk" TEST 2>&1); rc=$?
has "a take before any load address is refused" "$out" "take before any load address"
! died $rc && ok "...without dying" || no "...without dying (rc=$rc)"

# A load address pointing past the end of the drive.  077 77 77 is block 131071;
# an rp06 has 38305.
mkfixture "$T/bad5.dsk"
poke "$T/bad5.dsk" 498 11 0777777000000
out=$("$ITSFS" ls -l "$T/bad5.dsk" TEST 2>&1)
has "a block past the end of the drive is refused" "$out" "past the end of the drive"

# A descriptor with no terminating zero: every byte a "take 1".
mkfixture "$T/bad6.dsk"
w=0
i=0
while [ $i -lt 6 ]; do w=$((w * 64 + 1)); i=$((i + 1)); done
j=11
while [ $j -lt 1014 ]; do poke "$T/bad6.dsk" 498 $j $w; j=$((j + 1)); done
out=$(cd "$T" && "$ITSFS" ls -l bad6.dsk TEST 2>&1); rc=$?
! died $rc && ok "a descriptor that never ends is bounded, not followed forever" ||
	no "a descriptor that never ends is bounded, not followed forever (rc=$rc)"

# A TUT whose QLASTB is before its QFRSTB, and one that maps more blocks than
# its own table can hold.
mkfixture "$T/bad7.dsk"
poke "$T/bad7.dsk" $TUTBLK 4 100 50
out=$("$ITSFS" free "$T/bad7.dsk" 2>&1); rc=$?
chk "a TUT that ends before it begins is refused" "$rc" "1"

mkfixture "$T/bad8.dsk"
poke "$T/bad8.dsk" $TUTBLK 5 9999999
out=$("$ITSFS" free "$T/bad8.dsk" 2>&1); rc=$?
chk "a TUT mapping more blocks than it has room for is refused" "$rc" "1"
has "...saying so" "$out" "does not fit"

# ------------------------------------------------------ the checker
#
# `check` shares no code with the reader: it re-derives the geometry, the
# directory-slot arithmetic, the descriptor bytecode and the TUT from its.h.  So
# these are not the same checks again with a different command name -- a bug in
# structure.c that the reader tests miss is one of the things they are for.
#
# The bar for each damage case is that it is named.  "Problems: 1" would pass a
# check that reported the wrong thing.

out=$("$ITSFS" check "$T/pack.dsk"); rc=$?
chk "an undamaged pack checks clean" "$rc" "0"
has "...saying so" "$out" "no problems found"
has "...and accounting for the space" "$out" "claimed        4 blocks, in 2 files"
has "...and the locked-out blocks" "$out" "505 locked out"

# A LINK THAT DOES NOT RESOLVE IS A NOTE, NOT A PROBLEM.  The fixture's link
# points at .;@ DDT, which is not there -- exactly like the seven on the
# reference pack.  A live file system has broken links in it; a checker that
# calls that damage is one people learn to ignore.
has "an unresolved link is counted as a note" "$out" "1 links (1 of them unresolved)"
out=$("$ITSFS" check -v "$T/pack.dsk")
has "...and -v says which link and where it pointed" "$out" "TEST;A LINK is a link to .;@ DDT"

mkfixture "$T/k1.dsk"
poke "$T/k1.dsk" $MFDBLK 5 0123456701234
out=$("$ITSFS" check "$T/k1.dsk" 2>&1); rc=$?
chk "an MFD without its check word fails the check" "$rc" "1"
has "...naming MDCHK" "$out" "not an MFD"
has "...and refusing to report anything under it" "$out" "nothing below it could be checked"

mkfixture "$T/k2.dsk"
poke "$T/k2.dsk" $MFDBLK 6 9999
out=$("$ITSFS" check "$T/k2.dsk" 2>&1); rc=$?
chk "an MDNUDS that cannot fit in the MFD is refused" "$rc" "1"
has "...citing the assertion FSDEFS makes about it" "$out" "does not fit in a 1024-word MFD"

mkfixture "$T/k3.dsk"
poke "$T/k3.dsk" $MFDBLK 1022 "$(sb TEST)"
out=$("$ITSFS" check "$T/k3.dsk" 2>&1); rc=$?
chk "two MFD slots naming one directory is a problem" "$rc" "1"
has "...naming it" "$out" "two MFD slots name the directory |TEST|"

mkfixture "$T/k4.dsk"
poke "$T/k4.dsk" 498 2 "$(sb OTHER)"
out=$("$ITSFS" check "$T/k4.dsk" 2>&1)
has "a UDNAME that disagrees with the MFD is a problem" "$out" \
	"reached as |TEST| but its UDNAME is |OTHER|"

mkfixture "$T/k5.dsk"
poke "$T/k5.dsk" 498 0 7000
out=$("$ITSFS" check "$T/k5.dsk" 2>&1)
has "a descriptor area that has reached the name area is a problem" "$out" \
	"has overrun the name area"

mkfixture "$T/k6.dsk"
poke "$T/k6.dsk" 498 1011 $(((3 << 24) | 20))
out=$("$ITSFS" check "$T/k6.dsk" 2>&1)
has "a UNDSCP past the free pointer is a problem" "$out" "at or past the free pointer"

mkfixture "$T/k7.dsk"
poke "$T/k7.dsk" 498 3 9
out=$("$ITSFS" check "$T/k7.dsk" 2>&1); rc=$?
chk "a UDBLKS that disagrees with the descriptors is a problem" "$rc" "1"
has "...with both numbers" "$out" "descriptors name 4 blocks, but UDBLKS says 9"

# THE FOUR WAYS THE TUT AND THE FILES CAN DISAGREE.  They are different
# problems: one loses data, one only loses space, and the checker says which.
mkfixture "$T/k8.dsk"
tutpoke "$T/k8.dsk" 191 0
out=$("$ITSFS" check "$T/k8.dsk" 2>&1); rc=$?
chk "a block a file holds that the TUT calls free is a problem" "$rc" "1"
has "...naming the file that would be overwritten" "$out" \
	"block 2100 is claimed by TEST;NUL TXT, and the TUT calls it FREE"

mkfixture "$T/k9.dsk"
tutpoke "$T/k9.dsk" 266 "$(tutword 1 0 0 0 0 0 0 0 0 0 0 0)"
out=$("$ITSFS" check "$T/k9.dsk" 2>&1)
has "a block in use that no file claims is a problem" "$out" \
	"block 3000 is marked in use (1 references) and no file claims it"

mkfixture "$T/k10.dsk"
tutpoke "$T/k10.dsk" 191 "$(tutword 3 0 0 0 0 0 0 0 0 0 0 0)"
out=$("$ITSFS" check "$T/k10.dsk" 2>&1)
has "a reference COUNT that disagrees is a problem, not just a flag" "$out" \
	"block 2100: the TUT says 3 references, the files make 1"

mkfixture "$T/k11.dsk"
poke "$T/k11.dsk" 498 13 0640040000500
out=$("$ITSFS" check "$T/k11.dsk" 2>&1)
has "a file holding a locked-out block is a problem" "$out" "the TUT has it LOCKED OUT"

mkfixture "$T/k12.dsk"
tutpoke "$T/k12.dsk" 16 0
out=$("$ITSFS" check "$T/k12.dsk" 2>&1)
has "a directory block the TUT does not lock out is a problem" "$out" \
	"block 0 is a directory and the TUT does not lock it out"

mkfixture "$T/k13.dsk"
tutpoke "$T/k13.dsk" 266 "$(tutword 7 0 0 0 0 0 0 0 0 0 0 0)"
out=$("$ITSFS" check "$T/k13.dsk" 2>&1)
has "a locked-out block that is not a directory or a table is a problem" "$out" \
	"block 3000 is locked out and is not a directory, the MFD or the TUT"

# Two files holding the same block.  The format ALLOWS it -- the TUT is a
# reference count -- so the checker reports it and names both, rather than
# calling it corruption on its own authority.
mkfixture "$T/k14.dsk"
poke "$T/k14.dsk" 498 13 0640040372000
out=$("$ITSFS" check "$T/k14.dsk" 2>&1)
has "two files holding one block are both named" "$out" \
	"block 2000 is claimed by TEST;NUL TXT and already by TEST;HELLO TXT"

# And the checker is a parser like any other: it gets the fuzzer's input too.
out=$("$ITSFS" check "$T/rand.img" 2>&1); rc=$?
chk "check on an image of no known drive is refused" "$rc" "2"

# ------------------------------------------------- manifest, verify, shell

out=$("$ITSFS" manifest "$T/pack.dsk")
has "manifest names itself and its version" "$out" "# itsfs manifest 1"
has "...and records the file" "$out" "TEST;HELLO TXT"
has "...and the link, by its TARGET rather than by following it" "$out" "TEST;A LINK -> .;@ DDT"
has "...and the directory" "$out" "d "
has "...and counts what it walked" "$out" "2 directories, 2 files, 1 links"

# THE CHECKSUM IS OVER WORDS, WHICH IS THE WHOLE POINT.  A manifest taken from
# an le64 image must verify against the same file system stored in a packing
# that shares no byte boundary with it -- otherwise the manifest fingerprints
# the container rather than the file system.
"$ITSFS" manifest "$T/pack.dsk" > "$T/ref.mf" 2>/dev/null
"$ITSFS" repack -f -P dbd9 "$T/pack.dsk" "$T/pack.d9" 2>/dev/null

if cmp -s "$T/pack.dsk" "$T/pack.d9"; then
	no "the dbd9 copy differs from the le64 one (nothing was repacked)"
else
	ok "the dbd9 copy shares no bytes with the le64 one"
fi

out=$("$ITSFS" verify -p dbd9 "$T/pack.d9" "$T/ref.mf" 2>&1); rc=$?
chk "...and a manifest from le64 verifies against it" "$rc" "0"
has "...with nothing changed" "$out" "0 differences"

# A verify that cannot fail is decoration.  One word of one file, flipped.
cp "$T/pack.dsk" "$T/mf1.dsk"
poke "$T/mf1.dsk" 2000 0 0777777777777
out=$("$ITSFS" verify "$T/mf1.dsk" "$T/ref.mf" 2>&1); rc=$?
chk "one changed word is a difference" "$rc" "1"
has "...named, and named as a content change" "$out" "! TEST;HELLO TXT (contents differ)"

# A file whose length changed, rather than its contents.
cp "$T/pack.dsk" "$T/mf2.dsk"
poke "$T/mf2.dsk" 498 1016 $((7 << 24))
out=$("$ITSFS" verify "$T/mf2.dsk" "$T/ref.mf" 2>&1)
has "a changed length is reported as one" "$out" "words, was 2051"

# A file that is gone, and one that is new.
cp "$T/pack.dsk" "$T/mf3.dsk"
poke "$T/mf3.dsk" 498 1014 0 0
out=$("$ITSFS" verify "$T/mf3.dsk" "$T/ref.mf" 2>&1)
has "a file only in the manifest is -" "$out" "- TEST;HELLO TXT"

cp "$T/pack.dsk" "$T/mf4.dsk"
poke "$T/mf4.dsk" 498 1004 "$(sb EXTRA)" "$(sb TXT)" $(((1 << 24) | 14)) 0 0
poke "$T/mf4.dsk" 498 1 1004
out=$("$ITSFS" verify "$T/mf4.dsk" "$T/ref.mf" 2>&1)
has "a file only on the pack is +" "$out" "+ TEST;EXTRA TXT"

# A LINK IS COMPARED BY ITS TARGET.  Following it would checksum the same file
# under two names, and make an unrelated deletion look like damage to the link.
cp "$T/pack.dsk" "$T/mf5.dsk"
poke "$T/mf5.dsk" 498 12 0163340414243
out=$("$ITSFS" verify "$T/mf5.dsk" "$T/ref.mf" 2>&1)
has "a link that now points elsewhere is a difference" "$out" "points at"

# A damaged manifest is refused, not half-read: a verify that skipped the lines
# it could not parse would report a pack clean on the strength of not looking.
printf 'not a manifest at all\n' > "$T/bad.mf"
out=$("$ITSFS" verify "$T/pack.dsk" "$T/bad.mf" 2>&1); rc=$?
chk "a file that is not a manifest is refused" "$rc" "2"

# ...and one whose lines all parse but whose header is missing, which is the
# case the magic line exists for: it looks like a manifest and is not one.
grep -v '^# itsfs manifest' "$T/ref.mf" > "$T/bad2.mf"
out=$("$ITSFS" verify "$T/pack.dsk" "$T/bad2.mf" 2>&1); rc=$?
chk "a manifest without its header line is refused" "$rc" "2"
has "...by name" "$out" "does not begin with"

n=$(grep -n '^f ' "$T/ref.mf" | head -1 | cut -d: -f1)
sed "${n}s/^f /X /" "$T/ref.mf" > "$T/bad3.mf"
out=$("$ITSFS" verify "$T/pack.dsk" "$T/bad3.mf" 2>&1); rc=$?
chk "a malformed line is refused rather than skipped" "$rc" "2"
has "...with its line number" "$out" "line $n is malformed"

out=$("$ITSFS" manifest "$T/pack.dsk" NOSUCH 2>&1); rc=$?
chk "manifest of a directory that is not there is refused" "$rc" "1"

#
# A PACK WITH NO RESOLVABLE DIRECTORIES AT ALL.  MDNUDS is what the MFD-slot
# arithmetic subtracts, so zeroing it makes every slot resolve to a negative
# block and the walk finds nothing.  The manifest is then empty -- which is a
# legitimate answer, and was also undefined behaviour: `qsort(NULL, 0, ...)`.
# The corruption fuzzer found it; nothing written by hand had reached it,
# because a pack with no directories is not a case anybody thinks to build.
#
mkfixture "$T/mf6.dsk"
poke "$T/mf6.dsk" $MFDBLK 6 0
out=$("$ITSFS" manifest "$T/mf6.dsk" 2>&1); rc=$?
chk "a pack whose MDNUDS is zero yields an empty manifest, not a crash" "$rc" "0"
has "...that is still a manifest" "$out" "# itsfs manifest 1"
has "...and says it walked nothing" "$out" "0 directories, 0 files, 0 links"

# ...and verifying a pack against an empty manifest is the same zero on the
# other side: everything on the pack is new.
"$ITSFS" manifest "$T/mf6.dsk" > "$T/empty.mf" 2>/dev/null
out=$("$ITSFS" verify "$T/pack.dsk" "$T/empty.mf" 2>&1); rc=$?
chk "an empty manifest makes every file a difference" "$rc" "1"
has "...reported as additions" "$out" "+ TEST;HELLO TXT"

# --- the shell.  It reads stdin, which is what makes it testable.

out=$(printf 'cd TEST\npwd\nls\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "the shell changes directory" "$out" "TEST"
has "...and lists it" "$out" "HELLO  TXT"
has "...and counts what it listed" "$out" "3 entries"

out=$(printf 'cd test\nls\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "a lower-case name is upper-cased for the shell's own convenience" "$out" "HELLO  TXT"

out=$(printf 'cd TEST\ntype HELLO TXT\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "the shell prints a file" "$out" "HIHI"

out=$(printf 'cd TEST\nblocks HELLO TXT\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "blocks shows the run rather than three numbers" "$out" "2000..2002 (3)"

out=$(printf 'cd TEST\nstat HELLO TXT\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "stat shows the raw words as well as the decoded fields" "$out" "UNRNDM"
has "...including the ones nothing here interprets" "$out" "the unit is not established"

out=$(printf 'ls\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "ls with no current directory says what to do" "$out" "cd DIR"

out=$(printf 'cd NOSUCH\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "cd to a directory that is not there is refused" "$out" "no directory named 'NOSUCH'"

out=$(printf 'bogus\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "an unknown command says so" "$out" "unknown command 'bogus'"

out=$(printf 'cd TEST\ntype NOSUCH FILE\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "typing a file that is not there is refused" "$out" "no file"

out=$(printf 'cd TEST\ntype A LINK\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "typing a link says what it points at rather than following it" "$out" "is a link to .;@ DDT"

out=$(printf 'help\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "help says the shell is read-only" "$out" "no writer"

out=$(printf 'free\ninfo\ndirs\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
has "free works in the shell" "$out" "TESTPK"
has "info works in the shell" "$out" "rp06"
has "dirs works in the shell" "$out" "2 directories"

out=$(printf 'quit\nls\n' | "$ITSFS" shell "$T/pack.dsk" 2>&1)
hasnt "quit stops reading commands" "$out" "entries"

# ------------------------------------------------------------- the writer
#
# EVERY MUTATING FLOW ENDS WITH A CLEAN CHECK.  That is the rule the sibling
# projects are built on and it is what makes a writer's tests mean anything:
# `check` shares no code with the writer OR the reader, so a pack it calls clean
# after a mutation is a third opinion rather than the writer agreeing with
# itself.

mkfixture "$T/w1.dsk"
printf 'HELLO FROM ITSFS.\nA SECOND LINE.\n' > "$T/put.txt"

out=$("$ITSFS" put "$T/w1.dsk" 'TEST;NEW FILE' "$T/put.txt" 2>&1); rc=$?
chk "put writes a file" "$rc" "0"

out=$("$ITSFS" check "$T/w1.dsk" 2>&1); rc=$?
chk "...and the pack still checks clean" "$rc" "0"

"$ITSFS" cat "$T/w1.dsk" 'TEST;NEW FILE' > "$T/back.txt" 2>/dev/null
if cmp -s "$T/put.txt" "$T/back.txt"; then
	ok "...and it reads back byte-identical to what went in"
else
	no "...and it reads back byte-identical to what went in"
fi

out=$("$ITSFS" ls -l "$T/w1.dsk" TEST | grep '^NEW')
chk "...with the right length" "$(echo "$out" | awk '{print $3}')" "7"

# THE NAME AREA IS SORTED, and ITS keeps it that way -- 6,056 entries on the
# reference pack, none out of order.  A writer that appended instead would
# produce a directory ITS's own lookup walks wrongly.
"$ITSFS" put "$T/w1.dsk" 'TEST;AAA FIRST' "$T/put.txt" >/dev/null 2>&1
"$ITSFS" put "$T/w1.dsk" 'TEST;ZZZ LAST' "$T/put.txt" >/dev/null 2>&1
out=$("$ITSFS" ls "$T/w1.dsk" TEST | grep -v '^#' | awk '{print $1}' | tr '\n' ' ')
chk "entries stay in SIXBIT order as files are added" "$out" "A AAA HELLO NEW NUL ZZZ "

out=$("$ITSFS" check "$T/w1.dsk" 2>&1); rc=$?
chk "...and three more files later it is still clean" "$rc" "0"

# PUT THEN DEL IS A NO-OP AT THE FILE SYSTEM LEVEL.  Not byte-identical -- the
# descriptor area keeps its hole, as it does under ITS -- but nothing a reader
# or a checker can see has changed.
mkfixture "$T/w2.dsk"
"$ITSFS" manifest "$T/w2.dsk" > "$T/before.mf" 2>/dev/null
"$ITSFS" put "$T/w2.dsk" 'TEST;TEMP FILE' "$T/put.txt" >/dev/null 2>&1
"$ITSFS" del "$T/w2.dsk" 'TEST;TEMP FILE' >/dev/null 2>&1
"$ITSFS" manifest "$T/w2.dsk" > "$T/after.mf" 2>/dev/null

if diff -q "$T/before.mf" "$T/after.mf" >/dev/null 2>&1; then
	ok "put then del leaves the file system exactly as it was"
else
	no "put then del leaves the file system exactly as it was"
fi

out=$("$ITSFS" check "$T/w2.dsk" 2>&1); rc=$?
chk "...and clean" "$rc" "0"

# A file of no words at all.  FSDEFS: "A zero length file is described as two
# bytes: UDWPH then 0."
mkfixture "$T/w3.dsk"
: > "$T/empty.txt"
out=$("$ITSFS" put "$T/w3.dsk" 'TEST;EMPTY FILE' "$T/empty.txt" 2>&1); rc=$?
chk "a zero-length file can be written" "$rc" "0"
out=$("$ITSFS" check "$T/w3.dsk" 2>&1); rc=$?
chk "...and checks clean" "$rc" "0"
out=$("$ITSFS" ls -l "$T/w3.dsk" TEST | grep '^EMPTY')
chk "...and holds no blocks" "$(echo "$out" | awk '{print $4}')" "0"

# -w round-trips a file of arbitrary words, which is what makes get/put a pair
# that survives a binary.
mkfixture "$T/w4.dsk"
"$ITSFS" get -w "$T/w4.dsk" 'TEST;HELLO TXT' "$T/words.bin" >/dev/null 2>&1
"$ITSFS" put -w "$T/w4.dsk" 'TEST;COPY BIN' "$T/words.bin" >/dev/null 2>&1
"$ITSFS" get -w "$T/w4.dsk" 'TEST;COPY BIN' "$T/words2.bin" >/dev/null 2>&1

if cmp -s "$T/words.bin" "$T/words2.bin"; then
	ok "get -w then put -w round-trips a file word for word"
else
	no "get -w then put -w round-trips a file word for word"
fi

# --- mkdir.  IT ALLOCATES NO BLOCKS: every one of the NUDSL directory blocks is
# locked out in the TUT whether a directory lives there or not, so making one is
# an MFD entry and a zeroed block and the allocation table does not move.

mkfixture "$T/m1.dsk"
out=$("$ITSFS" mkdir "$T/m1.dsk" NEWDIR 2>&1); rc=$?
chk "mkdir makes a directory" "$rc" "0"

out=$("$ITSFS" check "$T/m1.dsk" 2>&1); rc=$?
chk "...and the pack still checks clean" "$rc" "0"

out=$("$ITSFS" dirs "$T/m1.dsk")
has "...and it is in the MFD" "$out" "NEWDIR"
has "...which now holds three" "$out" "3 directories"

# AN EMPTY DIRECTORY HAS UDNAMP == the block size, which is what ITS writes
# (QSKON: `MOVEI A,2000 / MOVEM A,UDNAMP-1(B)`).  This reader REFUSED that for
# four phases and nothing noticed, because every directory on the reference pack
# has at least one file in it.  Writing mkdir is what found it.
out=$("$ITSFS" ls "$T/m1.dsk" NEWDIR 2>&1); rc=$?
chk "an empty directory reads, rather than being refused as malformed" "$rc" "0"
has "...as empty" "$out" "0 entries"

out=$("$ITSFS" put "$T/m1.dsk" 'NEWDIR;FIRST FILE' "$T/put.txt" 2>&1); rc=$?
chk "a file can be put into a directory we made" "$rc" "0"
"$ITSFS" cat "$T/m1.dsk" 'NEWDIR;FIRST FILE' > "$T/mback.txt" 2>/dev/null

if cmp -s "$T/put.txt" "$T/mback.txt"; then
	ok "...and reads back byte-identical"
else
	no "...and reads back byte-identical"
fi

"$ITSFS" put "$T/m1.dsk" 'NEWDIR;AAA X' "$T/put.txt" >/dev/null 2>&1
"$ITSFS" put "$T/m1.dsk" 'NEWDIR;ZZZ X' "$T/put.txt" >/dev/null 2>&1
out=$("$ITSFS" ls "$T/m1.dsk" NEWDIR | grep -v '^#' | awk '{print $1}' | tr '\n' ' ')
chk "...and a fresh name area sorts from the first entry on" "$out" "AAA FIRST ZZZ "

out=$("$ITSFS" check "$T/m1.dsk" 2>&1); rc=$?
chk "...and the pack is still clean" "$rc" "0"

out=$("$ITSFS" mkdir "$T/m1.dsk" NEWDIR 2>&1); rc=$?
chk "mkdir refuses a name already in the MFD" "$rc" "1"
has "...saying where it is" "$out" "already in the MFD"

out=$("$ITSFS" mkdir "$T/m1.dsk" 'lower' 2>&1); rc=$?
chk "mkdir refuses a name that is not SIXBIT" "$rc" "1"

# A FREED SLOT IS REUSED BEFORE THE AREA IS EXTENDED, which is what QSKONC does
# and what keeps a pack from running out of its fixed number of directory blocks
# one deletion at a time.
#
# The slot freed here belongs to a directory that is EMPTY.  The first version
# of this test zeroed a live one, and `check` was right to object: nothing then
# points at that UFD, so the blocks its files hold are marked in use and claimed
# by nobody.  Removing a directory that still has files in it is a real
# operation with a real consequence, and it is not this test's subject.
mkfixture "$T/m2.dsk"
"$ITSFS" mkdir "$T/m2.dsk" FIRSTD >/dev/null 2>&1
before=$("$ITSFS" dump -z -w 2 "$T/m2.dsk" $MFDBLK | tail -1 | awk '{print $2}')
chk "mkdir extended the MFD name area by one slot" "$before" "000000001772"

# Zero the name FIRSTD left behind: an empty directory, so nothing is orphaned.
poke "$T/m2.dsk" $MFDBLK 1018 0
"$ITSFS" mkdir "$T/m2.dsk" REUSED >/dev/null 2>&1
out=$("$ITSFS" dirs "$T/m2.dsk")
has "mkdir reuses a freed MFD slot" "$out" "REUSED"
hasnt "...rather than leaving the old name" "$out" "FIRSTD"
namp=$("$ITSFS" dump -z -w 2 "$T/m2.dsk" $MFDBLK | tail -1 | awk '{print $2}')
chk "...without extending the name area again" "$namp" "$before"

out=$("$ITSFS" check "$T/m2.dsk" 2>&1); rc=$?
chk "...and the result checks clean" "$rc" "0"

# --- mkfs.  A file system where nothing was, following NSALV's own MFDINN,
# TUTINI and MARK69.  The image is sparse: an rp06 is 302 MB of mostly zeros.

out=$("$ITSFS" mkfs "$T/f1.dsk" NEWPAK 2>&1); rc=$?
chk "mkfs makes a file system" "$rc" "0"

out=$("$ITSFS" check "$T/f1.dsk" 2>&1); rc=$?
chk "...and it checks clean with nothing in it" "$rc" "0"
has "...with the whole structural claim in one line" "$out" "505 locked out"
has "...and nothing allocated" "$out" "claimed        0 blocks, in 0 files"

out=$("$ITSFS" info "$T/f1.dsk")
has "mkfs writes the MFD check word" "$out" "an ITS master file directory"
has "...an empty MFD" "$out" "directories   0"
has "...and the drive it was made for" "$out" "rp06"

out=$("$ITSFS" free "$T/f1.dsk")
has "mkfs writes the pack ID into the table" "$out" "NEWPAK"

# It is USABLE, which is the part a structural check does not cover.
"$ITSFS" mkdir "$T/f1.dsk" SYS >/dev/null 2>&1
"$ITSFS" put "$T/f1.dsk" 'SYS;HELLO TXT' "$T/put.txt" >/dev/null 2>&1
"$ITSFS" cat "$T/f1.dsk" 'SYS;HELLO TXT' > "$T/fback.txt" 2>/dev/null

if cmp -s "$T/put.txt" "$T/fback.txt"; then
	ok "a directory and a file can be made on it, and read back"
else
	no "a directory and a file can be made on it, and read back"
fi

out=$("$ITSFS" check "$T/f1.dsk" 2>&1); rc=$?
chk "...and it is still clean" "$rc" "0"

out=$("$ITSFS" mkfs "$T/f1.dsk" AGAIN 2>&1); rc=$?
chk "mkfs will not overwrite an image without -f" "$rc" "1"
has "...and says what -f would do" "$out" "THIS DESTROYS IT"

# FSDEFS asserts NUDSL*LMNBLK+LMIBLK <= 2000 octal at assembly time, so a slot
# count that breaks it is one no ITS could have been built with.
out=$("$ITSFS" mkfs -f -u 900 "$T/f2.dsk" TOOBIG 2>&1); rc=$?
chk "mkfs refuses more directory slots than an MFD holds" "$rc" "1"
has "...citing the assertion FSDEFS makes" "$out" "NUDSL*LMNBLK+LMIBLK"

out=$("$ITSFS" mkfs -f -s 99999 "$T/f3.dsk" TOOBIG 2>&1); rc=$?
chk "mkfs refuses a swapping area bigger than the drive" "$rc" "1"

out=$("$ITSFS" mkfs -f "$T/f4.dsk" 'lower' 2>&1); rc=$?
chk "mkfs refuses a pack ID that is not SIXBIT" "$rc" "1"

# A different drive, to prove the geometry is a parameter rather than rp06
# baked in.  An rm03 has a different NBLKSC, NTUTBL and MFD block.
out=$("$ITSFS" mkfs -f -d rm03 "$T/f5.dsk" RM03PK 2>&1); rc=$?
chk "mkfs makes an rm03 file system too" "$rc" "0"
out=$("$ITSFS" check "$T/f5.dsk" 2>&1); rc=$?
chk "...and that checks clean" "$rc" "0"
out=$("$ITSFS" info "$T/f5.dsk")
has "...with the rm03's own geometry" "$out" "rm03"
has "...and its own MFD block" "$out" "MFD           block 7379"

# --- the refusals.  A writer's refusals matter more than its successes: each
# one below leaves the pack usable, and the test checks that as well as the exit.

mkfixture "$T/r1.dsk"
"$ITSFS" manifest "$T/r1.dsk" > "$T/r1.mf" 2>/dev/null

out=$("$ITSFS" put "$T/r1.dsk" 'TEST;HELLO TXT' "$T/put.txt" 2>&1); rc=$?
chk "put refuses to overwrite an existing file" "$rc" "1"
has "...saying so" "$out" "already exists"

out=$("$ITSFS" put "$T/r1.dsk" 'TEST;lower case' "$T/put.txt" 2>&1); rc=$?
chk "put refuses a name that is not SIXBIT" "$rc" "1"
has "...and says there is no lower case" "$out" "no lower case"

out=$("$ITSFS" put "$T/r1.dsk" 'TEST;TOOLONGNAME X' "$T/put.txt" 2>&1); rc=$?
chk "put refuses a name too long, rather than truncating it" "$rc" "2"

out=$("$ITSFS" put "$T/r1.dsk" 'NOSUCH;A B' "$T/put.txt" 2>&1); rc=$?
chk "put refuses a directory that is not in the MFD" "$rc" "1"

printf 'high \377 bit\n' > "$T/bin.txt"
out=$("$ITSFS" put "$T/r1.dsk" 'TEST;BINARY X' "$T/bin.txt" 2>&1); rc=$?
chk "put refuses a byte that is not seven-bit rather than masking it" "$rc" "1"
has "...and says to use -w" "$out" "use -w"

out=$("$ITSFS" del "$T/r1.dsk" 'TEST;NOSUCH FILE' 2>&1); rc=$?
chk "del refuses a file that is not there" "$rc" "1"

# A DIRECTORY IS ONE BLOCK AND CAN BE FULL.  UDESCP is the free pointer into the
# descriptor area; setting it just below the name area leaves no room, and ITS
# has no way to grow a UFD -- so this is a refusal, not an allocation problem.
mkfixture "$T/r2.dsk"
poke "$T/r2.dsk" 498 0 5980
out=$("$ITSFS" put "$T/r2.dsk" 'TEST;NEW FILE' "$T/put.txt" 2>&1); rc=$?
chk "put refuses when the directory is full" "$rc" "1"
has "...saying which two numbers met" "$out" "is full"

# AND EVERY REFUSAL LEAVES THE PACK AS IT WAS.  This is the property that makes
# the others safe to rely on: a writer that half-does something is worse than
# one that cannot do it.
"$ITSFS" manifest "$T/r1.dsk" > "$T/r1b.mf" 2>/dev/null

if diff -q "$T/r1.mf" "$T/r1b.mf" >/dev/null 2>&1; then
	ok "six refusals later, the pack is unchanged"
else
	no "six refusals later, the pack is unchanged"
fi

out=$("$ITSFS" check "$T/r1.dsk" 2>&1); rc=$?
chk "...and still clean" "$rc" "0"

# THE IN-USE CHECK.  There is no lock to take, so the only signal is that
# another process holds the file open -- which is what an emulator with the pack
# attached looks like.
mkfixture "$T/r3.dsk"
(
	exec 9< "$T/r3.dsk"
	"$ITSFS" put "$T/r3.dsk" 'TEST;NEW FILE' "$T/put.txt" > "$T/inuse.out" 2>&1
	echo $? > "$T/inuse.rc"
)
rc=$(cat "$T/inuse.rc")

if [ "$rc" = "0" ]; then
	# /proc is how this is detected, so a host without it cannot tell.
	ok "the in-use check did not fire (no /proc: it cannot tell, and says so in write.c)"
else
	chk "put refuses an image another process has open" "$rc" "1"
	has "...naming the process" "$(cat "$T/inuse.out")" "refusing to write"
fi

(
	exec 9< "$T/r3.dsk"
	ITSFS_IGNORE_INUSE=1 "$ITSFS" put "$T/r3.dsk" 'TEST;NEW FILE' "$T/put.txt" >/dev/null 2>&1
	echo $? > "$T/inuse2.rc"
)
chk "...and ITSFS_IGNORE_INUSE overrides it" "$(cat "$T/inuse2.rc")" "0"

# ------------------------------------------------------------------ tapes
#
# The CONTAINER layer: SIMH .tap framing, records and tape marks.  What is
# inside the records is a separate format and this reads none of it.
#
# The fixture is built here, byte by byte, like every other one: a leading
# length, the record, a trailing length, and 0 for a tape mark.

# emit_le32 <value>
emit_le32() {
	_v=$1; _i=0

	while [ $_i -lt 4 ]; do
		printf "\\$(printf '%03o' "$(((_v >> (8 * _i)) & 255))")"
		_i=$((_i + 1))
	done
}

# A tape of two files: one record of 10 bytes, a mark, two of 10, two marks.
{
	emit_le32 10; printf 'AAAAAAAAAA'; emit_le32 10
	emit_le32 0
	emit_le32 10; printf 'BBBBBBBBBB'; emit_le32 10
	emit_le32 10; printf 'CCCCCCCCCC'; emit_le32 10
	emit_le32 0
	emit_le32 0
} > "$T/t1.tap"

out=$("$ITSFS" tape "$T/t1.tap" 2>&1); rc=$?
chk "tape reads the framing" "$rc" "0"
has "...counting the records" "$out" "3 records, 3 tape marks"
has "...and the data" "$out" "30 bytes of data"
has "...and separating the files at the marks" "$out" "file 0       1 records"
has "...both of them" "$out" "file 1       2 records"

# THE DEFAULT PACKING IS core, NOT le64.  A magtape is five frames per word and
# a disk is eight bytes; defaulting a tape to the disk convention would give
# files of the right length and entirely wrong words.
has "a tape defaults to the core packing, not the disk one" "$out" "packing       core"
has "...which is now confirmed against an ITS artifact" "$out" "core (confirmed)"

# THE ONE CHECK THE FORMAT OFFERS: the length is written on both sides so a tape
# can be read backwards, which is also the only way to notice a garbage header.
{
	emit_le32 10; printf 'AAAAAAAAAA'; emit_le32 11
} > "$T/t2.tap"
out=$("$ITSFS" tape "$T/t2.tap" 2>&1); rc=$?
chk "a record whose two lengths disagree is refused" "$rc" "1"
has "...saying which" "$out" "lengths disagree"

# A length that runs off the end of the file.
{
	emit_le32 9999; printf 'AAAA'
} > "$T/t3.tap"
out=$("$ITSFS" tape "$T/t3.tap" 2>&1); rc=$?
chk "a record that runs off the end is refused" "$rc" "1"
has "...saying so" "$out" "runs off the end"

# ...and one that is not a length at all.
{
	emit_le32 99999999; printf 'AAAA'
} > "$T/t4.tap"
out=$("$ITSFS" tape "$T/t4.tap" 2>&1); rc=$?
chk "an absurd record length is refused rather than allocated" "$rc" "1"

# End of medium stops the read without complaint.
{
	emit_le32 10; printf 'AAAAAAAAAA'; emit_le32 10
	emit_le32 4294967295
	printf 'this is not a record at all'
} > "$T/t5.tap"
out=$("$ITSFS" tape "$T/t5.tap" 2>&1); rc=$?
chk "end of medium ends the read cleanly" "$rc" "0"
has "...having read what came before it" "$out" "1 records"

# An odd-length record is padded to an even one, which is in the specification
# and is the kind of thing a reader gets wrong once.
{
	emit_le32 5; printf 'ABCDE'; printf '\0'; emit_le32 5
	emit_le32 5; printf 'FGHIJ'; printf '\0'; emit_le32 5
	emit_le32 0
} > "$T/t6.tap"
out=$("$ITSFS" tape "$T/t6.tap" 2>&1); rc=$?
chk "an odd-length record is padded to an even one" "$rc" "0"
has "...and both are read" "$out" "2 records"

# --------------------------------------------------------- DUMP save sets
#
# The ARCHIVE layer over the container above.  The fixture is built here, word
# by word, because the suite reads no real tape -- the tapes are not in this
# repo and never will be.  What a real one proves is a separate target,
# `make tape-test`, and what itstar says about one is in docs/validation.md.

# emit_word5 <octal word> -- one 36-bit word in the core packing, five bytes.
emit_word5() {
	_w=$(($1))
	printf "\\$(printf '%03o' "$(((_w >> 28) & 255))")"
	printf "\\$(printf '%03o' "$(((_w >> 20) & 255))")"
	printf "\\$(printf '%03o' "$(((_w >> 12) & 255))")"
	printf "\\$(printf '%03o' "$(((_w >> 4) & 255))")"
	printf "\\$(printf '%03o' "$((_w & 15))")"
}

# An AOBJN pointer for a header of n words: its left half is -n.
aobjn() { echo $(((0777777 - $1 + 1) << 18)); }

# A save set: a volume header, then two files, with the FIRST FILE'S HEADER
# SHARING THE VOLUME RECORD -- which is what every real ITS tape does and what
# a reader that assumed one header per record would silently lose.
{
	# record 1: volume header (4 words) + file header (4 words)
	emit_le32 40
	emit_word5 "$(aobjn 4)"; emit_word5 $(((1 << 18) | 0)); emit_word5 "$(sb 260627)"; emit_word5 0
	emit_word5 "$(aobjn 4)"; emit_word5 "$(sb TEST)"; emit_word5 "$(sb ONE)"; emit_word5 "$(sb TXT)"
	emit_le32 40
	# record 2: that file's data
	emit_le32 10; emit_word5 0111111111111; emit_word5 0222222222222; emit_le32 10
	emit_le32 0
	# record 3: a second file's header, with its data in the SAME record
	emit_le32 30
	emit_word5 "$(aobjn 4)"; emit_word5 "$(sb TEST)"; emit_word5 "$(sb TWO)"; emit_word5 "$(sb BIN)"
	emit_word5 0333333333333; emit_word5 0444444444444
	emit_le32 30
	emit_le32 0
	emit_le32 0
} > "$T/s1.tap"

out=$("$ITSFS" saveset "$T/s1.tap" 2>&1); rc=$?
chk "saveset reads a DUMP volume header" "$rc" "0"
has "...with the tape and reel" "$out" "tape 1, reel 0"
has "...the date, SIXBIT YYMMDD" "$out" "created 06/27/26"
has "...and the type" "$out" "type random"

# THE FIRST FILE HEADER SHARES THE VOLUME RECORD.  A reader that skipped to the
# next record here would list one file instead of two and say nothing about it.
has "the file header sharing the volume record is found" "$out" "TEST;ONE TXT"
has "...and so is the second file" "$out" "TEST;TWO BIN"
has "...both of them" "$out" "2 files, 0 links"

# ...and a file's data may share its own header's record, which is the same
# mistake in the other place.  TWO BIN's two data words are in its header record.
"$ITSFS" saveset -x "$T" "$T/s1.tap" >/dev/null 2>&1
chk "a file whose data shares its header record extracts 2 words" \
	"$(wc -c < "$T/TEST;TWO.BIN" | tr -d ' ')" "16"
chk "...and one whose data is in following records extracts 2 as well" \
	"$(wc -c < "$T/TEST;ONE.TXT" | tr -d ' ')" "16"

# A LINK, WITH ITS TARGET IN THE SAME RECORD AS ITS HEADER -- which is the case
# that cost this reader 61 of a real tape's 3,795 entries.  Reading a fresh
# record for the target swallowed the NEXT FILE'S HEADER, one per link, and the
# listing looked entirely plausible.  itstar reads a new record only if the
# current one is exhausted, and so does this now.
#
# The target is FN1, FN2, UFD -- not the order the header names them in.
{
	emit_le32 60
	emit_word5 "$(aobjn 4)"; emit_word5 $((1 << 18)); emit_word5 "$(sb 260627)"; emit_word5 0
	emit_word5 "$(aobjn 5)"; emit_word5 "$(sb TEST)"; emit_word5 "$(sb ALINK)"; emit_word5 "$(sb X)"
	emit_word5 $((1 << 18))
	emit_word5 "$(sb TFN1)"; emit_word5 "$(sb TFN2)"; emit_word5 "$(sb TDIR)"
	emit_le32 60
	emit_le32 0
	# ...and a file after it, which is what a reader that over-read would lose.
	emit_le32 20
	emit_word5 "$(aobjn 4)"; emit_word5 "$(sb TEST)"; emit_word5 "$(sb AFTER)"; emit_word5 "$(sb LINK)"
	emit_le32 20
	emit_le32 0
	emit_le32 0
} > "$T/s2.tap"

out=$("$ITSFS" saveset "$T/s2.tap" 2>&1)
has "a link is reported as one" "$out" "1 link"
has "...with its target read as FN1, FN2, UFD" "$out" "-> TDIR;TFN1 TFN2"
has "...and the file AFTER it is not swallowed" "$out" "TEST;AFTER LINK"
has "...so both entries are counted" "$out" "1 file, 1 link"

# A file that is not a save set at all is refused by the volume header's own
# length word, rather than read as one file of rubbish.
{
	emit_le32 10; emit_word5 0; emit_word5 0; emit_le32 10
} > "$T/s3.tap"
out=$("$ITSFS" saveset "$T/s3.tap" 2>&1); rc=$?
chk "a tape that is not a save set is refused" "$rc" "1"
has "...by its volume header" "$out" "does not begin with a DUMP volume header"

out=$("$ITSFS" saveset "$T/t2.tap" 2>&1); rc=$?
chk "and the container's own damage is still caught underneath" "$rc" "1"

# --- writing one.  A save set only this project can read proves nothing, so
# the round trip here is through our own reader and the real check is itstar,
# in `make tape-test`.

mkfixture "$T/sv.dsk"
out=$("$ITSFS" save "$T/sv.dsk" "$T/w.tap" 'TEST;HELLO TXT' 2>&1); rc=$?
chk "save writes a save set from a pack" "$rc" "0"

out=$("$ITSFS" saveset "$T/w.tap" 2>&1); rc=$?
chk "...which reads back" "$rc" "0"
has "...with the file on it" "$out" "TEST;HELLO TXT"
has "...and nothing else" "$out" "1 file, 0 links"

# THE WORDS MUST SURVIVE.  Extracting from the tape and from the pack must give
# the same bytes -- this is the only check that the data went through the
# record-buffering intact rather than merely the names.
"$ITSFS" saveset -x "$T" "$T/w.tap" >/dev/null 2>&1
"$ITSFS" get -w "$T/sv.dsk" 'TEST;HELLO TXT' "$T/direct.words" >/dev/null 2>&1

if [ -f "$T/TEST;HELLO.TXT" ] && cmp -s "$T/TEST;HELLO.TXT" "$T/direct.words"; then
	ok "the file's words come off the tape exactly as they come off the pack"
else
	no "the file's words come off the tape exactly as they come off the pack"
fi

# A link goes on the tape as a link, with its target -- not as the file it
# points at, which would put the same data on twice under two names.
out=$("$ITSFS" save "$T/sv.dsk" "$T/w2.tap" 'TEST;A LINK' 2>&1); rc=$?
chk "save writes a link" "$rc" "0"
out=$("$ITSFS" saveset "$T/w2.tap" 2>&1)
has "...as a link, with its target" "$out" "TEST;A LINK -> .;@ DDT"

out=$("$ITSFS" save "$T/sv.dsk" "$T/w3.tap" 'TEST;NOSUCH FILE' 2>&1); rc=$?
chk "save refuses a file that is not there" "$rc" "1"

out=$("$ITSFS" save "$T/sv.dsk" "$T/w4.tap" 'NOTANAME' 2>&1); rc=$?
chk "save refuses a name that is not DIR;FN1 FN2" "$rc" "1"

# ------------------------------------------------------------ command lines

out=$("$ITSFS" dump "$T/pack.dsk" 99999 2>&1); rc=$?
chk "a block past the end of the drive is refused" "$rc" "1"

out=$("$ITSFS" dump "$T/pack.dsk" 9..3 2>&1); rc=$?
chk "a backwards block range is refused" "$rc" "2"
has "...saying which way it runs" "$out" "backwards"

out=$("$ITSFS" dump -s "$T/rand.img" 1000 2>&1); rc=$?
chk "reading past the end of a small image is refused" "$rc" "1"
has "...rather than returning zeros" "$out" "past the end"

out=$("$ITSFS" info "$T/rand.img")
has "an image of no known drive size says so" "$out" "drive         unknown"
has "...and says which commands still work" "$out" "-d"

out=$("$ITSFS" ls "$T/rand.img" TEST 2>&1); rc=$?
chk "and a block-level command on it is refused" "$rc" "1"

out=$("$ITSFS" 2>&1); rc=$?
chk "no arguments prints the usage" "$rc" "2"
has "...listing the commands" "$out" "packings"

out=$("$ITSFS" nosuchcommand 2>&1); rc=$?
chk "an unknown command is refused" "$rc" "2"

out=$("$ITSFS" help); rc=$?
chk "help exits zero" "$rc" "0"

#
# THE DOCUMENTED COUNT MUST BE THIS COUNT.  A number written in prose in a
# validation document is read as a measurement, and a stale one is worse than
# none.  Not a `chk`, and not counted: it is a statement about the repository
# rather than about the code, and it must not change the total it is checking.
#
for doc in README.md docs/validation.md; do
	[ -f "$doc" ] || continue
	bad=$(grep -oE '[0-9]{2,4} checks' "$doc" | grep -v "^$pass checks" | sort -u)
	[ -z "$bad" ] || {
		echo
		echo "STALE: $doc says '$bad'; this run is $pass checks"
		fail=$((fail + 1))
	}
done

echo
echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ]
