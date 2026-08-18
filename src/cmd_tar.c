/*
 * cmd_tar.c -- `itsfs tar`, a pack in and out of an ordinary Unix tar archive.
 *
 *   itsfs tar c [-p packing] [-d drive] [-m mode] [-v] archive image [name...]
 *   itsfs tar t [-v] archive
 *   itsfs tar x [-p packing] [-d drive] [-m mode] [-v] archive image
 *
 * THE POINT OF THIS COMMAND is to get an ITS pack somewhere a Unix machine can
 * work on it.  `saveset` already moves files through ITS's own DUMP format,
 * which is the right thing when the other end is ITS; this is for when the
 * other end is `tar xf` and a text editor.
 *
 * The format written is USTAR (POSIX.1-1988): 512-byte header, 512-byte blocks,
 * two zero blocks at the end.  Nothing here needs GNU or pax extensions -- an
 * ITS path is at most `DIR/FN1 FN2`, twenty characters, against a 100-character
 * name field -- so what comes out is readable by every tar there is.
 *
 * Archiving a whole directory writes a directory member for it; archiving FILES
 * by name writes only those members, as tar does.  `tar x` therefore creates a
 * directory it does not find, which is also what tar does.
 *
 * THE NAME MAPPING.  ITS names a file `DIR;FN1 FN2`; that becomes `DIR/FN1 FN2`,
 * with a real directory member for `DIR/` ahead of its files.  The space stays a
 * space: it is part of the name, and turning it into a dot would make two
 * different ITS files collide on one host name.
 *
 * NAMES THAT ARE NOT PATH COMPONENTS ARE PERCENT-ENCODED, and this is not a
 * hypothetical: SIXBIT runs from 040 to 0137, so it holds `/`, `.`, `%` and the
 * space, and the reference pack uses three of the four.  There is a REAL ITS
 * DIRECTORY NAMED `.` on it -- 18 entries, 242 blocks, holding `@ ITS`, `@ DDT`
 * and `@ NSALV`, which is to say the monitor itself -- and `.` is not a name a
 * tar member can have.  Ten names contain a `%`.
 *
 * So each component is encoded on the way out and decoded on the way back:
 *
 *      %       %25     always, or the encoding would not be reversible
 *      /       %2F     it is the path separator here
 *      space   %20     it is the separator between FN1 and FN2 here
 *      .       %2E     only when the whole component is `.` or `..`
 *
 * Nothing else is touched, so an ordinary name is itself and `KSHACK/BUILD DOC`
 * reads as it should.  The directory named `.` becomes `%2E/`.
 *
 * THE HARD PART IS NOT THE ARCHIVE, IT IS WHAT A FILE *IS*.
 *
 * An ITS file is 36-bit words.  A tar member is bytes.  There are two honest
 * ways to make bytes out of a word and they are not interchangeable:
 *
 *   text    five 7-bit characters per word, which is what ITS text is and what
 *           `cat` and `get` write.  Right for source, documentation, mail.
 *           Applied to a binary it produces garbage, silently.
 *
 *   words   the 36-bit word in an 8-byte little-endian container, which is what
 *           `get -w` writes.  Lossless for anything, and unreadable as text.
 *
 * `-m` picks, and the default is `auto`, which decides per file by looking:
 * ITS text leaves the low bit of every word clear (five 7-bit characters use 35
 * of 36 bits) and uses a small set of control characters.  A file whose every
 * word passes both tests is text; anything else is words.  The count of each is
 * always reported, and `-v` says which was used for every file, because the one
 * thing this must not do is quietly hand somebody a mangled file.
 *
 *
 * A LINK WHOSE TARGET IS `>` DANGLES, and that is the honest answer rather than
 * a defect.  `>` is ITS's "the highest version there is", resolved when a file
 * is opened -- 88 of the 399 links on the reference pack point at one, and more
 * chain to one through another link.  A symlink is a fixed string, so there is
 * nothing to point it at; picking a version here would be this project deciding
 * what ITS decides at open time.  The symlink still carries exactly what the
 * disk records, so `readlink` shows the target ITS wrote.
 *
 * WHAT `tar x` CANNOT DO, said here rather than discovered: it writes files and
 * directories, and REFUSES a symbolic link.  Creating an ITS link means writing
 * a name into a descriptor field, and `write.c` has no operation for it -- `put`
 * writes files.  A link is reported and skipped rather than turned into a copy
 * of its target, which would be a different pack than the one that was archived.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "cmds.h"
#include "image.h"
#include "its.h"
#include "itsgeom.h"
#include "itspack.h"
#include "itstext.h"
#include "structure.h"
#include "util.h"
#include "write.h"

#define TAR_BLK 512			     /* the tar block, and the header's size */
#define TAR_NAME 100			     /* the name field */
#define TAR_MAXBLK 65536		     /* a file's blocks: a cap on untrusted descriptors */
#define TAR_MAXMEMBER (256u * 1024u * 1024u) /* refuse a member bigger than this */

/* -m */
enum {
	MODE_AUTO,
	MODE_TEXT,
	MODE_WORDS
};

/* ------------------------------------------------------------------ ustar */

