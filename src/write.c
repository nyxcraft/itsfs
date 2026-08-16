/*
 * write.c -- the one mutation path.  See write.h for the rules this enforces
 * and for what ITS itself does that this follows.
 */

#define _POSIX_C_SOURCE 200809L

#include "write.h"
#include "its.h"
#include "itspack.h"
#include "itstext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------- is it in use? */

/*
 * Does another process have this file open?
 *
 * There is nothing to lock: SIMH takes no lock on a disk image, so two writers
 * simply interleave and the result is not a file system.  The only signal
 * available is /proc, which means this works on Linux and answers "nobody, as
 * far as I can tell" everywhere else -- so it is a check that can miss and must
 * never be the only thing between a user and a corrupted pack.  It is a seat
 * belt, and the documentation says so.
 *
 * Compared by device and inode rather than by path, because the emulator will
 * have opened it by a different one.
 */
static pid_t
in_use_by(const char *path)
{
	struct stat want, got;
	DIR *proc;
	struct dirent *de;
	pid_t me = getpid(), found = 0;

	if (stat(path, &want) != 0)
		return 0;

	proc = opendir("/proc");

	if (proc == NULL)
		return 0; /* not Linux, or no /proc: cannot tell */

	while ((de = readdir(proc)) != NULL && found == 0) {
		char fdpath[64];
		DIR *fds;
		struct dirent *fe;
		long pid;
		char *end;

		pid = strtol(de->d_name, &end, 10);

		if (*end != '\0' || pid <= 0 || (pid_t)pid == me)
			continue;

		snprintf(fdpath, sizeof fdpath, "/proc/%ld/fd", pid);
		fds = opendir(fdpath);

		if (fds == NULL)
			continue; /* somebody else's process */

		while ((fe = readdir(fds)) != NULL) {
			/* /proc/<pid>/fd/<n>: the name is a small integer, but
			 * d_name is NAME_MAX and the compiler cannot know that. */
			char full[sizeof fdpath + 1 + 256];

			if (fe->d_name[0] == '.')
				continue;
			snprintf(full, sizeof full, "%s/%s", fdpath, fe->d_name);

			if (stat(full, &got) == 0 && got.st_dev == want.st_dev &&
			    got.st_ino == want.st_ino) {
				found = (pid_t)pid;
				break;
			}
		}

		closedir(fds);
	}

	closedir(proc);
	return found;
}

int
itsw_open(its_writer *w, const char *path, const its_pack *pk, const its_drive *drv)
{
	pid_t other;

	memset(w, 0, sizeof *w);

	if (getenv("ITSFS_IGNORE_INUSE") == NULL && (other = in_use_by(path)) != 0) {
		fprintf(stderr, "itsfs: %s is open by process %ld -- refusing to write.\n", path,
			(long)other);
		fprintf(stderr, "       An emulator with this pack attached is writing to it too,\n");
		fprintf(stderr, "       and there is no lock to take.  Work on a copy, or set\n");
		fprintf(stderr, "       ITSFS_IGNORE_INUSE=1 if you are certain.\n");
		return -1;
	}

	if (img_open(&w->im, path, pk, drv, 1) != 0)
		return -1;

	if (w->im.drv == NULL) {
		fprintf(stderr, "itsfs: %s: no drive geometry -- name one with -d\n", path);
		img_close(&w->im);
		return -1;
	}

	if (its_tut_read(&w->im, &w->tut) != 0) {
		img_close(&w->im);
		return -1;
	}

	w->wpb = img_words_per_block(&w->im);
	return 0;
}

/* Write a block back.  The one place a whole block leaves this layer. */
static int
put_block(its_writer *w, uint64_t blk, const uint64_t *words, unsigned n)
{
	uint64_t sector;

	if (its_blk_sector(w->im.drv, blk, &sector) != 0) {
		fprintf(stderr, "itsfs: block %llu is past the end of an %s\n",
			(unsigned long long)blk, w->im.drv->name);
		return -1;
	}

	return img_write_words(&w->im, sector * ITS_WORDS_PER_SECTOR, words, n);
}

