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
	its_path p;
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

	if (its_parse_path(argv, optind + 1, nargs, &p) != 0)
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

/*
 * `itsfs mkdir image NAME`
 *
 * ITS's own idiom for this is `:print DIR;..new. (udir)` typed at a DDT, which
 * is a file operation on a magic name rather than a command of its own.  This
 * is a command of its own, because a host tool has no DDT and the magic name
 * would be a worse interface than a verb.
 */
int
cmd_mkdir(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_writer w;
	int c, rc;

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

	if (optind != argc - 2)
		goto usage;

	if (itsw_open(&w, argv[optind], pk, drv) != 0)
		return 1;

	rc = itsw_mkdir(&w, argv[optind + 1]) == 0 ? 0 : 1;

	if (itsw_close(&w) != 0)
		rc = 1;

	if (rc == 0)
		fprintf(stderr, "made %s\n", argv[optind + 1]);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs mkdir [-p packing] [-d drive] image NAME\n"
			"       DESTRUCTIVE.  Work on a copy.\n");
	return 2;
}

/*
 * `itsfs mkfs [-f] [-d drive] [-n packnum] [-s blocks] [-u slots] image ID`
 *
 * WHAT THIS MAKES IS A FILE SYSTEM, NOT A BOOTABLE PACK.  ITS boots from the
 * front end's blocks at the very bottom of the disk -- the ones NSALV's ZAP
 * calls the "8080 'HOM' sectors" and refuses to touch -- and then loads a
 * system out of a directory.  Neither is here: this writes a master file
 * directory, an allocation table and NUDS empty directory blocks, which is
 * exactly what NSALV's own MARK does after it has formatted the platter.
 *
 * So the way to check one is NSALV, which boots from tape and does not care
 * whether the pack can boot.  `make mkfs-test` does that.
 */
int
cmd_mkfs(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_writer w;
	uint64_t packnum = 0, nuds = 500, swapa = 0;
	int c, force = 0, rc;
	unsigned nblksc;

	while ((c = getopt(argc, argv, "p:d:n:s:u:f")) != -1) {
		switch (c) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'd':
			if ((drv = opt_drive(optarg)) == NULL)
				return 2;
			break;
		case 'n':
			if (parse_count(optarg, &packnum) != 0)
				return 2;
			break;
		case 's':
			if (parse_count(optarg, &swapa) != 0)
				return 2;
			break;
		case 'u':
			if (parse_count(optarg, &nuds) != 0)
				return 2;
			break;
		case 'f':
			force = 1;
			break;
		default:
			goto usage;
		}
	}

	if (optind != argc - 2)
		goto usage;

	if (drv == NULL)
		drv = its_drive_by_name("rp06");

	if (!force && access(argv[optind], F_OK) == 0) {
		fprintf(stderr, "itsfs: %s exists (use -f to overwrite -- THIS DESTROYS IT)\n",
			argv[optind]);
		return 1;
	}

	/*
	 * The image is created at the drive's full size and left sparse: a
	 * fresh file system is almost entirely zeros, and 300 MB of them on
	 * disk to say so would be a waste.  An existing file is truncated to
	 * the same size, which is what makes -f mean "start over" rather than
	 * "write a file system into whatever was here".
	 */
	{
		uint64_t words = its_nsectors(drv) * ITS_WORDS_PER_SECTOR;
		uint64_t bytes = (words / pk->words) * pk->bytes;
		FILE *f = fopen(argv[optind], "wb");

		if (f == NULL) {
			perror(argv[optind]);
			return 1;
		}

		if (fseeko(f, (off_t)bytes - 1, SEEK_SET) != 0 || fputc(0, f) == EOF) {
			perror(argv[optind]);
			fclose(f);
			return 1;
		}

		fclose(f);
	}

	if (itsw_open(&w, argv[optind], pk, drv) != 0)
		return 1;

	/*
	 * The swapping allocation defaults to what the ITS build answers MARK's
	 * "Alloc?" question with -- 3000 octal, 1536 blocks -- rounded up to a
	 * whole cylinder, because FSDEFS says QSWAPA must be a multiple of
	 * DECADE and 1536 is not one.  On an RP06 that gives 1551, which is what
	 * the reference pack carries.
	 */
	nblksc = its_blks_per_cyl(drv);

	if (swapa == 0)
		swapa = 1536;
	swapa = ((swapa + nblksc - 1) / nblksc) * nblksc;

	rc = itsw_mkfs(&w, nuds, swapa, packnum, argv[optind + 1]) == 0 ? 0 : 1;

	if (itsw_close(&w) != 0)
		rc = 1;

	if (rc == 0)
		fprintf(stderr, "made an %s file system on %s: pack %llu, ID %s, "
				"%llu directory slots, %llu blocks of swapping\n",
			drv->name, argv[optind], (unsigned long long)packnum, argv[optind + 1],
			(unsigned long long)nuds, (unsigned long long)swapa);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs mkfs [-p packing] [-d drive] [-f] [-n packnum]\n"
			"                  [-s blocks] [-u slots] image ID\n"
			"       -d   the drive to make it for (default rp06)\n"
			"       -n   the pack number (default 0)\n"
			"       -s   blocks of swapping area (default 1536, rounded up\n"
			"            to a whole cylinder as FSDEFS requires)\n"
			"       -u   directory slots in the MFD (default 500)\n"
			"       -f   overwrite an existing image -- THIS DESTROYS IT\n"
			"\n"
			"       Makes a FILE SYSTEM, not a bootable pack: the boot blocks\n"
			"       and the system are somebody else's job.  Check one with\n"
			"       NSALV, which boots from tape.\n");
	return 2;
}
