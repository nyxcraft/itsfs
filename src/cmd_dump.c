/*
 * cmd_dump.c -- `itsfs info` and `itsfs dump`.
 *
 * These two are the debugging tools every later phase is built with, so they
 * are deliberately the least clever code here: they describe what is on the
 * disk and decline to interpret it.  `info` will say a block LOOKS like an MFD;
 * it will not say the image is an ITS pack.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "util.h"
#include "image.h"
#include "its.h"
#include "itstext.h"
#include "structure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
info_usage(void)
{
	fprintf(stderr, "usage: itsfs info [-p packing] [-d drive] image\n");
}

/* A size in bytes, and the same size in the units the drive is described in. */
static void
describe_drive(const its_drive *d)
{
	unsigned nblksc = its_blks_per_cyl(d);
	unsigned waste = d->nheds * d->nsecs - nblksc * d->secblk;

	printf("drive         %s (%s)\n", d->name, d->defs);
	printf("geometry      %u+%u cylinders, %u surfaces, %u sectors/track, %u sectors/block\n",
	       d->ncyls, d->xcyls, d->nheds, d->nsecs, d->secblk);
	printf("blocks/cyl    %u", nblksc);

	if (waste != 0)
		printf("   (%u sectors per cylinder are unreachable -- %llu on the pack)",
		       waste, (unsigned long long)waste * (d->ncyls + d->xcyls));
	printf("\n");
	printf("blocks        %llu in the file area, %llu including the spare cylinders\n",
	       (unsigned long long)its_nblks(d), (unsigned long long)its_tblks(d));
	printf("MFD           block %llu\n", (unsigned long long)its_mfd_block(d));
	printf("TUT           blocks %llu..%llu\n", (unsigned long long)its_tut_block(d),
	       (unsigned long long)its_tut_block(d) + d->ntutbl - 1);
}

int
cmd_info(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_image im;
	int c, rc = 1;

	while ((c = getopt(argc, argv, "p:d:")) != -1) {
		switch (c) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'd':
			if ((drv = opt_drive(optarg)) == NULL)
				return 2;
			break;
		default:
			info_usage();
			return 2;
		}
	}

	if (optind != argc - 1) {
		info_usage();
		return 2;
	}

	if (img_open(&im, argv[optind], pk, drv, 0) != 0)
		return 1;

	printf("file          %s\n", im.path);
	printf("size          %llu bytes\n", (unsigned long long)im.bytes);
	printf("packing       %s (%s) -- %s\n", pk->name, pk->status, pk->desc);
	printf("word          36 bits, %u per %u bytes\n", pk->words, pk->bytes);
	printf("words         %llu\n", (unsigned long long)im.words);

	if (im.drv == NULL) {
		printf("drive         unknown -- no drive ITS supported is this size.\n");
		printf("              Word-level commands still work; block-level ones need -d.\n");
		rc = 0;
		goto out;
	}

	describe_drive(im.drv);

	/*
	 * And then the one interpretation this command makes: read the word the
	 * MFD's check word would be in, and say whether it is the right one.
	 * MDCHK exists so that this question has an answer.
	 */
	{
		uint64_t w[ITS_LMIBLK];
		char sb[ITS_NAME_MAX];

		if (img_read_block(&im, its_mfd_block(im.drv), w, ITS_LMIBLK) != 0)
			goto out;

		its_sixbit_word(w[ITS_MD_CHK], sb);
		printf("MDCHK         %012llo = SIXBIT |%s|  %s\n", (unsigned long long)w[ITS_MD_CHK], sb,
		       w[ITS_MD_CHK] == ITS_MFD_MAGIC ? "-- an ITS master file directory"
						      : "-- NOT |M.F.D.|, so this is not an ITS pack "
							"(or not this drive)");

		/*
		 * MDNAMP is where the name area STARTS, and the area runs to
		 * the end of the block -- so the two numbers are the directories
		 * that exist and the directories there is room for.  They are
		 * printed as two different things because they are two different
		 * things: MDNUDS is NUDSL, a constant the monitor that built the
		 * pack was assembled with, not a count of anything.
		 */
		if (w[ITS_MD_CHK] == ITS_MFD_MAGIC) {
			uint64_t wpb = (uint64_t)im.drv->secblk * ITS_WORDS_PER_SECTOR;
			uint64_t *all = calloc((size_t)wpb, sizeof *all);
			uint64_t n = 0;

			/*
			 * COUNTING NAMES NEEDS THE WHOLE BLOCK, not the header
			 * read above: the name area is at the far END of it.
			 * The slots from MDNAMP on are allocated, and the ones
			 * carrying a name are directories -- `rmdir` frees a
			 * name and leaves the slot, so counting slots reports
			 * directories that are not there.
			 */
			if (all == NULL) {
				fprintf(stderr, "itsfs: out of memory\n");
				goto out;
			}

			if (img_read_block(&im, its_mfd_block(im.drv), all, (size_t)wpb) != 0) {
				free(all);
				goto out;
			}

			for (uint64_t i = all[ITS_MD_NAMP]; i + ITS_LMNBLK <= wpb; i += ITS_LMNBLK)
				if (all[i + ITS_MN_UNAM] != 0)
					n++;

			printf("directories   %llu, in an MFD with room for %llu (MDNUDS)\n",
			       (unsigned long long)n, (unsigned long long)all[ITS_MD_NUDS]);
			free(all);
		}
	}

	rc = 0;
