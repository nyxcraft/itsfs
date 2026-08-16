/*
 * cmd_saveset.c -- `itsfs saveset`, the ITS DUMP archive.
 *
 *   itsfs saveset [-p packing] tapefile          list what is on it
 *   itsfs saveset -x DIR [-p packing] tapefile   extract each file
 *
 * THE ARCHIVE LAYER, over the container `itsfs tape` reads.  A `.tap` file is a
 * sequence of records; a DUMP save set is a particular thing to put in them,
 * with a volume header, a header per file and its data, and a tape mark after
 * each.  The two are separate commands because they are separate formats and
 * because most ITS tapes are not save sets at all -- the ones the build makes
 * with `tapewrite` are simply a file per tape mark, which `tape` handles.
 *
 * WHERE THE FORMAT COMES FROM.  There is no specification.  ITS's DUMP wrote
 * these and `tools/itstar` in the PDP-10/its tree reads them; its `itstar.doc`
 * describes the CONTAINER exactly and the archive not at all.  So the layout
 * below is transcribed from what itstar's `scantape` reads, in its order, and
 * the field names are the ones its comments use.  itstar is GPL; no line of it
 * is here, and what is taken is what the format IS -- the same line this project
 * draws with FSDEFS and with NSALV.  See docs/sources.md.
 *
 * VOLUME HEADER, the first record:
 *
 *      1  an AOBJN pointer: the header's length is 1000000 octal minus its
 *         left half.  This is how every header here says how long it is.
 *      2  tape,,reel
 *      3  SIXBIT creation date, YYMMDD
 *      4  type: 0 random, positive full, negative incremental
 *
 * FILE HEADER, one per file:
 *
 *      1  an AOBJN pointer, as above
 *      2  UFD          SIXBIT
 *      3  FN1          SIXBIT
 *      4  FN2          SIXBIT
 *      5  linkf,,pack  a non-zero left half means this is a link
 *      6  creation date
 *      7  reference date
 *
 * and then the file's data in the records that follow, UP TO A TAPE MARK.  That
 * is what delimits a file: not a length in the header, which is why a truncated
 * tape loses the last file rather than reporting one.
 *
 * TWO THINGS THAT WOULD BE GUESSED WRONG:
 *
 *   - THE FIRST FILE HEADER MAY SHARE THE VOLUME RECORD.  itstar checks whether
 *     anything is left in the record after the volume header and jumps straight
 *     into the file-header path if so.  Every tape in the ITS tree does this, so
 *     a reader that assumed one header per record would lose the first file --
 *     and would list 73 of 74 without any sign of a problem.
 *
 *   - A LINK'S TARGET IS FN1, FN2, UFD -- in that order, which is NOT the order
 *     the header names them in.  It is in the record after the header.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "util.h"
#include "itspack.h"
#include "itstext.h"
#include "image.h"
#include "structure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define SS_MAXREC (1024u * 1024u)
#define SS_MAXWORDS (SS_MAXREC / 4u)

/* An AOBJN pointer counts up to zero from -n in its left half, so the length
 * it carries is 1000000 octal minus that half. */
#define SS_AOBJN_LEN(w) (01000000u - (unsigned)ITS_LH(w))

struct ss {
	const unsigned char *d;
	size_t n;
	size_t at;
	const its_pack *pk;

	uint64_t w[SS_MAXWORDS]; /* the current record, as words */
	size_t nw;
	size_t pos; /* how far into it we have read */
};

/*
 * The next record, decoded into words.  Returns 1 for a record, 0 for a tape
 * mark, -1 at the end or on damage.
 */
