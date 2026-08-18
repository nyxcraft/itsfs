/*
 * cmd_shell.c -- `itsfs shell`, an interactive explorer over one pack.
 *
 *   itsfs shell [-p packing] [-d drive] image
 *
 * A pack has hundreds of directories and thousands of files, and answering
 * "what is in here" with the batch commands means retyping the image path and
 * the directory for every question.  The shell keeps both, so `cd KSHACK` then
 * `ls` then `type ITS 15` is three short lines instead of three long ones.
 *
 * IT IS READ-ONLY, and not because of a flag.  There is no writer in this
 * project, so unlike its counterpart in t10fs this shell has no `-w` and no
 * mutating command to guard.  When there is a writer, the rule that applies is
 * the one t10fs learned: an explorer is the tool you poke a damaged pack with,
 * and it should not be able to write to one by accident.  The prompt says
 * `(ro)` today so that the day a writable mode exists, the difference is
 * visible rather than assumed.
 *
 * It reads commands from standard input, which is what makes it scriptable and
 * therefore testable.  The test suite drives it with a here-document.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "util.h"
#include "image.h"
#include "structure.h"
#include "its.h"
#include "itspack.h"
#include "itstext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define SH_MAXARG 8
#define SH_MAXBLK 200000

struct sh {
	its_image *im;
	its_mfd mfd;
	int have_mfd;

	char cwd[ITS_NAME_MAX]; /* the current directory, or empty for none */
	uint64_t cwdblk;

	int interactive;
};

/* ------------------------------------------------------------------ helpers */

/* Find a directory by name.  -1 and a message if it is not in the MFD. */
static int
find_dir(struct sh *s, const char *name, uint64_t *blk)
{
	char have[ITS_NAME_MAX];

	for (unsigned i = 0; i < its_mfd_slots(&s->mfd); i++) {
		uint64_t b;

		if (its_mfd_dir(&s->mfd, i, have, &b) != 0)
			continue;

		if (have[0] != '\0' && strcmp(have, name) == 0) {
			*blk = b;
			return 0;
		}
	}

	printf("no directory named '%s'\n", name);
	return -1;
}

/* Open the current directory.  -1 if there is not one. */
static int
open_cwd(struct sh *s, its_ufd *u)
{
	if (s->cwd[0] == '\0') {
		printf("no current directory -- `cd DIR` first, or `dirs` to see them\n");
		return -1;
	}

	return its_ufd_read(s->im, s->cwdblk, u);
}

/*
 * A file's blocks and length.  The caller frees `*blocks`.
 *
 * `fn2` may be NULL, which matches an entry whose second name is empty -- ITS
 * allows that and the reference pack has several.
 */
static int
find_file(struct sh *s, its_ufd *u, const char *fn1, const char *fn2, uint64_t **blocks,
	  long *nblocks, uint64_t *nwords, its_ent *out)
{
	unsigned idx = (unsigned)u->namp;
	its_ent e;
	const char *err = NULL;
	int found = 0;

	while (its_ufd_next(u, &idx, &e)) {
		if (strcmp(e.fn1, fn1) != 0)
			continue;

		if (strcmp(e.fn2, fn2 ? fn2 : "") != 0)
			continue;
		found = 1;
		break;
	}

	if (!found) {
		printf("no file '%s;%s %s'\n", s->cwd, fn1, fn2 ? fn2 : "");
		return -1;
	}

	*out = e;

	if (e.is_link) {
		char tgt[64];

		if (its_link_target(u, e.desc, tgt, sizeof tgt, &err) == 0)
			printf("'%s %s' is a link to %s\n", fn1, fn2 ? fn2 : "", tgt);
		else
			printf("'%s %s' is an unreadable link: %s\n", fn1, fn2 ? fn2 : "", err);
		return -1;
	}

	*nblocks = its_desc_blocks(u, s->im->drv, e.desc, NULL, 0, &err);

	if (*nblocks < 0 || *nblocks > SH_MAXBLK) {
		printf("'%s %s': %s\n", fn1, fn2 ? fn2 : "", err ? err : "impossibly long");
		return -1;
	}

	*blocks = calloc((size_t)*nblocks + 1, sizeof **blocks);

	if (*blocks == NULL) {
		printf("out of memory\n");
		return -1;
	}

	if (its_desc_blocks(u, s->im->drv, e.desc, *blocks, (size_t)*nblocks, &err) != *nblocks) {
		free(*blocks);
		printf("'%s %s': the descriptor decoded differently twice\n", fn1, fn2 ? fn2 : "");
		return -1;
	}

	*nwords = its_file_words(u->wpb, *nblocks, e.lastwc);
	return 0;
}

