/*
 * cmd_tape.c -- `itsfs tape`, the SIMH .tap container.
 *
 *   itsfs tape [-p packing] tapefile            describe the framing
 *   itsfs tape -x DIR [-p packing] tapefile     extract each file to DIR
 *
 * THIS IS THE CONTAINER LAYER AND NOT THE ARCHIVE ONE, and the difference
 * matters enough to be the first thing said.  A `.tap` file is a sequence of
 * records with their lengths on both sides -- that is all this reads.  What the
 * records MEAN is a separate format: an ITS DUMP save set has its own headers
 * and file names inside those records, and nothing here parses one.  `tapewrite`
 * in the ITS tree writes tapes that are simply a file per tape mark, and those
 * this can take apart completely.
 *
 * THE DEFAULT PACKING IS `core`, NOT `le64`.  Every other command in this
 * project defaults to the disk convention, one word per eight bytes; a magtape
 * is five frames per word, which is what the formatter writes and what these
 * files hold.  Getting that wrong produces a file of the right length and
 * entirely wrong words, so the two defaults differ on purpose.
 *
 * THE FRAMING, from the SIMH magtape specification:
 *
 *      4 bytes   record length, little-endian
 *      n bytes   the record, PADDED TO AN EVEN LENGTH
 *      4 bytes   the same length again
 *
 *      0          a tape mark: the end of a file
 *      0xFFFFFFFF end of medium
 *
 * The two lengths are compared rather than assumed, because a tape image is
 * untrusted input like everything else here and a mismatch is the one thing the
 * format itself lets you notice.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "util.h"
#include "itspack.h"
#include "itstext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* A record longer than this is not a record, it is a length read out of
 * damage.  Real ITS tapes use 2560 bytes; SIMH's own limit is far below this. */
#define TAPE_MAXREC (1024u * 1024u)

#define TAPE_MARK 0u
#define TAPE_EOM 0xFFFFFFFFu

struct tape {
	const unsigned char *d;
	size_t n;
	size_t at;
};

/*
 * The next record.  Returns 1 with *len and *data set, 0 at a tape mark (with
 * *len 0), and -1 at the end of medium or on damage, with a message.
 */
