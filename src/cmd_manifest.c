/*
 * cmd_manifest.c -- `itsfs manifest` and `itsfs verify`.
 *
 *   itsfs manifest [opts] image [DIR]      one line per file, to stdout
 *   itsfs verify   [opts] image manifest   diff a pack against one
 *
 * A fingerprint of a pack: for every directory and every file in it, the type,
 * the length in words, the blocks it holds, and a checksum of its contents.
 * `verify` walks a second pack and reports what differs -- so a transformation
 * can be shown to have changed nothing, or shown exactly what it did change.
 *
 * THE CHECKSUM IS OVER WORDS, NOT BYTES.  A PDP-10 file is a sequence of 36-bit
 * words, and `itsfs repack` rewrites every byte of an image without altering a
 * single word.  A byte-wise checksum would make a manifest a fingerprint of the
 * CONTAINER: the same pack in `le64` and in `dbd9` would disagree about every
 * file, which is precisely backwards.  So each word is fed to the CRC as five
 * bytes -- bits 0-7, 8-15, 16-23, 24-31, and 32-35 in the low nibble of the
 * fifth: the `core` convention, chosen because it is lossless, canonical, and
 * already documented in docs/word-packing.md.  A manifest therefore survives
 * repacking, which is the property that makes it worth having.
 *
 * WHAT IS COMPARED, AND WHAT IS ONLY RECORDED.  `verify` compares the type, the
 * path and, for a file, the length and the checksum.  It does NOT compare the
 * block count: which blocks a file occupies is a property of the pack's
 * allocation history rather than of the file, and a file copied to a differently
 * filled pack legitimately lands somewhere else.  A fingerprint that reports
 * differences nobody cares about gets ignored, and then it reports nothing.
 *
 * A LINK IS RECORDED BY ITS TARGET, not by what the target contains.  Following
 * it would checksum the same file twice under two names, and -- since a link may
 * point at a file that is not there, which is an ordinary state of a live pack
 * (see cmd_check.c) -- would also make an unrelated deletion look like damage to
 * the link.  The target text is what changes when the link changes.
 *
 * Lines come out sorted by path, so two manifests can be compared with plain
 * `diff` even when the two packs list their directories in different orders --
 * which is the common case after a monitor has written to one.
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
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define MF_MAGIC "# itsfs manifest 1"

/* `DIR;FN1 FN2` is at most 6+1+6+1+6 = 20; the rest is slack. */
#define MF_PATHSZ 64

/* A file cannot need more blocks than the drive has. */
#define MF_MAXBLK 200000

/* One manifest line, parsed. */
struct mfline {
	char type; /* 'd', 'f' or 'l' */
	char path[MF_PATHSZ];
	uint64_t words;
	uint64_t blocks;
	char sum[MF_PATHSZ]; /* eight hex digits, or a link target */
};

/* ------------------------------------------------------------- the checksum */

