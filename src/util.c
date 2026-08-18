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

/*
 * `DIR;FN1 FN2` in one argument, or as three.  One copy: cmd_fs.c and
 * cmd_write.c each had their own, byte-for-byte the same but for the argument
 * convention, and they had already begun to drift apart.
 */
int
its_parse_path(char **argv, int i, int navail, its_path *p)
{
	memset(p, 0, sizeof *p);

	if (navail == 3) {
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
			fprintf(stderr, "itsfs: '%s' is not a file name: it wants DIR;FN1 FN2\n",
				s);
			return -1;
		}

		n = (size_t)(semi - s);

		if (n >= ITS_NAME_MAX)
			goto toolong;

		memcpy(p->dir, s, n);
		p->dir[n] = '\0';

		s = semi + 1;
		sp = strchr(s, ' ');

		/* No space: FN2 is empty, which is a legal ITS name. */
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

int
its_write_words(FILE *out, const uint64_t *w, size_t n, const char *what)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char b[8];

		for (int k = 0; k < 8; k++)
			b[k] = (unsigned char)(w[i] >> (8 * k));

		if (fwrite(b, 1, 8, out) != 8) {
			perror(what);
			return -1;
		}
	}

	return 0;
}

int
its_find_dir(const its_mfd *m, const char *name, uint64_t *blk)
{
	char have[ITS_NAME_MAX];

	for (unsigned i = 0; i < its_mfd_slots(m); i++) {
		uint64_t b;

		if (its_mfd_dir(m, i, have, &b) != 0)
			continue;

		if (have[0] != '\0' && strcmp(have, name) == 0) {
			*blk = b;
			return 0;
		}
	}

	fprintf(stderr, "itsfs: no directory named '%s' in the MFD\n", name);
	return -1;
}

/*
 * One ITS name component to one path component.  See the note at the top for
 * what is encoded and why; -1 if it does not fit.
 */
int
its_enc_component(char *out, size_t sz, const char *s)
{
	size_t o = 0;

	if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
		int n = snprintf(out, sz, strcmp(s, ".") == 0 ? "%%2E" : "%%2E%%2E");

		return (n > 0 && (size_t)n < sz) ? 0 : -1;
	}

	for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
		const char *esc = NULL;

		switch (*p) {
		case '%':
			esc = "%25";
			break;
		case '/':
			esc = "%2F";
			break;
		case ' ':
			esc = "%20";
			break;
		default:
			break;
		}

		if (esc != NULL) {
			if (o + 3 >= sz)
				return -1;

			memcpy(out + o, esc, 3);
			o += 3;
		}
		else {
			if (o + 1 >= sz)
				return -1;

			out[o++] = (char)*p;
		}
	}

	out[o] = '\0';
	return 0;
}

/* And back.  -1 on a percent escape that is not two hex digits. */
int
its_dec_component(char *out, size_t sz, const char *s)
{
	size_t o = 0;

	for (const char *p = s; *p != '\0'; p++) {
		unsigned v;

		if (*p == '%') {
			char h[3];

			if (p[1] == '\0' || p[2] == '\0')
				return -1;

			h[0] = p[1];
			h[1] = p[2];
			h[2] = '\0';

			if (sscanf(h, "%2x", &v) != 1)
				return -1;

			p += 2;
		}
		else {
			v = (unsigned char)*p;
		}

		if (o + 1 >= sz)
			return -1;

		out[o++] = (char)v;
	}

	out[o] = '\0';
	return 0;
}
