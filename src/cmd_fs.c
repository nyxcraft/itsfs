/*
 * cmd_fs.c -- the read-only file commands: dirs, ls, cat, get, free.
 *
 * NAMES.  ITS writes a file name as `DIR;FN1 FN2` -- a directory, a first name
 * and a second name, each up to six SIXBIT characters, and no extension in the
 * DEC sense.  The space in the middle means a name has to be quoted for the
 * shell, so every command here also accepts the three parts as three separate
 * arguments:
 *
 *      itsfs cat pack.dsk 'KSHACK;BUILD DOC'
 *      itsfs cat pack.dsk KSHACK BUILD DOC
 *
 * Both spellings go through one parser, so they cannot drift.
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

/* A file that needs more blocks than this is not a file, it is a corrupt
 * descriptor: the whole of an RP07 is 108,360 blocks. */
#define MAX_FILE_BLOCKS 200000

/* Options every command here takes.  One parser, one set of defaults. */
struct fsopts {
	const its_pack *pk;
	const its_drive *drv;
	int lng;
	int raw;
};

static int
fs_getopt(int argc, char **argv, const char *extra, struct fsopts *o)
{
	char opts[16];
	int c;

	o->pk = its_pack_for(ITS_PACK_LE64);
	o->drv = NULL;
	o->lng = 0;
	o->raw = 0;

	snprintf(opts, sizeof opts, "p:d:%s", extra);

	while ((c = getopt(argc, argv, opts)) != -1) {
		switch (c) {
		case 'p':
			if ((o->pk = opt_pack(optarg)) == NULL)
				return -1;
			break;
		case 'd':
			if ((o->drv = opt_drive(optarg)) == NULL)
				return -1;
			break;
		case 'l':
			o->lng = 1;
			break;
		case 'w':
			o->raw = 1;
			break;
		default:
			return -1;
		}
	}

	return 0;
}

/* Find a directory in the MFD by name.  -1 and a message if it is not there. */
static int
find_dir(const its_mfd *m, const char *name, uint64_t *blk)
{
	char have[ITS_NAME_MAX];

	for (unsigned i = 0; i < its_mfd_slots(m); i++) {
		uint64_t b;

		if (its_mfd_dir(m, i, have, &b) != 0)
			continue;

		if (have[0] != '\0' && strcmp(have, name) == 0) {
			*blk = b;
			return 0;
		}
	}

	fprintf(stderr, "itsfs: no directory named '%s' in the MFD\n", name);
	return -1;
}

static void
print_date(const its_ent *e)
{
	/* clang-format off */
	static const char *mon[] = { "???", "jan", "feb", "mar", "apr", "may", "jun",
				     "jul", "aug", "sep", "oct", "nov", "dec" };
	/* clang-format on */

	if (e->month >= 1 && e->month <= 12)
		printf("%2u-%s-%4u", e->day, mon[e->month], e->year);
	else
		printf("  (no date) ");
}

int
cmd_dirs(int argc, char **argv)
{
	struct fsopts o;
	its_image im;
	its_mfd m;
	unsigned n = 0;
	int rc = 1;

	if (fs_getopt(argc, argv, "l", &o) != 0 || optind != argc - 1) {
		fprintf(stderr, "usage: itsfs dirs [-p packing] [-d drive] [-l] image\n"
				"       -l   also read each UFD: how many files and blocks it holds\n");
		return 2;
	}

	if (img_open(&im, argv[optind], o.pk, o.drv, 0) != 0)
		return 1;

	if (its_mfd_read(&im, &m) != 0)
		goto out;

	printf("# MFD in block %llu: %u directories, room for %llu (MDNUDS)\n",
	       (unsigned long long)m.blk, its_mfd_slots(&m), (unsigned long long)m.nudsl);

	for (unsigned i = 0; i < its_mfd_slots(&m); i++) {
		char name[ITS_NAME_MAX];
		uint64_t blk;

		if (its_mfd_dir(&m, i, name, &blk) != 0 || name[0] == '\0')
			continue;

		n++;

		if (!o.lng) {
			printf("%s\n", name);
			continue;
		}

		{
			its_ufd u;
			unsigned files = 0, links = 0, idx;
			its_ent e;

			if (its_ufd_read(&im, blk, &u) != 0) {
				printf("%-6s  block %5llu  (unreadable)\n", name,
				       (unsigned long long)blk);
				continue;
			}

			idx = (unsigned)u.namp;

			while (its_ufd_next(&u, &idx, &e))
				if (e.fn1[0] != '\0' || e.fn2[0] != '\0') {
					if (e.is_link)
						links++;
					else
						files++;
				}

			printf("%-6s  block %5llu  %4u files  %3u links  %6llu blocks", name,
			       (unsigned long long)blk, files, links,
			       (unsigned long long)ITS_RH(u.w[ITS_UD_BLKS]));

			if (strcmp(name, u.name) != 0)
				printf("  [UDNAME says |%s|]", u.name);
			printf("\n");
			its_ufd_free(&u);
		}
	}

	printf("# %u directories\n", n);
	rc = 0;
out:
	its_mfd_free(&m);
	img_close(&im);
	return rc;
}