static uint32_t
crc32_upd(uint32_t crc, const uint8_t *p, size_t n)
{
	static uint32_t tab[256];
	static int init = 0;

	if (!init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;

			for (uint32_t j = 0; j < 8; j++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}

		init = 1;
	}

	crc = ~crc;

	while (n-- > 0)
		crc = tab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/*
 * The checksum of `nwords` words of a file, read block by block.
 *
 * The five-bytes-per-word encoding is written out here rather than borrowed
 * from itspack.c's `core` codec, because the two are answering different
 * questions: `core` is a way of STORING a word that a tape happens to use, and
 * this is a canonical form for HASHING one.  They agree today, and if a future
 * packing changed `core`'s frame order for some hardware reason, a manifest
 * taken before that must still verify after it.
 */
static uint32_t
file_sum(its_image *im, const uint64_t *blocks, long nblocks, uint64_t nwords, unsigned wpb, int *err)
{
	uint32_t crc = 0;
	uint64_t left = nwords;
	uint64_t *w = calloc(wpb, sizeof *w);
	uint8_t *buf = malloc((size_t)wpb * 5);

	*err = 0;

	if (w == NULL || buf == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		*err = 1;
		free(w);
		free(buf);
		return 0;
	}

	for (long i = 0; i < nblocks && left > 0; i++) {
		size_t n = (left < wpb) ? (size_t)left : wpb;

		if (img_read_block(im, blocks[i], w, n) != 0) {
			*err = 1;
			break;
		}

		for (size_t k = 0; k < n; k++) {
			buf[k * 5 + 0] = (uint8_t)(w[k] >> 28);
			buf[k * 5 + 1] = (uint8_t)(w[k] >> 20);
			buf[k * 5 + 2] = (uint8_t)(w[k] >> 12);
			buf[k * 5 + 3] = (uint8_t)(w[k] >> 4);
			buf[k * 5 + 4] = (uint8_t)(w[k] & 0x0Fu);
		}

		crc = crc32_upd(crc, buf, n * 5);
		left -= n;
	}

	free(w);
	free(buf);
	return crc;
}

/* --------------------------------------------------------------- the walker */

struct mfctx {
	its_image *im;
	FILE *out;
	unsigned long nfiles, ndirs, nlinks, nbad;
	const char *only; /* a single directory, or NULL for all */
};

static int
walk_dir(struct mfctx *c, const char *name, uint64_t blk)
{
	its_ufd u;
	unsigned idx;
	its_ent e;

	if (its_ufd_read(c->im, blk, &u) != 0)
		return -1;

	fprintf(c->out, "d %8s %6llu %8s %s\n", "-", (unsigned long long)ITS_RH(u.w[ITS_UD_BLKS]),
		"-", name);
	c->ndirs++;

	idx = (unsigned)u.namp;

	while (its_ufd_next(&u, &idx, &e)) {
		const char *err = NULL;
		uint64_t *blocks;
		long nb;
		uint64_t words;
		uint32_t sum;
		int rerr = 0;

		if (e.fn1[0] == '\0' && e.fn2[0] == '\0')
			continue;

		if (e.is_link) {
			char tgt[MF_PATHSZ];

			if (its_link_target(&u, e.desc, tgt, sizeof tgt, &err) != 0) {
				fprintf(c->out, "! %8s %6s %8s %s;%s %s (%s)\n", "-", "-", "-",
					name, e.fn1, e.fn2, err);
				c->nbad++;
				continue;
			}

			fprintf(c->out, "l %8s %6s %8s %s;%s %s -> %s\n", "-", "-", "-", name,
				e.fn1, e.fn2, tgt);
			c->nlinks++;
			continue;
		}

		nb = its_desc_blocks(&u, c->im->drv, e.desc, NULL, 0, &err);

		if (nb < 0 || nb > MF_MAXBLK) {
			fprintf(c->out, "! %8s %6s %8s %s;%s %s (%s)\n", "-", "-", "-", name,
				e.fn1, e.fn2, err ? err : "impossibly long");
			c->nbad++;
			continue;
		}

		blocks = calloc((size_t)nb + 1, sizeof *blocks);

		if (blocks == NULL) {
			fprintf(stderr, "itsfs: out of memory\n");
			its_ufd_free(&u);
			return -1;
		}

		if (its_desc_blocks(&u, c->im->drv, e.desc, blocks, (size_t)nb, &err) != nb) {
			free(blocks);
			fprintf(c->out, "! %8s %6s %8s %s;%s %s (decoded differently twice)\n", "-",
				"-", "-", name, e.fn1, e.fn2);
			c->nbad++;
			continue;
		}

		words = its_file_words(u.wpb, nb, e.lastwc);
		sum = file_sum(c->im, blocks, nb, words, u.wpb, &rerr);
		free(blocks);

		if (rerr) {
			fprintf(c->out, "! %8s %6s %8s %s;%s %s (unreadable)\n", "-", "-", "-", name,
				e.fn1, e.fn2);
			c->nbad++;
			continue;
		}

		fprintf(c->out, "f %8llu %6ld %08lx %s;%s %s\n", (unsigned long long)words, nb,
			(unsigned long)sum, name, e.fn1, e.fn2);
		c->nfiles++;
	}

	its_ufd_free(&u);
	return 0;
}

/*
 * Walk the whole pack, or one directory.
 *
 * Output is piped through `sort` in the caller's shell?  No -- it is sorted
 * here, because a manifest that only sorts when somebody remembers to pipe it is
 * a manifest that silently stops being comparable.  The MFD's own order is by
 * directory block, which is stable for one pack and not across two.
 */
/*
 * Where the path starts in a manifest line: after the type and three fields.
 *
 * This exists because sorting the whole line does NOT sort by path -- the word
 * count is the second field, so `strcmp` on the line orders files by length.
 * The first version did exactly that, and it looked plausible until a directory
 * came out ordered 23, 98, 116, 269 words.  A sort key that is almost right is
 * worse than none, because the manifest still diffs cleanly against itself.
 */
static const char *
line_path(const char *s)
{
	const char *p = s;

	for (int f = 0; f < 4; f++) {
		while (*p == ' ')
			p++;

		while (*p != '\0' && *p != ' ' && *p != '\n')
			p++;
	}

	while (*p == ' ')
		p++;
	return p;
}

static int
cmp_line(const void *a, const void *b)
{
	return strcmp(line_path(*(const char *const *)a), line_path(*(const char *const *)b));
}

static int
manifest(its_image *im, FILE *out, const char *only)
{
	its_mfd m;
	struct mfctx c;
	char **lines = NULL;
	size_t nlines = 0, cap = 0;
	FILE *tmp;
	char buf[512];
	int rc = -1;

	if (its_mfd_read(im, &m) != 0)
		return -1;

	/* Collect into a temporary file, then sort.  A pack holds a few thousand
	 * lines, so this is small; doing it in memory would mean a growing
	 * buffer per line and no simpler. */
	tmp = tmpfile();

	if (tmp == NULL) {
		perror("tmpfile");
		its_mfd_free(&m);
		return -1;
	}

	memset(&c, 0, sizeof c);
	c.im = im;
	c.out = tmp;
	c.only = only;

	for (unsigned i = 0; i < its_mfd_slots(&m); i++) {
		char name[ITS_NAME_MAX];
		uint64_t blk;

		if (its_mfd_dir(&m, i, name, &blk) != 0 || name[0] == '\0')
			continue;

		if (only != NULL && strcmp(only, name) != 0)
			continue;

		if (walk_dir(&c, name, blk) != 0)
			fprintf(tmp, "! %8s %6s %8s %s (unreadable directory)\n", "-", "-", "-",
				name);
	}

	if (only != NULL && c.ndirs == 0) {
		fprintf(stderr, "itsfs: no directory named '%s' in the MFD\n", only);
		goto out;
	}

	rewind(tmp);

	while (fgets(buf, sizeof buf, tmp) != NULL) {
		if (nlines == cap) {
			size_t want = cap ? cap * 2 : 1024;
			char **bigger = realloc(lines, want * sizeof *bigger);

			if (bigger == NULL) {
				fprintf(stderr, "itsfs: out of memory\n");
				goto out;
			}

			lines = bigger;
			cap = want;
		}

		lines[nlines] = malloc(strlen(buf) + 1);

		if (lines[nlines] == NULL) {
			fprintf(stderr, "itsfs: out of memory\n");
			goto out;
		}

		strcpy(lines[nlines++], buf);
	}

	/*
	 * SORT ON THE PATH, not on the whole line, so that a file whose length
	 * or checksum changed still sorts next to its old self and `diff` shows
	 * one changed line rather than a deletion and an insertion.
	 *
	 * THE ZERO GUARD IS NOT DEFENSIVE PADDING.  `qsort(NULL, 0, ...)` is
	 * undefined behaviour -- the first argument is declared never-null
	 * regardless of the count -- and a pack whose MDNUDS is zero produces
	 * exactly that: every MFD slot fails to resolve, so nothing is walked
	 * and `lines` is still NULL.  The corruption fuzzer found it by zeroing
	 * word 6 of the MFD; nothing else in the suite reached it, because a
	 * pack with no directories at all is not a case anybody writes by hand.
	 */
	if (nlines > 0)
		qsort(lines, nlines, sizeof *lines, cmp_line);

	fprintf(out, "%s\n", MF_MAGIC);
	fprintf(out, "# pack %s, %u directories, %s\n", im->drv->name, its_mfd_ndirs(&m),
		im->pk->name);
	fprintf(out, "# type    words blocks checksum path\n");

	for (size_t i = 0; i < nlines; i++)
		fputs(lines[i], out);

	fprintf(out, "# %lu directories, %lu files, %lu links", c.ndirs, c.nfiles, c.nlinks);

	if (c.nbad != 0)
		fprintf(out, ", %lu unreadable", c.nbad);
	fprintf(out, "\n");

	rc = 0;
out:
	for (size_t i = 0; i < nlines; i++)
		free(lines[i]);
	free(lines);
	fclose(tmp);
	its_mfd_free(&m);
	return rc;
}

/* ---------------------------------------------------------------- the diff */

/*
 * Parse a manifest line.  Returns 0, 1 for a comment or blank, or -1 with a
 * message -- and a damaged manifest is refused rather than half-read, because a
 * verify that silently skipped the lines it could not parse would report a pack
 * as clean on the strength of not having looked.
 */
static int
parse_line(const char *s, unsigned long lineno, struct mfline *out)
{
	char type;
	char words[32], blocks[32], sum[MF_PATHSZ];
	const char *p;
	size_t n;

	if (s[0] == '#' || s[0] == '\n' || s[0] == '\0')
		return 1;

	memset(out, 0, sizeof *out);

	if (sscanf(s, "%c %31s %31s %63s", &type, words, blocks, sum) != 4)
		goto bad;

	if (type != 'd' && type != 'f' && type != 'l' && type != '!')
		goto bad;

	/* The path is the rest of the line: it contains a space, so it cannot be
	 * scanned as a field.  Find it by stepping over the four before it. */
	p = s;

	for (int f = 0; f < 4; f++) {
		while (*p == ' ')
			p++;

		while (*p != '\0' && *p != ' ' && *p != '\n')
			p++;
	}

	while (*p == ' ')
		p++;

	n = strcspn(p, "\n");

	/* A link's line ends `PATH -> TARGET`; the target belongs in `sum`, so
	 * that a changed link shows as a changed line rather than a moved one. */
	if (type == 'l') {
		const char *arrow = strstr(p, " -> ");

		if (arrow != NULL && (size_t)(arrow - p) < n) {
			n = (size_t)(arrow - p);
			snprintf(out->sum, sizeof out->sum, "%s", arrow + 4);
			out->sum[strcspn(out->sum, "\n")] = '\0';
		}
	}
	else {
		snprintf(out->sum, sizeof out->sum, "%s", sum);
	}

	if (n == 0 || n >= sizeof out->path)
		goto bad;

	memcpy(out->path, p, n);
	out->path[n] = '\0';
	out->type = type;

	/* `-` where a number would be; anything else must parse. */
	if (strcmp(words, "-") != 0 && parse_u64_base(words, 10, &out->words) != 0)
		goto bad;

	if (strcmp(blocks, "-") != 0 && parse_u64_base(blocks, 10, &out->blocks) != 0)
		goto bad;

	return 0;

bad:
	fprintf(stderr, "itsfs: manifest line %lu is malformed: %.60s", lineno, s);

	if (strchr(s, '\n') == NULL)
		fprintf(stderr, "\n");
	return -1;
}

static int
load_manifest(const char *path, struct mfline **out, size_t *nout)
{
	FILE *f = fopen(path, "r");
	char buf[512];
	struct mfline *v = NULL;
	size_t n = 0, cap = 0;
	unsigned long lineno = 0;
	int seen_magic = 0;

	if (f == NULL) {
		perror(path);
		return -1;
	}

	while (fgets(buf, sizeof buf, f) != NULL) {
		struct mfline l;
		int r;

		lineno++;

		if (lineno == 1 && strncmp(buf, MF_MAGIC, strlen(MF_MAGIC)) == 0)
			seen_magic = 1;

		r = parse_line(buf, lineno, &l);

		if (r < 0)
			goto bad;

		if (r > 0)
			continue;

		if (n == cap) {
			size_t want = cap ? cap * 2 : 1024;
			struct mfline *bigger = realloc(v, want * sizeof *bigger);

			if (bigger == NULL) {
				fprintf(stderr, "itsfs: out of memory\n");
				goto bad;
			}

			v = bigger;
			cap = want;
		}

		v[n++] = l;
	}

	fclose(f);

	if (!seen_magic) {
		fprintf(stderr, "itsfs: %s does not begin with `%s'\n", path, MF_MAGIC);
		free(v);
		return -1;
	}

	*out = v;
	*nout = n;
	return 0;

bad:
	fclose(f);
	free(v);
	return -1;
}

static int
cmp_mfline(const void *a, const void *b)
{
	return strcmp(((const struct mfline *)a)->path, ((const struct mfline *)b)->path);
}

/*
 * Diff a pack against a manifest.
 *
 * The pack is fingerprinted into a temporary manifest and the two are compared
 * as sorted lists, which is why `manifest` sorts at all.  Reporting is `!` for
 * changed, `+` for only on the pack and `-` for only in the manifest, which is
 * the vocabulary s5fs and t10fs both use.
 */
static int
verify(its_image *im, const char *mfpath)
{
	struct mfline *want = NULL, *have = NULL;
	size_t nwant = 0, nhave = 0, i = 0, j = 0;
	unsigned long diffs = 0;
	FILE *tmp;
	char tmpname[] = "/tmp/itsfs-verify-XXXXXX";
	int fd, rc = -1;

	if (load_manifest(mfpath, &want, &nwant) != 0)
		return -1;

	fd = mkstemp(tmpname);

	if (fd < 0) {
		perror("mkstemp");
		goto out;
	}

	tmp = fdopen(fd, "w+");

	if (tmp == NULL) {
		perror(tmpname);
		close(fd);
		goto out;
	}

	if (manifest(im, tmp, NULL) != 0) {
		fclose(tmp);
		unlink(tmpname);
		goto out;
	}

	/*
	 * CHECK THE CLOSE.  This manifest is written to a temporary file and
	 * read straight back to compare against, so a write that failed half
	 * way -- a full disk is the ordinary way -- would not produce an error
	 * here, it would produce a SHORTER MANIFEST, and every file missing
	 * from the truncated end would be reported as a difference.
	 *
	 * fclose is where that surfaces: a deferred write error is reported
	 * when the stream is flushed, not when fprintf returned.  The fflush
	 * that used to be here was unchecked and did the same job twice.
	 */
	if (fclose(tmp) != 0) {
		fprintf(stderr, "itsfs: %s: %s\n", tmpname, strerror(errno));
		unlink(tmpname);
		goto out;
	}

	if (load_manifest(tmpname, &have, &nhave) != 0) {
		unlink(tmpname);
		goto out;
	}

	unlink(tmpname);

	/* Both guarded for the reason above: an empty manifest and a pack with
	 * no readable directories are each a legitimate way to reach zero. */
	if (nwant > 0)
		qsort(want, nwant, sizeof *want, cmp_mfline);

	if (nhave > 0)
		qsort(have, nhave, sizeof *have, cmp_mfline);

	while (i < nwant || j < nhave) {
		int c;

		if (i >= nwant)
			c = 1;
		else if (j >= nhave)
			c = -1;
		else
			c = strcmp(want[i].path, have[j].path);

		if (c < 0) {
			printf("- %s\n", want[i].path);
			diffs++;
			i++;
		}
		else if (c > 0) {
			printf("+ %s\n", have[j].path);
			diffs++;
			j++;
		}
		else {
			if (want[i].type != have[j].type)
				printf("! %s (%c became %c)\n", want[i].path, want[i].type,
				       have[j].type);
			else if (want[i].type == 'l' && strcmp(want[i].sum, have[j].sum) != 0)
				printf("! %s (points at %s, was %s)\n", want[i].path, have[j].sum,
				       want[i].sum);
			else if (want[i].type == 'f' && want[i].words != have[j].words)
				printf("! %s (%llu words, was %llu)\n", want[i].path,
				       (unsigned long long)have[j].words,
				       (unsigned long long)want[i].words);
			else if (want[i].type == 'f' && strcmp(want[i].sum, have[j].sum) != 0)
				printf("! %s (contents differ)\n", want[i].path);
			else {
				i++;
				j++;
				continue;
			}

			diffs++;
			i++;
			j++;
		}
	}

	printf("%lu difference%s\n", diffs, diffs == 1 ? "" : "s");
	rc = diffs ? 1 : 0;
out:
	free(want);
	free(have);
	return rc;
}

/* ------------------------------------------------------------------- main */

static int
run(int argc, char **argv, int is_verify)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_image im;
	int c, rc = 2;

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

	if (is_verify ? (optind != argc - 2) : (optind != argc - 1 && optind != argc - 2))
		goto usage;

	if (img_open(&im, argv[optind], pk, drv, 0) != 0)
		return 2;

	if (im.drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", im.path);
		img_close(&im);
		return 2;
	}

	if (is_verify) {
		rc = verify(&im, argv[optind + 1]);

		/* A manifest that cannot be read is an input error, not a
		 * difference: 2 like every other refusal here, never a negative
		 * that the shell would show as 255. */
		if (rc < 0)
			rc = 2;
	}
	else
		rc = manifest(&im, stdout, optind == argc - 2 ? argv[optind + 1] : NULL) == 0 ? 0 : 1;

	img_close(&im);
	return rc;

usage:
	if (is_verify)
		fprintf(stderr, "usage: itsfs verify [-p packing] [-d drive] image manifest\n");
	else
		fprintf(stderr, "usage: itsfs manifest [-p packing] [-d drive] image [DIR]\n");
	return 2;
}

int
cmd_manifest(int argc, char **argv)
{
	return run(argc, argv, 0);
}

int
cmd_verify(int argc, char **argv)
{
	return run(argc, argv, 1);
}
