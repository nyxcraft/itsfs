/*
 * cmd_write.c -- `itsfs put` and `itsfs del`, the destructive commands.
 *
 *   itsfs put [-p packing] [-d drive] [-w] [-f] image 'DIR;FN1 FN2' hostfile
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
#include "image.h"
#include "structure.h"

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

		/*
		 * PAD THE LAST WORD WITH ^C, WHICH IS WHAT ITS DOES.
		 *
		 * A file's length is kept in WORDS, so a file whose character
		 * count is not a multiple of five leaves character slots at the
		 * end of the last word with nothing to put in them.  This wrote
		 * zeros there.  ITS writes 003.
		 *
		 * Measured on the reference pack rather than assumed: of 131
		 * text files sampled, 100 pad with 003 and 2 with 000; the rest
		 * end exactly on a word boundary and pad with nothing.  A reader
		 * that stops at ^C -- and MDL's does -- runs off the end of a
		 * NUL-padded file and keeps going, which is how this was found:
		 * an init file this wrote was opened and never evaluated.
		 *
		 * The convention was already written down HERE, in
		 * tests/crosscheck.sh, which compensates for it when comparing
		 * extracted files against host originals.  The knowledge was in
		 * the repository and the writer did not follow it.
		 */
		{
			long tail = nbytes % ITS_ASCII_CHARS;

			if (tail != 0)
				for (long i = tail; i < ITS_ASCII_CHARS; i++)
					w[nwords - 1] |= (uint64_t)03 << (29 - 7 * i);
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
	int c, raw = 0, force = 0, rc = 2, nargs;

	while ((c = getopt(argc, argv, is_del ? "p:d:" : "p:d:wf")) != -1) {
		switch (c) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'd':
			if ((drv = opt_drive(optarg)) == NULL)
				return 2;
			break;
		case 'f':
			force = 1;
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
		rc = itsw_put(&w, p.dir, p.fn1, p.fn2, words, (uint64_t)nwords, force) == 0 ? 0 : 1;

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
		fprintf(stderr, "usage: itsfs put [-p packing] [-d drive] [-w] [-f] image "
				"'DIR;FN1 FN2' hostfile\n"
				"       -w   the host file already holds 36-bit words (what "
				"`get -w` writes)\n"
				"       -f   overwrite a file that is already there.  The entry is\n"
				"            replaced in place -- new data first, then one\n"
				"            directory write -- so there is no moment at which\n"
				"            neither the old file nor the new one exists\n"
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

/*
 * `itsfs mv [-p packing] [-d drive] image OLD NEW`
 *
 * Rename inside one directory.  ITS has no operation that moves a file BETWEEN
 * directories -- an entry's position in the MFD is its directory, and moving one
 * would mean rewriting two directory blocks with the file's blocks referenced
 * from neither in between -- so this refuses a cross-directory rename by name
 * rather than doing half of it.  `get` and `put` are the way across.
 */
int
cmd_mv(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_writer w;
	its_path from, to;
	int c, rc, nargs;

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

	/* image OLD NEW, with each name one argument or three. */
	nargs = argc - optind - 1;

	if (nargs == 2)
		nargs = 1;
	else if (nargs == 6)
		nargs = 3;
	else
		goto usage;

	if (its_parse_path(argv, optind + 1, nargs, &from) != 0)
		return 2;

	if (its_parse_path(argv, optind + 1 + nargs, nargs, &to) != 0)
		return 2;

	if (strcmp(from.dir, to.dir) != 0) {
		fprintf(stderr, "itsfs: '%s' and '%s' are different directories, and ITS has no "
				"operation that moves a file between them -- `get` it out and "
				"`put` it back\n",
			from.dir, to.dir);
		return 2;
	}

	if (itsw_open(&w, argv[optind], pk, drv) != 0)
		return 1;

	rc = itsw_rename(&w, from.dir, from.fn1, from.fn2, to.fn1, to.fn2) == 0 ? 0 : 1;

	if (itsw_close(&w) != 0)
		rc = 1;

	if (rc == 0)
		fprintf(stderr, "%s;%s %s -> %s %s\n", from.dir, from.fn1, from.fn2, to.fn1,
			to.fn2);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs mv [-p packing] [-d drive] image OLD NEW\n"
			"       a name is 'DIR;FN1 FN2' or DIR FN1 FN2, and both must name\n"
			"       the same directory\n"
			"       DESTRUCTIVE.  Work on a copy.\n");
	return 2;
}

/*
 * `itsfs rmdir [-p packing] [-d drive] image NAME`
 *
 * Free a directory's MFD slot.  Refuses one that still holds entries; see
 * itsw_rmdir for why that refusal is not a convenience.
 */
int
cmd_rmdir(int argc, char **argv)
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

	rc = itsw_rmdir(&w, argv[optind + 1]) == 0 ? 0 : 1;

	if (itsw_close(&w) != 0)
		rc = 1;

	if (rc == 0)
		fprintf(stderr, "removed %s\n", argv[optind + 1]);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs rmdir [-p packing] [-d drive] image NAME\n"
			"       the directory must be empty\n"
			"       DESTRUCTIVE.  Work on a copy.\n");
	return 2;
}

