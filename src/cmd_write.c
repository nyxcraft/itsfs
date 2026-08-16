/*
 * cmd_write.c -- `itsfs put` and `itsfs del`, the destructive commands.
 *
 *   itsfs put [-p packing] [-d drive] [-w] image 'DIR;FN1 FN2' hostfile
 *   itsfs del [-p packing] [-d drive] image 'DIR;FN1 FN2'
 *
 * These are front ends and nothing else: every word they change goes through
 * write.c, which is the only code in the project that mutates a pack.  What
 * lives here is the part write.c must not have an opinion about -- turning a
 * host file into 36-bit words.
 *
 * THE CONVERSION IS THE INTERESTING HALF.  A host file is bytes and an ITS file
 * is words, and there is no single right way across:
 *
 *   text (default)   five seven-bit characters to a word, most significant
 *                    first, bit 35 unused.  This is what ITS text files are and
 *                    what `cat` reads back.
 *   -w               the file already holds 36-bit words, one per 8-byte
 *                    little-endian container -- which is what `get -w` writes,
 *                    so the pair round-trips a file of any kind.
 *
 * Neither translates line endings.  ITS text uses CRLF and a host file usually
 * does not; converting would make `put` then `get` return something other than
 * what went in, and a tool that quietly changes a file is worse than one that
 * makes you convert it yourself.  `docs/user-guide.md` says so out loud.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "util.h"
#include "write.h"
#include "its.h"
#include "itspack.h"
#include "itstext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* A host file is read whole: the largest thing anybody would put on an RP06 is
 * a few megabytes, and streaming would buy nothing but a harder failure mode
 * when the pack turns out to be full. */
#define PUT_MAXBYTES (64u * 1024u * 1024u)

typedef struct {
	char dir[ITS_NAME_MAX];
	char fn1[ITS_NAME_MAX];
	char fn2[ITS_NAME_MAX];
} wpath;

/* `DIR;FN1 FN2` in one argument, or as three.  The same grammar cmd_fs.c
 * accepts, and refusing what SIXBIT cannot hold rather than truncating it. */
static int
parse_path(char **argv, int i, int navail, wpath *p)
{
	memset(p, 0, sizeof *p);

	if (navail >= 3) {
		if (strlen(argv[i]) >= ITS_NAME_MAX || strlen(argv[i + 1]) >= ITS_NAME_MAX ||
		    strlen(argv[i + 2]) >= ITS_NAME_MAX)
			goto toolong;
		snprintf(p->dir, sizeof p->dir, "%s", argv[i]);
		snprintf(p->fn1, sizeof p->fn1, "%s", argv[i + 1]);
		snprintf(p->fn2, sizeof p->fn2, "%s", argv[i + 2]);
		return 0;
	}

	if (navail == 1) {
		const char *s = argv[i];
		const char *semi = strchr(s, ';');
		const char *sp;
		size_t n;

		if (semi == NULL) {
			fprintf(stderr, "itsfs: '%s' is not a file name: it wants DIR;FN1 FN2\n", s);
			return -1;
		}

		n = (size_t)(semi - s);

		if (n >= ITS_NAME_MAX)
			goto toolong;
		memcpy(p->dir, s, n);
		p->dir[n] = '\0';

		s = semi + 1;
		sp = strchr(s, ' ');

		if (sp == NULL) {
			if (strlen(s) >= ITS_NAME_MAX)
				goto toolong;
			snprintf(p->fn1, sizeof p->fn1, "%s", s);
			return 0;
		}

		n = (size_t)(sp - s);

		if (n >= ITS_NAME_MAX || strlen(sp + 1) >= ITS_NAME_MAX)
			goto toolong;
		memcpy(p->fn1, s, n);
		p->fn1[n] = '\0';
		snprintf(p->fn2, sizeof p->fn2, "%s", sp + 1);
		return 0;
	}

	fprintf(stderr, "itsfs: a file name is 'DIR;FN1 FN2', or three arguments\n");
	return -1;

toolong:
	fprintf(stderr, "itsfs: a name component is at most %d characters\n", ITS_SIXBIT_CHARS);
	return -1;
}

/*
 * A host file as 36-bit words.  Returns the word count, or -1.
 *
 * `raw` reads the file as le64 containers, which is what `get -w` writes; the
 * default packs seven-bit characters five to a word.
 */