/*
 * A ustar numeric field is octal, right-justified in `n-1` characters, zero
 * padded, then a NUL.  `size` and `mtime` are 12 bytes, everything else 8.
 */
static void
octal_field(char *dst, size_t n, uint64_t v)
{
	size_t i = n - 1;

	dst[i] = '\0';

	while (i > 0) {
		dst[--i] = (char)('0' + (v & 7));
		v >>= 3;
	}
}

/*
 * The header checksum is the sum of every byte of the header with the checksum
 * field itself taken as eight spaces.  It is written as six octal digits, a
 * NUL and a space -- the odd layout is what the format says, and what every
 * reader expects.
 */
static void
tar_checksum(unsigned char *h)
{
	unsigned sum = 0;

	memset(h + 148, ' ', 8);

	for (int i = 0; i < TAR_BLK; i++)
		sum += h[i];

	octal_field((char *)h + 148, 7, sum);
	h[154] = ' ';
}

/* Days since the epoch, so a date can become an mtime without timegm(). */
static long
days_from_civil(long y, unsigned m, unsigned d)
{
	long era, yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153L * ((long)m + (m > 2 ? -3L : 9L)) + 2) / 5 + (long)d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + doe - 719468;
}

/*
 * An ITS date is a DAY: UNDATE holds year, month and day, and the time beside it
 * (UNTIM) is in a unit this project has not established -- see its.h.  So there
 * is no time of day to carry, and the question is only which instant to name.
 *
 * NOON UTC, not midnight, and the reason is what a reader sees.  An mtime is an
 * instant; `ls` and `tar tv` print it in the reader's own zone.  Midnight UTC on
 * the 30th is the 29th to anybody west of Greenwich, so a pack that says
 * 30-dec-1985 would list as 29-dec-1985 across the Americas -- the date changed,
 * silently, by extraction.  Noon puts the whole span from UTC-12 to UTC+11 on
 * the day the disk actually says.  The 12:00 is not a claim about the file; the
 * date is, and it is the part ITS recorded.
 *
 * A date of zero (nothing recorded) becomes 0 rather than a guess.
 *
 * AND SO DOES A DATE BEFORE 1970, which ITS packs really do carry -- files
 * dated 1969 exist on the reference pack.  A ustar mtime is an OCTAL field and
 * cannot hold a negative number; writing one as unsigned turns 30-jun-1969 into
 * the year 2241, which is what this did until `tar xf` complained the timestamp
 * was six billion seconds in the future.  The epoch is the nearest thing ustar
 * can say, and the date the disk records is still in the listing `tar c -v`
 * prints and in `ls -l`.
 */
static uint64_t
its_mtime(unsigned year, unsigned month, unsigned day)
{
	long t;

	if (year == 0 || month == 0 || month > 12 || day == 0 || day > 31)
		return 0;

	t = days_from_civil((long)year, month, day) * 86400L + 43200L;
	return t < 0 ? 0 : (uint64_t)t;
}

static int
tar_header(FILE *out, const char *name, uint64_t size, unsigned mode, char type,
	   const char *linkname, uint64_t mtime)
{
	unsigned char h[TAR_BLK];

	memset(h, 0, sizeof h);

	if (strlen(name) >= TAR_NAME) {
		fprintf(stderr, "itsfs: |%s| is too long for a tar name field\n", name);
		return -1;
	}

	memcpy(h, name, strlen(name));
	octal_field((char *)h + 100, 8, mode);
	octal_field((char *)h + 108, 8, 0); /* uid: ITS has no host uid to claim */
	octal_field((char *)h + 116, 8, 0); /* gid */
	octal_field((char *)h + 124, 12, size);
	octal_field((char *)h + 136, 12, mtime);
	h[156] = (unsigned char)type;

	if (linkname != NULL) {
		if (strlen(linkname) >= TAR_NAME) {
			fprintf(stderr, "itsfs: link target |%s| is too long for tar\n", linkname);
			return -1;
		}

		memcpy(h + 157, linkname, strlen(linkname));
	}

	memcpy(h + 257, "ustar", 5);
	memcpy(h + 263, "00", 2);
	tar_checksum(h);

	if (fwrite(h, 1, sizeof h, out) != sizeof h) {
		perror("tar");
		return -1;
	}

	return 0;
}

/* Pad a member out to a whole number of 512-byte blocks. */
static int
tar_pad(FILE *out, uint64_t size)
{
	unsigned char z[TAR_BLK];
	size_t n = (size_t)(size % TAR_BLK);

	if (n == 0)
		return 0;

	memset(z, 0, sizeof z);
	n = TAR_BLK - n;

	if (fwrite(z, 1, n, out) != n) {
		perror("tar");
		return -1;
	}

	return 0;
}

/* ---------------------------------------------------------- words to bytes */

/*
 * Does this file look like ITS text?
 *
 * Five 7-bit characters use bits 0..34 of a 36-bit word, so ITS text leaves bit
 * 35 -- the low bit, in PDP-10 numbering the last one -- clear in every word.
 * That alone is a strong signal and a cheap one.  The character test is the
 * second half: text uses printable ASCII plus a handful of controls, and ITS
 * pads the last word with ^C rather than NUL.
 *
 * A wrong answer here is a mangled file, so the test is deliberately strict:
 * anything it is not sure about is words, which is never wrong, only ugly.
 */