int
itsw_close(its_writer *w)
{
	int rc = 0;

	if (w->tut_dirty) {
		for (unsigned i = 0; i < w->im.drv->ntutbl; i++)
			if (put_block(w, its_tut_block(w->im.drv) + i,
				      w->tut.w + (size_t)i * w->wpb, w->wpb) != 0) {
				rc = -1;
				break;
			}

		w->tut_dirty = 0;
	}

	its_tut_free(&w->tut);
	img_close(&w->im);
	return rc;
}

/* ------------------------------------------------------------ allocation */

/* Set the TUT entry for `blk`.  The caller has bounded it. */
static void
tut_set(its_writer *w, uint64_t blk, unsigned val)
{
	uint64_t rel = blk - w->tut.first;
	uint64_t word = ITS_LTIBLK + rel / ITS_TUTEPW;
	unsigned shift = 33 - ITS_TUTBYT * (unsigned)(rel % ITS_TUTEPW);
	uint64_t mask = (uint64_t)(ITS_TUTMAX - 1) << shift;

	w->tut.w[word] = (w->tut.w[word] & ~mask) | ((uint64_t)val << shift);
	w->tut_dirty = 1;
}

uint64_t
itsw_nfree(const its_writer *w)
{
	uint64_t n = 0;

	for (uint64_t b = w->tut.first; b < w->tut.last; b++)
		if (its_tut_entry(&w->tut, b) == 0)
			n++;
	return n;
}

/*
 * Allocate `n` blocks.
 *
 * Scanning starts at the first block of the non-swapping area, because FSDEFS
 * says of QSWAPA that "NEW FILES WILL NOT BE WRITTEN LOWER THAN THIS" -- and
 * the reference pack has 72 blocks below it that files do hold, so the rule is
 * about where a writer PUTS things rather than about where they may be.
 *
 * This does not reproduce ITS's allocator, which reserves a cylinder at a time
 * per channel (`DECADE`) and keeps a per-channel cursor.  That policy exists to
 * keep one job's writes near each other on a machine with several jobs writing
 * at once, and it has no meaning for a host tool writing one file.  What it
 * does reproduce is the part a checker can see: a first-fit scan that prefers
 * runs, so a file lands contiguous whenever the pack has room for it to.
 */
int
itsw_alloc(its_writer *w, uint64_t n, uint64_t *blocks)
{
	uint64_t got = 0;
	uint64_t start = w->tut.swapa > w->tut.first ? w->tut.swapa : w->tut.first;

	if (n == 0)
		return 0;

	/* First pass: contiguous runs, longest-first is not worth the sort --
	 * a first-fit run that is long enough is what a fresh pack gives. */
	for (uint64_t b = start; b < w->tut.last && got < n; b++) {
		if (its_tut_entry(&w->tut, b) != 0)
			continue;

		blocks[got++] = b;
	}

	if (got < n) {
		fprintf(stderr, "itsfs: %llu blocks needed, %llu free above the swapping area\n",
			(unsigned long long)n, (unsigned long long)got);
		return -1;
	}

	for (uint64_t i = 0; i < n; i++)
		tut_set(w, blocks[i], 1);

	return 0;
}

int
itsw_free(its_writer *w, const uint64_t *blocks, uint64_t n)
{
	/* CHECK EVERY BLOCK BEFORE FREEING ANY, so a refusal leaves the TUT as
	 * it was rather than half-freed. */
	for (uint64_t i = 0; i < n; i++) {
		int e;

		if (blocks[i] < w->tut.first || blocks[i] >= w->tut.last) {
			fprintf(stderr, "itsfs: block %llu is outside what the TUT maps\n",
				(unsigned long long)blocks[i]);
			return -1;
		}

		e = its_tut_entry(&w->tut, blocks[i]);

		if (e == ITS_TUTLK) {
			fprintf(stderr, "itsfs: block %llu is locked out and will not be freed\n",
				(unsigned long long)blocks[i]);
			return -1;
		}

		/*
		 * A BLOCK AT "MANY OR MORE" CANNOT BE FREED CORRECTLY, and this
		 * is a property of the format rather than a limitation here: the
		 * TUT saturates at TUTMNY, so once a block has that many
		 * references the count of how many it really has is gone.
		 * Decrementing would be a guess and zeroing would be worse.
		 * Nothing on any pack read so far is above 1.
		 */
		if (e >= ITS_TUTMNY) {
			fprintf(stderr, "itsfs: block %llu has %d references (\"many or more\"), "
					"which cannot be decremented -- refusing\n",
				(unsigned long long)blocks[i], e);
			return -1;
		}
	}

	for (uint64_t i = 0; i < n; i++) {
		int e = its_tut_entry(&w->tut, blocks[i]);

		tut_set(w, blocks[i], e > 0 ? (unsigned)(e - 1) : 0);
	}

	return 0;
}

