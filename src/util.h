/*
 * util.h -- argument parsing shared by the subcommands.
 *
 * These live in one place because every command takes -p and -d and they have
 * to mean the same thing everywhere, and because a value parser that silently
 * accepts garbage is how `chmod rwx` becomes `chmod 0`.  Every function here
 * refuses what it cannot parse rather than returning a plausible default.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "itspack.h"
#include "itsgeom.h"
#include "structure.h"

/* -p: packing by name.  Prints the accepted names and returns NULL if unknown. */
const its_pack *opt_pack(const char *name);

/* -d: drive by name.  Prints the accepted names and returns NULL if unknown. */
const its_drive *opt_drive(const char *name);

/*
 * A 36-bit word, written the way the machine's own documentation writes it:
 *
 *   777777777777    octal, the default -- this is a PDP-10
 *   0x3FFFFFFFF     hex
 *   d:4294967295    decimal, tagged because a bare 12 would be ambiguous
 *   sixbit:M.F.D.   SIXBIT, space padded, refused if it cannot be represented
 *   lh,,rh          the two 18-bit halves, each octal
 *
 * Returns 0, or -1 with a message on stderr.  A value that does not fit in 36
 * bits is an error, not a truncation.
 */
int parse_word(const char *s, uint64_t *out);

/*
 * A block number, and this one is DECIMAL by default -- deliberately unlike
 * parse_word.
 *
 * ITS writes block numbers in decimal in its own documentation and in the
 * arithmetic in FSDEFS (`MFDBLK==NBLKS/2-1` with NBLKS==38164.), while it writes
 * WORD CONTENTS in octal.  Following each convention where it belongs is less
 * surprising than picking one base and being wrong half the time; `0o` or a
 * leading `0` still asks for octal explicitly.
 */
int parse_block(const char *s, uint64_t *out);

/* A plain non-negative decimal count. */
int parse_count(const char *s, uint64_t *out);

/*
 * The same strictness in a given base, for parsing files rather than arguments:
 * the whole string must be consumed, a sign or leading space is refused, and
 * overflow is an error rather than ULLONG_MAX.  Silent on failure -- the caller
 * knows the file and line number and this does not.
 */
int parse_u64_base(const char *s, int base, uint64_t *out);

/*
 * A block selector: "7", "7-9" or "7..9".  *last is inclusive.  Refuses a
 * reversed range rather than quietly printing nothing.
 */
int parse_blkrange(const char *s, uint64_t *first, uint64_t *last);

/*
 * `DIR;FN1 FN2`, given either as one argument or as three.
 *
 * A name component that SIXBIT cannot hold is REFUSED, not truncated: a
 * seven-character name cut to six matches a different file, and matching the
 * wrong file quietly is worse than not matching at all.
 *
 * `navail` is how many of argv[i..] are name arguments -- 1 or 3.
 */
typedef struct {
	char dir[ITS_NAME_MAX];
	char fn1[ITS_NAME_MAX];
	char fn2[ITS_NAME_MAX];
} its_path;

int its_parse_path(char **argv, int i, int navail, its_path *p);

/*
 * Words to a host file, one per 8-byte little-endian container.
 *
 * This IS the `-w` file format -- what `get -w` writes, `put -w` reads, and
 * both extractors produce -- so it lives in one place.  `what` names the file
 * in the error message.  Returns 0, or -1 with perror already called.
 */
int its_write_words(FILE *out, const uint64_t *w, size_t n, const char *what);

#endif /* UTIL_H */
