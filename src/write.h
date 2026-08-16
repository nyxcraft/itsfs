/*
 * write.h -- THE ONE MUTATION PATH.
 *
 * Every command that changes a pack goes through this file and no other.  That
 * rule is the reason the project has one: s5fs and t10fs both learned that a
 * front end which grows its own idea of how to make a file becomes a second
 * writer, and two writers produce two subtly different file systems.
 *
 * THE THREE RULES THIS ENFORCES, each of which cost somebody a pack once:
 *
 *   1. REFUSE, DO NOT HALF-DO.  Every check that can fail happens before
 *      anything is written -- names, space in the directory, blocks in the TUT.
 *      A refusal leaves the pack byte-identical to how it was found, and says
 *      the number it refused at.
 *
 *   2. THE DIRECTORY ENTRY GOES LAST.  Data blocks, then the TUT, then the
 *      descriptor, and only then the name.  A write interrupted anywhere before
 *      the last step leaves blocks marked in use that no file claims -- which
 *      `check` reports as lost space and NSALV as a TUT difference, and which
 *      loses nothing.  The other order loses a file.
 *
 *   3. NEVER WRITE A PACK SOMEBODY ELSE HAS OPEN.  An emulator with the pack
 *      attached is writing to it too, and the result is not a file system.
 *      There is no lock to take -- SIMH takes none -- so the only available
 *      signal is that another process holds the file open.
 *
 * WHAT ITS ITSELF DOES, and this follows, because the point is to produce a
 * pack ITS accepts rather than one that merely reads back:
 *
 *   - A UFD's name area is SORTED.  6,056 entries on the reference pack, none
 *     out of order and no gaps.  `QRELOC` in disk.1228 places each new name in
 *     order; `QSQSH` closes the gap on removal.  So inserting shifts the
 *     entries below the insertion point down by LUNBLK and decrements UDNAMP.
 *   - Allocation SETS the TUT entry to 1 (`QGTK2: MOVEI B,1 / DPB B,TT`), and
 *     no block on the reference pack is referenced twice.
 *   - New files are not written below QSWAPA, the swapping area.
 *   - A deleted file's descriptor bytes are ZEROED in place; the area is not
 *     compacted.
 */
#ifndef WRITE_H
#define WRITE_H

#include <stdint.h>
#include <stddef.h>

#include "image.h"
#include "structure.h"

/*
 * A writable pack.  Held open for the duration of one operation, with the TUT
 * in memory: allocation reads and writes it many times and a pack is one file.
 */
typedef struct {
	its_image im;
	its_tut tut;
	int tut_dirty;
	unsigned wpb;
} its_writer;

/*
 * Open for writing.  Refuses if another process has the image open, unless
 * ITSFS_IGNORE_INUSE is set in the environment -- which exists because the
 * check cannot be perfect (it reads /proc, so off Linux it cannot tell) and a
 * tool that cannot be overridden gets worked around in worse ways.
 *
 * Returns 0, or -1 with a message.
 */
int itsw_open(its_writer *w, const char *path, const its_pack *pk, const its_drive *drv);
int itsw_close(its_writer *w); /* flushes the TUT; -1 if that failed */

/*
 * Allocate `n` free blocks, into `blocks`.  Returns 0, or -1 with a message
 * naming how many were available -- nothing is marked and the TUT is untouched
 * on failure, so a caller may try again for fewer.
 *
 * Blocks come back in ascending order, which is what makes the descriptor
 * encoder able to produce runs.
 */
int itsw_alloc(its_writer *w, uint64_t n, uint64_t *blocks);

/* Free them again: the TUT entry goes to zero.  A block the TUT calls
 * many-or-more cannot be freed correctly and is refused by name. */
int itsw_free(its_writer *w, const uint64_t *blocks, uint64_t n);

/* How many blocks are free, for the message a refusal wants to print. */
uint64_t itsw_nfree(const its_writer *w);

/*
 * Encode a block list as a UFD descriptor.  `bytes` receives the six-bit bytes,
 * `*nbytes` their count; `max` bounds it.  Returns 0, or -1 if it does not fit.
 *
 * Exported because the encoder is the exact inverse of the decoder in
 * structure.c, and the test suite checks that round trip directly rather than
 * only through a written pack.
 */
int itsw_desc_encode(const uint64_t *blocks, long nblocks, unsigned char *bytes, size_t max,
		     size_t *nbytes);

/*
 * Write a host file into a directory.  `words` and `nwords` are the file's
 * contents already converted; `put` in cmd_write.c does the conversion, so that
 * this layer never has an opinion about what a file means.
 *
 * Returns 0, or -1 with a message.
 */
int itsw_put(its_writer *w, const char *dir, const char *fn1, const char *fn2,
	     const uint64_t *words, uint64_t nwords);

/* Remove a file: free its blocks, zero its descriptor, close the name gap. */
int itsw_del(its_writer *w, const char *dir, const char *fn1, const char *fn2);

#endif /* WRITE_H */