static int
looks_like_text(const uint64_t *w, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (w[i] & 1)
			return 0;

		for (unsigned k = 0; k < ITS_ASCII_CHARS; k++) {
			unsigned c = (unsigned)((w[i] >> (29 - 7 * k)) & 0177u);

			if (c >= 040 && c <= 0176)
				continue;

			switch (c) {
			case 0:	   /* padding */
			case 003:  /* ^C, which is what ITS pads a last word with */
			case 011:  /* tab */
			case 012:  /* line feed */
			case 013:  /* vertical tab */
			case 014:  /* form feed */
			case 015:  /* carriage return */
			case 0177: /* rubout, which ITS text does contain */
				continue;
			default:
				return 0;
			}
		}
	}

	return 1;
}

/*
 * The bytes of a file, in `mode`.
 *
 * Text follows `cat` and `get` exactly, including the one narrow exception
 * there: a run of NULs at the very END is the padding of the last word and is
 * dropped, and a NUL anywhere else is data and is kept.  Getting that wrong
 * lost interior NULs in a sibling command once; the rule is repeated here
 * rather than reached for, because the two must agree.
 */
static unsigned char *
words_to_bytes(const uint64_t *w, size_t n, int mode, size_t *outlen, int *used_text)
{
	unsigned char *b;
	size_t len = 0;
	int text = (mode == MODE_TEXT) || (mode == MODE_AUTO && looks_like_text(w, n));

	*used_text = text;

	if (!text) {
		b = malloc(n * 8 + 1);

		if (b == NULL) {
			fprintf(stderr, "itsfs: out of memory\n");
			return NULL;
		}

		for (size_t i = 0; i < n; i++)
			for (size_t k = 0; k < 8; k++)
				b[i * 8 + k] = (unsigned char)(w[i] >> (8 * k));

		*outlen = n * 8;
		return b;
	}

	b = malloc(n * ITS_ASCII_CHARS + 1);

	if (b == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		return NULL;
	}

	{
		size_t pending = 0;

		for (size_t i = 0; i < n; i++)
			for (unsigned k = 0; k < ITS_ASCII_CHARS; k++) {
				unsigned c = (unsigned)((w[i] >> (29 - 7 * k)) & 0177u);

				if (c == 0) {
					pending++;
					continue;
				}

				while (pending > 0) {
					b[len++] = 0;
					pending--;
				}

				b[len++] = (unsigned char)c;
			}
	}

	*outlen = len;
	return b;
}

/* ------------------------------------------------------------------ create */

struct tctx {
	its_image *im;
	FILE *out;
	int mode;
	int verbose;
	unsigned long nfiles, nlinks, ndirs, ntext, nwords, nskip;
};

/* `DIR;FN1 FN2` -> `DIR/FN1 FN2`, or `DIR/FN1` when there is no second name. */
static int
member_path(char *out, size_t sz, const char *dir, const char *fn1, const char *fn2)
{
	char d[ITS_NAME_MAX * 3], a[ITS_NAME_MAX * 3], b[ITS_NAME_MAX * 3];

	if (its_enc_component(d, sizeof d, dir) != 0 || its_enc_component(a, sizeof a, fn1) != 0 ||
	    its_enc_component(b, sizeof b, fn2) != 0)
		return -1;

	if (fn2[0] == '\0')
		return snprintf(out, sz, "%s/%s", d, a) < (int)sz ? 0 : -1;

	return snprintf(out, sz, "%s/%s %s", d, a, b) < (int)sz ? 0 : -1;
}

