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

# Build one.  Every number here is the value ITS itself would write; see its.h
# for the symbol each belongs to.
MFDBLK=19081
TUTBLK=19077
NUDSL=500
mkpack "$T/pack.dsk"

# UDNAMP is 1009, so the name area holds three entries.
# The MFD: header, then two directory names at the top of the block.  The
# POSITION of a name is the address of its directory:
#   word 1020 -> block (1020 - 1024 + 2*500)/2 = 498
#   word 1022 -> block (1022 - 1024 + 2*500)/2 = 499
poke "$T/pack.dsk" $MFDBLK 0 1 1020 0 0 0 0551646164416 $NUDSL
poke "$T/pack.dsk" $MFDBLK 1020 "$(sb TEST)" 0 "$(sb EMPTY)" 0

# The UFD for TEST, in block 498.  UDNAMP puts the name area at word 1014,
# which leaves room for two entries.
poke "$T/pack.dsk" 498 0 30 1009 "$(sb TEST)" 4 0

# Its descriptor area, at word 11 (UDDESC).  Six-bit bytes:
#   40 37 20   load address: (0<<12) | (037<<6) | 020 = block 2000
#   02         take 2 more, so blocks 2000, 2001, 2002
#   00         end
#   at byte 6:  a link, ".;@;DDT" in SIXBIT, ending in 0
#   at byte 14: 40 40 64 00, a one-block file at block 2100
poke "$T/pack.dsk" 498 11 0403720020000 0163340334444 0640040406400

# Three name blocks.  HELLO TXT: descriptor at byte 0, and 3 words in the last
# block, so the file is 2*1024 + 3 = 2051 words.  A LINK: the link bit set, and
# its descriptor at byte 6.  NUL TXT: one block, one word, at byte 14.
poke "$T/pack.dsk" 498 1009 "$(sb NUL)" "$(sb TXT)" $(((1 << 24) | 14)) 0 0
poke "$T/pack.dsk" 498 1014 "$(sb HELLO)" "$(sb TXT)" $((3 << 24)) 0 0
poke "$T/pack.dsk" 498 1019 "$(sb A)" "$(sb LINK)" $(((1 << 18) | 6)) 0 0

# Three blocks of text.  "HI" is 0110 0111 in seven-bit ASCII, five to a word.
poke "$T/pack.dsk" 2000 0 $(((0110 << 29) | (0111 << 22)))
poke "$T/pack.dsk" 2002 0 $(((0110 << 29) | (0111 << 22))) 0 0

# ...and one word holding  A NUL B NUL NUL, for the trailing-versus-interior
# question below.
poke "$T/pack.dsk" 2100 0 $(((0101 << 29) | (0102 << 15)))

# The TUT: a pack name, and blocks 0..38164 mapped.
poke "$T/pack.dsk" $TUTBLK 0 0 "$(sb TESTPK)" 3102 1551 0 38164 0

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

cp "$T/pack.dsk" "$T/bad.dsk"
poke "$T/bad.dsk" $MFDBLK 5 0123456701234
out=$("$ITSFS" dirs "$T/bad.dsk" 2>&1); rc=$?
chk "an MFD without its check word is refused" "$rc" "1"
has "...naming MDCHK" "$out" "not an MFD"
out=$("$ITSFS" info "$T/bad.dsk" 2>&1)
has "and info says so rather than pretending" "$out" "NOT |M.F.D.|"

cp "$T/pack.dsk" "$T/bad2.dsk"
poke "$T/bad2.dsk" $MFDBLK 1 9999
out=$("$ITSFS" dirs "$T/bad2.dsk" 2>&1); rc=$?
chk "an MDNAMP outside the block is refused" "$rc" "1"
has "...with the value" "$out" "9999"

cp "$T/pack.dsk" "$T/bad3.dsk"
poke "$T/bad3.dsk" 498 1 0
out=$("$ITSFS" ls "$T/bad3.dsk" TEST 2>&1); rc=$?
chk "a UFD with a zero UDNAMP is refused" "$rc" "1"
has "...as not a UFD" "$out" "not a UFD"

# A descriptor that takes blocks before any load address has set one.
cp "$T/pack.dsk" "$T/bad4.dsk"
poke "$T/bad4.dsk" 498 11 0020000000000
out=$("$ITSFS" ls -l "$T/bad4.dsk" TEST 2>&1); rc=$?
has "a take before any load address is refused" "$out" "take before any load address"
! died $rc && ok "...without dying" || no "...without dying (rc=$rc)"

# A load address pointing past the end of the drive.  077 77 77 is block 131071;
# an rp06 has 38305.
cp "$T/pack.dsk" "$T/bad5.dsk"
poke "$T/bad5.dsk" 498 11 0777777000000
out=$("$ITSFS" ls -l "$T/bad5.dsk" TEST 2>&1)
has "a block past the end of the drive is refused" "$out" "past the end of the drive"

# A descriptor with no terminating zero: every byte a "take 1".
cp "$T/pack.dsk" "$T/bad6.dsk"
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
cp "$T/pack.dsk" "$T/bad7.dsk"
poke "$T/bad7.dsk" $TUTBLK 4 100 50
out=$("$ITSFS" free "$T/bad7.dsk" 2>&1); rc=$?
chk "a TUT that ends before it begins is refused" "$rc" "1"

cp "$T/pack.dsk" "$T/bad8.dsk"
poke "$T/bad8.dsk" $TUTBLK 5 9999999
out=$("$ITSFS" free "$T/bad8.dsk" 2>&1); rc=$?
chk "a TUT mapping more blocks than it has room for is refused" "$rc" "1"
has "...saying so" "$out" "does not fit"

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