static int
ss_read(struct ss *s, const char **err)
{
	uint32_t n, tr;
	size_t pad;

	*err = NULL;
	s->nw = 0;
	s->pos = 0;

	if (s->at + 4 > s->n)
		return -1;

	n = (uint32_t)s->d[s->at] | ((uint32_t)s->d[s->at + 1] << 8) |
	    ((uint32_t)s->d[s->at + 2] << 16) | ((uint32_t)s->d[s->at + 3] << 24);
	s->at += 4;

	if (n == 0xFFFFFFFFu)
		return -1;

	if (n == 0)
		return 0;

	if (n > SS_MAXREC) {
		*err = "a record longer than a megabyte";
		return -1;
	}

	pad = n & 1u;

	if (s->at + n + pad + 4 > s->n) {
		*err = "a record that runs off the end of the file";
		return -1;
	}

	s->nw = n / s->pk->bytes * s->pk->words;

	if (s->nw > SS_MAXWORDS)
		s->nw = SS_MAXWORDS;
	its_get_words(s->pk, s->d + s->at, s->w, s->nw);
	s->at += n + pad;

	tr = (uint32_t)s->d[s->at] | ((uint32_t)s->d[s->at + 1] << 8) |
	     ((uint32_t)s->d[s->at + 2] << 16) | ((uint32_t)s->d[s->at + 3] << 24);
	s->at += 4;

	if (tr != n) {
		*err = "a record whose leading and trailing lengths disagree";
		return -1;
	}

	return 1;
}

/* Skip to just past the next tape mark.  This is what ends a file's data. */
static void
ss_skip_file(struct ss *s)
{
	const char *err;
	int r;

	while ((r = ss_read(s, &err)) > 0)
		;

	(void)r;
}

static unsigned char *
slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long sz;

	if (f == NULL) {
		perror(path);
		return NULL;
	}

	if (fseeko(f, 0, SEEK_END) != 0 || (sz = (long)ftello(f)) < 0) {
		perror(path);
		fclose(f);
		return NULL;
	}

	rewind(f);
	buf = malloc((size_t)sz + 1);

	if (buf == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		fclose(f);
		return NULL;
	}

	if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		perror(path);
		free(buf);
		fclose(f);
		return NULL;
	}

	fclose(f);
	*n = (size_t)sz;
	return buf;
}

/* A host file name for `DIR;FN1 FN2`, in itstar's shape: dir/fn1.fn2 */
static void
hostname(char *out, size_t sz, const char *dir, const char *fn1, const char *fn2)
{
	if (fn2[0] != '\0')
		snprintf(out, sz, "%s.%s", fn1, fn2);
	else
		snprintf(out, sz, "%s", fn1);
	(void)dir;
}

/* --------------------------------------------------------------- writing */

/*
 * A save set is written the way itstar writes one, and the shape is not
 * obvious: WORDS ARE APPENDED TO A RECORD BUFFER AND FLUSHED WHEN IT FILLS,
 * not one structure per record.  itstar's `save` has the flush after the file
 * header COMMENTED OUT on purpose -- the header and the data share a record,
 * and so do the volume header and the first file's header.  That is why the
 * reader above has to cope with it, and writing the "tidy" one-per-record form
 * would produce a tape unlike any ITS made.
 *
 * The constants are itstar's: RECLEN9 is 5*1024, so 1024 words to a record,
 * and a record must be at least 12 bytes -- short ones are padded with zero
 * words "for tape hardware".
 */
#define SS_RECWORDS 1024u
#define SS_MINBYTES 12u

struct sw {
	FILE *f;
	const its_pack *pk;
	uint64_t w[SS_RECWORDS];
	size_t nw;
	unsigned char bytes[SS_RECWORDS * 8];
};

static void
sw_le32(FILE *f, uint32_t v)
{
	fputc((int)(v & 0xFF), f);
	fputc((int)((v >> 8) & 0xFF), f);
	fputc((int)((v >> 16) & 0xFF), f);
	fputc((int)((v >> 24) & 0xFF), f);
}

/* Flush the buffer as one record, padded to the minimum if it is short. */
static int
sw_flush(struct sw *s)
{
	size_t n;

	if (s->nw == 0)
		return 0;

	while (s->nw * s->pk->bytes / s->pk->words < SS_MINBYTES && s->nw < SS_RECWORDS)
		s->w[s->nw++] = 0;

	n = s->nw / s->pk->words * s->pk->bytes;
	its_put_words(s->pk, s->bytes, s->w, s->nw);

	sw_le32(s->f, (uint32_t)n);

	if (fwrite(s->bytes, 1, n, s->f) != n)
		return -1;

	if (n & 1u)
		fputc(0, s->f); /* records are padded to an even length */
	sw_le32(s->f, (uint32_t)n);
	s->nw = 0;
	return 0;
}