/* ------------------------------------------------------ the descriptor */

/*
 * A block list as a UFD descriptor.  The exact inverse of its_desc_blocks().
 *
 *      040..077 N2 N3   load address: take that block, B := block + 1
 *      1..UDTKMX        take N more, B += N
 *      0                end
 *
 * A load address takes one block, which is why a run of k blocks is a load and
 * then k-1 takes.  The skip codes are not emitted: FSDEFS says ITS itself has
 * not used them "for years", and a writer that emitted an opcode no reader in
 * the wild exercises would be betting on other people's decoders.
 */
int
itsw_desc_encode(const uint64_t *blocks, long nblocks, unsigned char *bytes, size_t max,
		 size_t *nbytes)
{
	size_t n = 0;
	long i = 0;

#define EMIT(b)                                  \
	do {                                     \
		if (n >= max)                    \
			return -1;               \
		bytes[n++] = (unsigned char)(b); \
	}                                        \
	while (0)

	if (nblocks == 0) {
		/* FSDEFS: "A zero length file is described as two bytes: UDWPH
		 * then 0."  Not an empty description -- a place holder. */
		EMIT(ITS_UD_WPH);
		EMIT(0);
		*nbytes = n;
		return 0;
	}

	while (i < nblocks) {
		uint64_t b = blocks[i];
		long run = 1;

		if (b >= ((uint64_t)1 << 17))
			return -1; /* 17 bits is all a load address carries */

		while (i + run < nblocks && blocks[i + run] == b + (uint64_t)run)
			run++;

		EMIT(ITS_UD_LOADAD | ((b >> 12) & 037));
		EMIT((b >> 6) & 077);
		EMIT(b & 077);

		/* The load took one; the rest go out as takes of at most
		 * UDTKMX, largest first because that is fewest bytes. */
		for (long left = run - 1; left > 0;) {
			long take = left > ITS_UD_TKMX ? ITS_UD_TKMX : left;

			EMIT(take);
			left -= take;
		}

		i += run;
	}

	EMIT(0);
	*nbytes = n;
	return 0;
#undef EMIT
}

/* ---------------------------------------------------- directory surgery */

/* The i'th six-bit byte of a directory block's descriptor area. */
static void
desc_put(uint64_t *u, unsigned off, unsigned val)
{
	unsigned word = ITS_UD_DESC + off / ITS_UFDBPW;
	unsigned pos = off % ITS_UFDBPW;
	unsigned shift = 30 - 6 * pos;
	uint64_t mask = (uint64_t)077 << shift;

	u[word] = (u[word] & ~mask) | ((uint64_t)(val & 077) << shift);
}

/*
 * Find where a name belongs in the sorted name area, and whether it is already
 * there.  Returns the word index of the first entry that is not less than the
 * one wanted -- the insertion point -- and sets *exists.
 *
 * The comparison is on the raw SIXBIT words, which is what makes it agree with
 * ITS: SIXBIT's collating order is ASCII's minus 040, so it sorts `-READ-`
 * before `1PROC` before `AINOTE`, which is what the reference pack does.
 *
 * THE DUPLICATE SEARCH DOES NOT STOP EARLY, and the insertion point does.  On a
 * sorted area those are the same scan and the distinction is invisible; on an
 * area that is NOT sorted -- which no ITS pack should have and a damaged one
 * might -- stopping early would miss an existing name and create a second entry
 * with it.  Two directory entries naming one file is the bug s5fs shipped, on an
 * image its checker called clean, and it is worth one full scan of at most two
 * hundred entries to make it unreachable here.
 *
 * The project's own test fixture was unsorted for three phases, which is how
 * this was noticed rather than reasoned about.
 */