/* ----------------------------------------------------------------- commands */

static void
cmd_help(void)
{
	/* clang-format off */	/* the columns are the help text */
	printf("dirs [-l]           the directories in the MFD\n");
	printf("cd DIR              change to one;  `cd` alone leaves it\n");
	printf("pwd                 where you are\n");
	printf("ls [-l]             the current directory\n");
	printf("type FN1 [FN2]      print a file\n");
	printf("blocks FN1 [FN2]    which blocks a file holds\n");
	printf("stat FN1 [FN2]      everything the directory entry says\n");
	printf("free                what the TUT says about the pack\n");
	printf("info                the pack, the drive and the geometry\n");
	printf("help                this\n");
	printf("quit                (or EOF)\n");
	/* clang-format on */
	printf("\nRead-only: there is no writer in this project.\n");
}

static void
sh_dirs(struct sh *s, int lng)
{
	unsigned n = 0;

	for (unsigned i = 0; i < its_mfd_slots(&s->mfd); i++) {
		char name[ITS_NAME_MAX];
		uint64_t blk;

		if (its_mfd_dir(&s->mfd, i, name, &blk) != 0 || name[0] == '\0')
			continue;

		n++;

		if (!lng) {
			printf("%s\n", name);
			continue;
		}

		{
			its_ufd u;

			if (its_ufd_read(s->im, blk, &u) != 0) {
				printf("%-6s  block %5llu  (unreadable)\n", name,
				       (unsigned long long)blk);
				continue;
			}

			printf("%-6s  block %5llu  %6llu blocks\n", name, (unsigned long long)blk,
			       (unsigned long long)ITS_RH(u.w[ITS_UD_BLKS]));
			its_ufd_free(&u);
		}
	}

	printf("%u directories\n", n);
}

static void
sh_ls(struct sh *s, int lng)
{
	its_ufd u;
	unsigned idx, n = 0;
	its_ent e;
	uint64_t words = 0, blocks = 0;

	if (open_cwd(s, &u) != 0)
		return;

	idx = (unsigned)u.namp;

	while (its_ufd_next(&u, &idx, &e)) {
		const char *err = NULL;
		long nb;

		if (e.fn1[0] == '\0' && e.fn2[0] == '\0')
			continue;

		n++;

		if (e.is_link) {
			char tgt[64];

			printf("%-6s %-6s", e.fn1, e.fn2);

			if (lng)
				printf("  %8s %6s", "link", "");

			if (its_link_target(&u, e.desc, tgt, sizeof tgt, &err) == 0)
				printf("  -> %s\n", tgt);
			else
				printf("  -> (unreadable: %s)\n", err);
			continue;
		}

		nb = its_desc_blocks(&u, s->im->drv, e.desc, NULL, 0, &err);

		if (nb < 0) {
			printf("%-6s %-6s  (bad descriptor: %s)\n", e.fn1, e.fn2, err);
			continue;
		}

		blocks += (uint64_t)nb;
		words += its_file_words(u.wpb, nb, e.lastwc);

		if (!lng) {
			printf("%-6s %-6s\n", e.fn1, e.fn2);
			continue;
		}

		printf("%-6s %-6s  %8llu %6ld  %2u-%02u-%4u\n", e.fn1, e.fn2,
		       (unsigned long long)its_file_words(u.wpb, nb, e.lastwc), nb, e.day, e.month,
		       e.year);
	}

	printf("%u entries, %llu blocks, %llu words\n", n, (unsigned long long)blocks,
	       (unsigned long long)words);
	its_ufd_free(&u);
}

