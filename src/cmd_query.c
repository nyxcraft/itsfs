/*
 * cmd_query.c -- `itsfs ncheck` and `itsfs du`: questions about a whole pack.
 *
 *   itsfs ncheck [-p packing] [-d drive] image BLOCK...
 *   itsfs du     [-p packing] [-d drive] [-a] image [DIR...]
 *
 * Both walk every directory and every descriptor, which is the same walk
 * `manifest` and `check` make -- and both answer a question those two compute
 * and then throw away.
 *
 * `ncheck` IS THE INVERSE OF THE DESCRIPTOR: given a block, which file claims
 * it?  The checker builds exactly that map to notice a block claimed twice and
 * discards it once it has reported; this is the map itself, for the case where
 * something else -- a salvager's output, a bad-block list off a drive, a word
 * that looked wrong in `dump` -- names a block and the question is what would be
 * lost with it.  s5fs calls its version `ncheck` because it maps an inode to a
 * path; ITS has no inodes, and a block is the thing worth naming instead.
 *
 * `du` IS PER DIRECTORY, because that is the only tree ITS has: two levels, no
 * nesting, so a subtree total is a directory total and nothing has to recurse.
 * The number that matters is not the sum of the files -- a directory's own
 * UDBLKS records what it thinks it holds, and the two disagreeing is worth
 * seeing, so both are printed.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cmds.h"
#include "image.h"
#include "its.h"
#include "itsgeom.h"
#include "itspack.h"
#include "structure.h"
#include "util.h"

#define Q_MAXBLK 65536 /* a file's blocks: a cap on an untrusted descriptor */

/* ---------------------------------------------------------------- ncheck */

struct owner {
	char dir[ITS_NAME_MAX];
	char fn1[ITS_NAME_MAX];
	char fn2[ITS_NAME_MAX];
	int found;
	int twice; /* a second claimant, which is damage `check` reports */
};

/*
 * Walk everything, recording who claims each of the blocks asked about.
 *
 * The walk is over FILES rather than over the blocks wanted, because a
 * descriptor is the only thing that says which blocks a file holds: there is no
 * index from a block back to its file anywhere on the disk.  So the cost is one
 * pass over the pack however many blocks are asked about, and asking about
 * twenty costs what asking about one does.
 */
static int
find_owners(its_image *im, its_mfd *m, const uint64_t *want, struct owner *own, unsigned nwant)
{
	for (unsigned i = 0; i < its_mfd_slots(m); i++) {
		char dname[ITS_NAME_MAX];
		uint64_t dblk;
		its_ufd u;
		unsigned idx;
		its_ent e;

		if (its_mfd_dir(m, i, dname, &dblk) != 0 || dname[0] == '\0')
			continue;

		if (its_ufd_read(im, dblk, &u) != 0)
			continue;

		idx = (unsigned)u.namp;

		while (its_ufd_next(&u, &idx, &e)) {
			const char *err = NULL;
			uint64_t *blocks;
			long nb;

			if (e.is_link || (e.fn1[0] == '\0' && e.fn2[0] == '\0'))
				continue;

			nb = its_desc_blocks(&u, im->drv, e.desc, NULL, 0, &err);

			if (nb <= 0 || nb > Q_MAXBLK)
				continue;

			blocks = calloc((size_t)nb, sizeof *blocks);

			if (blocks == NULL) {
				fprintf(stderr, "itsfs: out of memory\n");
				its_ufd_free(&u);
				return -1;
			}

			if (its_desc_blocks(&u, im->drv, e.desc, blocks, (size_t)nb, &err) == nb) {
				for (long b = 0; b < nb; b++)
					for (unsigned k = 0; k < nwant; k++) {
						if (blocks[b] != want[k])
							continue;

						if (own[k].found) {
							own[k].twice = 1;
							continue;
						}

						own[k].found = 1;
						snprintf(own[k].dir, sizeof own[k].dir, "%s",
							 dname);
						snprintf(own[k].fn1, sizeof own[k].fn1, "%s",
							 e.fn1);
						snprintf(own[k].fn2, sizeof own[k].fn2, "%s",
							 e.fn2);
					}
			}

			free(blocks);
		}

		its_ufd_free(&u);
	}

	return 0;
}