int
cmd_ls(int argc, char **argv)
{
	struct fsopts o;
	its_image im;
	its_mfd m;
	its_ufd u;
	uint64_t blk;
	unsigned idx, nf = 0;
	uint64_t words = 0, blocks = 0;
	its_ent e;
	int rc = 1, opened = 0;

	if (fs_getopt(argc, argv, "l", &o) != 0 || optind != argc - 2) {
		fprintf(stderr, "usage: itsfs ls [-p packing] [-d drive] [-l] image DIR\n");
		return 2;
	}

	if (img_open(&im, argv[optind], o.pk, o.drv, 0) != 0)
		return 1;

	if (its_mfd_read(&im, &m) != 0)
		goto out;

	if (find_dir(&m, argv[optind + 1], &blk) != 0)
		goto out;

	if (its_ufd_read(&im, blk, &u) != 0)
		goto out;
	opened = 1;

	idx = (unsigned)u.namp;

	while (its_ufd_next(&u, &idx, &e)) {
		const char *err = NULL;
		long nb;

		if (e.fn1[0] == '\0' && e.fn2[0] == '\0')
			continue;

		nf++;

		if (e.is_link) {
			char tgt[64];

			printf("%-6s %-6s", e.fn1, e.fn2);

			if (o.lng) {
				printf("  %8s %6s  ", "link", "");
				print_date(&e);
			}

			if (its_link_target(&u, e.desc, tgt, sizeof tgt, &err) == 0)
				printf("  -> %s\n", tgt);
			else
				printf("  -> (unreadable link: %s)\n", err);
			continue;
		}

		nb = its_desc_blocks(&u, im.drv, e.desc, NULL, 0, &err);

		if (nb < 0) {
			printf("%-6s %-6s  (bad descriptor: %s)\n", e.fn1, e.fn2, err);
			continue;
		}

		blocks += (uint64_t)nb;
		words += its_file_words(u.wpb, nb, e.lastwc);

		if (!o.lng) {
			printf("%-6s %-6s\n", e.fn1, e.fn2);
			continue;
		}

		printf("%-6s %-6s  %8llu %6ld  ", e.fn1, e.fn2,
		       (unsigned long long)its_file_words(u.wpb, nb, e.lastwc), nb);
		print_date(&e);

		if (e.deleted)
			printf("  deleted");
		printf("\n");
	}

	printf("# %u entries, %llu blocks, %llu words in %s;  UDBLKS says %llu blocks\n", nf,
	       (unsigned long long)blocks, (unsigned long long)words, u.name,
	       (unsigned long long)ITS_RH(u.w[ITS_UD_BLKS]));

	rc = 0;
out:
	if (opened)
		its_ufd_free(&u);
	its_mfd_free(&m);
	img_close(&im);
	return rc;
}

/*
 * Open a file: resolve the name, decode the descriptor, hand back the block
 * list and the length.  `cat` and `get` differ only in what they do with it.
 */