static void
sh_type(struct sh *s, const char *fn1, const char *fn2)
{
	its_ufd u;
	its_ent e;
	uint64_t *blocks = NULL, nwords = 0;
	long nblocks = 0;
	uint64_t *w;

	if (open_cwd(s, &u) != 0)
		return;

	if (find_file(s, &u, fn1, fn2, &blocks, &nblocks, &nwords, &e) != 0) {
		its_ufd_free(&u);
		return;
	}

	w = calloc(u.wpb, sizeof *w);

	if (w == NULL) {
		printf("out of memory\n");
		goto out;
	}

	{
		uint64_t left = nwords;
		size_t pending = 0;

		for (long i = 0; i < nblocks && left > 0; i++) {
			size_t n = (left < u.wpb) ? (size_t)left : u.wpb;

			if (img_read_block(s->im, blocks[i], w, n) != 0)
				break;

			/* The same rule as `cat`: interior NULs are data, a
			 * trailing run of them is padding.  See cmd_fs.c. */
			for (size_t k = 0; k < n; k++)
				for (unsigned b = 0; b < ITS_ASCII_CHARS; b++) {
					int ch = (int)((w[k] >> (29 - 7 * b)) & 0177u);

					if (ch == 0) {
						pending++;
						continue;
					}

					while (pending > 0) {
						putchar(0);
						pending--;
					}

					putchar(ch);
				}

			left -= n;
		}
	}

	free(w);
out:
	free(blocks);
	its_ufd_free(&u);
}

static void
sh_blocks(struct sh *s, const char *fn1, const char *fn2)
{
	its_ufd u;
	its_ent e;
	uint64_t *blocks = NULL, nwords = 0;
	long nblocks = 0;

	if (open_cwd(s, &u) != 0)
		return;

	if (find_file(s, &u, fn1, fn2, &blocks, &nblocks, &nwords, &e) != 0) {
		its_ufd_free(&u);
		return;
	}

	printf("%s;%s %s: %ld blocks, %llu words\n", s->cwd, fn1, fn2 ? fn2 : "", nblocks,
	       (unsigned long long)nwords);

	/* Runs, rather than a list of several thousand numbers: a descriptor is
	 * run-length coded, so its output usually is too, and seeing WHERE it is
	 * fragmented is the question worth asking of a real file. */
	{
		long i = 0;
		unsigned runs = 0;

		while (i < nblocks) {
			long j = i;

			while (j + 1 < nblocks && blocks[j + 1] == blocks[j] + 1)
				j++;

			if (i == j)
				printf("  %llu\n", (unsigned long long)blocks[i]);
			else
				printf("  %llu..%llu (%ld)\n", (unsigned long long)blocks[i],
				       (unsigned long long)blocks[j], j - i + 1);
			runs++;
			i = j + 1;
		}

		printf("  %u run%s\n", runs, runs == 1 ? "" : "s");
	}

	free(blocks);
	its_ufd_free(&u);
}

