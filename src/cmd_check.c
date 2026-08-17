/*
 * cmd_check.c -- an independent checker for the ITS file system.
 *
 * INDEPENDENT IS THE WHOLE POINT.  This file does not include structure.h and
 * calls nothing in it.  It re-derives the MFD, the directory-slot arithmetic,
 * the UFD layout, the descriptor bytecode and the TUT from its.h -- which is a
 * transcription of SYSTEM;FSDEFS 43, not an interpretation of it.
 *
 * It also re-derives THE GEOMETRY.  itsgeom.h is included for the drive table,
 * which is data, but `its_blk_sector()` is not called: the block-to-sector
 * conversion is written out again below.  That is deliberate and it is the one
 * place the second implementation is most worth having, because the geometry is
 * the part of this format with no check word, no pointer and no redundancy --
 * being wrong about it silently reads somebody else's mail.
 *
 * What IS shared is image.c, the word layer, which is proven byte-for-byte
 * against a real pack and has nothing to interpret.
 *
 * If the checker shared the reader's walk, "clean" would mean only that the
 * reader agrees with itself.  It has to be possible for the two to DISAGREE, or
 * a clean check is not evidence.  s5fs's fsck shares no code with its writer for
 * the same reason, and it caught real bugs there.
 *
 * THE HONEST LIMIT: both readers get their constants from its.h, so both inherit
 * any misreading in it.  A second reading catches a wrong reading; it cannot
 * catch what the source never says.  The corruption fuzzer is the tool for that
 * half, and in t10fs it was the fuzzer rather than the second implementation
 * that found the one bug both readers shared.
 *
 * WHAT IT CHECKS, and why each one is worth doing:
 *
 *   the MFD          MDCHK, and that MDNAMP and MDNUDS satisfy the assertion
 *                    FSDEFS makes about them itself
 *   directory slots  every name resolves to a block below NUDSL, and no two
 *                    slots name the same directory
 *   each UFD         its UDNAME matches the name that addressed it, its two
 *                    areas have not overrun each other, and every UNDSCP points
 *                    inside the descriptor area
 *   descriptors      decoded, with every block bounded by the drive
 *   UDBLKS           against the blocks the descriptors actually name
 *   THE TUT          per block, a COUNT rather than a flag: the TUT is a
 *                    reference count, so the question is not "is this block
 *                    allocated" but "is it referenced as many times as it says"
 *   locked out       exactly the directories, the MFD and the TUT
 *   links            resolved, and reported when they do not resolve -- as a
 *                    note, never a problem.  A link to a file somebody deleted
 *                    is an ordinary state of a live system, not damage.
 *
 * usage: itsfs check [-p packing] [-d drive] [-v] image
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "util.h"
#include "image.h"
#include "its.h"
#include "itspack.h"
#include "itsgeom.h"
#include "itstext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

/*
 * A cap on how much a damaged pack may make this print.  The fuzzer's whole job
 * is producing packs where everything is wrong at once, and a checker that
 * answers with a hundred thousand lines is one nobody reads twice.  The COUNT is
 * never capped -- only the printing.
 */
#define CK_MAXPRINT 100

/* A file cannot need more blocks than the drive has; this is a sanity bound on
 * a descriptor's output, not a limit on a legitimate file. */
#define CK_MAXFILEBLK 200000

struct ckdir {
	char name[ITS_SIXBIT_CHARS + 1];
	uint64_t blk;
	uint64_t *w; /* the whole directory block, read once */
	int ok;	     /* header passed its checks */
};

struct ck {
	its_image *im;
	const its_drive *d;

	/* geometry, re-derived here rather than asked of itsgeom.c */
	unsigned nblksc;
	unsigned wpb;
	uint64_t nblks, tblks;
	uint64_t mfdblk, tutblk;

	uint64_t *mfd;
	uint64_t nudsl; /* C(MDNUDS) */

	struct ckdir *dirs;
	unsigned ndirs;

	uint16_t *ref;	    /* computed reference count, per block */
	const char **owner; /* who claimed it first */
	char **names;	    /* every "DIR;FN1 FN2" string, owned */
	size_t nnames, cnames;

	unsigned long nfiles, nlinks, ndangling, nzero;
	uint64_t nclaimed; /* distinct blocks claimed at least once */

	unsigned long problems, notes;
	unsigned long printed;
	int verbose;
};

/* --------------------------------------------------------------- reporting */