static int
write_file(struct tctx *c, its_ufd *u, const char *dir, const its_ent *e)
{
	char path[TAR_NAME];
	const char *err = NULL;
	uint64_t *blocks = NULL;
	uint64_t *words = NULL;
	unsigned char *bytes = NULL;
	uint64_t nwords;
	long nb;
	size_t len = 0, got = 0;
	int text = 0, rc = -1;

	if (member_path(path, sizeof path, dir, e->fn1, e->fn2) != 0) {
		fprintf(stderr, "itsfs: skipping %s;%s %s: not a name a tar path can hold\n", dir,
			e->fn1, e->fn2);
		c->nskip++;
		return 0;
	}

	nb = its_desc_blocks(u, c->im->drv, e->desc, NULL, 0, &err);

	if (nb < 0 || nb > TAR_MAXBLK) {
		fprintf(stderr, "itsfs: skipping %s: %s\n", path, err ? err : "impossibly long");
		c->nskip++;
		return 0;
	}

	nwords = its_file_words(u->wpb, nb, e->lastwc);

	if (nwords > TAR_MAXMEMBER / 8) {
		fprintf(stderr, "itsfs: skipping %s: %llu words is more than this will archive\n",
			path, (unsigned long long)nwords);
		c->nskip++;
		return 0;
	}

	blocks = calloc((size_t)nb + 1, sizeof *blocks);
	words = calloc((size_t)nwords + 1, sizeof *words);

	if (blocks == NULL || words == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		goto out;
	}

	if (its_desc_blocks(u, c->im->drv, e->desc, blocks, (size_t)nb, &err) != nb) {
		fprintf(stderr, "itsfs: skipping %s: descriptor decoded differently twice\n", path);
		c->nskip++;
		rc = 0;
		goto out;
	}

	for (long i = 0; i < nb && got < nwords; i++) {
		size_t want = nwords - got < u->wpb ? (size_t)(nwords - got) : u->wpb;

		if (img_read_block(c->im, blocks[i], words + got, want) != 0) {
			fprintf(stderr, "itsfs: skipping %s: unreadable at block %llu\n", path,
				(unsigned long long)blocks[i]);
			c->nskip++;
			rc = 0;
			goto out;
		}

		got += want;
	}

	bytes = words_to_bytes(words, (size_t)nwords, c->mode, &len, &text);

	if (bytes == NULL)
		goto out;

	if (tar_header(c->out, path, len, 0644, '0', NULL,
		       its_mtime(e->year, e->month, e->day)) != 0)
		goto out;

	if (len > 0 && fwrite(bytes, 1, len, c->out) != len) {
		perror("tar");
		goto out;
	}

	if (tar_pad(c->out, len) != 0)
		goto out;

	c->nfiles++;
	text ? c->ntext++ : c->nwords++;

	if (c->verbose)
		printf("%-24s %8lu bytes  %s\n", path, (unsigned long)len,
		       text ? "text" : "words");

	rc = 0;
out:
	free(bytes);
	free(words);
	free(blocks);
	return rc;
}

static int
write_link(struct tctx *c, its_ufd *u, const char *dir, const its_ent *e)
{
	char path[TAR_NAME];
	char tgt[ITS_NAME_MAX * 3];
	char lpath[TAR_NAME];
	const char *err = NULL;
	char *sep, *sp;

	if (member_path(path, sizeof path, dir, e->fn1, e->fn2) != 0) {
		fprintf(stderr, "itsfs: skipping %s;%s %s: not a name a tar path can hold\n", dir,
			e->fn1, e->fn2);
		c->nskip++;
		return 0;
	}

	if (its_link_target(u, e->desc, tgt, sizeof tgt, &err) != 0) {
		fprintf(stderr, "itsfs: skipping link %s: %s\n", path, err ? err : "unreadable");
		c->nskip++;
		return 0;
	}

	/*
	 * A target reads `DIR;FN1 FN2`, and becomes a RELATIVE path so the link
	 * still resolves wherever the archive is unpacked: one `..` out of the
	 * directory the link sits in, then the target's own directory.
	 */
	sep = strchr(tgt, ';');

	if (sep == NULL) {
		fprintf(stderr, "itsfs: skipping link %s: target |%s| has no directory\n", path,
			tgt);
		c->nskip++;
		return 0;
	}

	*sep = '\0';
	sp = sep + 1;

	while (*sp == ' ')
		sp++;

	{
		char *space = strchr(sp, ' ');
		char t1[ITS_NAME_MAX], t2[ITS_NAME_MAX];

		if (space != NULL) {
			*space = '\0';
			snprintf(t1, sizeof t1, "%s", sp);
			snprintf(t2, sizeof t2, "%s", space + 1);
		}
		else {
			snprintf(t1, sizeof t1, "%s", sp);
			t2[0] = '\0';
		}

		/*
		 * `.` IS A DIRECTORY NAME HERE, not a self-reference.  Two links
		 * on the reference pack point at `.;@ DDT` and `.;RAM RAM`, and
		 * the first reading of that -- "the directory the link sits in",
		 * which is what `.` means in a Unix path -- is wrong.  KSHACK,
		 * where both links live, holds no `@ DDT` at all; the directory
		 * whose name is literally `.` holds one, along with `@ ITS` and
		 * `@ NSALV`.  So the target resolves out and back in like any
		 * other, and `.` is encoded like any other name.
		 */
		char ed[ITS_NAME_MAX * 3], e1[ITS_NAME_MAX * 3], e2[ITS_NAME_MAX * 3];

		if (its_enc_component(ed, sizeof ed, tgt) != 0 ||
		    its_enc_component(e1, sizeof e1, t1) != 0 ||
		    its_enc_component(e2, sizeof e2, t2) != 0) {
			fprintf(stderr, "itsfs: skipping link %s: target is not a path here\n",
				path);
			c->nskip++;
			return 0;
		}

		if (t2[0] == '\0')
			snprintf(lpath, sizeof lpath, "../%s/%s", ed, e1);
		else
			snprintf(lpath, sizeof lpath, "../%s/%s %s", ed, e1, e2);
	}

	if (tar_header(c->out, path, 0, 0777, '2', lpath, its_mtime(e->year, e->month, e->day)) !=
	    0)
		return -1;

	c->nlinks++;

	if (c->verbose)
		printf("%-24s %8s        -> %s\n", path, "link", lpath);

	return 0;
}