/*
 * `itsfs ln [-p packing] [-d drive] image TARGET LINKNAME`
 *
 * The argument order is ln(1)'s: what it points at, then what it is called.
 *
 * A link may point ANYWHERE, including at nothing: ITS resolves a target when
 * the file is opened, and 7 of the 399 links on the reference pack point at a
 * file that is not there.  So this does not check that the target exists --
 * refusing to make a link ITS would have made would be this project inventing a
 * rule.  It does check that the target is a name SIXBIT can hold.
 */
int
cmd_ln(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_writer w;
	its_path tgt, name;
	int c, rc, nargs;

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

	nargs = argc - optind - 1;

	if (nargs == 2)
		nargs = 1;
	else if (nargs == 6)
		nargs = 3;
	else
		goto usage;

	if (its_parse_path(argv, optind + 1, nargs, &tgt) != 0 ||
	    its_parse_path(argv, optind + 1 + nargs, nargs, &name) != 0)
		return 2;

	if (itsw_open(&w, argv[optind], pk, drv) != 0)
		return 1;

	rc = itsw_link(&w, name.dir, name.fn1, name.fn2, tgt.dir, tgt.fn1, tgt.fn2) == 0 ? 0 : 1;

	if (itsw_close(&w) != 0)
		rc = 1;

	if (rc == 0)
		fprintf(stderr, "%s;%s %s -> %s;%s %s\n", name.dir, name.fn1, name.fn2, tgt.dir,
			tgt.fn1, tgt.fn2);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs ln [-p packing] [-d drive] image TARGET LINKNAME\n"
			"       a name is 'DIR;FN1 FN2' or DIR FN1 FN2\n"
			"       the target need not exist: ITS resolves one when the file is\n"
			"       opened, and links to nothing are ordinary on a real pack\n"
			"       DESTRUCTIVE.  Work on a copy.\n");
	return 2;
}

/*
 * `itsfs cp [-p packing] [-d drive] image SRC DST`
 *
 * Copy inside one pack.  Unlike `mv` this may cross directories, and the reason
 * is the failure it can leave: a copy READS one entry and WRITES a second, so an
 * interruption leaves the source untouched and the destination unmade.  A move
 * across directories would have to unmake the source, and between the two the
 * file's blocks would be claimed by neither directory.
 *
 * A LINK IS COPIED AS A LINK, pointing where the original points -- not
 * followed.  Copying the data a link resolves to would silently produce a pack
 * with two copies of one file where ITS had one file and a reference to it.
 */