static int
sw_word(struct sw *s, uint64_t w)
{
	if (s->nw == SS_RECWORDS && sw_flush(s) != 0)
		return -1;
	s->w[s->nw++] = w & ITS_WORD_MASK;
	return 0;
}

static int
sw_mark(struct sw *s)
{
	if (sw_flush(s) != 0)
		return -1;
	sw_le32(s->f, 0);
	return 0;
}

int
cmd_save(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_pack *tpk = its_pack_for(ITS_PACK_CORE);
	const its_drive *drv = NULL;
	its_image im;
	struct sw s;
	its_mfd m;
	int c, rc = 1, opened = 0;
	unsigned long nfile = 0;

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

	if (argc - optind < 3)
		goto usage;

	if (img_open(&im, argv[optind], pk, drv, 0) != 0)
		return 1;
	opened = 1;

	if (im.drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", im.path);
		goto out;
	}

	memset(&s, 0, sizeof s);
	s.pk = tpk;
	s.f = fopen(argv[optind + 1], "wb");

	if (s.f == NULL) {
		perror(argv[optind + 1]);
		goto out;
	}

	if (its_mfd_read(&im, &m) != 0)
		goto out;

	/*
	 * The volume header.  The date is the MFD's own MDYEAR and today is not
	 * available to this layer, so it is written as zeros rather than
	 * invented: a save set that claimed a creation date this project made up
	 * would be worse than one that admits it has none.
	 */
	sw_word(&s, ((uint64_t)(01000000 - 4) << 18));
	sw_word(&s, ((uint64_t)1 << 18) | 0); /* tape 1, reel 0 */
	sw_word(&s, 0);			      /* SIXBIT date: unknown */
	sw_word(&s, 0);			      /* type: random */

	for (int i = optind + 2; i < argc; i++) {
		char dir[ITS_SIXBIT_CHARS + 1], fn1[ITS_SIXBIT_CHARS + 1],
			fn2[ITS_SIXBIT_CHARS + 1];
		const char *semi = strchr(argv[i], ';');
		const char *sp;
		uint64_t dirblk = 0, n1, n2, nd;
		its_ufd u;
		unsigned idx;
		its_ent e;
		int found = 0;
		char have[ITS_SIXBIT_CHARS + 1];

		if (semi == NULL) {
			fprintf(stderr, "itsfs: '%s' is not a file name: it wants DIR;FN1 FN2\n",
				argv[i]);
			goto out;
		}

		memset(dir, 0, sizeof dir);
		memset(fn1, 0, sizeof fn1);
		memset(fn2, 0, sizeof fn2);

		if ((size_t)(semi - argv[i]) >= sizeof dir)
			goto toolong;
		memcpy(dir, argv[i], (size_t)(semi - argv[i]));
		sp = strchr(semi + 1, ' ');

		if (sp == NULL) {
			if (strlen(semi + 1) >= sizeof fn1)
				goto toolong;
			snprintf(fn1, sizeof fn1, "%s", semi + 1);
		}
		else {
			if ((size_t)(sp - semi - 1) >= sizeof fn1 || strlen(sp + 1) >= sizeof fn2)
				goto toolong;
			memcpy(fn1, semi + 1, (size_t)(sp - semi - 1));
			snprintf(fn2, sizeof fn2, "%s", sp + 1);
		}

		if (its_sixbit_make(dir, &nd) != 0 || its_sixbit_make(fn1, &n1) != 0 ||
		    its_sixbit_make(fn2, &n2) != 0) {
			fprintf(stderr, "itsfs: '%s' is not a SIXBIT name\n", argv[i]);
			goto out;
		}

		for (unsigned k = 0; k < its_mfd_slots(&m); k++) {
			uint64_t b;

			if (its_mfd_dir(&m, k, have, &b) != 0 || have[0] == '\0')
				continue;

			if (strcmp(have, dir) == 0) {
				dirblk = b;
				found = 1;
				break;
			}
		}

		if (!found) {
			fprintf(stderr, "itsfs: no directory named '%s'\n", dir);
			goto out;
		}

		if (its_ufd_read(&im, dirblk, &u) != 0)
			goto out;

		found = 0;
		idx = (unsigned)u.namp;

		while (its_ufd_next(&u, &idx, &e))
			if (strcmp(e.fn1, fn1) == 0 && strcmp(e.fn2, fn2) == 0) {
				found = 1;
				break;
			}

		if (!found) {
			fprintf(stderr, "itsfs: no file '%s;%s %s'\n", dir, fn1, fn2);
			its_ufd_free(&u);
			goto out;
		}

		/*
		 * The file header.  Seven words, which is itstar's `len` unless
		 * -O asks for the six-word form.
		 *
		 * THE DATE WORD IS COPIED STRAIGHT FROM THE DISK, because the
		 * two layouts are the same one: itstar reads the tape's date as
		 * year<<9 | month<<5 | day in the left half, and FSDEFS puts
		 * UNYRB at bit 27, UNMON at 23 and UNDAY at 18 -- which is
		 * exactly that, measured from the start of the halfword.  So
		 * there is nothing to convert and nothing to get wrong.
		 */
		sw_word(&s, ((uint64_t)(01000000 - 7) << 18));
		sw_word(&s, nd);
		sw_word(&s, n1);
		sw_word(&s, n2);
		sw_word(&s, e.is_link ? ((uint64_t)1 << 18) : 0);
		sw_word(&s, e.date);
		sw_word(&s, e.ref & ~((uint64_t)0777777)); /* the ref date half */

		if (e.is_link) {
			char tgt[64];
			const char *err = NULL;
			char t1[ITS_SIXBIT_CHARS + 1], t2[ITS_SIXBIT_CHARS + 1],
				td[ITS_SIXBIT_CHARS + 1];
			uint64_t w1, w2, wd;
			char *semi2, *sp2;

			if (its_link_target(&u, e.desc, tgt, sizeof tgt, &err) != 0) {
				fprintf(stderr, "itsfs: '%s;%s %s': %s\n", dir, fn1, fn2, err);
				its_ufd_free(&u);
				goto out;
			}

			/* `DIR;FN1 FN2` back apart, to write it as FN1, FN2, UFD. */
			memset(td, 0, sizeof td);
			memset(t1, 0, sizeof t1);
			memset(t2, 0, sizeof t2);
			semi2 = strchr(tgt, ';');
			sp2 = semi2 ? strchr(semi2 + 1, ' ') : NULL;

			if (semi2 != NULL && sp2 != NULL) {
				size_t a = (size_t)(semi2 - tgt), b = (size_t)(sp2 - semi2 - 1);

				if (a < sizeof td && b < sizeof t1 && strlen(sp2 + 1) < sizeof t2) {
					memcpy(td, tgt, a);
					memcpy(t1, semi2 + 1, b);
					snprintf(t2, sizeof t2, "%s", sp2 + 1);
				}
			}

			if (its_sixbit_make(td, &wd) != 0 || its_sixbit_make(t1, &w1) != 0 ||
			    its_sixbit_make(t2, &w2) != 0) {
				fprintf(stderr, "itsfs: '%s;%s %s' has a target this cannot "
						"write: %s\n",
					dir, fn1, fn2, tgt);
				its_ufd_free(&u);
				goto out;
			}

			/* FN1, FN2, UFD -- itstar's own comment calls it a funny order. */
			sw_word(&s, w1);
			sw_word(&s, w2);
			sw_word(&s, wd);
		}
		else {
			uint64_t *blocks = NULL;
			const char *err = NULL;
			long nb = its_desc_blocks(&u, im.drv, e.desc, NULL, 0, &err);
			uint64_t nwords, left;
			uint64_t *blk;

			if (nb < 0 || nb > 200000) {
				fprintf(stderr, "itsfs: '%s;%s %s': %s\n", dir, fn1, fn2,
					err ? err : "impossibly long");
				its_ufd_free(&u);
				goto out;
			}

			blocks = calloc((size_t)nb + 1, sizeof *blocks);
			blk = calloc(u.wpb, sizeof *blk);

			if (blocks == NULL || blk == NULL) {
				fprintf(stderr, "itsfs: out of memory\n");
				free(blocks);
				free(blk);
				its_ufd_free(&u);
				goto out;
			}

			its_desc_blocks(&u, im.drv, e.desc, blocks, (size_t)nb, &err);
			nwords = its_file_words(u.wpb, nb, e.lastwc);
			left = nwords;

			for (long k = 0; k < nb && left > 0; k++) {
				size_t got = (left < u.wpb) ? (size_t)left : u.wpb;

				if (img_read_block(&im, blocks[k], blk, got) != 0) {
					free(blocks);
					free(blk);
					its_ufd_free(&u);
					goto out;
				}

				for (size_t j = 0; j < got; j++)
					sw_word(&s, blk[j]);
				left -= got;
			}

			free(blocks);
			free(blk);
		}

		its_ufd_free(&u);

		if (sw_mark(&s) != 0) {
			perror(argv[optind + 1]);
			goto out;
		}

		printf("%s;%s %s\n", dir, fn1, fn2);
		nfile++;
		continue;

	toolong:
		fprintf(stderr, "itsfs: a name component is at most %d characters\n",
			ITS_SIXBIT_CHARS);
		goto out;
	}

	/* A second mark ends the tape, which is what itstar's closetape does. */
	if (sw_mark(&s) != 0) {
		perror(argv[optind + 1]);
		goto out;
	}

	printf("%lu file%s written to %s\n", nfile, nfile == 1 ? "" : "s", argv[optind + 1]);
	rc = 0;
out:
	if (s.f != NULL)
		fclose(s.f);
	its_mfd_free(&m);

	if (opened)
		img_close(&im);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs save [-p packing] [-d drive] image tapefile 'DIR;FN1 FN2'...\n"
			"\n"
			"       Writes an ITS DUMP save set of files taken off a pack.\n");
	return 2;
}

