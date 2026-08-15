/*
 * util.c -- see util.h.  Everything here refuses rather than guesses.
 */

#include "util.h"
#include "itstext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

const its_pack *
opt_pack(const char *name)
{
	its_packing p = its_pack_parse(name);

	if (p == ITS_NPACK) {
		fprintf(stderr, "itsfs: unknown packing '%s'.  Known:", name);

		for (int i = 0; i < ITS_NPACK; i++)
			fprintf(stderr, " %s", its_pack_name((its_packing)i));
		fprintf(stderr, "\n");
		return NULL;
	}

	return its_pack_for(p);
}

const its_drive *
opt_drive(const char *name)
{
	const its_drive *d = its_drive_by_name(name);

	if (d == NULL) {
		fprintf(stderr, "itsfs: unknown drive '%s'.  Known:", name);

		for (unsigned i = 0; i < ITS_NDRIVE; i++)
			fprintf(stderr, " %s", its_drive_at(i)->name);
		fprintf(stderr, "\n");
		return NULL;
	}

	return d;
}

int
parse_u64_base(const char *s, int base, uint64_t *out)
{
	char *end;
	unsigned long long v;

	if (s == NULL || *s == '\0' || isspace((unsigned char)*s) || *s == '-' || *s == '+')
		return -1;

	errno = 0;
	v = strtoull(s, &end, base);

	if (errno != 0 || *end != '\0')
		return -1;

	*out = (uint64_t)v;
	return 0;
}

/* The shared body of parse_word and parse_block: the same syntax, a different
 * default base.  One parser, so the two cannot drift. */
static int
parse_number(const char *s, int defbase, uint64_t *out)
{
	if (s == NULL || *s == '\0')
		return -1;

	if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0)
		return parse_u64_base(s + 2, 16, out);

	if (strncmp(s, "0o", 2) == 0 || strncmp(s, "0O", 2) == 0)
		return parse_u64_base(s + 2, 8, out);

	if (strncmp(s, "d:", 2) == 0)
		return parse_u64_base(s + 2, 10, out);

	if (s[0] == '0' && s[1] != '\0')
		return parse_u64_base(s + 1, 8, out);

	return parse_u64_base(s, defbase, out);
}

int
parse_word(const char *s, uint64_t *out)
{
	const char *comma;
	uint64_t v;

	if (s == NULL || *s == '\0') {
		fprintf(stderr, "itsfs: empty value\n");
		return -1;
	}

	if (strncmp(s, "sixbit:", 7) == 0) {
		if (its_sixbit_make(s + 7, out) != 0) {
			fprintf(stderr, "itsfs: '%s' is not SIXBIT: at most six characters, "
					"and each one 040..137 (no lower case)\n",
				s + 7);
			return -1;
		}

		return 0;
	}

	/* lh,,rh -- the halfword notation, both halves octal. */
	comma = strstr(s, ",,");

	if (comma != NULL) {
		char lh[32];
		uint64_t l = 0, r = 0;
		size_t n = (size_t)(comma - s);

		if (n >= sizeof lh) {
			fprintf(stderr, "itsfs: '%s': left half too long\n", s);
			return -1;
		}

		memcpy(lh, s, n);
		lh[n] = '\0';

		/* An empty half is zero: `,,17` and `17,,` are both ITS's own
		 * usage, and refusing them would be pedantry. */
		if ((n > 0 && parse_u64_base(lh, 8, &l) != 0) ||
		    (comma[2] != '\0' && parse_u64_base(comma + 2, 8, &r) != 0)) {
			fprintf(stderr, "itsfs: '%s': halves must be octal\n", s);
			return -1;
		}

		if (l > 0777777 || r > 0777777) {
			fprintf(stderr, "itsfs: '%s': a half is 18 bits\n", s);
			return -1;
		}

		*out = (l << 18) | r;
		return 0;
	}

	if (parse_number(s, 8, &v) != 0) {
		fprintf(stderr, "itsfs: '%s' is not a number (octal by default; "
				"0x hex, d: decimal, lh,,rh, sixbit:NAME)\n",
			s);
		return -1;
	}

	if (v > ITS_WORD_MASK) {
		fprintf(stderr, "itsfs: %s does not fit in 36 bits\n", s);
		return -1;
	}

	*out = v;
	return 0;
}

int
parse_block(const char *s, uint64_t *out)
{
	if (parse_number(s, 10, out) != 0) {
		fprintf(stderr, "itsfs: '%s' is not a block number (decimal; "
				"0 or 0o for octal, 0x for hex)\n",
			s);
		return -1;
	}

	return 0;
}

int
parse_count(const char *s, uint64_t *out)
{
	if (parse_u64_base(s, 10, out) != 0) {
		fprintf(stderr, "itsfs: '%s' is not a count\n", s);
		return -1;
	}

	return 0;
}

int
parse_blkrange(const char *s, uint64_t *first, uint64_t *last)
{
	const char *sep = strstr(s, "..");
	char head[64];
	size_t n;

	if (sep == NULL) {
		sep = strchr(s, '-');

		if (sep == NULL) {
			if (parse_block(s, first) != 0)
				return -1;
			*last = *first;
			return 0;
		}
	}

	n = (size_t)(sep - s);

	if (n == 0 || n >= sizeof head) {
		fprintf(stderr, "itsfs: '%s' is not a block range\n", s);
		return -1;
	}

	memcpy(head, s, n);
	head[n] = '\0';

	if (parse_block(head, first) != 0)
		return -1;

	if (parse_block(sep + (sep[0] == '.' ? 2 : 1), last) != 0)
		return -1;

	if (*last < *first) {
		fprintf(stderr, "itsfs: '%s': the range runs backwards\n", s);
		return -1;
	}

	return 0;
}