int
cmd_cp(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_writer w;
	its_path from, to;
	its_mfd m;
	its_ufd u;
	its_ent e;
	uint64_t dirblk = 0, *blocks = NULL, *words = NULL, nwords = 0, got = 0;
	const char *err = NULL;
	long nb = 0;
	unsigned idx;
	int c, rc = 1, nargs, found = 0, have_ufd = 0;
	char have[ITS_NAME_MAX];

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

	nargs = argc - optind - 1;

	if (nargs == 2)
		nargs = 1;
	else if (nargs == 6)
		nargs = 3;
	else
		goto usage;

	if (its_parse_path(argv, optind + 1, nargs, &from) != 0 ||
	    its_parse_path(argv, optind + 1 + nargs, nargs, &to) != 0)
		return 2;

	if (itsw_open(&w, argv[optind], pk, drv) != 0)
		return 1;

	/* ---- read the source, before anything is written */

	if (its_mfd_read(&w.im, &m) != 0)
		goto out;

	for (unsigned i = 0; i < its_mfd_slots(&m); i++) {
		uint64_t b;

		if (its_mfd_dir(&m, i, have, &b) != 0 || have[0] == '\0')
			continue;

		if (strcmp(have, from.dir) == 0) {
			dirblk = b;
			found = 1;
			break;
		}
	}

	its_mfd_free(&m);

	if (!found) {
		fprintf(stderr, "itsfs: no directory named '%s' in the MFD\n", from.dir);
		goto out;
	}

	if (its_ufd_read(&w.im, dirblk, &u) != 0)
		goto out;
	have_ufd = 1;

	found = 0;
	idx = (unsigned)u.namp;

	while (its_ufd_next(&u, &idx, &e))
		if (strcmp(e.fn1, from.fn1) == 0 && strcmp(e.fn2, from.fn2) == 0) {
			found = 1;
			break;
		}

	if (!found) {
		fprintf(stderr, "itsfs: no entry '%s;%s %s'\n", from.dir, from.fn1, from.fn2);
		goto out;
	}

	if (e.is_link) {
		char tgt[ITS_NAME_MAX * 3];
		char *sep, *sp, t1[ITS_NAME_MAX], t2[ITS_NAME_MAX];

		if (its_link_target(&u, e.desc, tgt, sizeof tgt, &err) != 0) {
			fprintf(stderr, "itsfs: %s;%s %s: %s\n", from.dir, from.fn1, from.fn2,
				err ? err : "unreadable link");
			goto out;
		}

		sep = strchr(tgt, ';');

		if (sep == NULL) {
			fprintf(stderr, "itsfs: link target |%s| has no directory\n", tgt);
			goto out;
		}

		*sep = '\0';
		sp = sep + 1;

		while (*sp == ' ')
			sp++;

		{
			char *space = strchr(sp, ' ');

			if (space != NULL) {
				*space = '\0';
				snprintf(t1, sizeof t1, "%s", sp);
				snprintf(t2, sizeof t2, "%s", space + 1);
			}
			else {
				snprintf(t1, sizeof t1, "%s", sp);
				t2[0] = '\0';
			}
		}

		its_ufd_free(&u);
		have_ufd = 0;
		rc = itsw_link(&w, to.dir, to.fn1, to.fn2, tgt, t1, t2) == 0 ? 0 : 1;

		if (rc == 0)
			fprintf(stderr, "%s;%s %s -> %s;%s %s (a link, copied as one)\n", to.dir,
				to.fn1, to.fn2, tgt, t1, t2);

		goto out;
	}

	nb = its_desc_blocks(&u, w.im.drv, e.desc, NULL, 0, &err);

	if (nb < 0) {
		fprintf(stderr, "itsfs: %s;%s %s: %s\n", from.dir, from.fn1, from.fn2,
			err ? err : "bad descriptor");
		goto out;
	}

	nwords = its_file_words(u.wpb, nb, e.lastwc);
	blocks = calloc((size_t)nb + 1, sizeof *blocks);
	words = calloc((size_t)nwords + 1, sizeof *words);

	if (blocks == NULL || words == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		goto out;
	}

	if (its_desc_blocks(&u, w.im.drv, e.desc, blocks, (size_t)nb, &err) != nb) {
		fprintf(stderr, "itsfs: %s;%s %s: the descriptor decoded differently twice\n",
			from.dir, from.fn1, from.fn2);
		goto out;
	}

	for (long i = 0; i < nb && got < nwords; i++) {
		size_t want = nwords - got < u.wpb ? (size_t)(nwords - got) : u.wpb;

		if (img_read_block(&w.im, blocks[i], words + got, want) != 0)
			goto out;

		got += want;
	}

	its_ufd_free(&u);
	have_ufd = 0;

	rc = itsw_put(&w, to.dir, to.fn1, to.fn2, words, nwords, 0) == 0 ? 0 : 1;

	if (rc == 0)
		fprintf(stderr, "%s;%s %s -> %s;%s %s (%llu words)\n", from.dir, from.fn1,
			from.fn2, to.dir, to.fn1, to.fn2, (unsigned long long)nwords);
out:
	free(words);
	free(blocks);

	if (have_ufd)
		its_ufd_free(&u);

	if (itsw_close(&w) != 0)
		rc = 1;

	return rc;

usage:
	fprintf(stderr, "usage: itsfs cp [-p packing] [-d drive] image SRC DST\n"
			"       a name is 'DIR;FN1 FN2' or DIR FN1 FN2; the two may be in\n"
			"       different directories, unlike mv\n"
			"       a link is copied as a link, not followed\n"
			"       DESTRUCTIVE.  Work on a copy.\n");
	return 2;
}

