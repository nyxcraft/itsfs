/*
 * cmd_mount.c -- `itsfs mount`, an ITS pack on a host directory, READ-ONLY.
 *
 *   itsfs mount [-p packing] [-d drive] [-m mode] [-f] [-o opt] image dir
 *   itsfs umount dir
 *
 * BUILT ONLY WITH `make FUSE=1`.  libfuse3 is the one dependency this project
 * has, so it is optional and off by default: everything else here is C99 and
 * POSIX and builds on a machine that has never heard of FUSE.  The file
 * commands -- `ls`, `cat`, `get`, `tar` -- do the same work without a mount and
 * are the only path on a system that has none.
 *
 * READ-ONLY, AND NOT AS A PLACEHOLDER.  A writable mount is not a small step
 * from here, because of what an ITS write IS: `itsw_put` takes a whole file and
 * writes data, then the allocation table, then the descriptor, then the name,
 * in that order, so that an interruption strands blocks rather than losing a
 * file.  FUSE hands out byte-range writes at arbitrary offsets against a file
 * that already exists.  Bridging the two means holding a whole file in memory
 * and flushing it on release() -- a write-back cache with its own failure modes,
 * in front of a writer whose first rule is "refuse, do not half-do".  That is a
 * project, not a flag, so the mount says read-only and means it.
 *
 * THE NAMESPACE IS TWO LEVELS AND THAT IS ALL THERE IS:
 *
 *      /                       the master file directory
 *      /KSHACK                 a directory
 *      /KSHACK/BUILD DOC       ITS's KSHACK;BUILD DOC
 *
 * There is no deeper path because ITS has none.  Names are percent-encoded by
 * util.c, the same encoding `tar` uses -- `/` and the space and `%` inside a
 * name, and the whole component when it is `.` or `..`.  The pack really does
 * have a directory named `.`, holding `@ ITS`, so it appears as `/%2E`.
 *
 * AN ITS LINK BECOMES A SYMLINK, relative, so it resolves inside the mount.
 *
 * A LINK WHOSE TARGET IS `>` DANGLES, and that is the honest answer rather than
 * a defect.  `>` is ITS's "the highest version there is", resolved when a file
 * is opened -- 88 of the 399 links on the reference pack point at one, and more
 * chain to one through another link.  A symlink is a fixed string, so there is
 * nothing to point it at; picking a version here would be this project deciding
 * what ITS decides at open time.  The symlink still carries exactly what the
 * disk records, so `readlink` shows the target ITS wrote.
 *
 * WHAT A FILE IS, HERE.  The same question `tar` answers and the same `-m`
 * answer: a 36-bit word is either five 7-bit characters or eight bytes, and
 * `auto` decides per file by looking at the words.  A mount makes it sharper,
 * because `stat` must agree with `read` -- the size in getattr is the length of
 * the bytes read() will produce, so getattr decodes the file to measure it.
 * That makes `ls -l` of a directory cost a decode per file.  It is bounded work
 * on a local image and it is the only way the two can agree.
 *
 * SINGLE-THREADED, DELIBERATELY.  its_image holds a file handle and a buffer
 * and is not thread-safe; FUSE is multi-threaded by default.  `-s` is forced
 * below rather than left to the caller.
 */
#define FUSE_USE_VERSION 31
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cmds.h"
#include "image.h"
#include "its.h"
#include "itsgeom.h"
#include "itspack.h"
#include "itstext.h"
#include "structure.h"
#include "util.h"

enum {
	MODE_AUTO,
	MODE_TEXT,
	MODE_WORDS
};

#define MNT_MAXBLK 65536

/*
 * One mount, one process, and single-threaded -- so the state is a file-scope
 * struct rather than something threaded through fuse_context.
 */
static struct {
	its_image im;
	its_mfd mfd;
	int mode;
	time_t mtime; /* the image's own, for the root and for directories */
} M;

/* ------------------------------------------------------------------ paths */

enum {
	P_BAD = -1,
	P_ROOT = 0,
	P_DIR = 1,
	P_ENT = 2
};

/*
 * `/`, `/DIR`, or `/DIR/FN1 FN2`.  Components are decoded on the way in, so
 * `/%2E/@ ITS` is the directory named `.` and the file `@ ITS` in it.
 */
