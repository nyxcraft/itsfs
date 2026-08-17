/*
 * structure.c -- the read-only reader.  See structure.h for the shape of what
 * is being read and its.h for where every offset comes from.
 */

#define _POSIX_C_SOURCE 200809L

#include "structure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t *
read_blocks(its_image *im, uint64_t blk, unsigned n, unsigned *wpb_out)
{
	unsigned wpb = img_words_per_block(im);
	uint64_t *w;

	if (wpb == 0) {
		fprintf(stderr, "itsfs: %s: the drive is unknown, so there are no blocks\n", im->path);
		return NULL;
	}

	w = calloc((size_t)wpb * n, sizeof *w);

	if (w == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		return NULL;
	}

	for (unsigned i = 0; i < n; i++)
		if (img_read_block(im, blk + i, w + (size_t)i * wpb, wpb) != 0) {
			free(w);
			return NULL;
		}

	*wpb_out = wpb;
	return w;
}

int
its_mfd_read(its_image *im, its_mfd *m)
{
	memset(m, 0, sizeof *m);

	if (im->drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", im->path);
		return -1;
	}

	m->blk = its_mfd_block(im->drv);
	m->w = read_blocks(im, m->blk, 1, &m->wpb);

	if (m->w == NULL)
		return -1;

	/*
	 * THE CHECK WORD IS CHECKED.  MDCHK exists precisely so that something
	 * reading this block can tell whether it is the MFD, and a reader that
	 * skips it will happily present a mail file as a directory listing.
	 */
	if (m->w[ITS_MD_CHK] != ITS_MFD_MAGIC) {
		char got[ITS_NAME_MAX];

		its_sixbit_word(m->w[ITS_MD_CHK], got);
		fprintf(stderr, "itsfs: %s: block %llu is not an MFD: MDCHK is |%s| (%012llo), "
				"not |M.F.D.|\n",
			im->path, (unsigned long long)m->blk, got,
			(unsigned long long)m->w[ITS_MD_CHK]);
		free(m->w);
		m->w = NULL;
		return -1;
	}

	m->namp = m->w[ITS_MD_NAMP];
	m->nudsl = m->w[ITS_MD_NUDS];

	/* Both came off the disk and both are about to be used as indices.
	 * MDNAMP == wpb is an MFD with no directories in it, which is legal for
	 * the same reason UDNAMP == wpb is -- see its_ufd_read. */
	if (m->namp > m->wpb || m->namp < ITS_LMIBLK) {
		fprintf(stderr, "itsfs: %s: MDNAMP is %llu, outside a %u-word block\n",
			im->path, (unsigned long long)m->namp, m->wpb);
		free(m->w);
		m->w = NULL;
		return -1;
	}

	return 0;
}

void
its_mfd_free(its_mfd *m)
{
	free(m->w);
	m->w = NULL;
}

unsigned
its_mfd_slots(const its_mfd *m)
{
	return (unsigned)((m->wpb - m->namp) / ITS_LMNBLK);
}

/*
 * The position of an entry is the address of the directory.
 *
 *      block = (A - 2000 + LMNBLK*NUDSL) / 2          [FSDEFS, and QFL2]
 *
 * with 2000 octal being the block size.  Two things worth stating plainly,
 * because both are easy to get wrong and neither is checkable by inspection:
 *
 *   - NUDSL is taken from the pack (C(MDNUDS)), not from a constant here.  It
 *     is a build-time parameter of the monitor that wrote the pack, and a pack
 *     built with a different one is still a legal pack.
 *   - the arithmetic can go negative in principle, which on unsigned words
 *     would produce an enormous block number rather than an error, so it is
 *     done signed and checked.
 */
int
its_mfd_dir(const its_mfd *m, unsigned i, char name[ITS_NAME_MAX], uint64_t *blk)
{
	uint64_t a = m->namp + (uint64_t)i * ITS_LMNBLK;
	long long b;

	if (i >= its_mfd_slots(m))
		return 1;

	its_sixbit_name(m->w[a], name);

	b = (long long)a - (long long)m->wpb + (long long)ITS_LMNBLK * (long long)m->nudsl;

	if (b < 0 || b % ITS_LMNBLK != 0)
		return -1;
	*blk = (uint64_t)(b / ITS_LMNBLK);

	return 0;
}