static int
open_file(its_image *im, const its_path *p, its_ufd *u, uint64_t **blocks, long *nblocks,
	  uint64_t *nwords)
{
	its_mfd m;
	uint64_t blk;
	unsigned idx;
	its_ent e;
	const char *err = NULL;
	int found = 0;

	if (its_mfd_read(im, &m) != 0)
		return -1;

	if (find_dir(&m, p->dir, &blk) != 0) {
		its_mfd_free(&m);
		return -1;
	}

	its_mfd_free(&m);

	if (its_ufd_read(im, blk, u) != 0)
		return -1;

	idx = (unsigned)u->namp;

	while (its_ufd_next(u, &idx, &e)) {
		if (strcmp(e.fn1, p->fn1) != 0 || strcmp(e.fn2, p->fn2) != 0)
			continue;
		found = 1;
		break;
	}

	if (!found) {
		fprintf(stderr, "itsfs: no file '%s;%s %s'\n", p->dir, p->fn1, p->fn2);
		its_ufd_free(u);
		return -1;
	}

	if (e.is_link) {
		char tgt[64];

		if (its_link_target(u, e.desc, tgt, sizeof tgt, &err) == 0)
			fprintf(stderr, "itsfs: '%s;%s %s' is a link to %s -- name the target\n",
				p->dir, p->fn1, p->fn2, tgt);
		else
			fprintf(stderr, "itsfs: '%s;%s %s' is an unreadable link: %s\n", p->dir,
				p->fn1, p->fn2, err);
		its_ufd_free(u);
		return -1;
	}

	*nblocks = its_desc_blocks(u, im->drv, e.desc, NULL, 0, &err);

	if (*nblocks < 0) {
		fprintf(stderr, "itsfs: '%s;%s %s': %s\n", p->dir, p->fn1, p->fn2, err);
		its_ufd_free(u);
		return -1;
	}

	if (*nblocks > MAX_FILE_BLOCKS) {
		fprintf(stderr, "itsfs: '%s;%s %s' claims %ld blocks\n", p->dir, p->fn1, p->fn2,
			*nblocks);
		its_ufd_free(u);
		return -1;
	}

	*blocks = calloc((size_t)*nblocks + 1, sizeof **blocks);

	if (*blocks == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		its_ufd_free(u);
		return -1;
	}

	if (its_desc_blocks(u, im->drv, e.desc, *blocks, (size_t)*nblocks, &err) != *nblocks) {
		fprintf(stderr, "itsfs: '%s;%s %s': %s\n", p->dir, p->fn1, p->fn2,
			err ? err : "the descriptor decoded differently the second time");
		free(*blocks);
		its_ufd_free(u);
		return -1;
	}

	*nwords = its_file_words(u->wpb, *nblocks, e.lastwc);
	return 0;
}

/*
 * Write `n` words as text.
 *
 * ITS text is five 7-bit characters to a word, most significant first, and bit
 * 35 is not part of any of them.  Nothing is translated -- line endings and
 * control characters go out exactly as they are on the disk, because this is an
 * extraction tool and the thing it must never do is quietly change the file.
 * (ITS text uses CRLF, and pads the last word with ^C.  Both are the file's.)
 *
 * THE ONE EXCEPTION IS THE TRAILING RUN OF NULs, and it is deliberately narrow.
 * A file's length is known in words, not in characters, so the last word may
 * hold up to four characters that are not part of the file; a NUL run at the
 * very end is almost certainly that padding.  A NUL anywhere ELSE is data.
 *
 * The first version of this dropped every NUL, which is the same code and one
 * fewer state.  It was wrong, and the thing that caught it was comparing an
 * extracted file against the host file it was built from: two of them had NULs
 * in the middle -- `ASCII \^@E^@A` in a macro argument -- and lost them
 * silently.  Trailing padding and interior data are not the same thing.
 */
static void
write_text(FILE *f, const uint64_t *w, size_t n)
{
	size_t pending = 0; /* NULs seen but not yet written */

	for (size_t i = 0; i < n; i++)
		for (unsigned k = 0; k < ITS_ASCII_CHARS; k++) {
			int c = (int)((w[i] >> (29 - 7 * k)) & 0177u);

			if (c == 0) {
				pending++;
				continue;
			}

			while (pending > 0) {
				fputc(0, f);
				pending--;
			}

			fputc(c, f);
		}
}

static int
copy_out(its_image *im, its_ufd *u, const uint64_t *blocks, long nblocks, uint64_t nwords, FILE *out,
	 int raw)
{
	uint64_t *w = calloc(u->wpb, sizeof *w);
	uint64_t left = nwords;
	int rc = -1;

	if (w == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		return -1;
	}

	for (long i = 0; i < nblocks && left > 0; i++) {
		size_t n = (left < u->wpb) ? (size_t)left : u->wpb;

		if (img_read_block(im, blocks[i], w, n) != 0)
			goto out;

		if (raw) {
			/* The words themselves, one per 8-byte little-endian
			 * container -- the same le64 the pack uses, so the host
			 * file is the file's words and nothing else. */
			if (its_write_words(out, w, n, "write") != 0)
				goto out;
		}
		else {
			write_text(out, w, n);
		}

		left -= n;
	}

	rc = 0;
out:
	free(w);
	return rc;
}