static int
split_path(const char *path, char *dir, char *fn1, char *fn2)
{
	const char *slash;
	char comp[ITS_NAME_MAX * 3];
	const char *sp;

	dir[0] = fn1[0] = fn2[0] = '\0';

	if (path[0] != '/')
		return P_BAD;

	if (path[1] == '\0')
		return P_ROOT;

	slash = strchr(path + 1, '/');

	{
		size_t n = slash ? (size_t)(slash - path - 1) : strlen(path + 1);

		if (n == 0 || n >= sizeof comp)
			return P_BAD;

		memcpy(comp, path + 1, n);
		comp[n] = '\0';

		if (its_dec_component(dir, ITS_NAME_MAX, comp) != 0)
			return P_BAD;
	}

	if (slash == NULL)
		return P_DIR;

	if (slash[1] == '\0')
		return P_DIR; /* a trailing slash on a directory */

	if (strchr(slash + 1, '/') != NULL)
		return P_BAD; /* ITS has no third level */

	sp = strchr(slash + 1, ' ');

	{
		size_t n = sp ? (size_t)(sp - slash - 1) : strlen(slash + 1);

		if (n == 0 || n >= sizeof comp)
			return P_BAD;

		memcpy(comp, slash + 1, n);
		comp[n] = '\0';

		if (its_dec_component(fn1, ITS_NAME_MAX, comp) != 0)
			return P_BAD;
	}

	if (sp == NULL)
		return P_ENT;

	if (strlen(sp + 1) >= sizeof comp)
		return P_BAD;

	return its_dec_component(fn2, ITS_NAME_MAX, sp + 1) == 0 ? P_ENT : P_BAD;
}

/* The entry named by a path, and the directory block it lives in. */
static int
find_entry(const char *dir, const char *fn1, const char *fn2, its_ufd *u, its_ent *e)
{
	uint64_t blk;
	unsigned idx;

	if (its_find_dir(&M.mfd, dir, &blk) != 0)
		return -1;

	if (its_ufd_read(&M.im, blk, u) != 0)
		return -1;

	idx = (unsigned)u->namp;

	while (its_ufd_next(u, &idx, e))
		if (strcmp(e->fn1, fn1) == 0 && strcmp(e->fn2, fn2) == 0)
			return 0;

	its_ufd_free(u);
	return -1;
}

/* ------------------------------------------------------------- file bytes */

/* The same test `tar` uses; see cmd_tar.c for why it is this strict. */
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
			case 0:
			case 003:
			case 011:
			case 012:
			case 013:
			case 014:
			case 015:
			case 0177:
				continue;
			default:
				return 0;
			}
		}
	}

	return 1;
}

/*
 * A file's bytes, in whichever representation `-m` asks for.  The caller frees.
 * Returns NULL on anything wrong -- a descriptor that does not decode, a block
 * that will not read -- because a mount cannot report "partly".
 */
static unsigned char *
file_bytes(its_ufd *u, const its_ent *e, size_t *len)
{
	const char *err = NULL;
	uint64_t *blocks = NULL, *words = NULL;
	unsigned char *b = NULL;
	uint64_t nwords, got = 0;
	long nb;
	int text;

	nb = its_desc_blocks(u, M.im.drv, e->desc, NULL, 0, &err);

	if (nb < 0 || nb > MNT_MAXBLK)
		return NULL;

	nwords = its_file_words(u->wpb, nb, e->lastwc);
	blocks = calloc((size_t)nb + 1, sizeof *blocks);
	words = calloc((size_t)nwords + 1, sizeof *words);

	if (blocks == NULL || words == NULL)
		goto out;

	if (its_desc_blocks(u, M.im.drv, e->desc, blocks, (size_t)nb, &err) != nb)
		goto out;

	for (long i = 0; i < nb && got < nwords; i++) {
		size_t want = nwords - got < u->wpb ? (size_t)(nwords - got) : u->wpb;

		if (img_read_block(&M.im, blocks[i], words + got, want) != 0)
			goto out;

		got += want;
	}

	text = (M.mode == MODE_TEXT) ||
	       (M.mode == MODE_AUTO && looks_like_text(words, (size_t)nwords));

	if (text) {
		size_t o = 0, pending = 0;

		b = malloc((size_t)nwords * ITS_ASCII_CHARS + 1);

		if (b == NULL)
			goto out;

		for (uint64_t i = 0; i < nwords; i++)
			for (unsigned k = 0; k < ITS_ASCII_CHARS; k++) {
				unsigned c = (unsigned)((words[i] >> (29 - 7 * k)) & 0177u);

				if (c == 0) {
					pending++;
					continue;
				}

				while (pending > 0) {
					b[o++] = 0;
					pending--;
				}

				b[o++] = (unsigned char)c;
			}

		*len = o;
	}
	else {
		b = malloc((size_t)nwords * 8 + 1);

		if (b == NULL)
			goto out;

		for (uint64_t i = 0; i < nwords; i++)
			for (size_t k = 0; k < 8; k++)
				b[i * 8 + k] = (unsigned char)(words[i] >> (8 * k));

		*len = (size_t)nwords * 8;
	}

out:
	free(words);
	free(blocks);
	return b;
}