int
its_ufd_read(its_image *im, uint64_t blk, its_ufd *u)
{
	memset(u, 0, sizeof *u);
	u->blk = blk;
	u->w = read_blocks(im, blk, 1, &u->wpb);

	if (u->w == NULL)
		return -1;

	its_sixbit_name(u->w[ITS_UD_NAME], u->name);
	u->namp = u->w[ITS_UD_NAMP];

	/*
	 * UDNAMP indexes this block, and a UFD that has never been written --
	 * every slot the MFD has room for exists on the disk whether or not a
	 * directory was ever made there -- reads as zero.  That is refused: a
	 * name area at word 0 would overlap the header it is supposed to follow.
	 *
	 * BUT UDNAMP == WPB IS LEGAL AND MEANS EMPTY.  ITS writes exactly that
	 * when it makes a directory -- `MOVEI A,2000 / MOVEM A,UDNAMP-1(B)` in
	 * QSKON, disk.1228 -- so the name area starts past the end of the block
	 * and there are no entries yet.  This reader refused it once
	 * and nobody noticed, because every directory on the reference pack has
	 * at least one file in it (the lowest UDNAMP there is 1019, which is
	 * one entry).  Writing `mkdir` is what found it.
	 */
	if (u->namp < ITS_UD_DESC || u->namp > u->wpb) {
		fprintf(stderr, "itsfs: block %llu is not a UFD: UDNAMP is %llu\n",
			(unsigned long long)blk, (unsigned long long)u->namp);
		free(u->w);
		u->w = NULL;
		return -1;
	}

	return 0;
}

void
its_ufd_free(its_ufd *u)
{
	free(u->w);
	u->w = NULL;
}

int
its_ufd_next(const its_ufd *u, unsigned *idx, its_ent *e)
{
	unsigned i = *idx;

	if (i < u->namp)
		i = (unsigned)u->namp;

	if (i + ITS_LUNBLK > u->wpb)
		return 0;

	memset(e, 0, sizeof *e);
	e->word = i;
	its_sixbit_name(u->w[i + ITS_UN_FN1], e->fn1);
	its_sixbit_name(u->w[i + ITS_UN_FN2], e->fn2);

	e->rndm = u->w[i + ITS_UN_RNDM];
	e->date = u->w[i + ITS_UN_DATE];
	e->ref = u->w[i + ITS_UN_REF];

	e->desc = (unsigned)ITS_FIELD(e->rndm, ITS_UN_DSCP_P, ITS_UN_DSCP_S);
	e->pack = (unsigned)ITS_FIELD(e->rndm, ITS_UN_PKN_P, ITS_UN_PKN_S);
	e->lastwc = (unsigned)ITS_FIELD(e->rndm, ITS_UN_WRDC_P, ITS_UN_WRDC_S);
	e->is_link = (int)ITS_FIELD(e->rndm, ITS_UN_LNK_P, ITS_UN_LNK_S);

	/* The ignore bits sit in the field the link bit is the bottom of; see
	 * its.h, where the field's width is marked as a deduction. */
	e->deleted = ((unsigned)ITS_FIELD(e->rndm, ITS_UN_FLAGS_P, ITS_UN_FLAGS_S) & ITS_UN_IGFL) != 0;

	e->day = (unsigned)ITS_FIELD(e->date, ITS_UN_DAY_P, ITS_UN_DAY_S);
	e->month = (unsigned)ITS_FIELD(e->date, ITS_UN_MON_P, ITS_UN_MON_S);
	e->year = (unsigned)ITS_FIELD(e->date, ITS_UN_YRB_P, ITS_UN_YRB_S) + 1900;
	e->time = (unsigned)ITS_FIELD(e->date, ITS_UN_TIM_P, ITS_UN_TIM_S);

	e->author = (unsigned)ITS_FIELD(e->ref, ITS_UN_AUTH_P, ITS_UN_AUTH_S);
	e->bytesz = (unsigned)ITS_FIELD(e->ref, ITS_UN_BYTE_P, ITS_UN_BYTE_S);

	*idx = i + ITS_LUNBLK;
	return 1;
}