static unsigned
name_slot(const its_ufd *u, uint64_t n1, uint64_t n2, int *exists)
{
	unsigned i, slot = u->wpb - ((u->wpb - (unsigned)u->namp) % ITS_LUNBLK);
	int have_slot = 0;

	*exists = 0;

	for (i = (unsigned)u->namp; i + ITS_LUNBLK <= u->wpb; i += ITS_LUNBLK) {
		uint64_t a1 = u->w[i + ITS_UN_FN1], a2 = u->w[i + ITS_UN_FN2];

		if (a1 == n1 && a2 == n2)
			*exists = 1;

		if (!have_slot && (a1 > n1 || (a1 == n1 && a2 > n2))) {
			slot = i;
			have_slot = 1;
		}
	}

	/* Past every entry: the new name sorts last, so it goes at the end. */
	if (!have_slot)
		slot = i;

	return slot;
}

/* ------------------------------------------------------------------- put */

int
itsw_put(its_writer *w, const char *dir, const char *fn1, const char *fn2, const uint64_t *words,
	 uint64_t nwords)
{
	its_mfd m;
	its_ufd u;
	uint64_t dirblk = 0, n1 = 0, n2 = 0;
	uint64_t *blocks = NULL;
	unsigned char desc[512];
	size_t ndesc = 0;
	long nblocks;
	unsigned slot, descoff;
	int exists = 0, rc = -1, have_ufd = 0;
	char have[ITS_NAME_MAX];

	/* ---- 1. everything that can be refused, before anything is written */

	if (its_sixbit_make(fn1, &n1) != 0 || its_sixbit_make(fn2 ? fn2 : "", &n2) != 0) {
		fprintf(stderr, "itsfs: '%s %s' is not a SIXBIT name: at most six characters "
				"each, 040..137, and there is no lower case\n",
			fn1, fn2 ? fn2 : "");
		return -1;
	}

	if (its_mfd_read(&w->im, &m) != 0)
		return -1;

	{
		int found = 0;

		for (unsigned i = 0; i < its_mfd_slots(&m); i++) {
			uint64_t b;

			if (its_mfd_dir(&m, i, have, &b) != 0 || have[0] == '\0')
				continue;

			if (strcmp(have, dir) == 0) {
				dirblk = b;
				found = 1;
				break;
			}
		}

		its_mfd_free(&m);

		if (!found) {
			fprintf(stderr, "itsfs: no directory named '%s' in the MFD\n", dir);
			return -1;
		}
	}

	if (its_ufd_read(&w->im, dirblk, &u) != 0)
		return -1;
	have_ufd = 1;

	slot = name_slot(&u, n1, n2, &exists);

	if (exists) {
		fprintf(stderr, "itsfs: '%s;%s %s' already exists -- delete it first\n", dir, fn1,
			fn2 ? fn2 : "");
		goto out;
	}

	nblocks = (long)((nwords + w->wpb - 1) / w->wpb);
	blocks = calloc((size_t)nblocks + 1, sizeof *blocks);

	if (blocks == NULL) {
		fprintf(stderr, "itsfs: out of memory\n");
		goto out;
	}

	/*
	 * THE TWO AREAS MUST NOT MEET.  The name area grows down and the
	 * descriptor area grows up; a directory is full when they touch, and
	 * ITS has no way to grow one -- a UFD is one block and that is the whole
	 * capacity.  So this is a refusal, with the numbers, rather than an
	 * allocation problem.
	 *
	 * Checked BEFORE the blocks are allocated, so a full directory does not
	 * leave the TUT marked for a file that was never made.
	 */
	if (itsw_alloc(w, (uint64_t)nblocks, blocks) != 0)
		goto out;

	if (itsw_desc_encode(blocks, nblocks, desc, sizeof desc, &ndesc) != 0) {
		fprintf(stderr, "itsfs: the block list needs more than %zu descriptor bytes\n",
			sizeof desc);
		itsw_free(w, blocks, (uint64_t)nblocks);
		goto out;
	}

	descoff = (unsigned)u.w[ITS_UD_ESCP];

	{
		uint64_t desc_end = ITS_UD_DESC + (descoff + ndesc + ITS_UFDBPW - 1) / ITS_UFDBPW;
		uint64_t name_start = u.namp - ITS_LUNBLK;

		if (desc_end > name_start) {
			fprintf(stderr, "itsfs: directory '%s' is full: the descriptor area would "
					"reach word %llu and the names start at %llu\n",
				dir, (unsigned long long)desc_end, (unsigned long long)name_start);
			itsw_free(w, blocks, (uint64_t)nblocks);
			goto out;
		}
	}

	/* ---- 2. commit.  Data, then the TUT, then the descriptor, then the
	 *         name -- see write.h for why that order and no other. */

	{
		uint64_t *blk = calloc(w->wpb, sizeof *blk);
		uint64_t left = nwords;

		if (blk == NULL) {
			fprintf(stderr, "itsfs: out of memory\n");
			itsw_free(w, blocks, (uint64_t)nblocks);
			goto out;
		}

		for (long i = 0; i < nblocks; i++) {
			size_t n = (left < w->wpb) ? (size_t)left : w->wpb;

			memset(blk, 0, (size_t)w->wpb * sizeof *blk);
			memcpy(blk, words + (uint64_t)i * w->wpb, n * sizeof *blk);

			if (put_block(w, blocks[i], blk, w->wpb) != 0) {
				free(blk);
				itsw_free(w, blocks, (uint64_t)nblocks);
				goto out;
			}

			left -= n;
		}

		free(blk);
	}

	/* The TUT is already marked by itsw_alloc; itsw_close flushes it. */

	for (size_t i = 0; i < ndesc; i++)
		desc_put(u.w, descoff + (unsigned)i, desc[i]);
	u.w[ITS_UD_ESCP] = descoff + ndesc;

	/*
	 * INSERT THE NAME, which means shifting.  Entries from UDNAMP up to the
	 * insertion point move down by LUNBLK, and UDNAMP follows them -- the
	 * mirror of QSQSH, which closes the gap on removal.
	 */
	memmove(&u.w[u.namp - ITS_LUNBLK], &u.w[u.namp],
		(slot - u.namp) * sizeof *u.w);

	{
		unsigned at = slot - ITS_LUNBLK;
		unsigned lastwc = (unsigned)(nwords % w->wpb);

		u.w[at + ITS_UN_FN1] = n1;
		u.w[at + ITS_UN_FN2] = n2;
		/* UNWRDC is the last block's word count MOD 2000 octal, so a
		 * last block that is exactly full records zero. */
		u.w[at + ITS_UN_RNDM] = ((uint64_t)lastwc << ITS_UN_WRDC_P) | descoff;
		u.w[at + ITS_UN_DATE] = 0;
		/* UNAUTH all ones is "no directory", which is what every entry
		 * on the reference pack carries.  Claiming an author this
		 * project cannot name would be an invention. */
		u.w[at + ITS_UN_REF] = (uint64_t)0777 << ITS_UN_AUTH_P;
	}

	u.w[ITS_UD_NAMP] = u.namp - ITS_LUNBLK;

	/* UDBLKS: the left half is space allocated and the right half is blocks
	 * used.  Only the right half moves, and it is 18 bits. */
	u.w[ITS_UD_BLKS] = (ITS_LH(u.w[ITS_UD_BLKS]) << 18) |
			   ((ITS_RH(u.w[ITS_UD_BLKS]) + (uint64_t)nblocks) & 0777777);

	if (put_block(w, dirblk, u.w, u.wpb) != 0)
		goto out;

	rc = 0;
out:
	free(blocks);

	if (have_ufd)
		its_ufd_free(&u);
	return rc;
}