static void
problem(struct ck *c, const char *fmt, ...)
{
	va_list ap;

	c->problems++;

	if (c->printed >= CK_MAXPRINT) {
		if (c->printed == CK_MAXPRINT)
			printf("  ... (further messages suppressed; the counts below are complete)\n");
		c->printed++;
		return;
	}

	c->printed++;
	printf("  ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* A note is something true and worth saying that is NOT damage.  It never
 * changes the exit status, and the difference is the point: a checker that
 * calls an ordinary state of a live file system an error trains people to
 * ignore it. */
static void
note(struct ck *c, const char *fmt, ...)
{
	va_list ap;

	c->notes++;

	if (!c->verbose)
		return;

	printf("  note: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* --------------------------------------------------------------- geometry */

/*
 * The block-to-sector conversion, written out a second time.  See the header
 * comment for why this is not a call to its_blk_sector().
 *
 * FSDEFS, for the one block whose address is a constant:
 *      MFDCYL==MFDBLK/NBLKSC
 *      MFDSRF==<MFDBLK-MFDCYL*NBLKSC>*SECBLK/NSECS
 *      MFDSEC==<MFDBLK-MFDCYL*NBLKSC>*SECBLK-MFDSRF*NSECS
 */
static int
ck_read(struct ck *c, uint64_t blk, uint64_t *w)
{
	uint64_t cyl, within, srf, sec, sector;

	if (blk >= c->tblks)
		return -1;

	cyl = blk / c->nblksc;
	within = (blk - cyl * c->nblksc) * c->d->secblk;
	srf = within / c->d->nsecs;
	sec = within - srf * c->d->nsecs;
	sector = (cyl * c->d->nheds + srf) * c->d->nsecs + sec;

	return img_read_words(c->im, sector * ITS_WORDS_PER_SECTOR, w, c->wpb);
}

/* ------------------------------------------------------------- the claims */

/* Remember a name for as long as the run lasts, so a block can say who has it. */
static const char *
keep(struct ck *c, const char *s)
{
	char *dup;

	if (c->nnames == c->cnames) {
		size_t want = c->cnames ? c->cnames * 2 : 256;
		char **bigger = realloc(c->names, want * sizeof *bigger);

		if (bigger == NULL)
			return "(out of memory)";
		c->names = bigger;
		c->cnames = want;
	}

	dup = malloc(strlen(s) + 1);

	if (dup == NULL)
		return "(out of memory)";
	strcpy(dup, s);
	c->names[c->nnames++] = dup;
	return dup;
}

static void
claim(struct ck *c, uint64_t blk, const char *who)
{
	if (blk >= c->tblks) {
		problem(c, "%s claims block %llu, past the end of an %s (%llu blocks)", who,
			(unsigned long long)blk, c->d->name, (unsigned long long)c->tblks);
		return;
	}

	if (c->ref[blk] == 0) {
		c->owner[blk] = who;
		c->nclaimed++;
	}
	else {
		/*
		 * NOT NECESSARILY DAMAGE -- the TUT is a reference count, so
		 * the format allows a block to be referenced more than once.
		 * Nothing on any pack read so far does it, and nothing here
		 * knows what a legitimate second reference looks like, so this
		 * is reported and counted and left for a human.  If the TUT
		 * agrees with the count, check_tut() will say nothing further.
		 */
		problem(c, "block %llu is claimed by %s and already by %s", (unsigned long long)blk,
			who, c->owner[blk]);
	}

	if (c->ref[blk] < 0xFFFF)
		c->ref[blk]++;
}

/* ------------------------------------------------------- the MFD, and slots */

static int
check_mfd(struct ck *c)
{
	uint64_t namp;
	unsigned slots, i;
	char got[ITS_SIXBIT_CHARS + 1];

	if (ck_read(c, c->mfdblk, c->mfd) != 0) {
		fprintf(stderr, "itsfs check: cannot read the MFD (block %llu)\n",
			(unsigned long long)c->mfdblk);
		return -1;
	}

	if (c->mfd[ITS_MD_CHK] != ITS_MFD_MAGIC) {
		its_sixbit_word(c->mfd[ITS_MD_CHK], got);
		problem(c, "block %llu is not an MFD: MDCHK is |%s| (%012llo), not |M.F.D.|",
			(unsigned long long)c->mfdblk, got, (unsigned long long)c->mfd[ITS_MD_CHK]);
		return -1; /* nothing below this can mean anything */
	}

	namp = c->mfd[ITS_MD_NAMP];
	c->nudsl = c->mfd[ITS_MD_NUDS];

	if (namp < ITS_LMIBLK || namp > c->wpb) {
		problem(c, "MDNAMP is %llu, outside a %u-word block", (unsigned long long)namp,
			c->wpb);
		return -1;
	}

	if ((c->wpb - namp) % ITS_LMNBLK != 0)
		problem(c, "MDNAMP is %llu, so the name area is not a whole number of %d-word slots",
			(unsigned long long)namp, ITS_LMNBLK);

	/*
	 * FSDEFS ASSERTS THIS ABOUT ITSELF, at assembly time:
	 *
	 *     IF1 IFDEF NUDSL, IFG NUDSL*LMNBLK+LMIBLK-2000,.ERR MFD LOSES
	 *
	 * -- the name area plus the header must fit in one block.  A pack whose
	 * MDNUDS breaks it was written by something that was not ITS.
	 */
	if (c->nudsl == 0 || c->nudsl * ITS_LMNBLK + ITS_LMIBLK > c->wpb)
		problem(c, "MDNUDS is %llu, which does not fit in a %u-word MFD "
			   "(FSDEFS: NUDSL*LMNBLK+LMIBLK must not exceed 2000 octal)",
			(unsigned long long)c->nudsl, c->wpb);

	if (c->nudsl > c->tblks)
		return -1;

	slots = (unsigned)((c->wpb - namp) / ITS_LMNBLK);
	c->dirs = calloc(slots ? slots : 1, sizeof *c->dirs);

	if (c->dirs == NULL) {
		fprintf(stderr, "itsfs check: out of memory\n");
		return -1;
	}

	for (i = 0; i < slots; i++) {
		uint64_t a = namp + (uint64_t)i * ITS_LMNBLK;
		long long b;
		unsigned j;

		its_sixbit_name(c->mfd[a], got);

		if (got[0] == '\0')
			continue;

		/*
		 * AN EMPTY SLOT IS NOT A TERMINATOR, and this comment exists
		 * because a check was briefly added here saying it was.
		 *
		 * QFL, the monitor's directory lookup in `disk.1228`, is
		 *
		 *      QFL1:   LDB J,[1200,,Q]
		 *              JUMPE J,QFL3    ;give up
		 *              CAMN C,MNUNAM(Q)
		 *               JRST QFL2      ;found
		 *              ADDI Q,LMNBLK
		 *              JRST QFL1
		 *
		 * which reads as "stop at the first zero entry" -- and is not.
		 * `Q=10` in `its.1652`: Q is an ACCUMULATOR, so `[1200,,Q]`
		 * addresses the register, and the LDB takes the low ten bits of
		 * THE ENTRY'S ADDRESS, not of the entry.  That is why QFL2 can
		 * use J as the address; it is the same "position is the
		 * address" this file relies on further down.  The loop ends
		 * when the offset walks off the block, and an empty slot simply
		 * fails to match.
		 *
		 * The measurement said so before the source did: 105 of the
		 * reference pack's 247 directory names have zero in their low
		 * ten bits -- names shorter than six characters, NUL-padded --
		 * and the first is the eleventh entry.  A scan that stopped
		 * there would leave ITS with eleven directories.
		 */
		/*
		 * THE POSITION IS THE ADDRESS: block = (A - 2000 + 2*NUDSL)/2,
		 * which is QFL2 in disk.1228.  Done signed, because on unsigned
		 * words a negative result is an enormous block number rather
		 * than an error.
		 */
		b = (long long)a - (long long)c->wpb + (long long)ITS_LMNBLK * (long long)c->nudsl;

		if (b < 0 || b % ITS_LMNBLK != 0) {
			problem(c, "MFD slot at word %llu (|%s|) does not resolve to a block",
				(unsigned long long)a, got);
			continue;
		}

		b /= ITS_LMNBLK;

		/* Directories occupy blocks 0..NUDSL-1 and nowhere else. */
		if ((uint64_t)b >= c->nudsl) {
			problem(c, "directory |%s| resolves to block %lld, at or past NUDSL (%llu)",
				got, b, (unsigned long long)c->nudsl);
			continue;
		}

		for (j = 0; j < c->ndirs; j++)
			if (strcmp(c->dirs[j].name, got) == 0) {
				problem(c, "two MFD slots name the directory |%s| (blocks %llu and %lld)",
					got, (unsigned long long)c->dirs[j].blk, b);
				break;
			}

		snprintf(c->dirs[c->ndirs].name, sizeof c->dirs[c->ndirs].name, "%s", got);
		c->dirs[c->ndirs].blk = (uint64_t)b;
		c->ndirs++;
	}

	/* The MFD and the TUT are locked out, not allocated: they are checked
	 * against the TUT's locked-out set rather than claimed here. */
	return 0;
}

/* --------------------------------------------------- descriptors and links */

/* One six-bit byte of a directory block, counted from word UDDESC. */
static int
dbyte(const uint64_t *u, unsigned wpb, unsigned off, unsigned *out)
{
	unsigned word = ITS_UD_DESC + off / ITS_UFDBPW;

	if (word >= wpb)
		return -1;
	*out = its_byte6(u[word], off % ITS_UFDBPW);
	return 0;
}

/*
 * Decode one file's descriptor, claiming each block.  Returns the number of
 * blocks, or -1 having reported why.
 *
 * The bytecode, from FSDEFS:
 *      0                 end
 *      1..UDTKMX         take N blocks
 *      UDTKMX+1..UDWPH-1 skip N-UDTKMX, take one
 *      UDWPH             a no-op place holder
 *      040..077          load address, three bytes, take one
 */
static long
walk_desc(struct ck *c, const uint64_t *u, unsigned off, const char *who)
{
	uint64_t b = 0;
	int loaded = 0;
	long n = 0;
	unsigned steps = c->wpb * ITS_UFDBPW;

	while (steps-- > 0) {
		unsigned code, run, k;

		if (dbyte(u, c->wpb, off++, &code) != 0) {
			problem(c, "%s: descriptor ran off the end of the directory block", who);
			return -1;
		}

		if (code == 0)
			return n;

		if (code >= ITS_UD_LOADAD) {
			unsigned c2, c3;

			if (dbyte(u, c->wpb, off++, &c2) != 0 ||
			    dbyte(u, c->wpb, off++, &c3) != 0) {
				problem(c, "%s: truncated load address", who);
				return -1;
			}

			b = ((uint64_t)(code & 037) << 12) | ((uint64_t)c2 << 6) | c3;
			loaded = 1;
		}
		else if (code == ITS_UD_WPH) {
			continue;
		}
		else if (code <= ITS_UD_TKMX) {
			if (!loaded) {
				problem(c, "%s: takes %u blocks before any load address", who, code);
				return -1;
			}
		}
		else {
			if (!loaded) {
				problem(c, "%s: skips before any load address", who);
				return -1;
			}

			b += code - ITS_UD_TKMX;
		}

		run = (code <= ITS_UD_TKMX) ? code : 1;

		for (k = 0; k < run; k++, b++) {
			if (n >= CK_MAXFILEBLK) {
				problem(c, "%s: more than %d blocks", who, CK_MAXFILEBLK);
				return -1;
			}

			claim(c, b, who);
			n++;
		}
	}

	problem(c, "%s: descriptor never ended", who);
	return -1;
}

/*
 * Decode a link's target: three SIXBIT components ended by a zero byte, short
 * ones ended by ";" and ";", ":" and space quoted by ":".  Returns 0, or -1
 * having reported why.
 */
static int
walk_link(struct ck *c, const uint64_t *u, unsigned off, char tgt[3][ITS_SIXBIT_CHARS + 1],
	  const char *who)
{
	int comp;

	memset(tgt, 0, 3 * (ITS_SIXBIT_CHARS + 1));

	for (comp = 0; comp < 3; comp++) {
		int n = 0;

		while (n < ITS_SIXBIT_CHARS) {
			unsigned ch;

			if (dbyte(u, c->wpb, off++, &ch) != 0) {
				problem(c, "%s: link ran off the end of the directory block", who);
				return -1;
			}

			if (ch == 0)
				return 0; /* the zero byte ends the whole name */

			if (ch == ITS_LNK_QUOTE) {
				if (dbyte(u, c->wpb, off++, &ch) != 0) {
					problem(c, "%s: truncated quote in a link", who);
					return -1;
				}
			}
			else if (ch == ITS_LNK_SEP) {
				break;
			}

			tgt[comp][n++] = (char)(ch + 040);
		}
	}

	return 0;
}

/*
 * Does a link's target exist?
 *
 * `>` and `<` are not ordinary names.  The monitor's own lookup treats them as
 * special in either name -- disk.1228, QLOOK:
 *
 *      CAMN A,[SIXBIT />/]
 *       TLOA J,400000
 *        CAMN A,[SIXBIT /</]
 *         JRST QLOOKA     ;4.9 BIT OF J SET IF >
 *
 * and QLOOK9 below it walks the directory for the best "numeric part", so `>` is
 * the highest version and `<` the lowest.  Resolving one here to "some file with
 * that other name exists" is an approximation of that, and is marked as one: it
 * would accept a link whose exact version is gone while another remains.  For a
 * check whose output is a note rather than a problem, that is the right side to
 * err on.
 */
static int
resolve_link(struct ck *c, char tgt[3][ITS_SIXBIT_CHARS + 1])
{
	unsigned i, j;
	int wild1 = strcmp(tgt[1], ">") == 0 || strcmp(tgt[1], "<") == 0;
	int wild2 = strcmp(tgt[2], ">") == 0 || strcmp(tgt[2], "<") == 0;

	for (i = 0; i < c->ndirs; i++) {
		const uint64_t *u;
		uint64_t namp;

		if (strcmp(c->dirs[i].name, tgt[0]) != 0)
			continue;

		if (!c->dirs[i].ok)
			return 1; /* the directory is unreadable, not the link's fault */

		u = c->dirs[i].w;
		namp = u[ITS_UD_NAMP];

		for (j = (unsigned)namp; j + ITS_LUNBLK <= c->wpb; j += ITS_LUNBLK) {
			char fn1[ITS_SIXBIT_CHARS + 1], fn2[ITS_SIXBIT_CHARS + 1];

			its_sixbit_name(u[j + ITS_UN_FN1], fn1);
			its_sixbit_name(u[j + ITS_UN_FN2], fn2);

			if (fn1[0] == '\0' && fn2[0] == '\0')
				continue;

			if ((wild1 || strcmp(fn1, tgt[1]) == 0) &&
			    (wild2 || strcmp(fn2, tgt[2]) == 0))
				return 1;
		}

		return 0;
	}

	return 0;
}

/* ------------------------------------------------------------ a directory */

static void
check_ufd(struct ck *c, struct ckdir *dir)
{
	const uint64_t *u = dir->w;
	uint64_t namp = u[ITS_UD_NAMP];
	uint64_t prev1, prev2;
	int seen;
	uint64_t descp = u[ITS_UD_ESCP];
	uint64_t udblks = ITS_RH(u[ITS_UD_BLKS]);
	uint64_t descwords;
	uint64_t blocks = 0;
	char name[ITS_SIXBIT_CHARS + 1];
	unsigned j;

	its_sixbit_name(u[ITS_UD_NAME], name);

	if (strcmp(name, dir->name) != 0)
		problem(c, "block %llu was reached as |%s| but its UDNAME is |%s|",
			(unsigned long long)dir->blk, dir->name, name);

	/* namp == wpb is an EMPTY name area, which is what ITS writes for a new
	 * directory (QSKON: `MOVEI A,2000 / MOVEM A,UDNAMP-1(B)`).  Refusing it
	 * was this checker's bug, not a pack's. */
	if (namp < ITS_UD_DESC || namp > c->wpb) {
		problem(c, "%s: UDNAMP is %llu, outside the header and the block", dir->name,
			(unsigned long long)namp);
		return;
	}

	if ((c->wpb - namp) % ITS_LUNBLK != 0)
		problem(c, "%s: UDNAMP is %llu, so the name area is not a whole number of "
			   "%d-word blocks",
			dir->name, (unsigned long long)namp, ITS_LUNBLK);

	/*
	 * THE TWO AREAS MUST NOT HAVE MET.  Descriptors grow up from UDDESC and
	 * name blocks grow down from the end of the block; UDESCP is the free
	 * pointer into the first, in six-bit bytes, and UDNAMP is the start of
	 * the second.  A directory is full when they touch, and a directory
	 * where they have crossed has lost data one way or the other.
	 */
	descwords = (descp + ITS_UFDBPW - 1) / ITS_UFDBPW;

	if (ITS_UD_DESC + descwords > namp)
		problem(c, "%s: the descriptor area (UDESCP %llu, %llu words from word %d) has "
			   "overrun the name area at %llu",
			dir->name, (unsigned long long)descp, (unsigned long long)descwords,
			ITS_UD_DESC, (unsigned long long)namp);

	dir->ok = 1;

	/*
	 * THE NAME AREA IS SORTED, AND ITS DEPENDS ON IT.
	 *
	 * QLOOK does not scan a name area -- it calls QLGLK, a binary search
	 * (`disk.1228`):
	 *
	 *	ADDI J,600	;128. NAME BLOCKS FROM END
	 *	REPEAT 7,[
	 *		...
	 *		CAML A,D
	 *		ADDI J,<1_<7-.RPCNT>>*LUNBLK
	 *		SUBI J,<1_<6-.RPCNT>>*LUNBLK
	 *	]
	 *
	 * Seven halving steps over 128 name blocks, so an out-of-order name area
	 * makes files UNFINDABLE by the monitor while every block stays
	 * accounted for.  NSALV does not check it: given a pack with two entries
	 * swapped it walks the whole thing and returns to DDT without a word.
	 * That combination -- the pack broken and the second opinion silent -- is
	 * what makes this worth checking here.
	 *
	 * STRICTER THAN THE MONITOR REQUIRES.  QLGLK searches on FN1 alone;
	 * QLOOK then walks backwards through the run of equal FN1s comparing
	 * both (`SUBI Q,LUNBLK / CAML Q,J`, "SEARCH THROUGH * FILES"), so
	 * entries sharing an FN1 could be in any FN2 order and still be found.
	 * On the reference pack 1,913 adjacent pairs share an FN1 and none has
	 * its FN2 out of order -- QRELOC sorts on the whole name.  This tests
	 * what ITS writes rather than the minimum it can read, which catches a
	 * broken writer; a pack failing only on FN2 order would still work.
	 */
	prev1 = 0;
	prev2 = 0;
	seen = 0;

	for (j = (unsigned)namp; j + ITS_LUNBLK <= c->wpb; j += ITS_LUNBLK) {
		char fn1[ITS_SIXBIT_CHARS + 1], fn2[ITS_SIXBIT_CHARS + 1];
		char who[64];
		uint64_t rndm = u[j + ITS_UN_RNDM];
		unsigned dscp = (unsigned)ITS_FIELD(rndm, ITS_UN_DSCP_P, ITS_UN_DSCP_S);
		int is_link = (int)ITS_FIELD(rndm, ITS_UN_LNK_P, ITS_UN_LNK_S);
		long n;
		uint64_t k1 = u[j + ITS_UN_FN1], k2 = u[j + ITS_UN_FN2];

		its_sixbit_name(u[j + ITS_UN_FN1], fn1);
		its_sixbit_name(u[j + ITS_UN_FN2], fn2);

		if (fn1[0] == '\0' && fn2[0] == '\0') {
			c->nzero++;
			continue;
		}

		if (seen && (k1 < prev1 || (k1 == prev1 && k2 < prev2))) {
			char was[ITS_SIXBIT_CHARS + 1], was2[ITS_SIXBIT_CHARS + 1];

			its_sixbit_name(prev1, was);
			its_sixbit_name(prev2, was2);
			problem(c,
				"%s;%s %s follows |%s %s| in the name area, out of order "
				"-- QLOOK binary-searches it, so this file is unfindable",
				dir->name, fn1, fn2, was, was2);
		}

		prev1 = k1;
		prev2 = k2;
		seen = 1;

		snprintf(who, sizeof who, "%s;%s %s", dir->name, fn1, fn2);

		/*
		 * UDESCP bounds every descriptor in the directory: it is where
		 * the next one would go, so a pointer at or past it points into
		 * space nothing has written.  On a real pack all 5,657 files
		 * satisfy this.
		 */
		if (dscp >= descp)
			problem(c, "%s: UNDSCP is %u, at or past the free pointer UDESCP (%llu)",
				who, dscp, (unsigned long long)descp);

		if (is_link) {
			char tgt[3][ITS_SIXBIT_CHARS + 1];

			c->nlinks++;

			if (walk_link(c, u, dscp, tgt, who) != 0)
				continue;

			if (!resolve_link(c, tgt)) {
				c->ndangling++;
				note(c, "%s is a link to %s;%s %s, which is not there", who, tgt[0],
				     tgt[1], tgt[2]);
			}

			continue;
		}

		c->nfiles++;
		n = walk_desc(c, u, dscp, keep(c, who));

		if (n >= 0)
			blocks += (uint64_t)n;
	}

	/*
	 * UDBLKS IS A SECOND OPINION FROM SOMEBODY ELSE.  The monitor maintains
	 * it in the header as files are written; nothing here computes it and
	 * nothing here can influence it.  It is the check that catches a
	 * descriptor decoder that is wrong in a way that is otherwise silent --
	 * which is exactly how this project's own reader was found to be reading
	 * UNDSCP from the wrong origin, decoding every file to zero blocks
	 * without a single error message.
	 */
	if (blocks != udblks)
		problem(c, "%s: the descriptors name %llu blocks, but UDBLKS says %llu", dir->name,
			(unsigned long long)blocks, (unsigned long long)udblks);
}

/* ---------------------------------------------------------------- the TUT */

/*
 * The entry for the `rel`th block the TUT maps: three bits, twelve to a word,
 * most significant first -- the same left-to-right order every PDP-10 byte
 * pointer walks in.  The map begins at word LTIBLK.
 *
 * The caller has already bounded `rel` against what the table can hold; that
 * check is in check_tut and is done once rather than here, where it would be
 * done a hundred thousand times.
 */
static unsigned
tut_at(const uint64_t *t, uint64_t rel)
{
	uint64_t word = ITS_LTIBLK + rel / ITS_TUTEPW;

	return (unsigned)((t[word] >> (33 - ITS_TUTBYT * (rel % ITS_TUTEPW))) & (ITS_TUTMAX - 1));
}

static int
check_tut(struct ck *c)
{
	uint64_t *t;
	size_t nwords = (size_t)c->wpb * c->d->ntutbl;
	uint64_t first, last, b;
	uint64_t nfree = 0, used = 0, locked = 0;
	uint64_t lost = 0, unmarked = 0, miscount = 0, onlocked = 0;
	unsigned i;
	int rc = -1;

	t = calloc(nwords, sizeof *t);

	if (t == NULL) {
		fprintf(stderr, "itsfs check: out of memory\n");
		return -1;
	}

	for (i = 0; i < c->d->ntutbl; i++)
		if (ck_read(c, c->tutblk + i, t + (size_t)i * c->wpb) != 0) {
			fprintf(stderr, "itsfs check: cannot read the TUT (block %llu)\n",
				(unsigned long long)c->tutblk + i);
			goto out;
		}

	first = t[ITS_Q_FRSTB];
	last = t[ITS_Q_LASTB];

	if (last < first || last - first > (nwords - ITS_LTIBLK) * ITS_TUTEPW) {
		problem(c, "the TUT says it maps blocks %llu..%llu, which does not fit in %zu words",
			(unsigned long long)first, (unsigned long long)last, nwords);
		goto out;
	}

	if (t[ITS_Q_SWAPA] > last)
		problem(c, "QSWAPA is block %llu, past the end of what the TUT maps (%llu)",
			(unsigned long long)t[ITS_Q_SWAPA], (unsigned long long)last);

	if (t[ITS_Q_TUTP] > last)
		problem(c, "QTUTP is block %llu, past the end of what the TUT maps (%llu)",
			(unsigned long long)t[ITS_Q_TUTP], (unsigned long long)last);

	for (b = 0; b < c->tblks; b++) {
		unsigned stored, want;

		if (b < first || b >= last) {
			if (c->ref[b] != 0) {
				problem(c, "block %llu is claimed by %s, and the TUT does not map it "
					   "(it maps %llu..%llu)",
					(unsigned long long)b, c->owner[b],
					(unsigned long long)first, (unsigned long long)last);
			}

			continue;
		}

		stored = tut_at(t, b - first);

		/* The stored count saturates at TUTMNY, "many or more". */
		want = c->ref[b] > ITS_TUTMNY ? ITS_TUTMNY : c->ref[b];

		if (stored == ITS_TUTLK) {
			locked++;

			if (c->ref[b] != 0) {
				onlocked++;
				problem(c, "block %llu is claimed by %s, and the TUT has it LOCKED OUT",
					(unsigned long long)b, c->owner[b]);
			}

			continue;
		}

		if (stored == 0)
			nfree++;
		else
			used++;

		if (stored == 0 && want != 0) {
			/* THE DANGEROUS ONE: the allocator would hand this block
			 * out again, and the file that has it would be overwritten. */
			unmarked++;
			problem(c, "block %llu is claimed by %s, and the TUT calls it FREE",
				(unsigned long long)b, c->owner[b]);
		}
		else if (stored != 0 && want == 0) {
			/* Space nothing can reach: wasted, but nothing is at risk. */
			lost++;
			problem(c, "block %llu is marked in use (%u references) and no file claims it",
				(unsigned long long)b, stored);
		}
		else if (stored != want) {
			miscount++;
			problem(c, "block %llu: the TUT says %u references, the files make %u",
				(unsigned long long)b, stored, want);
		}
	}

	/*
	 * AND THE LOCKED-OUT SET, which is the only check there is on the
	 * MFD-slot arithmetic: there is no pointer from an MFD entry to its
	 * directory, so the one way to know the formula is right is that the
	 * blocks it calls directories are exactly the blocks the allocator
	 * refuses to touch.
	 */
	{
		uint64_t missing = 0, extra = 0;

		for (b = first; b < last; b++) {
			unsigned stored = tut_at(t, b - first);
			int expect = b < c->nudsl || b == c->mfdblk ||
				     (b >= c->tutblk && b < c->tutblk + c->d->ntutbl);

			if (expect && stored != ITS_TUTLK) {
				missing++;
				problem(c, "block %llu is %s and the TUT does not lock it out",
					(unsigned long long)b,
					b < c->nudsl ? "a directory"
						     : (b == c->mfdblk ? "the MFD" : "a TUT block"));
			}
			else if (!expect && stored == ITS_TUTLK) {
				extra++;
				problem(c, "block %llu is locked out and is not a directory, "
					   "the MFD or the TUT",
					(unsigned long long)b);
			}
		}

		if (missing == 0 && extra == 0 && c->verbose)
			printf("  locked out: %llu blocks == %llu directories + 1 MFD + %u TUT blocks\n",
			       (unsigned long long)locked, (unsigned long long)c->nudsl,
			       c->d->ntutbl);
	}

	{
		char id[ITS_SIXBIT_CHARS + 1];

		its_sixbit_name(t[ITS_Q_PAKID], id);
		printf("\n");
		printf("%-14s %s (number %llu)\n", "pack", id[0] ? id : "(unnamed)",
		       (unsigned long long)t[ITS_Q_PKNUM]);
	}

	printf("%-14s %llu..%llu\n", "TUT maps", (unsigned long long)first, (unsigned long long)last);
	printf("%-14s %llu free, %llu in use, %llu locked out\n", "blocks",
	       (unsigned long long)nfree, (unsigned long long)used, (unsigned long long)locked);
	printf("%-14s %llu blocks, in %lu files\n", "claimed", (unsigned long long)c->nclaimed,
	       c->nfiles);

	if (lost || unmarked || miscount || onlocked)
		printf("%-14s %llu free but claimed, %llu in use but unclaimed, %llu miscounted, "
		       "%llu on locked-out blocks\n",
		       "disagreements", (unsigned long long)unmarked, (unsigned long long)lost,
		       (unsigned long long)miscount, (unsigned long long)onlocked);

	rc = 0;
out:
	free(t);
	return rc;
}

/* ------------------------------------------------------------------- main */

static int
usage(void)
{
	fprintf(stderr, "usage: itsfs check [-p packing] [-d drive] [-v] image\n"
			"       -v   also list the notes: links that do not resolve, and\n"
			"            the checks that passed quietly\n");
	return 2;
}

int
cmd_check(int argc, char **argv)
{
	const its_pack *pk = its_pack_for(ITS_PACK_LE64);
	const its_drive *drv = NULL;
	its_image im;
	struct ck c;
	int opt, rc = 2;
	unsigned i;

	memset(&c, 0, sizeof c);

	while ((opt = getopt(argc, argv, "p:d:v")) != -1) {
		switch (opt) {
		case 'p':
			if ((pk = opt_pack(optarg)) == NULL)
				return 2;
			break;
		case 'd':
			if ((drv = opt_drive(optarg)) == NULL)
				return 2;
			break;
		case 'v':
			c.verbose = 1;
			break;
		default:
			return usage();
		}
	}

	if (optind != argc - 1)
		return usage();

	if (img_open(&im, argv[optind], pk, drv, 0) != 0)
		return 2;

	if (im.drv == NULL) {
		fprintf(stderr, "itsfs check: %s: no drive geometry -- name one with -d\n", im.path);
		img_close(&im);
		return 2;
	}

	/* The geometry, re-derived.  See the header comment. */
	c.im = &im;
	c.d = im.drv;
	c.nblksc = c.d->nheds * c.d->nsecs / c.d->secblk;
	c.wpb = c.d->secblk * ITS_WORDS_PER_SECTOR;
	c.nblks = (uint64_t)c.d->ncyls * c.nblksc;
	c.tblks = (uint64_t)(c.d->ncyls + c.d->xcyls) * c.nblksc;
	c.mfdblk = c.nblks / 2 - 1;
	c.tutblk = c.mfdblk - c.d->ntutbl;

	c.mfd = calloc(c.wpb, sizeof *c.mfd);
	c.ref = calloc((size_t)c.tblks, sizeof *c.ref);
	c.owner = calloc((size_t)c.tblks, sizeof *c.owner);

	if (c.mfd == NULL || c.ref == NULL || c.owner == NULL) {
		fprintf(stderr, "itsfs check: out of memory\n");
		goto out;
	}

	printf("checking %s: %s, %llu blocks, MFD at %llu, TUT at %llu\n", im.path, c.d->name,
	       (unsigned long long)c.tblks, (unsigned long long)c.mfdblk,
	       (unsigned long long)c.tutblk);

	if (check_mfd(&c) != 0) {
		printf("\nthe MFD did not check; nothing below it could be checked\n");
		rc = 1;
		goto out;
	}

	/*
	 * Read every directory block once and keep it.  NUDSL is bounded by the
	 * MFD check above at (2000-LMIBLK)/LMNBLK, so this is at most about 500
	 * blocks -- four megabytes -- and having them all in memory is what
	 * makes resolving a link cheap enough to do for every link.
	 */
	for (i = 0; i < c.ndirs; i++) {
		c.dirs[i].w = calloc(c.wpb, sizeof *c.dirs[i].w);

		if (c.dirs[i].w == NULL) {
			fprintf(stderr, "itsfs check: out of memory\n");
			goto out;
		}

		if (ck_read(&c, c.dirs[i].blk, c.dirs[i].w) != 0) {
			problem(&c, "cannot read the directory |%s| in block %llu", c.dirs[i].name,
				(unsigned long long)c.dirs[i].blk);
			free(c.dirs[i].w);
			c.dirs[i].w = NULL;
		}
	}

	/* Headers first, so that link resolution can trust every directory it
	 * looks in -- resolve_link reads other directories' name areas. */
	for (i = 0; i < c.ndirs; i++)
		if (c.dirs[i].w != NULL) {
			uint64_t namp = c.dirs[i].w[ITS_UD_NAMP];

			c.dirs[i].ok = namp >= ITS_UD_DESC && namp <= c.wpb;
		}

	for (i = 0; i < c.ndirs; i++) {
		if (c.dirs[i].w == NULL)
			continue;

		if (c.verbose)
			printf("  %s (block %llu)\n", c.dirs[i].name,
			       (unsigned long long)c.dirs[i].blk);
		check_ufd(&c, &c.dirs[i]);
	}

	if (check_tut(&c) != 0) {
		rc = 1;
		goto out;
	}

	printf("%-14s %u, %lu files, %lu links (%lu of them unresolved)\n", "directories", c.ndirs,
	       c.nfiles, c.nlinks, c.ndangling);
	printf("\n");

	if (c.problems == 0)
		printf("no problems found (%lu notes)\n", c.notes);
	else
		printf("%lu problems, %lu notes\n", c.problems, c.notes);

	rc = c.problems ? 1 : 0;
out:
	for (i = 0; i < c.ndirs; i++)
		free(c.dirs[i].w);
	free(c.dirs);

	for (size_t k = 0; k < c.nnames; k++)
		free(c.names[k]);
	free(c.names);
	free(c.owner);
	free(c.ref);
	free(c.mfd);
	img_close(&im);
	return rc;
}
