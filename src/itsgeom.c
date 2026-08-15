/*
 * itsgeom.c -- the drive table, and block-number arithmetic.
 *
 * Every row is transcribed from that drive's own parameter file in the ITS
 * source tree; the file is named in the row so a constant can be checked
 * against its source without going looking.  The derived quantities are
 * COMPUTED, never transcribed -- see its_blks_per_cyl.
 */

#include "itsgeom.h"
#include "itspack.h"

#include <string.h>
#include <strings.h>

/* clang-format off */	/* the columns are the transcription; keep them aligned */
static const its_drive drives[ITS_NDRIVE] = {
	/* name    defs file            ncyls xcyls nheds nsecs secblk ntutbl */
	{ "rp04",  "SYSTEM;RP04 DEFS 1",   406,    5,   19,   20,     8,     2 },
	{ "rp06",  "SYSTEM;RP06 DEFS 1",   812,    3,   19,   20,     8,     4 },
	{ "rp07",  "SYSTEM;RP07 DEFS 1",   627,    3,   32,   43,     8,     9 },
	{ "rm03",  "SYSTEM;RM03 DEFS 5",   820,    3,    5,   30,     8,     2 },
	{ "rm80",  "SYSTEM;RM80 DEFS 4",   556,    3,   14,   30,     8,     3 },
};
/* clang-format on */

const its_drive *
its_drive_at(unsigned i)
{
	if (i >= ITS_NDRIVE)
		return NULL;
	return &drives[i];
}

const its_drive *
its_drive_by_name(const char *name)
{
	if (name == NULL)
		return NULL;

	for (unsigned i = 0; i < ITS_NDRIVE; i++)
		if (strcasecmp(name, drives[i].name) == 0)
			return &drives[i];

	return NULL;
}

/*
 * NBLKSC == NHEDS*NSECS/SECBLK, and the division is MIDAS's: it truncates.
 *
 * THE REMAINDER IS THE POINT.  An RP06 cylinder has 380 sectors and holds 47
 * ITS blocks, using 376 of them; the last four are addressable by the hardware
 * and reachable by no block number.  Computing this rather than transcribing it
 * is deliberate -- the RP07 divides exactly (32*43/8 == 172) and the RM03 wastes
 * six sectors per cylinder rather than four, so a hand-written table would have
 * three different right answers to get wrong.
 */
unsigned
its_blks_per_cyl(const its_drive *d)
{
	return d->nheds * d->nsecs / d->secblk;
}

uint64_t
its_nblks(const its_drive *d)
{
	return (uint64_t)d->ncyls * its_blks_per_cyl(d);
}

uint64_t
its_tblks(const its_drive *d)
{
	return (uint64_t)(d->ncyls + d->xcyls) * its_blks_per_cyl(d);
}

uint64_t
its_nsectors(const its_drive *d)
{
	return (uint64_t)(d->ncyls + d->xcyls) * d->nheds * d->nsecs;
}

/* FSDEFS: MFDBLK==NBLKS/2-1.  The MFD sits in the middle of the pack, which is
 * a seek-time decision from a time when that mattered a great deal. */
uint64_t
its_mfd_block(const its_drive *d)
{
	return its_nblks(d) / 2 - 1;
}

/* FSDEFS: TUTBLK==MFDBLK-NTUTBL.  The TUT is NTUTBL blocks ending just below
 * the MFD, so the two things a salvager needs first are next to each other. */
uint64_t
its_tut_block(const its_drive *d)
{
	return its_mfd_block(d) - d->ntutbl;
}

int
its_blk_sector(const its_drive *d, uint64_t blk, uint64_t *sector)
{
	unsigned nblksc = its_blks_per_cyl(d);
	uint64_t cyl, within, srf, sec;

	if (blk >= its_tblks(d))
		return -1;

	cyl = blk / nblksc;
	within = (blk - cyl * nblksc) * d->secblk; /* sector within cylinder */
	srf = within / d->nsecs;
	sec = within - srf * d->nsecs;

	*sector = (cyl * d->nheds + srf) * d->nsecs + sec;
	return 0;
}

int
its_sector_blk(const its_drive *d, uint64_t sector, uint64_t *blk)
{
	unsigned percyl = d->nheds * d->nsecs;
	uint64_t cyl = sector / percyl;
	uint64_t within = sector - cyl * percyl;

	if (sector >= its_nsectors(d))
		return -1;

	/* The tail of each cylinder that no block number reaches. */
	if (within >= (uint64_t)its_blks_per_cyl(d) * d->secblk)
		return -1;

	*blk = cyl * its_blks_per_cyl(d) + within / d->secblk;
	return 0;
}

/*
 * Identify the drive by the size of the image.
 *
 * A pack is the whole physical surface -- spare cylinders included -- so the
 * size is (NCYLS+XCYLS) * NHEDS * NSECS sectors of 128 words, and no two drives
 * ITS supported give the same number.  An exact match is required: a truncated
 * or padded image is a fact the caller wants to know rather than something to
 * be tolerated, because every block address in it is still correct and only the
 * end is missing.
 */
const its_drive *
its_drive_by_size(uint64_t bytes, unsigned words_per_group, unsigned bytes_per_group)
{
	for (unsigned i = 0; i < ITS_NDRIVE; i++) {
		uint64_t words = its_nsectors(&drives[i]) * ITS_WORDS_PER_SECTOR;
		uint64_t want = (words / words_per_group) * bytes_per_group;

		if (words % words_per_group != 0)
			want += bytes_per_group;
		if (want == bytes)
			return &drives[i];
	}

	return NULL;
}