int
cmd_ncheck(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_image im;
	its_mfd m;
	its_tut t;
	uint64_t *want = NULL;
	struct owner *own = NULL;
	unsigned nwant = 0;
	int c, rc = 2, have_tut = 0;

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
			goto usage;
		}
	}

	if (optind + 2 > argc)
		goto usage;

	nwant = (unsigned)(argc - optind - 1);
	want = calloc(nwant, sizeof *want);
	own = calloc(nwant, sizeof *own);

	if (want == NULL || own == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		goto out;
	}

	for (unsigned i = 0; i < nwant; i++)
		if (parse_block(argv[optind + 1 + (int)i], &want[i]) != 0)
			goto out;

	if (img_open(&im, argv[optind], pk, drv, 0) != 0)
		goto out;

	if (im.drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", im.path);
		img_close(&im);
		goto out;
	}

	if (its_mfd_read(&im, &m) != 0) {
		img_close(&im);
		goto out;
	}

	have_tut = its_tut_read(&im, &t) == 0;

	if (find_owners(&im, &m, want, own, nwant) != 0) {
		its_mfd_free(&m);
		img_close(&im);
		goto out;
	}

	for (unsigned i = 0; i < nwant; i++) {
		printf("%-8llu ", (unsigned long long)want[i]);

		if (own[i].found) {
			printf("%s;%s %s%s\n", own[i].dir, own[i].fn1, own[i].fn2,
			       own[i].twice ? "   AND SOMETHING ELSE -- run `check`" : "");
			continue;
		}

		/*
		 * Nothing claims it, and the table says which kind of nothing.
		 * "In use and unclaimed" is the interesting one: it is space a
		 * file used to hold, and `check` reports it as a miscount.
		 */
		if (!have_tut) {
			printf("no file claims it\n");
			continue;
		}

		switch (its_tut_entry(&t, want[i])) {
		case 0:
			printf("no file claims it, and the table says free\n");
			break;
		case ITS_TUTMAX - 1:
			printf("no file claims it, and the table says locked out\n");
			break;
		case -1:
			printf("outside the area the table maps\n");
			break;
		default:
			printf("NO FILE CLAIMS IT, and the table says in use\n");
			break;
		}
	}

	if (have_tut)
		its_tut_free(&t);

	its_mfd_free(&m);
	img_close(&im);
	rc = 0;
out:
	free(want);
	free(own);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs ncheck [-p packing] [-d drive] image BLOCK...\n"
			"       which file claims each block -- the inverse of a descriptor\n"
			"       a block number is DECIMAL by default; 0 or 0o for octal\n");
	return 2;
}

/* -------------------------------------------------------------------- du */

int
cmd_du(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_image im;
	its_mfd m;
	uint64_t tblocks = 0, twords = 0;
	unsigned long tfiles = 0, tlinks = 0, tdirs = 0;
	int c, all = 0;

	while ((c = getopt(argc, argv, "p:d:a")) != -1) {
		switch (c) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'd':
			if ((drv = opt_drive(optarg)) == NULL)
				return 2;
			break;
		case 'a':
			all = 1;
			break;
		default:
			goto usage;
		}
	}

	if (optind >= argc)
		goto usage;

	if (img_open(&im, argv[optind], pk, drv, 0) != 0)
		return 2;

	if (im.drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", im.path);
		img_close(&im);
		return 2;
	}

	if (its_mfd_read(&im, &m) != 0) {
		img_close(&im);
		return 2;
	}

	printf("%8s %8s %5s %5s  %s\n", "blocks", "words", "files", "links", "directory");

	for (unsigned i = 0; i < its_mfd_slots(&m); i++) {
		char dname[ITS_NAME_MAX];
		uint64_t dblk;
		its_ufd u;
		unsigned idx;
		its_ent e;
		uint64_t blocks = 0, words = 0, udblks;
		unsigned long files = 0, links = 0;

		if (its_mfd_dir(&m, i, dname, &dblk) != 0 || dname[0] == '\0')
			continue;

		/* Named directories only, when any were named. */
		if (optind + 1 < argc) {
			int wanted = 0;

			for (int a = optind + 1; a < argc; a++)
				if (strcmp(argv[a], dname) == 0)
					wanted = 1;

			if (!wanted)
				continue;
		}

		if (its_ufd_read(&im, dblk, &u) != 0)
			continue;

		udblks = ITS_RH(u.w[ITS_UD_BLKS]);
		idx = (unsigned)u.namp;

		while (its_ufd_next(&u, &idx, &e)) {
			const char *err = NULL;
			long nb;

			if (e.fn1[0] == '\0' && e.fn2[0] == '\0')
				continue;

			if (e.is_link) {
				links++;

				if (all)
					printf("%8s %8s %5s %5s  %s;%s %s\n", "-", "-", "-", "-",
					       dname, e.fn1, e.fn2);

				continue;
			}

			files++;
			nb = its_desc_blocks(&u, im.drv, e.desc, NULL, 0, &err);

			if (nb < 0 || nb > Q_MAXBLK)
				continue;

			blocks += (uint64_t)nb;
			words += its_file_words(u.wpb, nb, e.lastwc);

			if (all)
				printf("%8ld %8llu %5s %5s  %s;%s %s\n", nb,
				       (unsigned long long)its_file_words(u.wpb, nb, e.lastwc), "-",
				       "-", dname, e.fn1, e.fn2);
		}

		its_ufd_free(&u);

		printf("%8llu %8llu %5lu %5lu  %s%s\n", (unsigned long long)blocks,
		       (unsigned long long)words, files, links, dname,
		       blocks == udblks ? ""
					: "   (UDBLKS disagrees -- run `check`)");

		tblocks += blocks;
		twords += words;
		tfiles += files;
		tlinks += links;
		tdirs++;
	}

	printf("%8llu %8llu %5lu %5lu  total, in %lu directories\n", (unsigned long long)tblocks,
	       (unsigned long long)twords, tfiles, tlinks, tdirs);

	its_mfd_free(&m);
	img_close(&im);
	return 0;

usage:
	fprintf(stderr, "usage: itsfs du [-p packing] [-d drive] [-a] image [DIR...]\n"
			"       blocks and words per directory; -a lists the files too\n"
			"       ITS has two levels and no nesting, so a directory IS a subtree\n");
	return 2;
}
