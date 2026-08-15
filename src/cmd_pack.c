/*
 * cmd_pack.c -- `itsfs packings`, `itsfs drives` and `itsfs repack`.
 *
 * `repack` is the phase-1 acceptance test made into a command: rewrite an image
 * word for word in another packing.  Round-tripping a real pack back to its own
 * packing and comparing bytes is the strongest statement the bottom layer can
 * make about itself, and `make oracle` is exactly that on a 300 MB pack.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "util.h"
#include "image.h"
#include "itsgeom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
cmd_packings(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("%-6s %-13s %s\n", "name", "status", "layout");

	for (int i = 0; i < ITS_NPACK; i++) {
		const its_pack *pk = its_pack_for((its_packing)i);

		printf("%-6s %-13s %s\n", pk->name, pk->status, pk->desc);
	}

	printf("\nstatus:  confirmed    measured on a real ITS artifact, or given by the\n");
	printf("                      hardware manual for the device that packs it\n");
	printf("         corroborated another implementation lays it out this way, but nothing\n");
	printf("                      here has measured it against an ITS artifact\n");
	printf("         structural   a container choice with no format to get wrong\n");
	printf("         unverified   believed, and not yet checked against anything\n");
	return 0;
}

int
cmd_drives(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	/* clang-format off */	/* a table, and the columns are the point */
	printf("%-6s %5s %5s %5s %5s %6s %8s %8s %7s %9s\n",
	       "drive", "cyls", "spare", "surf", "sec/t", "bl/cyl", "blocks", "total", "TUT blk", "image");

	for (unsigned i = 0; i < ITS_NDRIVE; i++) {
		const its_drive *d = its_drive_at(i);

		printf("%-6s %5u %5u %5u %5u %6u %8llu %8llu %7llu %8lluM\n",
		       d->name, d->ncyls, d->xcyls, d->nheds, d->nsecs,
		       its_blks_per_cyl(d),
		       (unsigned long long)its_nblks(d),
		       (unsigned long long)its_tblks(d),
		       (unsigned long long)d->ntutbl,
		       (unsigned long long)(its_nsectors(d) * ITS_WORDS_PER_SECTOR * 8 / (1024 * 1024)));
	}
	/* clang-format on */

	printf("\nA block is %u sectors of %u words.  BLOCKS PER CYLINDER IS TRUNCATED --\n",
	       its_drive_at(0)->secblk, ITS_WORDS_PER_SECTOR);
	printf("surfaces x sectors is not always a multiple of 8, and the remainder of each\n");
	printf("cylinder is addressable by the hardware and reachable by no block number:\n");

	for (unsigned i = 0; i < ITS_NDRIVE; i++) {
		const its_drive *d = its_drive_at(i);
		unsigned waste = d->nheds * d->nsecs - its_blks_per_cyl(d) * d->secblk;

		printf("  %-6s %u sectors per cylinder unused", d->name, waste);

		if (waste != 0)
			printf(" -- %llu sectors, %llu KB, on the pack",
			       (unsigned long long)waste * (d->ncyls + d->xcyls),
			       (unsigned long long)waste * (d->ncyls + d->xcyls) *
				       ITS_WORDS_PER_SECTOR * 8 / 1024);
		printf("\n");
	}

	printf("\nThe last column is the size of a full image at 8 bytes per word, which is how\n");
	printf("`info` tells one drive from another.  Every constant is from that drive's own\n");
	printf("parameter file in the ITS source; the derived columns are computed here.\n");
	return 0;
}

static void
repack_usage(void)
{
	fprintf(stderr, "usage: itsfs repack [-p from] [-P to] [-f] in out\n"
			"       -p   the packing `in` is stored in   (default le64)\n"
			"       -P   the packing to write `out` in   (default le64)\n"
			"       -f   overwrite `out` if it exists\n");
}

int
cmd_repack(int argc, char **argv)
{
	const its_pack *from = its_pack_for(ITS_PACK_LE64);
	const its_pack *to = its_pack_for(ITS_PACK_LE64);
	its_image in;
	FILE *out = NULL;
	uint64_t done = 0;
	uint64_t *w = NULL;
	uint8_t *buf = NULL;
	int c, force = 0, rc = 1;
	/* Words per pass.  Large enough that the I/O is not the bottleneck,
	 * small enough to be a heap allocation nobody thinks about. */
	const size_t chunk = 65536;

	while ((c = getopt(argc, argv, "p:P:f")) != -1) {
		switch (c) {
		case 'p':
			if ((from = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'P':
			if ((to = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'f':
			force = 1;
			break;
		default:
			repack_usage();
			return 2;
		}
	}

	if (optind != argc - 2) {
		repack_usage();
		return 2;
	}

	if (!force && access(argv[optind + 1], F_OK) == 0) {
		fprintf(stderr, "itsfs: %s exists (use -f to overwrite)\n", argv[optind + 1]);
		return 1;
	}

	if (img_open(&in, argv[optind], from, NULL, 0) != 0)
		return 1;

	out = fopen(argv[optind + 1], "wb");

	if (out == NULL) {
		perror(argv[optind + 1]);
		goto out;
	}

	w = calloc(chunk, sizeof *w);
	buf = malloc(its_pack_bytes(to, chunk));

	if (w == NULL || buf == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		goto out;
	}

	while (done < in.words) {
		size_t n = chunk;
		size_t nbytes;

		if ((uint64_t)n > in.words - done)
			n = (size_t)(in.words - done);

		if (img_read_words(&in, done, w, n) != 0)
			goto out;

		nbytes = its_pack_bytes(to, n);
		its_put_words(to, buf, w, n);

		if (fwrite(buf, 1, nbytes, out) != nbytes) {
			perror(argv[optind + 1]);
			goto out;
		}

		done += n;
	}

	if (fclose(out) != 0) {
		perror(argv[optind + 1]);
		out = NULL;
		goto out;
	}

	out = NULL;
	rc = 0;
out:
	free(w);
	free(buf);

	if (out != NULL)
		fclose(out);
	img_close(&in);
	return rc;
}
