/*
 * itspack.h -- how a 36-bit PDP-10 word is stored in 8-bit bytes.
 *
 * The bottom layer, and a bigger abstraction than the byte-order codec a
 * byte-addressed filesystem needs.  An ITS disk is a sequence of 36-bit words,
 * and 36 does not tile onto 8: there is no "the" way to lay a word down in
 * bytes, only a set of conventions, and each emulator and container format
 * picked one.  So the packing is pluggable from the start, and nothing above
 * this layer ever sees a byte.
 *
 * The invariant, inherited from s5fs and t10fs and worth more here, not less:
 * NEVER lay a host integer over image bytes.  Every word enters through get()
 * and leaves through put(), masked to 36 bits.  A uint64_t carrying a word
 * always has its top 28 bits clear.
 *
 * Bit numbering is DEC's throughout: bit 0 is the most significant, bit 35 the
 * least.  A word is conventionally written as two 18-bit halves, `lh,,rh`,
 * which is exactly the first six and last six digits of the 12-digit octal.
 *
 * SIXBIT is NOT here.  It is an interpretation of a word's contents, not a way
 * of storing one; it lives in itstext.c, one layer up.  ITS block numbering is
 * not here either -- that is geometry, and it lives in itsgeom.c.
 *
 * PROVENANCE.  Each packing carries a status, printed by `itsfs packings`,
 * because "we implemented it" and "we know an ITS pack was stored this way" are
 * different claims and keeping them apart is the discipline of this project:
 *
 *   confirmed     measured against a real ITS image, or given frame by frame by
 *                 the manual for the hardware that does the packing.
 *   corroborated  an independent implementation lays it out the same way, but
 *                 nothing here has measured it against a real ITS artifact.
 *   structural    a container-level choice with no format to get wrong (the byte
 *                 order of a fixed-width container).
 *   unverified    believed to be the historical layout, NOT yet checked against
 *                 documentation, an implementation, or an artifact.  Do not
 *                 build a format decision on one of these until it is promoted.
 */
#ifndef ITSPACK_H
#define ITSPACK_H

#include <stdint.h>
#include <stddef.h>

#define ITS_WORD_BITS 36
#define ITS_WORD_MASK UINT64_C(0777777777777)

/* the two 18-bit halves, DEC's `lh,,rh` */
#define ITS_LH(w) (((uint64_t)(w) >> 18) & UINT64_C(0777777))
#define ITS_RH(w) ((uint64_t)(w) & UINT64_C(0777777))

/*
 * A disk SECTOR is 128 words on every drive ITS supported.  This is a hardware
 * fact, not a filesystem one: an ITS BLOCK is SECBLK sectors, and how many that
 * is depends on the drive.  See itsgeom.h.
 */
#define ITS_WORDS_PER_SECTOR 128

typedef enum {
	ITS_PACK_LE64 = 0, /* keep 0: the zero-initialised default */
	ITS_PACK_BE64,
	ITS_PACK_CORE,
	ITS_PACK_DBD9,
	ITS_NPACK
} its_packing;

/*
 * A packing converts between a group of `words` words and a group of `bytes`
 * bytes.  Groups exist because a packing need not be one word per fixed number
 * of bytes -- get/put take an index within the group so a dense packing (two
 * words in nine bytes, sharing a byte between them) drops in without changing a
 * caller.  dbd9 is exactly that, and it is why the interface is shaped this way
 * rather than as a single stride.
 *
 * A packing whose group holds more than one word may SHARE bytes between them.
 * put() therefore reads the byte it is modifying, and a group is only meaningful
 * once every word in it has been written -- which its_put_words guarantees and
 * no other caller may assume.
 */
typedef struct {
	const char *name;
	const char *status; /* confirmed / corroborated / structural /
			     * unverified -- see the header comment */
	const char *desc;
	unsigned words; /* words per group */
	unsigned bytes; /* bytes per group */
	uint64_t (*get)(const uint8_t *grp, unsigned i);
	void (*put)(uint8_t *grp, unsigned i, uint64_t w);
} its_pack;

/* The packing for a regime, or NULL if `p` is out of range. */
const its_pack *its_pack_for(its_packing p);

/* Parse "le64"/"be64"/"core"/"dbd9" (and a few aliases); ITS_NPACK if unknown. */
its_packing its_pack_parse(const char *s);
const char *its_pack_name(its_packing p);

/*
 * Bytes needed to hold `nwords` words, rounded up to a whole group.  This is the
 * ONE definition of that arithmetic: s5fs learned that open-coding a layout
 * calculation in four readers gets it wrong in three of them.
 */
size_t its_pack_bytes(const its_pack *pk, size_t nwords);

/*
 * Bulk convert.  `buf` must hold its_pack_bytes(pk, n) bytes.  A trailing
 * partial group is handled: on get, the missing words read as 0; on put, the
 * unused words in the final group are written as 0.
 */
void its_get_words(const its_pack *pk, const uint8_t *buf, uint64_t *w, size_t n);
void its_put_words(const its_pack *pk, uint8_t *buf, const uint64_t *w, size_t n);

#endif /* ITSPACK_H */