/* One ITS directory: the directory member, then everything in it. */
static int
write_dir(struct tctx *c, const char *name, uint64_t blk, const char *only1, const char *only2)
{
	char dpath[TAR_NAME];
	its_ufd u;
	unsigned idx;
	its_ent e;
	int rc = 0;

	char ename[ITS_NAME_MAX * 3];

	if (its_enc_component(ename, sizeof ename, name) != 0) {
		fprintf(stderr, "itsfs: skipping directory %s: not a name a tar path can hold\n",
			name);
		c->nskip++;
		return 0;
	}

	if (its_ufd_read(c->im, blk, &u) != 0)
		return -1;

	if (only1 == NULL) {
		/*
		 * A directory carries no date -- the MFD holds a name and
		 * nothing else -- so this is "none", written as noon on the
		 * epoch day for the same reason a file's date is noon: mtime 0
		 * is 31-dec-1969 to every reader west of Greenwich.
		 */
		if (snprintf(dpath, sizeof dpath, "%s/", ename) >= (int)sizeof dpath ||
		    tar_header(c->out, dpath, 0, 0755, '5', NULL, 43200) != 0) {
			its_ufd_free(&u);
			return -1;
		}

		c->ndirs++;

		if (c->verbose)
			printf("%-24s %8s\n", dpath, "dir");
	}

	idx = (unsigned)u.namp;

	while (its_ufd_next(&u, &idx, &e)) {
		if (e.fn1[0] == '\0' && e.fn2[0] == '\0')
			continue;

		if (only1 != NULL &&
		    (strcmp(e.fn1, only1) != 0 || strcmp(e.fn2, only2 ? only2 : "") != 0))
			continue;

		rc = e.is_link ? write_link(c, &u, name, &e) : write_file(c, &u, name, &e);

		if (rc != 0)
			break;
	}

	its_ufd_free(&u);
	return rc;
}

/*
 * A selector on the command line: `DIR`, or `DIR;FN1 FN2` for one file.  The
 * same two spellings every other command takes, and split here rather than in
 * util.c because this one is a single string per argument.
 */
static int
split_sel(const char *arg, char *dir, char *fn1, char *fn2)
{
	const char *semi = strchr(arg, ';');
	const char *sp;

	fn1[0] = fn2[0] = '\0';

	if (semi == NULL) {
		if (strlen(arg) >= ITS_NAME_MAX)
			return -1;

		snprintf(dir, ITS_NAME_MAX, "%s", arg);
		return 0;
	}

	if ((size_t)(semi - arg) >= ITS_NAME_MAX)
		return -1;

	snprintf(dir, ITS_NAME_MAX, "%.*s", (int)(semi - arg), arg);
	sp = strchr(semi + 1, ' ');

	if (sp == NULL) {
		if (strlen(semi + 1) >= ITS_NAME_MAX)
			return -1;

		snprintf(fn1, ITS_NAME_MAX, "%s", semi + 1);
		return 0;
	}

	if ((size_t)(sp - semi - 1) >= ITS_NAME_MAX || strlen(sp + 1) >= ITS_NAME_MAX)
		return -1;

	snprintf(fn1, ITS_NAME_MAX, "%.*s", (int)(sp - semi - 1), semi + 1);
	snprintf(fn2, ITS_NAME_MAX, "%s", sp + 1);
	return 0;
}

static int
tar_create(int argc, char **argv, const its_pack *pk, const its_drive *drv, int mode, int verbose,
	   int optind_)
{
	its_image im;
	its_mfd m;
	struct tctx c;
	unsigned char z[TAR_BLK * 2];
	int rc = 2;

	memset(&c, 0, sizeof c);

	if (img_open(&im, argv[optind_ + 1], pk, drv, 0) != 0)
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

	c.im = &im;
	c.mode = mode;
	c.verbose = verbose;
	c.out = strcmp(argv[optind_], "-") == 0 ? stdout : fopen(argv[optind_], "wb");

	if (c.out == NULL) {
		perror(argv[optind_]);
		goto out;
	}

	if (optind_ + 2 >= argc) {
		/* The whole pack, in the MFD's own order. */
		for (unsigned i = 0; i < its_mfd_slots(&m); i++) {
			char name[ITS_NAME_MAX];
			uint64_t blk;

			if (its_mfd_dir(&m, i, name, &blk) != 0 || name[0] == '\0')
				continue;

			if (write_dir(&c, name, blk, NULL, NULL) != 0)
				goto out;
		}
	}
	else {
		for (int a = optind_ + 2; a < argc; a++) {
			char dir[ITS_NAME_MAX], fn1[ITS_NAME_MAX], fn2[ITS_NAME_MAX];
			uint64_t blk;

			if (split_sel(argv[a], dir, fn1, fn2) != 0) {
				fprintf(stderr, "itsfs: |%s| is not a name\n", argv[a]);
				goto out;
			}

			if (its_find_dir(&m, dir, &blk) != 0) {
				fprintf(stderr, "itsfs: no directory %s\n", dir);
				goto out;
			}

			if (write_dir(&c, dir, blk, fn1[0] ? fn1 : NULL, fn2) != 0)
				goto out;
		}
	}

	/* Two zero blocks end an archive. */
	memset(z, 0, sizeof z);

	if (fwrite(z, 1, sizeof z, c.out) != sizeof z) {
		perror("tar");
		goto out;
	}

	if (c.out != stdout && fclose(c.out) != 0) {
		perror(argv[optind_]);
		c.out = NULL;
		goto out;
	}

	if (c.out == stdout)
		fflush(stdout);

	c.out = NULL;

	fprintf(stderr, "%lu files (%lu text, %lu words), %lu links, %lu directories",
		c.nfiles, c.ntext, c.nwords, c.nlinks, c.ndirs);

	if (c.nskip)
		fprintf(stderr, ", %lu SKIPPED", c.nskip);

	fprintf(stderr, "\n");
	rc = c.nskip ? 1 : 0;
out:
	if (c.out != NULL && c.out != stdout)
		fclose(c.out);

	its_mfd_free(&m);
	img_close(&im);
	return rc;
}

