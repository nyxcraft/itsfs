/*
 * structure.h -- reading an ITS file system: the MFD, a UFD, the TUT, and the
 * block list of a file.
 *
 * READ-ONLY, and deliberately so at this stage.  Nothing here writes; when a
 * writer arrives it goes in one file of its own and everything mutating goes
 * through it, which is the rule s5fs and t10fs are both built on.
 *
 * The shape of the thing being read:
 *
 *      MFD          one block, at NBLKS/2-1.  A header and a list of SIXBIT
 *                   directory names, whose POSITION gives each directory's
 *                   block -- there are no pointers.
 *      UFD          one block per directory.  A header, a descriptor area
 *                   growing up from word 11, and five-word name blocks growing
 *                   down from the end of the block.
 *      descriptor   a run-length program in six-bit bytes that produces a
 *                   file's block list; or, for a link, the target's name.
 *      TUT          three bits per block of the pack: a reference count, not a
 *                   bitmap.
 *
 * A DIRECTORY IS ONE BLOCK AND THAT IS THE WHOLE CAPACITY.  There is no growth,
 * no chaining and no second cluster: 1013 words hold both the descriptors and
 * the names, and when they meet the directory is full.  TOPS-10 grows a UFD;
 * ITS does not, and a reader written on the assumption that it does will look
 * for a continuation that is not there.
 */
#ifndef STRUCTURE_H
#define STRUCTURE_H

#include <stdint.h>
#include <stddef.h>

#include "image.h"
#include "its.h"
#include "itstext.h"

#define ITS_NAME_MAX (ITS_SIXBIT_CHARS + 1)

/* The master file directory: one block, held whole. */
typedef struct {
	uint64_t blk; /* where it was read from */
	uint64_t *w;  /* wpb words */
	unsigned wpb;
	uint64_t namp;	/* C(MDNAMP): first word of the name area */
	uint64_t nudsl; /* C(MDNUDS): how many directories the MFD can hold */
} its_mfd;

int its_mfd_read(its_image *im, its_mfd *m);
void its_mfd_free(its_mfd *m);

/*
 * Directory `i` of the MFD, counting from the start of the name area.  Returns
 * 0 and fills `name` and `blk`, 1 when `i` is past the end, -1 on a slot that
 * cannot be a directory.  An empty slot is skipped by the caller, not here --
 * `name` comes back empty and the caller decides what that means.
 */
int its_mfd_dir(const its_mfd *m, unsigned i, char name[ITS_NAME_MAX], uint64_t *blk);
unsigned its_mfd_slots(const its_mfd *m);

/* A user file directory: also one block, also held whole. */
typedef struct {
	uint64_t blk;
	uint64_t *w;
	unsigned wpb;
	uint64_t namp; /* C(UDNAMP) */
	char name[ITS_NAME_MAX];
} its_ufd;

int its_ufd_read(its_image *im, uint64_t blk, its_ufd *u);
void its_ufd_free(its_ufd *u);

/* One name block, decoded. */
typedef struct {
	char fn1[ITS_NAME_MAX];
	char fn2[ITS_NAME_MAX];
	unsigned word;	 /* where in the UFD block it sits */
	unsigned desc;	 /* UNDSCP: six-bit byte offset of the descriptor */
	unsigned pack;	 /* UNPKN */
	unsigned lastwc; /* UNWRDC: words in the last block; 0 means full */
	unsigned author; /* UNAUTH; all ones means "no directory" */
	unsigned bytesz; /* UNBYTE, raw */
	unsigned year, month, day;
	unsigned time; /* UNTIM, raw -- see its.h, the unit is not settled */
	int is_link;
	int deleted; /* any of UNIGFL set */
	uint64_t rndm, date, ref;
} its_ent;

/*
 * Walk the name area.  `*idx` is a word index; start it at u->namp and this
 * advances it.  Returns 1 on an entry, 0 at the end of the area, and skips
 * nothing -- an all-zero slot comes back with empty names so that a caller
 * counting free space sees it.
 */
int its_ufd_next(const its_ufd *u, unsigned *idx, its_ent *e);

/*
 * The block list of a file, decoded from its descriptor.
 *
 * `blocks` may be NULL to count without storing.  Returns the number of blocks,
 * or -1 with *err set to a fixed string saying which rule the descriptor broke.
 * A descriptor is untrusted input: every opcode is bounded, every block number
 * is checked against the drive, and the walk is capped at the size of the
 * directory block it lives in.
 */
long its_desc_blocks(const its_ufd *u, const its_drive *d, unsigned desc, uint64_t *blocks,
		     size_t max, const char **err);

/*
 * A link's target, rendered as `DIR;FN1 FN2`.  Same untrusted-input rules.
 * Returns 0, or -1 with *err set.
 */
int its_link_target(const its_ufd *u, unsigned desc, char *out, size_t outsz, const char **err);

/* How long a file is, in words, given its block count and UNWRDC. */
uint64_t its_file_words(unsigned wpb, long nblocks, unsigned lastwc);

/* The track utilization table, held whole (four blocks on an RP06). */
typedef struct {
	uint64_t *w;
	size_t nwords;
	uint64_t first, last; /* C(QFRSTB), C(QLASTB) */
	char pakid[ITS_NAME_MAX];
	uint64_t pknum, swapa, tutp;
} its_tut;

int its_tut_read(its_image *im, its_tut *t);
void its_tut_free(its_tut *t);

/*
 * The three-bit entry for a block: 0 free, 1..ITS_TUTMNY-1 that many
 * references, ITS_TUTMNY many-or-more, ITS_TUTLK locked out.  Returns -1 if the
 * block is outside what the TUT maps.
 */
int its_tut_entry(const its_tut *t, uint64_t blk);

#endif /* STRUCTURE_H */