static void
sh_stat(struct sh *s, const char *fn1, const char *fn2)
{
	its_ufd u;
	unsigned idx;
	its_ent e;
	int found = 0;

	if (open_cwd(s, &u) != 0)
		return;

	idx = (unsigned)u.namp;

	while (its_ufd_next(&u, &idx, &e)) {
		if (strcmp(e.fn1, fn1) == 0 && strcmp(e.fn2, fn2 ? fn2 : "") == 0) {
			found = 1;
			break;
		}
	}

	if (!found) {
		printf("no file '%s;%s %s'\n", s->cwd, fn1, fn2 ? fn2 : "");
		its_ufd_free(&u);
		return;
	}

	/* Everything the entry says, including the fields nothing here
	 * interprets -- see the gap register in docs/filesystem.md.  A stat that
	 * showed only the understood fields would hide exactly the words
	 * somebody investigating an unknown field needs to see. */
	printf("name       %s;%s %s\n", s->cwd, e.fn1, e.fn2);
	printf("entry      word %u of block %llu\n", e.word, (unsigned long long)s->cwdblk);
	printf("UNRNDM     %012llo\n", (unsigned long long)e.rndm);
	printf("  UNDSCP   %u  (six-bit byte offset from UDDESC)\n", e.desc);
	printf("  UNPKN    %u\n", e.pack);
	printf("  UNLNKB   %d%s\n", e.is_link, e.is_link ? "  (a link)" : "");
	printf("  UNWRDC   %u%s\n", e.lastwc, e.lastwc ? "" : "  (the last block is full)");
	printf("  flags    %s\n", e.deleted ? "UNIGFL set -- ignore this entry" : "none set");
	printf("UNDATE     %012llo\n", (unsigned long long)e.date);
	printf("  written  %u-%02u-%u\n", e.day, e.month, e.year);
	printf("  UNTIM    %u  (raw; the unit is not established -- see docs)\n", e.time);
	printf("UNREF      %012llo\n", (unsigned long long)e.ref);
	printf("  UNAUTH   %o%s\n", e.author, e.author == 0777 ? "  (all ones: no directory)" : "");
	printf("  UNBYTE   %o%s\n", e.bytesz, e.bytesz == 0 ? "  (36-bit bytes)" : "");

	if (!e.is_link) {
		const char *err = NULL;
		long nb = its_desc_blocks(&u, s->im->drv, e.desc, NULL, 0, &err);

		if (nb >= 0)
			printf("length     %llu words in %ld blocks\n",
			       (unsigned long long)its_file_words(u.wpb, nb, e.lastwc), nb);
		else
			printf("length     (bad descriptor: %s)\n", err);
	}
	else {
		char tgt[64];
		const char *err = NULL;

		if (its_link_target(&u, e.desc, tgt, sizeof tgt, &err) == 0)
			printf("target     %s\n", tgt);
		else
			printf("target     (unreadable: %s)\n", err);
	}

	its_ufd_free(&u);
}

static void
sh_free(struct sh *s)
{
	its_tut t;
	uint64_t nfree = 0, used = 0, locked = 0;

	if (its_tut_read(s->im, &t) != 0)
		return;

	for (uint64_t b = t.first; b < t.last; b++) {
		int e = its_tut_entry(&t, b);

		if (e < 0)
			continue;

		if (e == 0)
			nfree++;
		else if (e == ITS_TUTLK)
			locked++;
		else
			used++;
	}

	printf("pack %s (number %llu), TUT maps %llu..%llu\n", t.pakid[0] ? t.pakid : "(unnamed)",
	       (unsigned long long)t.pknum, (unsigned long long)t.first,
	       (unsigned long long)t.last);
	printf("%llu free, %llu in use, %llu locked out\n", (unsigned long long)nfree,
	       (unsigned long long)used, (unsigned long long)locked);
	its_tut_free(&t);
}

static void
sh_info(struct sh *s)
{
	const its_drive *d = s->im->drv;

	printf("file       %s\n", s->im->path);
	printf("packing    %s (%s)\n", s->im->pk->name, s->im->pk->status);
	printf("drive      %s (%s)\n", d->name, d->defs);
	printf("geometry   %u+%u cylinders, %u surfaces, %u sectors/track, %u per block\n", d->ncyls,
	       d->xcyls, d->nheds, d->nsecs, d->secblk);
	printf("blocks     %llu of %u words, %u per cylinder\n", (unsigned long long)its_tblks(d),
	       d->secblk * ITS_WORDS_PER_SECTOR, its_blks_per_cyl(d));
	printf("MFD        block %llu\n", (unsigned long long)its_mfd_block(d));
	printf("TUT        block %llu, %u blocks\n", (unsigned long long)its_tut_block(d),
	       d->ntutbl);
}

/* -------------------------------------------------------------- the reader */

/*
 * Split a line into words.  Nothing clever: no quoting, no globbing, and no
 * escape characters, because ITS names are SIXBIT and cannot contain anything
 * that would need them -- a name is at most six characters from 040 to 137, and
 * the shell's own separator is the space that already separates FN1 from FN2.
 */
