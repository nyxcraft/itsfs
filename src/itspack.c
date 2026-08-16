/*
 * itspack.c -- the word packings.  See itspack.h for the contract and for what
 * "confirmed" / "corroborated" / "structural" / "unverified" mean.
 *
 * Reference word for the table below: 0551646164416, SIXBIT "M.F.D." -- the
 * check word ITS writes at MDCHK in its master file directory, and the first
 * thing this project ever found on a real pack.  As bytes:
 *
 *   le64   0E E9 98 4E 0B 00 00 00      (36 bits in a 64-bit LE container)
 *   be64   00 00 00 0B 4E 98 E9 0E      (the same container, BE)
 *   core   B4 E9 8E 90 0E               (bits 0-31 in four frames, 32-35 last)
 *
 * The le64 row is not an illustration: those are literally the eight bytes at
 * word 5 of the MFD block of ~/its/out/simh/rp0.dsk.
 *
 * le64 is what a SIMH disk image uses, and for ITS it is MEASURED rather than
 * assumed -- every word of a 300 MB RP06 ITS pack decodes and re-encodes
 * byte-for-byte, which also proves nothing on that pack sets a bit outside the
 * 36.  See docs/validation.md.  Two independent implementations agree that this
 * is the container format rather than an artifact of running on an x86:
 *
 *   SIMH  PDP10/pdp10_rp.c attaches the drive with a 128-word sector and an
 *         8-byte transfer element, and sim_disk.c byte-swaps that element only
 *         when the HOST is big-endian.  So the file is defined as
 *         little-endian per 8-byte word, on any host.
 *
 *   libword  the PDP-10/its tree's own tools/dasm/libword/data8-word.c writes
 *            exactly this, under the name "data8", and warns on any word with
 *            bits set above 36 -- the same invariant the round trip measures.
 *            (Read for its DESCRIPTION of the container only.  libword is GPL;
 *            no code here comes from it.  See docs/sources.md.)
 */

#include "itspack.h"

#include <string.h>
#include <strings.h>

/* ---- le64: one word per 8-byte little-endian container ---- */
static uint64_t
le64_get(const uint8_t *b, unsigned i)
{
	const uint8_t *p = b + 8u * i;
	uint64_t v = 0;

	for (int k = 7; k >= 0; k--)
		v = (v << 8) | p[k];
	return v & ITS_WORD_MASK;
}

static void
le64_put(uint8_t *b, unsigned i, uint64_t w)
{
	uint8_t *p = b + 8u * i;

	w &= ITS_WORD_MASK;

	for (int k = 0; k < 8; k++)
		p[k] = (uint8_t)(w >> (8 * k));
}

/* ---- be64: the same container, big-endian ---- */
static uint64_t
be64_get(const uint8_t *b, unsigned i)
{
	const uint8_t *p = b + 8u * i;
	uint64_t v = 0;

	for (int k = 0; k < 8; k++)
		v = (v << 8) | p[k];
	return v & ITS_WORD_MASK;
}

static void
be64_put(uint8_t *b, unsigned i, uint64_t w)
{
	uint8_t *p = b + 8u * i;

	w &= ITS_WORD_MASK;

	for (int k = 0; k < 8; k++)
		p[k] = (uint8_t)(w >> (8 * (7 - k)));
}

/*
 * ---- core: five frames per word ----
 * Bits 0-7, 8-15, 16-23, 24-31 in the first four frames; bits 32-35 in the low
 * four bits of the fifth, its high nibble zero.
 *
 * This is the magtape core-dump convention, and the authority for it is the
 * hardware that does the splitting rather than any operating system: the driver
 * hands the formatter words and a mode, and the FORMATTER writes the frames.
 * TM03 Magnetic Tape Formatter User Guide, EK-OTM03-UG-003, 3rd printing July
 * 1979, page 2-18, Table 2-12 "PDP-10 Core Dump Mode - Format Code 0000":
 *
 *      Tape Frames   TP    T7    T6   T5   T4    T3    T2    T1    T0
 *                        (MSB)                                  (LSB)
 *          1         P    B0    B1   B2   B3    B4    B5    B6    B7
 *          2         P    B8    B9   B10  B11   B12   B13   B14   B15
 *          3         P    B16   B17  B18  B19   B20   B21   B22   B23
 *          4         P    B24   B25  B26  B27   B28   B29   B30   B31
 *          5         P     0     0    0    0    B32   B33   B34   B35
 *
 * B0 is the word's MSB in DEC numbering and TP is the parity track.
 *
 * CONFIRMED, AGAINST AN ITS ARTIFACT, and here is the measurement.  It sat at
 * `corroborated` until phase 9 on the principle that borrowing t10fs's
 * measurement and calling it ours would be dishonest: the manual settles the
 * layout, but no ITS tape had been decoded through THIS code.
 *
 * `out/simh/salv.tape`, the tape NSALV boots from, is 79,890 bytes of program.
 * That number is a whole multiple of five and is NOT a multiple of eight
 * (9,986.25), so the file cannot be one word per eight bytes and can be one per
 * five.  Decoding it here and reading each word as five seven-bit characters
 * finds, in the middle of the image:
 *
 *      "Salvager"                 word 9259
 *      "Use MFD from unit"        word 9371
 *      "unprotected in old TUT"   word 10026
 *
 * which are three of the exact strings this project has watched NSALV print on
 * a console -- the third of them about a pack it damaged on purpose.  Text that
 * came out of the emulated machine, found by this decoder in the file the
 * machine loaded it from.
 *
 * And the round trip: `itsfs tape -x` decodes every record of that tape into
 * words, and re-encoding them reproduces both host files byte for byte.
 */