static long
read_host(const char *path, int raw, uint64_t **out)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long nbytes;
	uint64_t *w;
	uint64_t nwords;

	if (f == NULL) {
		perror(path);
		return -1;
	}

	if (fseeko(f, 0, SEEK_END) != 0 || (nbytes = (long)ftello(f)) < 0) {
		perror(path);
		fclose(f);
		return -1;
	}

	rewind(f);

	if ((unsigned long)nbytes > PUT_MAXBYTES) {
		fprintf(stderr, "itsfs: %s is %ld bytes, more than this will put (%u)\n", path,
			nbytes, PUT_MAXBYTES);
		fclose(f);
		return -1;
	}

	if (raw && nbytes % 8 != 0) {
		fprintf(stderr, "itsfs: %s is %ld bytes, which is not a whole number of 8-byte "
				"words -- `-w` wants what `get -w` wrote\n",
			path, nbytes);
		fclose(f);
		return -1;
	}

	buf = malloc((size_t)nbytes + 1);

	if (buf == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		fclose(f);
		return -1;
	}

	if (nbytes > 0 && fread(buf, 1, (size_t)nbytes, f) != (size_t)nbytes) {
		perror(path);
		free(buf);
		fclose(f);
		return -1;
	}

	fclose(f);

	nwords = raw ? (uint64_t)nbytes / 8 : ((uint64_t)nbytes + ITS_ASCII_CHARS - 1) / ITS_ASCII_CHARS;
	w = calloc(nwords ? (size_t)nwords : 1, sizeof *w);

	if (w == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		free(buf);
		return -1;
	}

	if (raw) {
		for (uint64_t i = 0; i < nwords; i++) {
			uint64_t v = 0;

			for (int k = 7; k >= 0; k--)
				v = (v << 8) | buf[i * 8 + (unsigned)k];
			w[i] = v & ITS_WORD_MASK;
		}
	}
	else {
		for (long i = 0; i < nbytes; i++) {
			unsigned char c = buf[i];

			/* SEVEN BITS IS ALL AN ITS TEXT FILE HOLDS.  A byte with
			 * the high bit set is refused rather than masked: masking
			 * would put a different character on the pack and say
			 * nothing about it. */
			if (c > 0177) {
				fprintf(stderr, "itsfs: %s byte %ld is %03o, which is not "
						"seven-bit -- use -w for a binary file\n",
					path, i, c);
				free(buf);
				free(w);
				return -1;
			}

			w[i / ITS_ASCII_CHARS] |= (uint64_t)c << (29 - 7 * (i % ITS_ASCII_CHARS));
		}
	}

	free(buf);
	*out = w;
	return (long)nwords;
}

static int
run(int argc, char **argv, int is_del)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_writer w;
	wpath p;
	uint64_t *words = NULL;
	long nwords = 0;
	int c, raw = 0, rc = 2, nargs;

	while ((c = getopt(argc, argv, is_del ? "p:d:" : "p:d:w")) != -1) {
		switch (c) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'd':
			if ((drv = opt_drive(optarg)) == NULL)
				return 2;
			break;
		case 'w':
			raw = 1;
			break;
		default:
			goto usage;
		}
	}

	/* put wants image, name, hostfile; del wants image and name. */
	nargs = argc - optind - (is_del ? 1 : 2);

	if (nargs != 1 && nargs != 3)
		goto usage;

	if (parse_path(argv, optind + 1, nargs, &p) != 0)
		return 2;

	if (!is_del && (nwords = read_host(argv[argc - 1], raw, &words)) < 0)
		return 1;

	if (itsw_open(&w, argv[optind], pk, drv) != 0) {
		free(words);
		return 1;
	}

	if (is_del)
		rc = itsw_del(&w, p.dir, p.fn1, p.fn2) == 0 ? 0 : 1;
	else
		rc = itsw_put(&w, p.dir, p.fn1, p.fn2, words, (uint64_t)nwords) == 0 ? 0 : 1;

	if (itsw_close(&w) != 0)
		rc = 1;

	if (rc == 0)
		fprintf(stderr, "%s %s;%s %s%s\n", is_del ? "deleted" : "wrote", p.dir, p.fn1, p.fn2,
			is_del ? "" : "");

	free(words);
	return rc;

usage:
	if (is_del)
		fprintf(stderr, "usage: itsfs del [-p packing] [-d drive] image 'DIR;FN1 FN2'\n"
				"       DESTRUCTIVE.  Work on a copy.\n");
	else
		fprintf(stderr, "usage: itsfs put [-p packing] [-d drive] [-w] image "
				"'DIR;FN1 FN2' hostfile\n"
				"       -w   the host file already holds 36-bit words (what "
				"`get -w` writes)\n"
				"       DESTRUCTIVE.  Work on a copy.\n");
	return 2;
}

int
cmd_put(int argc, char **argv)
{
	return run(argc, argv, 0);
}

int
cmd_del(int argc, char **argv)
{
	return run(argc, argv, 1);
}