int
cmd_saveset(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_CORE);
	const char *extract = NULL;
	unsigned char *buf;
	size_t nbuf;
	struct ss s;
	const char *err;
	unsigned long nfile = 0, nlink = 0;
	int c, rc = 1, first = 1;

	while ((c = getopt(argc, argv, "p:x:")) != -1) {
		switch (c) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'x':
			extract = optarg;
			break;
		default:
			goto usage;
		}
	}

	if (optind != argc - 1)
		goto usage;

	buf = slurp(argv[optind], &nbuf);

	if (buf == NULL)
		return 1;

	memset(&s, 0, sizeof s);
	s.d = buf;
	s.n = nbuf;
	s.pk = pk;

	if (ss_read(&s, &err) <= 0) {
		fprintf(stderr, "itsfs: %s\n", err ? err : "empty tape");
		goto out;
	}

	/* ---- the volume header. */
	{
		unsigned len = s.nw > 0 ? SS_AOBJN_LEN(s.w[0]) : 0;
		char date[ITS_SIXBIT_CHARS + 1];

		if (len < 4 || len > s.nw) {
			fprintf(stderr, "itsfs: %s does not begin with a DUMP volume header "
					"(its length word says %u)\n",
				argv[optind], len);
			goto out;
		}

		its_sixbit_name(s.w[2], date);
		printf("tape %llu, reel %llu, created %.2s/%.2s/%.2s, type %s\n",
		       (unsigned long long)ITS_LH(s.w[1]), (unsigned long long)ITS_RH(s.w[1]),
		       date + 2, date + 4, date,
		       (s.w[3] == 0)			 ? "random"
		       : (ITS_LH(s.w[3]) & 0400000) != 0 ? "incremental"
							 : "full");
		s.pos = len;
	}

	/* ---- and then a file header, its data, a tape mark, and again. */
	for (;;) {
		char dir[ITS_SIXBIT_CHARS + 1], fn1[ITS_SIXBIT_CHARS + 1], fn2[ITS_SIXBIT_CHARS + 1];
		unsigned len;
		int islink;

		/*
		 * The first file header may be in what is left of the volume
		 * record -- see the header comment.  Every other one starts a
		 * record of its own.
		 */
		if (!first || s.pos >= s.nw) {
			int r = ss_read(&s, &err);

			if (r < 0) {
				if (err != NULL) {
					fprintf(stderr, "itsfs: %s\n", err);
					goto out;
				}

				break; /* an ordinary end of tape */
			}

			if (r == 0)
				continue; /* a mark between files */
		}

		first = 0;

		if (s.pos + 4 > s.nw)
			break;

		len = SS_AOBJN_LEN(s.w[s.pos]);

		if (len < 4 || s.pos + len > s.nw)
			break; /* not a file header: the save set has ended */

		its_sixbit_name(s.w[s.pos + 1], dir);
		its_sixbit_name(s.w[s.pos + 2], fn1);
		its_sixbit_name(s.w[s.pos + 3], fn2);
		islink = (len > 4) && ITS_LH(s.w[s.pos + 4]) != 0;

		/*
		 * THE HEADER IS `len` WORDS AND THE RECORD MAY BE LONGER.  What
		 * follows in the same record is the file's data -- or, for a
		 * link, its target.  Skipping to the end of the record instead
		 * loses whatever shares it, and for links that is the NEXT
		 * FILE'S HEADER: this listed 3,734 of a tape's 3,795 files, one
		 * short per link, until itstar was asked the same question.
		 */
		s.pos += len;

		if (islink) {
			/*
			 * The target is three SIXBIT words -- FN1, FN2, UFD, in
			 * that order, which is NOT the order the header names
			 * them in -- and it is in this record if anything is
			 * left of it, otherwise the next.
			 */
			char t1[ITS_SIXBIT_CHARS + 1], t2[ITS_SIXBIT_CHARS + 1],
				td[ITS_SIXBIT_CHARS + 1];
			int have = 1;

			if (s.pos >= s.nw)
				have = ss_read(&s, &err) > 0;

			if (have && s.pos + 3 <= s.nw) {
				its_sixbit_name(s.w[s.pos + 0], t1);
				its_sixbit_name(s.w[s.pos + 1], t2);
				its_sixbit_name(s.w[s.pos + 2], td);
				printf("%s;%s %s -> %s;%s %s\n", dir, fn1, fn2, td, t1, t2);
			}
			else {
				printf("%s;%s %s -> (unreadable link)\n", dir, fn1, fn2);
			}

			nlink++;
			ss_skip_file(&s);
			continue;
		}

		nfile++;

		if (extract == NULL) {
			printf("%s;%s %s\n", dir, fn1, fn2);
			ss_skip_file(&s);
			continue;
		}

		/*
		 * Extraction writes WORDS, one per 8-byte little-endian
		 * container -- what `get -w` writes and `put -w` reads -- for
		 * the same reason `tape -x` does: the point of decoding is to
		 * stop caring which container it arrived in.  itstar instead
		 * converts to a host text encoding, which is a different and
		 * lossier job; see docs/validation.md.
		 */
		{
			char path[512], name[64];
			FILE *out;
			unsigned long long nw = 0;

			hostname(name, sizeof name, dir, fn1, fn2);
			snprintf(path, sizeof path, "%s/%s;%s", extract, dir, name);
			out = fopen(path, "wb");

			if (out == NULL) {
				perror(path);
				goto out;
			}

			/* THE DATA BEGINS IN WHAT IS LEFT OF THE HEADER'S OWN
			 * RECORD, for the reason above, and continues in the
			 * records after it up to a tape mark. */
			for (;;) {
				for (size_t i = s.pos; i < s.nw; i++) {
					unsigned char b[8];

					for (int k = 0; k < 8; k++)
						b[k] = (unsigned char)(s.w[i] >> (8 * k));

					if (fwrite(b, 1, 8, out) != 8) {
						perror(path);
						fclose(out);
						goto out;
					}

					nw++;
				}

				if (ss_read(&s, &err) <= 0)
					break;
			}

			fclose(out);
			printf("%s;%s %s -> %s (%llu words)\n", dir, fn1, fn2, path, nw);
		}
	}

	printf("%lu file%s, %lu link%s\n", nfile, nfile == 1 ? "" : "s", nlink,
	       nlink == 1 ? "" : "s");
	rc = 0;
out:
	free(buf);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs saveset [-p packing] [-x dir] tapefile\n"
			"       -p   the word packing (default core)\n"
			"       -x   extract each file to dir, as 36-bit words\n"
			"\n"
			"       Reads an ITS DUMP save set.  For the plain record\n"
			"       framing under it, use `itsfs tape`.\n");
	return 2;
}