static uint64_t
core_get(const uint8_t *b, unsigned i)
{
	const uint8_t *p = b + 5u * i;

	return (((uint64_t)p[0] << 28) | ((uint64_t)p[1] << 20) |
		((uint64_t)p[2] << 12) | ((uint64_t)p[3] << 4) |
		((uint64_t)p[4] & 0x0Fu)) &
	       ITS_WORD_MASK;
}

static void
core_put(uint8_t *b, unsigned i, uint64_t w)
{
	uint8_t *p = b + 5u * i;

	w &= ITS_WORD_MASK;
	p[0] = (uint8_t)(w >> 28);
	p[1] = (uint8_t)(w >> 20);
	p[2] = (uint8_t)(w >> 12);
	p[3] = (uint8_t)(w >> 4);
	p[4] = (uint8_t)(w & 0x0Fu);
}

/*
 * ---- dbd9: two words in nine bytes, no waste ----
 *
 * KLH10's "Disk_BigEnd_Double", and the packing of an ITS pack built with
 * `make EMULATOR=klh10` rather than under SIMH.  A plain big-endian bit stream:
 * the first 36 bits are word 0, the next 36 are word 1, so byte 4 is SHARED --
 * its high nibble ends the first word and its low nibble begins the second.
 *
 *      byte 0   word 0, bits 0-7        byte 5   word 1, bits 4-11
 *      byte 1   word 0, bits 8-15       byte 6   word 1, bits 12-19
 *      byte 2   word 0, bits 16-23      byte 7   word 1, bits 20-27
 *      byte 3   word 0, bits 24-31      byte 8   word 1, bits 28-35
 *      byte 4   SHARED: word 0 bits 32-35, then word 1 bits 0-3
 *
 * The shared byte is why put() reads before it writes, and why a group is only
 * meaningful once BOTH its words have been written.  its_put_words guarantees
 * that -- a full group always gets both, and a partial final group is zeroed
 * first.  Nothing else may call put() directly.
 *
 * This is the one packing here with more than one word per group, so it is what
 * actually exercises the group interface rather than merely fitting it.
 *
 * VERIFIED AGAINST KLH10'S OWN CODE, not against a description of it.  KLH10
 * declares the format in vdisk.h --
 *
 *      vdk_fmt(VDK_FMT_DBD9, "DBD9", "Disk_BigEnd_Double (9/2) (H36)",
 *                                      9, cvtfr_dbd9, cvtto_dbd9)
 *
 * -- and cvtfr_dbd9 in vdisk.c builds each word as LRHSET(w, lh, rh) from the
 * same nine bytes this does.  Feeding 200,000 random 9-byte groups through both
 * formulas gives no disagreement, so the two are the same function and not
 * merely the same intention.
 *
 * IT IS `confirmed` AS OF THE KLH10 BUILD, and both the failed attempt and the
 * one that worked are worth recording.
 *
 * THE ATTEMPT THAT FAILED.  KLH10's DSKDMP was pointed at a pack this repacked
 * into dbd9, and answered MFDCLB, "M.F.D. clobbered".  That looked like a
 * finding until the CONTROL was run -- the same KLH10, the same DSKDMP, the
 * UNTOUCHED le64 pack read through KLH10's own "SIMH" format -- which answers
 * MFDCLB as well.  A setup that cannot tell a good pack from a bad one says
 * nothing about a packing.  (That DSKDMP is build 216 and the pack's own is
 * 217; it probably wants a machine this is not.)
 *
 * WHAT SETTLED IT was building KLH10 from the tree, which turns out to ship
 * `vdkfmt` -- KLH10's own disk-format converter, so an artifact written by
 * KLH10's code rather than by ours:
 *
 *	vdkfmt ip=rp0.dsk op=klh10.dbd9 ifmt=SIMH ofmt=DBD9 dt=RP06
 *
 * Its output is byte-identical to `itsfs repack -P dbd9` over every one of the
 * 177,776,640 bytes it wrote, and `itsfs check -p dbd9` reads it to the same
 * 6719 free / 30940 in use / 505 locked out / 247 directories / 5657 files as
 * the le64 original.  Two directions, one artifact, neither of them ours.
 *
 * THE SIZE DIFFERENCE IS NOT DAMAGE, which is worth writing down because it
 * looks exactly like truncation.  vdkfmt's copy loop is
 *
 *	if (!zerosector(wbuff, 128))
 *		err = devwrite(&dvo, nsect, wbuff);
 *
 * -- it never writes an all-zero sector, so the file simply STOPS at the last
 * non-zero one, 1,060 sectors short of the drive.  Every sector it did write is
 * at its true offset, and our trailing 610,560 bytes are all zero.  A real
 * KLH10 pack therefore does not have the nominal size, which is why the
 * size-based drive inference refuses it and `-d rp06` has to be given.
 */