/* -------------------------------------------------------------- read a tar */

/*
 * A header off the archive is untrusted: the checksum is verified, the size is
 * parsed strictly, and the name is checked for the things that make `tar x`
 * write outside where it was told to.  An all-zero block ends the archive.
 */
struct member {
	char name[TAR_NAME + 1];
	char link[TAR_NAME + 1];
	uint64_t size;
	char type;
};

static int
parse_octal(const char *p, size_t n, uint64_t *out)
{
	uint64_t v = 0;
	size_t i = 0;

	while (i < n && (p[i] == ' ' || p[i] == '0'))
		i++;

	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
		if (v > (UINT64_MAX - (uint64_t)(p[i] - '0')) / 8)
			return -1;

		v = v * 8 + (uint64_t)(p[i] - '0');
	}

	for (; i < n; i++)
		if (p[i] != '\0' && p[i] != ' ')
			return -1;

	*out = v;
	return 0;
}

/* 1 a member, 0 end of archive, -1 a broken header. */
static int
read_header(FILE *f, struct member *mem)
{
	unsigned char h[TAR_BLK];
	unsigned sum = 0;
	uint64_t want;
	int all_zero = 1;

	if (fread(h, 1, sizeof h, f) != sizeof h)
		return 0; /* a short read at the end is an unterminated archive */

	for (int i = 0; i < TAR_BLK; i++)
		if (h[i] != 0) {
			all_zero = 0;
			break;
		}

	if (all_zero)
		return 0;

	if (parse_octal((char *)h + 148, 8, &want) != 0) {
		fprintf(stderr, "itsfs: tar header has no checksum\n");
		return -1;
	}

	for (int i = 0; i < TAR_BLK; i++)
		sum += (i >= 148 && i < 156) ? ' ' : h[i];

	if (sum != want) {
		fprintf(stderr, "itsfs: tar header checksum is %u, not the %llu it claims\n", sum,
			(unsigned long long)want);
		return -1;
	}

	memcpy(mem->name, h, TAR_NAME);
	mem->name[TAR_NAME] = '\0';
	memcpy(mem->link, h + 157, TAR_NAME);
	mem->link[TAR_NAME] = '\0';
	mem->type = (char)h[156];

	if (parse_octal((char *)h + 124, 12, &mem->size) != 0) {
		fprintf(stderr, "itsfs: tar member |%s| has no size\n", mem->name);
		return -1;
	}

	if (mem->size > TAR_MAXMEMBER) {
		fprintf(stderr, "itsfs: tar member |%s| is %llu bytes, more than this will read\n",
			mem->name, (unsigned long long)mem->size);
		return -1;
	}

	return 1;
}

static int
skip_data(FILE *f, uint64_t size)
{
	uint64_t n = (size + TAR_BLK - 1) / TAR_BLK * TAR_BLK;

	return fseeko(f, (off_t)n, SEEK_CUR) == 0 ? 0 : -1;
}

static int
tar_list(const char *path, int verbose)
{
	FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
	struct member mem;
	unsigned long n = 0;
	int r;

	if (f == NULL) {
		perror(path);
		return 2;
	}

	while ((r = read_header(f, &mem)) == 1) {
		if (verbose)
			printf("%c %10llu  %s%s%s\n", mem.type, (unsigned long long)mem.size,
			       mem.name, mem.type == '2' ? " -> " : "",
			       mem.type == '2' ? mem.link : "");
		else
			printf("%s%s%s\n", mem.name, mem.type == '2' ? " -> " : "",
			       mem.type == '2' ? mem.link : "");

		n++;

		if (skip_data(f, mem.size) != 0) {
			fprintf(stderr, "itsfs: %s: truncated at |%s|\n", path, mem.name);
			r = -1;
			break;
		}
	}

	if (f != stdin)
		fclose(f);

	if (r < 0)
		return 1;

	fprintf(stderr, "%lu members\n", n);
	return 0;
}

/* ----------------------------------------------------------------- extract */