/*
 * An ITS link target as a path inside the mount.
 *
 * `.` in a target is a DIRECTORY NAME, not a self-reference -- see the note in
 * cmd_tar.c, which is where that was got wrong first and then measured.
 */
static int
link_path(char *out, size_t sz, const char *target)
{
	char tgt[ITS_NAME_MAX * 3];
	char t1[ITS_NAME_MAX], t2[ITS_NAME_MAX];
	char ed[ITS_NAME_MAX * 3], e1[ITS_NAME_MAX * 3], e2[ITS_NAME_MAX * 3];
	char *sep, *sp;

	if (strlen(target) >= sizeof tgt)
		return -1;

	snprintf(tgt, sizeof tgt, "%s", target);
	sep = strchr(tgt, ';');

	if (sep == NULL)
		return -1;

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

	if (its_enc_component(ed, sizeof ed, tgt) != 0 ||
	    its_enc_component(e1, sizeof e1, t1) != 0 || its_enc_component(e2, sizeof e2, t2) != 0)
		return -1;

	return snprintf(out, sz, t2[0] ? "../%s/%s %s" : "../%s/%s", ed, e1, e2) < (int)sz ? 0 : -1;
}

/* ------------------------------------------------------- fuse operations */

static int
its_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	char dir[ITS_NAME_MAX], fn1[ITS_NAME_MAX], fn2[ITS_NAME_MAX];
	its_ufd u;
	its_ent e;
	uint64_t blk;
	int what;

	(void)fi;
	memset(st, 0, sizeof *st);
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_atime = st->st_mtime = st->st_ctime = M.mtime;

	what = split_path(path, dir, fn1, fn2);

	if (what == P_BAD)
		return -ENOENT;

	if (what == P_ROOT) {
		st->st_mode = S_IFDIR | 0555;
		st->st_nlink = 2;
		return 0;
	}

	if (what == P_DIR) {
		if (its_find_dir(&M.mfd, dir, &blk) != 0)
			return -ENOENT;

		st->st_mode = S_IFDIR | 0555;
		st->st_nlink = 2;
		return 0;
	}

	if (find_entry(dir, fn1, fn2, &u, &e) != 0)
		return -ENOENT;

	{
		struct tm tm;

		memset(&tm, 0, sizeof tm);

		if (e.year >= 1900 && e.month >= 1 && e.month <= 12) {
			tm.tm_year = (int)e.year - 1900;
			tm.tm_mon = (int)e.month - 1;
			tm.tm_mday = (int)e.day;
			tm.tm_hour = 12; /* see cmd_tar.c: the disk records a day */
			st->st_mtime = st->st_ctime = st->st_atime = mktime(&tm);
		}
	}

	if (e.is_link) {
		char tgt[ITS_NAME_MAX * 3], lp[ITS_NAME_MAX * 6];
		const char *err = NULL;

		st->st_mode = S_IFLNK | 0777;
		st->st_nlink = 1;
		st->st_size = 0;

		if (its_link_target(&u, e.desc, tgt, sizeof tgt, &err) == 0 &&
		    link_path(lp, sizeof lp, tgt) == 0)
			st->st_size = (off_t)strlen(lp);

		its_ufd_free(&u);
		return 0;
	}

	{
		unsigned char *b;
		size_t len = 0;

		st->st_mode = S_IFREG | 0444;
		st->st_nlink = 1;
		b = file_bytes(&u, &e, &len);
		its_ufd_free(&u);

		if (b == NULL)
			return -EIO;

		free(b);
		st->st_size = (off_t)len;
		st->st_blocks = (blkcnt_t)((len + 511) / 512);
	}

	return 0;
}