static int
cat_or_get(int argc, char **argv, int is_get)
{
	struct fsopts o;
	its_image im;
	its_ufd u;
	its_path p;
	uint64_t *blocks = NULL;
	uint64_t nwords = 0;
	long nblocks = 0;
	FILE *out = stdout;
	int rc = 1, opened = 0;
	int nargs = is_get ? 1 : 0; /* get takes a host file name as well */

	if (fs_getopt(argc, argv, "w", &o) != 0 || argc - optind < 2 + nargs) {
		if (is_get)
			fprintf(stderr, "usage: itsfs get [-p packing] [-d drive] [-w] image "
					"'DIR;FN1 FN2' hostfile\n"
					"       -w   write the file's 36-bit words (le64) rather "
					"than its text\n");
		else
			fprintf(stderr, "usage: itsfs cat [-p packing] [-d drive] [-w] image "
					"'DIR;FN1 FN2'\n");
		return 2;
	}

	if (img_open(&im, argv[optind], o.pk, o.drv, 0) != 0)
		return 1;

	if (its_parse_path(argv, optind + 1, argc - nargs - optind - 1, &p) != 0)
		goto out;

	if (open_file(&im, &p, &u, &blocks, &nblocks, &nwords) != 0)
		goto out;
	opened = 1;

	if (is_get) {
		out = fopen(argv[argc - 1], "wb");

		if (out == NULL) {
			perror(argv[argc - 1]);
			goto out;
		}
	}

	if (copy_out(&im, &u, blocks, nblocks, nwords, out, o.raw) != 0)
		goto out;

	if (is_get) {
		if (fclose(out) != 0) {
			perror(argv[argc - 1]);
			out = NULL;
			goto out;
		}

		out = NULL;
		fprintf(stderr, "%s;%s %s -> %s (%llu words in %ld blocks)\n", p.dir, p.fn1, p.fn2,
			argv[argc - 1], (unsigned long long)nwords, nblocks);
	}

	rc = 0;
out:
	free(blocks);

	if (opened)
		its_ufd_free(&u);

	if (is_get && out != NULL && out != stdout)
		fclose(out);
	img_close(&im);
	return rc;
}

int
cmd_cat(int argc, char **argv)
{
	return cat_or_get(argc, argv, 0);
}

int
cmd_get(int argc, char **argv)
{
	return cat_or_get(argc, argv, 1);
}

int
cmd_free(int argc, char **argv)
{
	struct fsopts o;
	its_image im;
	its_tut t;
	uint64_t hist[ITS_TUTMAX];
	uint64_t nfree = 0, used = 0, locked = 0;
	int rc = 1;

	if (fs_getopt(argc, argv, "", &o) != 0 || optind != argc - 1) {
		fprintf(stderr, "usage: itsfs free [-p packing] [-d drive] image\n");
		return 2;
	}

	if (img_open(&im, argv[optind], o.pk, o.drv, 0) != 0)
		return 1;

	if (its_tut_read(&im, &t) != 0)
		goto out;

	memset(hist, 0, sizeof hist);

	for (uint64_t b = t.first; b < t.last; b++) {
		int e = its_tut_entry(&t, b);

		if (e < 0)
			continue;

		hist[e]++;

		if (e == 0)
			nfree++;
		else if (e == ITS_TUTLK)
			locked++;
		else
			used++;
	}

	printf("pack          %s (number %llu)\n", t.pakid[0] ? t.pakid : "(unnamed)",
	       (unsigned long long)t.pknum);
	printf("TUT           blocks %llu..%llu at block %llu, %u blocks of table\n",
	       (unsigned long long)t.first, (unsigned long long)t.last,
	       (unsigned long long)its_tut_block(im.drv), im.drv->ntutbl);
	printf("swap area     blocks %llu..%llu (QSWAPA is the first non-swapping block)\n",
	       (unsigned long long)t.first, (unsigned long long)t.swapa);
	printf("search from   block %llu (QTUTP)\n", (unsigned long long)t.tutp);
	printf("\n");
	printf("free          %8llu blocks  %5.1f%%\n", (unsigned long long)nfree,
	       100.0 * (double)nfree / (double)(t.last - t.first));
	printf("in use        %8llu blocks  %5.1f%%\n", (unsigned long long)used,
	       100.0 * (double)used / (double)(t.last - t.first));
	printf("locked out    %8llu blocks  %5.1f%%\n", (unsigned long long)locked,
	       100.0 * (double)locked / (double)(t.last - t.first));
	printf("\n");
	printf("reference counts, which is what the TUT actually stores:\n");

	for (unsigned i = 0; i < ITS_TUTMAX; i++) {
		const char *what = i == 0	     ? "free"
				   : i == ITS_TUTLK  ? "locked out"
				   : i == ITS_TUTMNY ? "many or more references"
						     : "reference";

		if (hist[i] != 0)
			printf("  %u  %8llu blocks   %s%s\n", i, (unsigned long long)hist[i], what,
			       (i > 0 && i < ITS_TUTMNY && i != 1) ? "s" : "");
	}

	rc = 0;
out:
	its_tut_free(&t);
	img_close(&im);
	return rc;
}