/* `DIR/FN1 FN2` back to its three parts.  Refuses anything that is not one. */
static int
member_to_its(const char *path, char *dir, char *fn1, char *fn2)
{
	const char *slash = strchr(path, '/');
	const char *sp;
	size_t n;

	fn1[0] = fn2[0] = '\0';

	if (slash == NULL || slash == path || strchr(slash + 1, '/') != NULL)
		return -1;

	n = (size_t)(slash - path);

	if (n >= ITS_NAME_MAX)
		return -1;

	{
		char raw[ITS_NAME_MAX * 3];

		if (n >= sizeof raw)
			return -1;

		snprintf(raw, sizeof raw, "%.*s", (int)n, path);

		if (its_dec_component(dir, ITS_NAME_MAX, raw) != 0)
			return -1;
	}

	if (slash[1] == '\0')
		return 0; /* a directory member */

	sp = strchr(slash + 1, ' ');

	{
		char raw[ITS_NAME_MAX * 3];
		size_t len = sp ? (size_t)(sp - slash - 1) : strlen(slash + 1);

		if (len >= sizeof raw)
			return -1;

		snprintf(raw, sizeof raw, "%.*s", (int)len, slash + 1);

		if (its_dec_component(fn1, ITS_NAME_MAX, raw) != 0)
			return -1;
	}

	if (sp == NULL)
		return 0;

	if (strlen(sp + 1) >= ITS_NAME_MAX * 3)
		return -1;

	return its_dec_component(fn2, ITS_NAME_MAX, sp + 1);
}

/* Host bytes back into 36-bit words, the inverse of words_to_bytes. */
static uint64_t *
bytes_to_words(const unsigned char *b, size_t len, int text, uint64_t *nwords)
{
	uint64_t *w;
	uint64_t n;

	if (text)
		n = (len + ITS_ASCII_CHARS - 1) / ITS_ASCII_CHARS;
	else {
		if (len % 8 != 0) {
			fprintf(stderr, "itsfs: %zu bytes is not a whole number of 8-byte words "
					"-- `-m words` wants what `-m words` wrote\n",
				len);
			return NULL;
		}

		n = len / 8;
	}

	w = calloc((size_t)n + 1, sizeof *w);

	if (w == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		return NULL;
	}

	if (text) {
		for (size_t i = 0; i < len; i++) {
			if (b[i] & 0200) {
				fprintf(stderr, "itsfs: byte %zu is %u, and ITS text is seven "
						"bits -- use `-m words`\n",
					i, b[i]);
				free(w);
				return NULL;
			}

			w[i / ITS_ASCII_CHARS] |= (uint64_t)b[i]
						  << (29 - 7 * (i % ITS_ASCII_CHARS));
		}
	}
	else {
		for (uint64_t i = 0; i < n; i++) {
			uint64_t v = 0;

			for (int k = 0; k < 8; k++)
				v |= (uint64_t)b[i * 8 + (size_t)k] << (8 * k);

			w[i] = v & ITS_WORD_MASK;
		}
	}

	*nwords = n;
	return w;
}

static int
tar_extract(int argc, char **argv, const its_pack *pk, const its_drive *drv, int mode, int verbose,
	    int optind_)
{
	its_writer wr;
	FILE *f;
	struct member mem;
	unsigned long nf = 0, nd = 0, nskip = 0;
	int r, rc = 2;

	(void)argc;

	/*
	 * `auto` is a decision this direction cannot make.  Going out, the words
	 * are there to look at; coming back there are only bytes, and text and
	 * words are both "some bytes" -- a wrong guess writes a mangled file into
	 * a pack.  So it is asked for rather than guessed.
	 */
	if (mode == MODE_AUTO) {
		fprintf(stderr, "itsfs: tar x needs `-m text` or `-m words`: bytes coming back "
				"do not say which they are, and guessing wrong writes a "
				"mangled file\n");
		return 2;
	}

	f = strcmp(argv[optind_], "-") == 0 ? stdin : fopen(argv[optind_], "rb");

	if (f == NULL) {
		perror(argv[optind_]);
		return 2;
	}

	if (itsw_open(&wr, argv[optind_ + 1], pk, drv) != 0) {
		if (f != stdin)
			fclose(f);

		return 2;
	}

	while ((r = read_header(f, &mem)) == 1) {
		char dir[ITS_NAME_MAX], fn1[ITS_NAME_MAX], fn2[ITS_NAME_MAX];
		unsigned char *buf = NULL;
		uint64_t *w = NULL;
		uint64_t nw = 0;
		size_t pad;

		if (mem.type == '2' || mem.type == '1') {
			fprintf(stderr, "itsfs: skipping %s: this cannot create an ITS link\n",
				mem.name);
			nskip++;
			goto next;
		}

		if (member_to_its(mem.name, dir, fn1, fn2) != 0) {
			fprintf(stderr, "itsfs: skipping %s: not DIR/FN1 FN2\n", mem.name);
			nskip++;
			goto next;
		}

		if (mem.type == '5' || fn1[0] == '\0') {
			if (itsw_mkdir(&wr, dir) == 0) {
				nd++;

				if (verbose)
					printf("%s/\n", dir);
			}

			goto next;
		}

		/*
		 * THE DIRECTORY MAY NOT BE IN THE ARCHIVE AT ALL.  `tar c` with
		 * file names on the command line writes those members and no
		 * directory member -- so extracting `TEST/HELLO TXT` into a pack
		 * without a TEST would put nothing anywhere and say why, once per
		 * file.  Every tar creates a missing parent on extraction; this
		 * does the same, and an ITS directory is cheap because it owns no
		 * blocks.  Found by a fuzzer harness whose archive was built from
		 * two named files.
		 */
		if (itsw_have_dir(&wr, dir) == 0) {
			if (itsw_mkdir(&wr, dir) != 0) {
				nskip++;
				goto next;
			}

			nd++;

			if (verbose)
				printf("%s/\n", dir);
		}

		if (mem.type != '0' && mem.type != '\0') {
			fprintf(stderr, "itsfs: skipping %s: tar type %c is not a file\n", mem.name,
				mem.type);
			nskip++;
			goto next;
		}

		buf = malloc((size_t)mem.size + 1);

		if (buf == NULL) {
			fprintf(stderr, "itsfs: out of memory\n");
			goto done;
		}

		if (mem.size > 0 && fread(buf, 1, (size_t)mem.size, f) != (size_t)mem.size) {
			fprintf(stderr, "itsfs: %s: truncated inside |%s|\n", argv[optind_],
				mem.name);
			free(buf);
			goto done;
		}

		pad = (size_t)(mem.size % TAR_BLK);

		if (pad != 0 && fseeko(f, (off_t)(TAR_BLK - pad), SEEK_CUR) != 0) {
			free(buf);
			goto done;
		}

		w = bytes_to_words(buf, (size_t)mem.size, mode == MODE_TEXT, &nw);
		free(buf);

		if (w == NULL) {
			nskip++;
			continue; /* data already consumed */
		}

		if (itsw_put(&wr, dir, fn1, fn2, w, nw) != 0)
			nskip++;
		else {
			nf++;

			if (verbose)
				printf("%s;%s %s (%llu words)\n", dir, fn1, fn2,
				       (unsigned long long)nw);
		}

		free(w);
		continue;
	next:
		if (skip_data(f, mem.size) != 0) {
			fprintf(stderr, "itsfs: %s: truncated at |%s|\n", argv[optind_], mem.name);
			goto done;
		}
	}

	rc = (r < 0) ? 1 : 0;
done:
	if (itsw_close(&wr) != 0)
		rc = 2;

	if (f != stdin)
		fclose(f);

	fprintf(stderr, "%lu files, %lu directories", nf, nd);

	if (nskip)
		fprintf(stderr, ", %lu SKIPPED", nskip);

	fprintf(stderr, "\n");
	return rc ? rc : (nskip ? 1 : 0);
}