static int
its_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
	    struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	char dir[ITS_NAME_MAX], fn1[ITS_NAME_MAX], fn2[ITS_NAME_MAX];
	char enc[ITS_NAME_MAX * 3], enc2[ITS_NAME_MAX * 3], name[ITS_NAME_MAX * 6];
	int what;

	(void)off;
	(void)fi;
	(void)flags;

	what = split_path(path, dir, fn1, fn2);

	if (what != P_ROOT && what != P_DIR)
		return -ENOTDIR;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	if (what == P_ROOT) {
		for (unsigned i = 0; i < its_mfd_slots(&M.mfd); i++) {
			char nm[ITS_NAME_MAX];
			uint64_t blk;

			if (its_mfd_dir(&M.mfd, i, nm, &blk) != 0 || nm[0] == '\0')
				continue;

			if (its_enc_component(enc, sizeof enc, nm) == 0)
				filler(buf, enc, NULL, 0, 0);
		}

		return 0;
	}

	{
		its_ufd u;
		its_ent e;
		unsigned idx;
		uint64_t blk;

		if (its_find_dir(&M.mfd, dir, &blk) != 0)
			return -ENOENT;

		if (its_ufd_read(&M.im, blk, &u) != 0)
			return -EIO;

		idx = (unsigned)u.namp;

		while (its_ufd_next(&u, &idx, &e)) {
			if (e.fn1[0] == '\0' && e.fn2[0] == '\0')
				continue;

			if (its_enc_component(enc, sizeof enc, e.fn1) != 0 ||
			    its_enc_component(enc2, sizeof enc2, e.fn2) != 0)
				continue;

			if (e.fn2[0] == '\0')
				snprintf(name, sizeof name, "%s", enc);
			else
				snprintf(name, sizeof name, "%s %s", enc, enc2);

			filler(buf, name, NULL, 0, 0);
		}

		its_ufd_free(&u);
	}

	return 0;
}

static int
its_readlink(const char *path, char *buf, size_t sz)
{
	char dir[ITS_NAME_MAX], fn1[ITS_NAME_MAX], fn2[ITS_NAME_MAX];
	char tgt[ITS_NAME_MAX * 3], lp[ITS_NAME_MAX * 6];
	const char *err = NULL;
	its_ufd u;
	its_ent e;
	int rc = -EIO;

	if (split_path(path, dir, fn1, fn2) != P_ENT)
		return -EINVAL;

	if (find_entry(dir, fn1, fn2, &u, &e) != 0)
		return -ENOENT;

	if (!e.is_link)
		rc = -EINVAL;
	else if (its_link_target(&u, e.desc, tgt, sizeof tgt, &err) == 0 &&
		 link_path(lp, sizeof lp, tgt) == 0) {
		snprintf(buf, sz, "%s", lp);
		rc = 0;
	}

	its_ufd_free(&u);
	return rc;
}

static int
its_open(const char *path, struct fuse_file_info *fi)
{
	char dir[ITS_NAME_MAX], fn1[ITS_NAME_MAX], fn2[ITS_NAME_MAX];
	its_ufd u;
	its_ent e;
	unsigned char *b;
	size_t len = 0;

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EROFS;

	if (split_path(path, dir, fn1, fn2) != P_ENT)
		return -ENOENT;

	if (find_entry(dir, fn1, fn2, &u, &e) != 0)
		return -ENOENT;

	if (e.is_link) {
		its_ufd_free(&u);
		return -EINVAL;
	}

	/*
	 * Decoded once, on open, and held for the life of the handle: read()
	 * comes back at arbitrary offsets and re-decoding a run-length
	 * descriptor per read would be quadratic.  A file here is at most a few
	 * megabytes.
	 */
	b = file_bytes(&u, &e, &len);
	its_ufd_free(&u);

	if (b == NULL)
		return -EIO;

	{
		unsigned char **held = malloc(sizeof *held + sizeof len);

		if (held == NULL) {
			free(b);
			return -ENOMEM;
		}

		held[0] = b;
		memcpy(held + 1, &len, sizeof len);
		fi->fh = (uint64_t)(uintptr_t)held;
	}

	return 0;
}

static int
its_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
	unsigned char **held = (unsigned char **)(uintptr_t)fi->fh;
	unsigned char *b;
	size_t len;

	(void)path;

	if (held == NULL)
		return -EBADF;

	b = held[0];
	memcpy(&len, held + 1, sizeof len);

	if (off < 0 || (size_t)off >= len)
		return 0;

	if (size > len - (size_t)off)
		size = len - (size_t)off;

	memcpy(buf, b + off, size);
	return (int)size;
}

static int
its_release(const char *path, struct fuse_file_info *fi)
{
	unsigned char **held = (unsigned char **)(uintptr_t)fi->fh;

	(void)path;

	if (held != NULL) {
		free(held[0]);
		free(held);
		fi->fh = 0;
	}

	return 0;
}