out:
	img_close(&im);
	return rc;
}

static void
dump_usage(void)
{
	fprintf(stderr, "usage: itsfs dump [-p packing] [-d drive] [-s] [-w words] [-z] image block[..block]\n"
			"       -s   the number is a raw 128-word SECTOR, not an ITS block\n"
			"       -w   print this many words rather than the whole block\n"
			"       -z   print zero words too (they are elided by default)\n");
}

static void
dump_words(uint64_t base, const uint64_t *w, size_t n, int showzero)
{
	int eliding = 0;

	for (size_t i = 0; i < n; i++) {
		char sb[ITS_NAME_MAX], as[ITS_ASCII_CHARS + 1];

		if (!showzero && w[i] == 0) {
			if (!eliding)
				printf("      *  (zero words)\n");
			eliding = 1;
			continue;
		}

		eliding = 0;
		its_sixbit_word(w[i], sb);
		its_ascii_printable(w[i], as);
		printf("%5llu  %012llo  %06llo,,%06llo  |%s|  |%s|\n", (unsigned long long)(base + i),
		       (unsigned long long)w[i], (unsigned long long)ITS_LH(w[i]),
		       (unsigned long long)ITS_RH(w[i]), sb, as);
	}
}

int
cmd_dump(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_image im;
	uint64_t first, last, nwords = 0;
	int c, sectors = 0, showzero = 0, rc = 1;
	uint64_t *w = NULL;

	while ((c = getopt(argc, argv, "p:d:sw:z")) != -1) {
		switch (c) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'd':
			if ((drv = opt_drive(optarg)) == NULL)
				return 2;
			break;
		case 's':
			sectors = 1;
			break;
		case 'w':
			if (parse_count(optarg, &nwords) != 0)
				return 2;
			break;
		case 'z':
			showzero = 1;
			break;
		default:
			dump_usage();
			return 2;
		}
	}

	if (optind != argc - 2) {
		dump_usage();
		return 2;
	}

	if (parse_blkrange(argv[optind + 1], &first, &last) != 0)
		return 2;

	if (img_open(&im, argv[optind], pk, drv, 0) != 0)
		return 1;

	{
		unsigned wpb = sectors ? ITS_WORDS_PER_SECTOR : img_words_per_block(&im);

		if (wpb == 0) {
			fprintf(stderr, "itsfs: %s: the drive is unknown, so a block number has no "
					"meaning -- name one with -d, or use -s for raw sectors\n",
				im.path);
			goto out;
		}

		if (nwords == 0 || nwords > wpb)
			nwords = wpb;

		w = calloc(wpb, sizeof *w);

		if (w == NULL) {
			fprintf(stderr, "itsfs: out of memory\n");
			goto out;
		}

		for (uint64_t b = first; b <= last; b++) {
			if (sectors) {
				printf("sector %llu of %s (%s, %llu words)\n", (unsigned long long)b,
				       im.path, pk->name, (unsigned long long)nwords);

				if (img_read_words(&im, b * ITS_WORDS_PER_SECTOR, w, (size_t)nwords) != 0)
					goto out;
			}
			else {
				uint64_t sec = 0;

				if (its_blk_sector(im.drv, b, &sec) != 0) {
					fprintf(stderr, "itsfs: block %llu is past the end of an %s\n",
						(unsigned long long)b, im.drv->name);
					goto out;
				}

				printf("block %llu of %s (%s, %llu words, at sector %llu)\n",
				       (unsigned long long)b, im.path, pk->name,
				       (unsigned long long)nwords, (unsigned long long)sec);

				if (img_read_block(&im, b, w, (size_t)nwords) != 0)
					goto out;
			}

			dump_words(0, w, (size_t)nwords, showzero);
		}
	}

	rc = 0;
out:
	free(w);
	img_close(&im);
	return rc;
}