/* -------------------------------------------------------------------- main */

static void
usage(void)
{
	fprintf(stderr,
		"usage: itsfs tar c [-p packing] [-d drive] [-m mode] [-v] archive image "
		"[name...]\n"
		"       itsfs tar t [-v] archive\n"
		"       itsfs tar x [-p packing] [-d drive] [-m mode] [-v] archive image\n"
		"\n"
		"       c    write a tar archive from a pack (`-` is stdout)\n"
		"       t    list what is in an archive\n"
		"       x    read an archive back into an image -- DESTRUCTIVE\n"
		"\n"
		"       -m   how a 36-bit word becomes bytes:\n"
		"            auto   text if the file looks like ITS text, else words "
		"(default)\n"
		"            text   five 7-bit characters per word, as `cat` writes\n"
		"            words  the word in 8 little-endian bytes, as `get -w` writes\n"
		"       -v   name every member\n"
		"\n"
		"       A name is `DIR` for a whole directory or `DIR;FN1 FN2` for one file;\n"
		"       with none, the whole pack.  ITS `DIR;FN1 FN2` is tar `DIR/FN1 FN2`.\n");
}

int
cmd_tar(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	int mode = MODE_AUTO, verbose = 0, i;
	const char *what;

	if (argc < 3) {
		usage();
		return 2;
	}

	what = argv[1];

	if (strcmp(what, "c") != 0 && strcmp(what, "t") != 0 && strcmp(what, "x") != 0) {
		fprintf(stderr, "itsfs: tar: |%s| is not c, t or x\n", what);
		return 2;
	}

	for (i = 2; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "-v") == 0) {
			verbose = 1;
		}
		else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			if ((pk = opt_pack(argv[++i])) == NULL)
				return 2;
		}
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			if ((drv = opt_drive(argv[++i])) == NULL)
				return 2;
		}
		else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
			const char *m = argv[++i];

			if (strcmp(m, "auto") == 0)
				mode = MODE_AUTO;
			else if (strcmp(m, "text") == 0)
				mode = MODE_TEXT;
			else if (strcmp(m, "words") == 0)
				mode = MODE_WORDS;
			else {
				fprintf(stderr, "itsfs: -m takes auto, text or words, not |%s|\n",
					m);
				return 2;
			}
		}
		else {
			usage();
			return 2;
		}
	}

	if (strcmp(what, "t") == 0)
		return i < argc ? tar_list(argv[i], verbose) : (usage(), 2);

	if (i + 1 >= argc) {
		usage();
		return 2;
	}

	return strcmp(what, "c") == 0 ? tar_create(argc, argv, pk, drv, mode, verbose, i)
				      : tar_extract(argc, argv, pk, drv, mode, verbose, i);
}