/*
 * `itsfs labelit [-p packing] [-d drive] [-n number] image [ID]`
 *
 * Read the pack's label, or set it.  With no ID and no `-n` it only reports,
 * which makes the read the default and the write the thing you ask for.
 */
int
cmd_labelit(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_writer w;
	int64_t num = -1;
	int c, rc;

	while ((c = getopt(argc, argv, "p:d:n:")) != -1) {
		switch (c) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'd':
			if ((drv = opt_drive(optarg)) == NULL)
				return 2;
			break;
		case 'n': {
			uint64_t v;

			if (parse_count(optarg, &v) != 0)
				return 2;

			num = (int64_t)v;
			break;
		}
		default:
			goto usage;
		}
	}

	if (optind != argc - 1 && optind != argc - 2)
		goto usage;

	/* Read-only unless something is being set: opening for writing refuses
	 * an image another process has open, and reporting a label should not. */
	if (optind == argc - 1 && num < 0) {
		its_image im;
		its_tut t;

		if (img_open(&im, argv[optind], pk, drv, 0) != 0)
			return 1;

		if (im.drv == NULL) {
			fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n",
				im.path);
			img_close(&im);
			return 1;
		}

		if (its_tut_read(&im, &t) != 0) {
			img_close(&im);
			return 1;
		}

		printf("ID     %s\n", t.pakid);
		printf("number %llu\n", (unsigned long long)t.pknum);
		its_tut_free(&t);
		img_close(&im);
		return 0;
	}

	if (itsw_open(&w, argv[optind], pk, drv) != 0)
		return 1;

	rc = itsw_labelit(&w, optind == argc - 2 ? argv[optind + 1] : NULL,
			  num >= 0 ? &num : NULL) == 0
		     ? 0
		     : 1;

	if (itsw_close(&w) != 0)
		rc = 1;

	if (rc == 0)
		fprintf(stderr, "labelled %s\n", argv[optind]);
	return rc;

usage:
	fprintf(stderr, "usage: itsfs labelit [-p packing] [-d drive] [-n number] image [ID]\n"
			"       with no ID and no -n it reports the label and changes nothing\n"
			"       DESTRUCTIVE when it sets one.  Work on a copy.\n");
	return 2;
}