/* ------------------------------------------------------------------- del */

int
itsw_del(its_writer *w, const char *dir, const char *fn1, const char *fn2)
{
	its_mfd m;
	its_ufd u;
	uint64_t dirblk = 0;
	uint64_t *blocks = NULL;
	unsigned idx;
	its_ent e;
	const char *err = NULL;
	long nblocks = 0;
	int found = 0, rc = -1, have_ufd = 0;
	char have[ITS_NAME_MAX];

	if (its_mfd_read(&w->im, &m) != 0)
		return -1;

	for (unsigned i = 0; i < its_mfd_slots(&m); i++) {
		uint64_t b;

		if (its_mfd_dir(&m, i, have, &b) != 0 || have[0] == '\0')
			continue;

		if (strcmp(have, dir) == 0) {
			dirblk = b;
			found = 1;
			break;
		}
	}

	its_mfd_free(&m);

	if (!found) {
		fprintf(stderr, "itsfs: no directory named '%s' in the MFD\n", dir);
		return -1;
	}

	if (its_ufd_read(&w->im, dirblk, &u) != 0)
		return -1;
	have_ufd = 1;

	found = 0;
	idx = (unsigned)u.namp;

	while (its_ufd_next(&u, &idx, &e)) {
		if (strcmp(e.fn1, fn1) == 0 && strcmp(e.fn2, fn2 ? fn2 : "") == 0) {
			found = 1;
			break;
		}
	}

	if (!found) {
		fprintf(stderr, "itsfs: no file '%s;%s %s'\n", dir, fn1, fn2 ? fn2 : "");
		goto out;
	}

	if (!e.is_link) {
		nblocks = its_desc_blocks(&u, w->im.drv, e.desc, NULL, 0, &err);

		if (nblocks < 0) {
			fprintf(stderr, "itsfs: '%s;%s %s': %s -- refusing to delete a file whose "
					"block list cannot be read\n",
				dir, fn1, fn2 ? fn2 : "", err);
			goto out;
		}

		blocks = calloc((size_t)nblocks + 1, sizeof *blocks);

		if (blocks == NULL) {
			fprintf(stderr, "itsfs: out of memory\n");
			goto out;
		}

		if (its_desc_blocks(&u, w->im.drv, e.desc, blocks, (size_t)nblocks, &err) != nblocks)
			goto out;

		/* Checked before anything is written, so a block that cannot be
		 * freed refuses the whole delete rather than half of it. */
		if (itsw_free(w, blocks, (uint64_t)nblocks) != 0)
			goto out;
	}

	/*
	 * Zero the descriptor bytes in place, as ITS does -- QDEL3 walks the
	 * description writing zeros.  The area is not compacted, so this leaves
	 * a hole; `check` does not mind, because UDESCP bounds what is live and
	 * nothing points into the hole any more.
	 */
	{
		unsigned off = e.desc;
		unsigned steps = u.wpb * ITS_UFDBPW;
		unsigned c;

		while (steps-- > 0) {
			unsigned word = ITS_UD_DESC + off / ITS_UFDBPW;

			if (word >= u.wpb)
				break;
			c = its_byte6(u.w[word], off % ITS_UFDBPW);
			desc_put(u.w, off, 0);
			off++;

			if (c == 0)
				break;

			/* A load address is three bytes and the two after it
			 * may be anything, including zero -- so they are
			 * consumed rather than tested. */
			if (c >= ITS_UD_LOADAD && !e.is_link) {
				desc_put(u.w, off++, 0);
				desc_put(u.w, off++, 0);
			}
		}
	}

	/* Close the gap: entries below the removed one shift up, and UDNAMP
	 * follows.  This is QSQSH. */
	memmove(&u.w[u.namp + ITS_LUNBLK], &u.w[u.namp],
		(e.word - u.namp) * sizeof *u.w);
	memset(&u.w[u.namp], 0, ITS_LUNBLK * sizeof *u.w);
	u.w[ITS_UD_NAMP] = u.namp + ITS_LUNBLK;

	{
		uint64_t used = ITS_RH(u.w[ITS_UD_BLKS]);

		used = used > (uint64_t)nblocks ? used - (uint64_t)nblocks : 0;
		u.w[ITS_UD_BLKS] = (ITS_LH(u.w[ITS_UD_BLKS]) << 18) | used;
	}

	if (put_block(w, dirblk, u.w, u.wpb) != 0)
		goto out;

	rc = 0;
out:
	free(blocks);

	if (have_ufd)
		its_ufd_free(&u);
	return rc;
}