static uint64_t
dbd9_get(const uint8_t *b, unsigned i)
{
	if (i == 0)
		return (((uint64_t)b[0] << 28) | ((uint64_t)b[1] << 20) |
			((uint64_t)b[2] << 12) | ((uint64_t)b[3] << 4) |
			((uint64_t)b[4] >> 4)) &
		       ITS_WORD_MASK;
	return ((((uint64_t)b[4] & 0x0Fu) << 32) | ((uint64_t)b[5] << 24) |
		((uint64_t)b[6] << 16) | ((uint64_t)b[7] << 8) | (uint64_t)b[8]) &
	       ITS_WORD_MASK;
}

static void
dbd9_put(uint8_t *b, unsigned i, uint64_t w)
{
	w &= ITS_WORD_MASK;

	if (i == 0) {
		b[0] = (uint8_t)(w >> 28);
		b[1] = (uint8_t)(w >> 20);
		b[2] = (uint8_t)(w >> 12);
		b[3] = (uint8_t)(w >> 4);
		b[4] = (uint8_t)((b[4] & 0x0Fu) | ((w & 0x0Fu) << 4));
		return;
	}

	b[4] = (uint8_t)((b[4] & 0xF0u) | (uint8_t)(w >> 32));
	b[5] = (uint8_t)(w >> 24);
	b[6] = (uint8_t)(w >> 16);
	b[7] = (uint8_t)(w >> 8);
	b[8] = (uint8_t)w;
}

/* clang-format off */	/* packing table: the columns are the documentation */
static const its_pack packs[ITS_NPACK] = {
	[ITS_PACK_LE64] = { "le64", "confirmed",    "one word per 64-bit little-endian container (SIMH disk images; libword calls it data8)", 1, 8, le64_get, le64_put },
	[ITS_PACK_BE64] = { "be64", "structural",   "one word per 64-bit big-endian container",                                              1, 8, be64_get, be64_put },
	[ITS_PACK_CORE] = { "core", "confirmed",    "five frames per word (magtape core-dump mode; TM03 user guide table 2-12)",              1, 5, core_get, core_put },
	[ITS_PACK_DBD9] = { "dbd9", "confirmed",    "two words in nine bytes, no waste (KLH10 disk images; its H36 tape format)",             2, 9, dbd9_get, dbd9_put },
};

static const struct {
	const char *alias;
	its_packing p;
} aliases[] = {
	{ "data8",   ITS_PACK_LE64 },	/* libword's name for it */
	{ "simh",    ITS_PACK_LE64 },
	{ "le",      ITS_PACK_LE64 },
	{ "be",      ITS_PACK_BE64 },
	{ "coredump",ITS_PACK_CORE },
	{ "tape",    ITS_PACK_CORE },
	{ "klh10",   ITS_PACK_DBD9 },
	{ "h36",     ITS_PACK_DBD9 },
};

/* clang-format on */

const its_pack *
its_pack_for(its_packing p)
{
	if (p < 0 || p >= ITS_NPACK)
		return NULL;
	return &packs[p];
}

its_packing
its_pack_parse(const char *s)
{
	if (s == NULL)
		return ITS_NPACK;

	for (int i = 0; i < ITS_NPACK; i++)
		if (strcasecmp(s, packs[i].name) == 0)
			return (its_packing)i;

	for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; i++)
		if (strcasecmp(s, aliases[i].alias) == 0)
			return aliases[i].p;

	return ITS_NPACK;
}

const char *
its_pack_name(its_packing p)
{
	const its_pack *pk = its_pack_for(p);

	return pk ? pk->name : "?";
}

size_t
its_pack_bytes(const its_pack *pk, size_t nwords)
{
	size_t groups = (nwords + pk->words - 1) / pk->words;

	return groups * pk->bytes;
}

void
its_get_words(const its_pack *pk, const uint8_t *buf, uint64_t *w, size_t n)
{
	size_t done = 0;

	while (done < n) {
		const uint8_t *grp = buf + (done / pk->words) * pk->bytes;

		for (unsigned i = 0; i < pk->words && done < n; i++, done++)
			w[done] = pk->get(grp, i);
	}
}

void
its_put_words(const its_pack *pk, uint8_t *buf, const uint64_t *w, size_t n)
{
	size_t done = 0;

	/*
	 * A partial final group is zeroed before anything is written into it,
	 * because a packing may SHARE a byte between two words: writing only
	 * the first word of a dbd9 group would otherwise leave the other half
	 * of byte 4 holding whatever was there before.
	 */
	if (pk->words > 1 && n % pk->words != 0)
		memset(buf + (n / pk->words) * pk->bytes, 0, pk->bytes);

	while (done < n) {
		uint8_t *grp = buf + (done / pk->words) * pk->bytes;

		for (unsigned i = 0; i < pk->words && done < n; i++, done++)
			pk->put(grp, i, w[done]);
	}
}