/*
 * Read the descriptor byte at six-bit offset `off`, counting from word
 * ITS_UD_DESC of the directory block.
 *
 * THE ORIGIN IS THE THING TO GET RIGHT, and it is not word 0.  UNDSCP counts
 * from UDDESC, the first word a descriptor may occupy, so a pointer of 1012
 * means word 11 + 1012/6.  Reading it from word 0 instead lands 66 bytes early,
 * where a well-formed pack holds zero -- so every file appears to be empty and
 * nothing complains.  That is exactly what happened here first, and the bug is
 * recorded in docs/validation.md rather than only in this comment.
 */
static int
desc_byte(const its_ufd *u, unsigned off, unsigned *out)
{
	unsigned word = ITS_UD_DESC + off / ITS_UFDBPW;

	if (word >= u->wpb)
		return -1;
	*out = its_byte6(u->w[word], off % ITS_UFDBPW);
	return 0;
}

long
its_desc_blocks(const its_ufd *u, const its_drive *d, unsigned desc, uint64_t *blocks, size_t max,
		const char **err)
{
	uint64_t b = 0;
	int loaded = 0;
	long n = 0;
	unsigned off = desc;
	/* One pass over the block's worth of six-bit bytes is the hard cap: a
	 * descriptor cannot legitimately be longer than the block holding it,
	 * and a corrupt one that loops would otherwise run forever. */
	unsigned steps = u->wpb * ITS_UFDBPW;

	*err = NULL;

	while (steps-- > 0) {
		unsigned c;

		if (desc_byte(u, off++, &c) != 0) {
			*err = "descriptor ran off the end of the directory block";
			return -1;
		}

		if (c == 0)
			return n;

		if (c >= ITS_UD_LOADAD) {
			unsigned c2, c3;

			if (desc_byte(u, off++, &c2) != 0 || desc_byte(u, off++, &c3) != 0) {
				*err = "truncated load address";
				return -1;
			}

			b = ((uint64_t)(c & 037) << 12) | ((uint64_t)c2 << 6) | c3;
			loaded = 1;
		}
		else if (c == ITS_UD_WPH) {
			continue;
		}
		else if (c <= ITS_UD_TKMX) {
			if (!loaded) {
				*err = "take before any load address";
				return -1;
			}
			/* fall through to the run below with b unchanged */
		}
		else {
			if (!loaded) {
				*err = "skip before any load address";
				return -1;
			}
			b += c - ITS_UD_TKMX;
		}

		{
			/* Only "take N" produces a run; a load address and a skip
			 * each contribute exactly one block. */
			unsigned run = (c <= ITS_UD_TKMX) ? c : 1;

			for (unsigned k = 0; k < run; k++, b++) {
				if (d != NULL && b >= its_tblks(d)) {
					*err = "block number past the end of the drive";
					return -1;
				}

				if (blocks != NULL) {
					if ((size_t)n >= max) {
						*err = "more blocks than a file can have";
						return -1;
					}
					blocks[n] = b;
				}

				n++;
			}
		}
	}

	*err = "descriptor did not end";
	return -1;
}

/*
 * A link's target: three SIXBIT components -- directory, first name, second
 * name -- ended by a zero byte.
 *
 * A component shorter than six characters is ended by ";" (073), and ";", ":"
 * and space are each quoted by a preceding ":" (072).  FSDEFS's own examples,
 * which are the test cases:
 *
 *      123456123456123456      three full six-character names, no separators
 *      ALAN;FOO;BAR            three short ones
 *      .MAIL.: LISTS: : MSGS   quoted leading spaces in the second and third
 *      MOON;LUNAR;::EJ         a quoted colon inside the third
 *
 * It is rendered as ITS itself writes a file name: DIR;FN1 FN2.
 */
