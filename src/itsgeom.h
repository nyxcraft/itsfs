/*
 * itsgeom.h -- where an ITS block number lands on the platter.
 *
 * THIS LAYER HAS NO ANALOGUE IN t10fs OR s5fs, and it is the first place ITS
 * turns out not to be TOPS-10 with different field names.
 *
 * TOPS-10 numbers its blocks linearly: block n is at n * 128 words, and the
 * geometry of the pack is bookkeeping the monitor keeps but the addressing does
 * not use.  ITS does not.  An ITS block is SECBLK (always 8) consecutive
 * SECTORS, and blocks are numbered WITHIN A CYLINDER, one cylinder at a time:
 *
 *      NBLKSC == NHEDS * NSECS / SECBLK        <-- integer division, MIDAS's
 *
 * For an RP06 that is 19 * 20 / 8 == 47.5, which MIDAS truncates to 47.  So a
 * cylinder holds 380 sectors, ITS uses 376 of them, AND FOUR SECTORS PER
 * CYLINDER ARE UNREACHABLE -- 3,248 sectors, 3.2 MB, on a 300 MB pack.  That is
 * not a bug being described; it is the addressing, and a reader that assumes
 * linear blocks is off by a growing amount from the second cylinder onward.
 *
 * The conversion ITS itself writes, from SYSTEM;FSDEFS 43 (the MFD case):
 *
 *      MFDCYL==MFDBLK/NBLKSC
 *      MFDSRF==<MFDBLK-MFDCYL*NBLKSC>*SECBLK/NSECS
 *      MFDSEC==<MFDBLK-MFDCYL*NBLKSC>*SECBLK-MFDSRF*NSECS
 *
 * which is its_blk_sector() below, and a SIMH image stores sectors in the order
 * (cylinder, surface, sector), so the linear sector is
 *
 *      (cyl * NHEDS + srf) * NSECS + sec
 *
 * MEASURED, not deduced: MFDBLK for an RP06 is 19081, which the formula puts at
 * cylinder 405, surface 18, sector 8 -- linear sector 154268 -- and word 5 of
 * that sector on a real ITS pack is SIXBIT "M.F.D.", the check word ITS writes
 * at MDCHK for exactly this purpose.  Linear block numbering puts the MFD 3,244
 * sectors away, in the middle of somebody's mail file.
 *
 * A "TRACK" IN ITS SOURCE IS A BLOCK.  disk.1228 line 48 says so outright --
 * "TUT   TRACK (BLOCK) UTILIZATION TABLE" -- and the file system code uses the
 * two words interchangeably.  It is not a surface track.  This confuses every
 * reader once; it is written down here so it confuses the next one less.
 */
#ifndef ITSGEOM_H
#define ITSGEOM_H

#include <stdint.h>
#include <stddef.h>

/*
 * A drive.  Every field is transcribed from the drive's own parameter file in
 * the ITS source tree, named in `defs`; the derived quantities are computed the
 * way MIDAS computes them, truncation included, and are NOT transcribed -- so a
 * drive added later cannot get them subtly wrong by hand.
 */
typedef struct {
	const char *name;
	const char *defs; /* the ITS source file every constant came from */
	unsigned ncyls;	  /* cylinders ITS uses */
	unsigned xcyls;	  /* cylinders kept back for spares and hacks */
	unsigned nheds;	  /* surfaces per cylinder */
	unsigned nsecs;	  /* sectors per track */
	unsigned secblk;  /* sectors per ITS block */
	unsigned ntutbl;  /* blocks the TUT occupies */
} its_drive;

#define ITS_NDRIVE 5

const its_drive *its_drive_at(unsigned i);	      /* NULL past the end */
const its_drive *its_drive_by_name(const char *name); /* NULL if unknown */

/* The derived geometry, computed as MIDAS computes it. */
unsigned its_blks_per_cyl(const its_drive *d); /* NBLKSC, truncated */
uint64_t its_nblks(const its_drive *d);	       /* NBLKS  -- the file area */
uint64_t its_tblks(const its_drive *d);	       /* TBLKS  -- including spares */
uint64_t its_nsectors(const its_drive *d);     /* the whole physical surface */

/*
 * Where the two structures with fixed homes live.  Both are NBLKS-relative, and
 * both come from FSDEFS:  MFDBLK==NBLKS/2-1,  TUTBLK==MFDBLK-NTUTBL.
 */
uint64_t its_mfd_block(const its_drive *d);
uint64_t its_tut_block(const its_drive *d);

/*
 * Block number -> linear sector number, the conversion described above.
 * Returns 0 on success, -1 if the block is past the end of the drive.
 */
int its_blk_sector(const its_drive *d, uint64_t blk, uint64_t *sector);

/* ...and its inverse, for saying what a raw sector belongs to.  -1 if the
 * sector is one of the ones per cylinder that no block reaches. */
int its_sector_blk(const its_drive *d, uint64_t sector, uint64_t *blk);

/*
 * Which drive is an image of `bytes` bytes, at `wordbytes` bytes per word?  The
 * size of a pack identifies the drive on its own, because no two of these have
 * the same one.  NULL if nothing matches -- and the caller must then be told,
 * not given a default, because reading an RM80 as an RP06 finds no MFD at all.
 */
const its_drive *its_drive_by_size(uint64_t bytes, unsigned words_per_group, unsigned bytes_per_group);

#endif /* ITSGEOM_H */
