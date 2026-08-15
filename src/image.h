/*
 * image.h -- a pack image, addressed in words and in ITS blocks.
 *
 * Everything above this point talks about blocks and words; everything below it
 * talks about bytes.  The two things that make a byte offset out of a block
 * number -- the packing (itspack.h) and the drive geometry (itsgeom.h) -- are
 * both parameters here, and neither is guessed silently: `info` reports which
 * ones were used and where they came from.
 *
 * Reads are bounded.  Every entry point checks the request against the size of
 * the file rather than trusting a number that came off the disk, because most
 * of the numbers this program will eventually handle DID come off the disk, and
 * an ITS pack that has been sitting in a museum for forty years is exactly the
 * kind of input a parser should not extend credit to.
 */
#ifndef IMAGE_H
#define IMAGE_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "itspack.h"
#include "itsgeom.h"

typedef struct {
	FILE *fp;
	const char *path;
	const its_pack *pk;
	const its_drive *drv;
	uint64_t bytes; /* size of the file */
	uint64_t words; /* how many whole words that is */
	int writable;
} its_image;

/*
 * Open an image.  `drv` may be NULL, in which case the drive is identified from
 * the size of the file and left NULL if nothing matches -- an image that is not
 * a whole pack is still perfectly readable at the word level, which is the
 * point of having `dump` at all.
 *
 * Returns 0, or -1 with a message on stderr.
 */
int img_open(its_image *im, const char *path, const its_pack *pk, const its_drive *drv, int writable);
void img_close(its_image *im);

/* Words per ITS block for this image's drive, or 0 if the drive is unknown. */
unsigned img_words_per_block(const its_image *im);

/*
 * Read `n` words starting at word `first` of the image, counting words as the
 * packing lays them down.  Short at the end of the file is an error, not a
 * silent zero fill: a caller that wants the tail can ask how long the image is.
 */
int img_read_words(its_image *im, uint64_t first, uint64_t *w, size_t n);
int img_write_words(its_image *im, uint64_t first, const uint64_t *w, size_t n);

/*
 * Read a whole ITS block, or `n` words from the start of one.  Requires a known
 * drive, because a block number means nothing without the geometry that says
 * where it lands -- see itsgeom.h for why that is not simply `blk * words`.
 */
int img_read_block(its_image *im, uint64_t blk, uint64_t *w, size_t n);

#endif /* IMAGE_H */