int
its_link_target(const its_ufd *u, unsigned desc, char *out, size_t outsz, const char **err)
{
	unsigned off = desc;
	char comp[3][ITS_NAME_MAX];
	int ncomp = 0;

	*err = NULL;
	memset(comp, 0, sizeof comp);

	for (; ncomp < 3; ncomp++) {
		int n = 0;

		while (n < ITS_SIXBIT_CHARS) {
			unsigned c;

			if (desc_byte(u, off++, &c) != 0) {
				*err = "link ran off the end of the directory block";
				return -1;
			}

			if (c == 0)
				goto done; /* the zero byte ends the whole name */

			if (c == ITS_LNK_QUOTE) {
				if (desc_byte(u, off++, &c) != 0) {
					*err = "truncated quote in a link";
					return -1;
				}
			}
			else if (c == ITS_LNK_SEP) {
				break; /* a short component ends here */
			}

			comp[ncomp][n++] = (char)(c + 040);
		}

		comp[ncomp][n] = '\0';
	}

done:
	if ((size_t)snprintf(out, outsz, "%s;%s %s", comp[0], comp[1], comp[2]) >= outsz) {
		*err = "link target is longer than a link target can be";
		return -1;
	}

	return 0;
}

uint64_t
its_file_words(unsigned wpb, long nblocks, unsigned lastwc)
{
	if (nblocks <= 0)
		return 0;

	/* UNWRDC is the last block's word count MOD 2000 octal, so a last block
	 * that is exactly full records zero.  A file of n blocks always has at
	 * least (n-1)*wpb words, which is what makes that unambiguous. */
	return (uint64_t)(nblocks - 1) * wpb + (lastwc ? lastwc : wpb);
}

int
its_tut_read(its_image *im, its_tut *t)
{
	unsigned wpb = 0;

	memset(t, 0, sizeof *t);

	if (im->drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", im->path);
		return -1;
	}

	t->w = read_blocks(im, its_tut_block(im->drv), im->drv->ntutbl, &wpb);

	if (t->w == NULL)
		return -1;

	t->nwords = (size_t)wpb * im->drv->ntutbl;
	t->pknum = t->w[ITS_Q_PKNUM];
	its_sixbit_name(t->w[ITS_Q_PAKID], t->pakid);
	t->tutp = t->w[ITS_Q_TUTP];
	t->swapa = t->w[ITS_Q_SWAPA];
	t->first = t->w[ITS_Q_FRSTB];
	t->last = t->w[ITS_Q_LASTB];

	/*
	 * QFRSTB and QLASTB bound every later loop, so they are bounded here.
	 * The map has room for (nwords - LTIBLK) * TUTEPW blocks and no more.
	 */
	if (t->last < t->first || t->last - t->first > (t->nwords - ITS_LTIBLK) * ITS_TUTEPW) {
		fprintf(stderr, "itsfs: the TUT maps blocks %llu..%llu, which does not fit in %zu words\n",
			(unsigned long long)t->first, (unsigned long long)t->last, t->nwords);
		free(t->w);
		t->w = NULL;
		return -1;
	}

	return 0;
}

void
its_tut_free(its_tut *t)
{
	free(t->w);
	t->w = NULL;
}

int
its_tut_entry(const its_tut *t, uint64_t blk)
{
	uint64_t rel, word;
	unsigned slot;

	if (blk < t->first || blk >= t->last)
		return -1;

	rel = blk - t->first;
	word = ITS_LTIBLK + rel / ITS_TUTEPW;
	slot = (unsigned)(rel % ITS_TUTEPW);

	if (word >= t->nwords)
		return -1;

	/* Twelve three-bit entries, the first at the top of the word: the same
	 * left-to-right order every PDP-10 byte pointer walks in. */
	return (int)((t->w[word] >> (33 - ITS_TUTBYT * slot)) & (ITS_TUTMAX - 1));
}
