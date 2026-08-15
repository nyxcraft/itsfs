/*
 * image.c -- pack images: open, size, and read words and blocks.
 */

#define _POSIX_C_SOURCE 200809L

#include "image.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * One transfer's worth of bytes, and the words that come out of it.
 *
 * A group is at most 2 words in 9 bytes today, and every packing uses at least
 * as many bytes as words, so sizing the group count from the byte buffer bounds
 * the word buffer too.  That relationship is asserted rather than assumed --
 * see the check in img_open -- because a packing added later that inverted it
 * would overflow `stage` with no diagnostic at all.
 */
#define IMG_BYTES 8192
#define IMG_WORDS 8192

int
img_open(its_image *im, const char *path, const its_pack *pk, const its_drive *drv, int writable)
{
	FILE *fp;
	long long end;

	memset(im, 0, sizeof *im);

	fp = fopen(path, writable ? "r+b" : "rb");

	if (fp == NULL) {
		fprintf(stderr, "itsfs: %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (fseeko(fp, 0, SEEK_END) != 0 || (end = (long long)ftello(fp)) < 0) {
		fprintf(stderr, "itsfs: %s: cannot size the file: %s\n", path, strerror(errno));
		fclose(fp);
		return -1;
	}

	/*
	 * THE BUFFER RELATIONSHIP, CHECKED ONCE.  img_words sizes its word
	 * staging from the byte buffer, which is only safe while a group needs
	 * at least as many bytes as it holds words.  Every packing that exists
	 * satisfies that; a future one need not, and this is where it says so
	 * instead of writing off the end of a stack array.
	 */
	if (pk->words > pk->bytes) {
		fprintf(stderr, "itsfs: packing %s: %u words in %u bytes is not supported\n",
			pk->name, pk->words, pk->bytes);
		fclose(fp);
		return -1;
	}

	im->fp = fp;
	im->path = path;
	im->pk = pk;
	im->bytes = (uint64_t)end;
	im->words = (im->bytes / pk->bytes) * pk->words;
	im->writable = writable;

	/*
	 * A drive named on the command line WINS over the size, so that a
	 * truncated or oddly-padded image can still be read as what it is.
	 * Otherwise the size decides, and NULL is a legitimate answer.
	 */
	im->drv = drv ? drv : its_drive_by_size(im->bytes, pk->words, pk->bytes);

	return 0;
}

void
img_close(its_image *im)
{
	if (im->fp != NULL)
		fclose(im->fp);
	im->fp = NULL;
}

unsigned
img_words_per_block(const its_image *im)
{
	if (im->drv == NULL)
		return 0;
	return im->drv->secblk * ITS_WORDS_PER_SECTOR;
}

/*
 * The one place a word index becomes a byte offset.
 *
 * A read may start at any word, including one in the middle of a dbd9 group, so
 * the run is widened to whole groups at both ends and the wanted words are
 * taken out of the middle.  Doing it any other way means every caller has to
 * know whether its packing shares bytes between words, which is precisely what
 * this layer exists to hide.
 */
static int
img_words(its_image *im, uint64_t first, uint64_t *w, size_t n, int write)
{
	const its_pack *pk = im->pk;
	uint64_t g0;
	uint8_t buf[IMG_BYTES];
	uint64_t stage[IMG_WORDS];

	if (n == 0)
		return 0;

	if (first > im->words || n > im->words - first) {
		fprintf(stderr, "itsfs: %s: read of %zu words at %llu is past the end (%llu words)\n",
			im->path, n, (unsigned long long)first, (unsigned long long)im->words);
		return -1;
	}

	g0 = first / pk->words; /* first group touched */

	while (n > 0) {
		size_t skip = (size_t)(first - g0 * pk->words);
		size_t have, ngrp, nbytes, take;

		ngrp = IMG_BYTES / pk->bytes;
		nbytes = ngrp * pk->bytes;

		/* Do not read past the end of the file for the tail group. */
		if (g0 * pk->bytes + nbytes > im->bytes)
			nbytes = (size_t)(im->bytes - g0 * pk->bytes);
		ngrp = nbytes / pk->bytes;

		if (ngrp == 0) {
			fprintf(stderr, "itsfs: %s: truncated image\n", im->path);
			return -1;
		}

		have = ngrp * pk->words;

		if (fseeko(im->fp, (off_t)(g0 * pk->bytes), SEEK_SET) != 0) {
			fprintf(stderr, "itsfs: %s: seek: %s\n", im->path, strerror(errno));
			return -1;
		}

		if (fread(buf, 1, nbytes, im->fp) != nbytes) {
			fprintf(stderr, "itsfs: %s: short read\n", im->path);
			return -1;
		}

		take = have - skip;

		if (take > n)
			take = n;

		if (!write) {
			its_get_words(pk, buf, stage, have);
			memcpy(w, stage + skip, take * sizeof *w);
		}
		else {
			/* Read-modify-write: the group at either end may hold
			 * words this call is not touching. */
			its_get_words(pk, buf, stage, have);
			memcpy(stage + skip, w, take * sizeof *w);
			its_put_words(pk, buf, stage, have);

			if (fseeko(im->fp, (off_t)(g0 * pk->bytes), SEEK_SET) != 0 ||
			    fwrite(buf, 1, nbytes, im->fp) != nbytes) {
				fprintf(stderr, "itsfs: %s: write: %s\n", im->path, strerror(errno));
				return -1;
			}
		}

		w += take;
		n -= take;
		first += take;
		g0 = first / pk->words;
	}

	return 0;
}

int
img_read_words(its_image *im, uint64_t first, uint64_t *w, size_t n)
{
	return img_words(im, first, w, n, 0);
}

int
img_write_words(its_image *im, uint64_t first, const uint64_t *w, size_t n)
{
	if (!im->writable) {
		fprintf(stderr, "itsfs: %s: opened read-only\n", im->path);
		return -1;
	}

	/* The cast is safe: img_words does not modify the buffer when writing. */
	return img_words(im, first, (uint64_t *)(uintptr_t)w, n, 1);
}

int
img_read_block(its_image *im, uint64_t blk, uint64_t *w, size_t n)
{
	uint64_t sector;
	unsigned wpb = img_words_per_block(im);

	if (im->drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry, so block %llu has no address\n",
			im->path, (unsigned long long)blk);
		return -1;
	}

	if (n > wpb) {
		fprintf(stderr, "itsfs: block %llu: %zu words asked of a %u-word block\n",
			(unsigned long long)blk, n, wpb);
		return -1;
	}

	if (its_blk_sector(im->drv, blk, &sector) != 0) {
		fprintf(stderr, "itsfs: block %llu is past the end of an %s (%llu blocks)\n",
			(unsigned long long)blk, im->drv->name,
			(unsigned long long)its_tblks(im->drv));
		return -1;
	}

	return img_read_words(im, sector * ITS_WORDS_PER_SECTOR, w, n);
}