static int
its_statfs(const char *path, struct statvfs *st)
{
	its_tut t;

	(void)path;
	memset(st, 0, sizeof *st);

	st->f_bsize = st->f_frsize = (unsigned long)M.im.drv->secblk * ITS_WORDS_PER_SECTOR * 8;
	st->f_namemax = ITS_SIXBIT_CHARS * 2 + 1;

	if (its_tut_read(&M.im, &t) == 0) {
		uint64_t free_ = 0, used = 0;

		for (uint64_t b = t.first; b < t.last; b++) {
			int v = its_tut_entry(&t, b);

			if (v == 0)
				free_++;
			else
				used++;
		}

		st->f_blocks = (fsblkcnt_t)(free_ + used);
		st->f_bfree = st->f_bavail = (fsblkcnt_t)free_;
		its_tut_free(&t);
	}

	return 0;
}

static const struct fuse_operations its_ops = {
	.getattr = its_getattr,
	.readdir = its_readdir,
	.readlink = its_readlink,
	.open = its_open,
	.read = its_read,
	.release = its_release,
	.statfs = its_statfs,
};

/* -------------------------------------------------------------------- main */

int
cmd_mount(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	/* fuse_main takes char *argv[], so these are arrays rather than string
	 * literals: casting away const to satisfy it is a warning the tree does
	 * not carry anywhere else. */
	static char a_s[] = "-s", a_o[] = "-o", a_ro[] = "ro", a_f[] = "-f";
	char *fargv[16];
	int fargc = 0, foreground = 0, i;
	char *oopt = NULL;
	struct stat sb;

	M.mode = MODE_AUTO;

	for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "-f") == 0) {
			foreground = 1;
		}
		else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			if ((pk = opt_pack(argv[++i])) == NULL)
				return 2;
		}
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			if ((drv = opt_drive(argv[++i])) == NULL)
				return 2;
		}
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			oopt = argv[++i];
		}
		else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
			const char *m = argv[++i];

			if (strcmp(m, "auto") == 0)
				M.mode = MODE_AUTO;
			else if (strcmp(m, "text") == 0)
				M.mode = MODE_TEXT;
			else if (strcmp(m, "words") == 0)
				M.mode = MODE_WORDS;
			else {
				fprintf(stderr, "itsfs: -m takes auto, text or words, not |%s|\n",
					m);
				return 2;
			}
		}
		else {
			goto usage;
		}
	}

	if (i + 1 != argc - 1)
		goto usage;

	if (img_open(&M.im, argv[i], pk, drv, 0) != 0)
		return 1;

	if (M.im.drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", M.im.path);
		img_close(&M.im);
		return 1;
	}

	if (its_mfd_read(&M.im, &M.mfd) != 0) {
		img_close(&M.im);
		return 1;
	}

	M.mtime = stat(argv[i], &sb) == 0 ? sb.st_mtime : 0;

	fargv[fargc++] = argv[0];
	fargv[fargc++] = argv[i + 1];
	fargv[fargc++] = a_s; /* see the note at the top: not thread-safe */
	fargv[fargc++] = a_o;
	fargv[fargc++] = a_ro;

	if (foreground)
		fargv[fargc++] = a_f;

	if (oopt != NULL) {
		fargv[fargc++] = a_o;
		fargv[fargc++] = oopt;
	}

	fargv[fargc] = NULL;

	fprintf(stderr, "itsfs: %s on %s, %u directories, read-only\n", argv[i], argv[i + 1],
		its_mfd_ndirs(&M.mfd));
	fprintf(stderr, "itsfs: `itsfs umount %s` when finished\n", argv[i + 1]);

	{
		int rc = fuse_main(fargc, fargv, &its_ops, NULL);

		its_mfd_free(&M.mfd);
		img_close(&M.im);
		return rc == 0 ? 0 : 1;
	}

usage:
	fprintf(stderr, "usage: itsfs mount [-p packing] [-d drive] [-m mode] [-f] [-o opt] "
			"image dir\n"
			"       -m   how a 36-bit word becomes bytes: auto (default), text, "
			"words\n"
			"       -f   stay in the foreground\n"
			"       -o   passed through to FUSE\n"
			"\n"
			"       READ-ONLY.  /DIR/FN1 FN2 is ITS's DIR;FN1 FN2.\n");
	return 2;
}

int
cmd_umount(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: itsfs umount dir\n");
		return 2;
	}

	execlp("fusermount3", "fusermount3", "-u", argv[1], (char *)NULL);
	execlp("fusermount", "fusermount", "-u", argv[1], (char *)NULL);
	perror("fusermount3");
	return 1;
}