static int
split(char *line, char **argv, int max)
{
	int n = 0;
	char *p = line;

	while (*p != '\0' && n < max) {
		while (*p == ' ' || *p == '\t')
			p++;

		if (*p == '\0' || *p == '\n')
			break;
		argv[n++] = p;

		while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n')
			p++;

		if (*p != '\0')
			*p++ = '\0';
	}

	return n;
}

static void
upcase(char *s)
{
	for (; *s != '\0'; s++)
		*s = (char)toupper((unsigned char)*s);
}

int
cmd_shell(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_image im;
	struct sh s;
	char line[256];
	int c, rc = 2;

	memset(&s, 0, sizeof s);

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

	if (optind != argc - 1)
		goto usage;

	if (img_open(&im, argv[optind], pk, drv, 0) != 0)
		return 2;

	if (im.drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", im.path);
		img_close(&im);
		return 2;
	}

	s.im = &im;

	if (its_mfd_read(&im, &s.mfd) != 0) {
		img_close(&im);
		return 1;
	}

	s.have_mfd = 1;
	s.interactive = isatty(0);

	if (s.interactive)
		printf("%s, %s, %u directories.  `help` lists the commands.\n", im.path, im.drv->name,
		       its_mfd_ndirs(&s.mfd));

	for (;;) {
		char *av[SH_MAXARG];
		int ac;

		if (s.interactive) {
			printf("%s (ro)> ", s.cwd[0] ? s.cwd : "-");
			fflush(stdout);
		}

		if (fgets(line, sizeof line, stdin) == NULL)
			break;

		ac = split(line, av, SH_MAXARG);

		if (ac == 0)
			continue;

		if (strcmp(av[0], "quit") == 0 || strcmp(av[0], "exit") == 0)
			break;
		else if (strcmp(av[0], "help") == 0 || strcmp(av[0], "?") == 0)
			cmd_help();
		else if (strcmp(av[0], "dirs") == 0)
			sh_dirs(&s, ac > 1 && strcmp(av[1], "-l") == 0);
		else if (strcmp(av[0], "pwd") == 0)
			printf("%s\n", s.cwd[0] ? s.cwd : "(no current directory)");
		else if (strcmp(av[0], "cd") == 0) {
			if (ac < 2) {
				s.cwd[0] = '\0';
				printf("left the directory\n");
			}
			else {
				uint64_t blk;

				upcase(av[1]);

				if (find_dir(&s, av[1], &blk) == 0) {
					snprintf(s.cwd, sizeof s.cwd, "%s", av[1]);
					s.cwdblk = blk;
				}
			}
		}
		else if (strcmp(av[0], "ls") == 0)
			sh_ls(&s, ac > 1 && strcmp(av[1], "-l") == 0);
		else if (strcmp(av[0], "type") == 0 || strcmp(av[0], "cat") == 0) {
			if (ac < 2)
				printf("usage: type FN1 [FN2]\n");
			else {
				upcase(av[1]);

				if (ac > 2)
					upcase(av[2]);
				sh_type(&s, av[1], ac > 2 ? av[2] : NULL);
			}
		}
		else if (strcmp(av[0], "blocks") == 0) {
			if (ac < 2)
				printf("usage: blocks FN1 [FN2]\n");
			else {
				upcase(av[1]);

				if (ac > 2)
					upcase(av[2]);
				sh_blocks(&s, av[1], ac > 2 ? av[2] : NULL);
			}
		}
		else if (strcmp(av[0], "stat") == 0) {
			if (ac < 2)
				printf("usage: stat FN1 [FN2]\n");
			else {
				upcase(av[1]);

				if (ac > 2)
					upcase(av[2]);
				sh_stat(&s, av[1], ac > 2 ? av[2] : NULL);
			}
		}
		else if (strcmp(av[0], "free") == 0)
			sh_free(&s);
		else if (strcmp(av[0], "info") == 0)
			sh_info(&s);
		else
			printf("unknown command '%s' -- `help` lists them\n", av[0]);
	}

	if (s.interactive)
		printf("\n");
	rc = 0;

	if (s.have_mfd)
		its_mfd_free(&s.mfd);
	img_close(&im);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs shell [-p packing] [-d drive] image\n");
	return 2;
}