static int
tape_next(struct tape *t, const unsigned char **data, size_t *len, const char **err)
{
	uint32_t n, tr;
	size_t pad;

	*err = NULL;
	*data = NULL;
	*len = 0;

	if (t->at + 4 > t->n)
		return -1; /* ran out: an ordinary end for a file without EOM */

	n = (uint32_t)t->d[t->at] | ((uint32_t)t->d[t->at + 1] << 8) |
	    ((uint32_t)t->d[t->at + 2] << 16) | ((uint32_t)t->d[t->at + 3] << 24);
	t->at += 4;

	if (n == TAPE_EOM)
		return -1;

	if (n == TAPE_MARK)
		return 0;

	if (n > TAPE_MAXREC) {
		*err = "a record longer than a megabyte";
		return -1;
	}

	pad = n & 1u; /* records are padded to an even length */

	if (t->at + n + pad + 4 > t->n) {
		*err = "a record that runs off the end of the file";
		return -1;
	}

	*data = t->d + t->at;
	*len = n;
	t->at += n + pad;

	tr = (uint32_t)t->d[t->at] | ((uint32_t)t->d[t->at + 1] << 8) |
	     ((uint32_t)t->d[t->at + 2] << 16) | ((uint32_t)t->d[t->at + 3] << 24);
	t->at += 4;

	/* THE ONE CHECK THE FORMAT ITSELF OFFERS.  The length is written on both
	 * sides so that a tape can be read backwards; here it is the only way to
	 * notice that a record header was garbage. */
	if (tr != n) {
		*err = "a record whose leading and trailing lengths disagree";
		return -1;
	}

	return 1;
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

int
cmd_tape(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_CORE);
	const char *extract = NULL;
	unsigned char *buf;
	size_t nbuf;
	struct tape t;
	unsigned long nrec = 0, nmark = 0, nfile = 0, filerec = 0;
	unsigned long long total = 0, filebytes = 0;
	size_t reclen = 0;
	int mixed = 0, c, rc = 1;
	FILE *out = NULL;
	char path[512];

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

	t.d = buf;
	t.n = nbuf;
	t.at = 0;

	printf("file          %s\n", argv[optind]);
	printf("size          %zu bytes\n", nbuf);
	printf("packing       %s (%s) -- %s\n", pk->name, pk->status, pk->desc);

	for (;;) {
		const unsigned char *rec;
		size_t len;
		const char *err;
		int r = tape_next(&t, &rec, &len, &err);

		if (r < 0) {
			if (err != NULL) {
				printf("\n");
				fprintf(stderr, "itsfs: %s at byte %zu\n", err, t.at);
				goto out;
			}

			break;
		}

		if (r == 0) {
			/* A tape mark ends a file.  Two in a row is the usual
			 * end-of-tape marker and not an empty file. */
			nmark++;

			if (filerec > 0) {
				if (extract != NULL && out != NULL) {
					fclose(out);
					out = NULL;
				}

				printf("file %-3lu     %lu records, %llu bytes = %llu words\n", nfile,
				       filerec, filebytes, filebytes / pk->bytes * pk->words);
				nfile++;
				filerec = 0;
				filebytes = 0;
			}

			continue;
		}

		if (nrec == 0)
			reclen = len;
		else if (len != reclen)
			mixed = 1;

		nrec++;
		total += len;
		filerec++;
		filebytes += len;

		if (extract != NULL) {
			if (out == NULL) {
				snprintf(path, sizeof path, "%s/file%lu.words", extract, nfile);
				out = fopen(path, "wb");

				if (out == NULL) {
					perror(path);
					goto out;
				}
			}

			/*
			 * Extraction writes WORDS, one per 8-byte little-endian
			 * container -- the same thing `get -w` writes and `put
			 * -w` reads.  Writing the tape's own bytes back out
			 * would be a copy of the container rather than of the
			 * file, and the point of decoding is to stop caring
			 * which container it arrived in.
			 */
			{
				size_t nw = len / pk->bytes * pk->words;
				uint64_t *w = calloc(nw ? nw : 1, sizeof *w);

				if (w == NULL) {
					fprintf(stderr, "itsfs: out of memory\n");
					goto out;
				}

				its_get_words(pk, rec, w, nw);

				if (its_write_words(out, w, nw, path) != 0) {
					free(w);
					goto out;
				}

				free(w);
			}
		}
	}

	if (filerec > 0) {
		/* A tape that ends without a final mark. */
		if (extract != NULL && out != NULL) {
			fclose(out);
			out = NULL;
		}

		printf("file %-3lu     %lu records, %llu bytes = %llu words  (no closing tape mark)\n",
		       nfile, filerec, filebytes, filebytes / pk->bytes * pk->words);
		nfile++;
	}

	printf("\n");
	printf("%lu records, %lu tape marks, %llu bytes of data\n", nrec, nmark, total);

	if (nrec > 0 && !mixed)
		printf("record length %zu = %zu %s words\n", reclen, reclen / pk->bytes * pk->words,
		       pk->name);
	else if (mixed)
		printf("records are of mixed length\n");

	if (extract != NULL)
		printf("%lu file%s written to %s as 36-bit words, one per 8 bytes\n", nfile,
		       nfile == 1 ? "" : "s", extract);

	rc = 0;
out:
	if (out != NULL)
		fclose(out);
	free(buf);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs tape [-p packing] [-x dir] tapefile\n"
			"       -p   the word packing (default core, the magtape\n"
			"            convention -- NOT le64, which is for disks)\n"
			"       -x   extract each file to dir, as 36-bit words\n"
			"\n"
			"       Reads the SIMH .tap container: records and tape marks.\n"
			"       What is INSIDE the records is a separate format.\n");
	return 2;
}
