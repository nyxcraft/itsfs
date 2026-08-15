/*
 * itstext.c -- SIXBIT, seven-bit ASCII, and six-bit bytes.  See itstext.h.
 */

#include "itstext.h"
#include "itspack.h"

#include <string.h>

unsigned
its_byte6(uint64_t w, unsigned i)
{
	/* Byte 0 is the most significant: DEC numbers bits from the left, and a
	 * byte pointer walking a word starts at bit 0 and moves right. */
	return (unsigned)((w >> (30 - 6 * i)) & 077u);
}

void
its_sixbit_word(uint64_t w, char out[ITS_SIXBIT_CHARS + 1])
{
	for (unsigned i = 0; i < ITS_SIXBIT_CHARS; i++)
		out[i] = (char)(its_byte6(w, i) + 040);
	out[ITS_SIXBIT_CHARS] = '\0';
}

void
its_sixbit_name(uint64_t w, char out[ITS_SIXBIT_CHARS + 1])
{
	size_t n;

	its_sixbit_word(w, out);
	n = strlen(out);

	while (n > 0 && out[n - 1] == ' ')
		out[--n] = '\0';
}

int
its_sixbit_make(const char *s, uint64_t *w)
{
	uint64_t v = 0;
	unsigned i = 0;

	for (; s[i] != '\0'; i++) {
		unsigned char c = (unsigned char)s[i];

		if (i >= ITS_SIXBIT_CHARS)
			return -1;

		/* 040 (space) through 137 (underscore).  Everything outside is
		 * not a SIXBIT character, and that includes every lower-case
		 * letter -- see the header for why they are refused rather than
		 * folded. */
		if (c < 040 || c > 0137)
			return -1;
		v = (v << 6) | (uint64_t)(c - 040);
	}

	for (; i < ITS_SIXBIT_CHARS; i++)
		v = (v << 6) | 0; /* space pads */

	*w = v & ITS_WORD_MASK;
	return 0;
}

void
its_ascii_word(uint64_t w, char out[ITS_ASCII_CHARS + 1])
{
	for (unsigned i = 0; i < ITS_ASCII_CHARS; i++)
		out[i] = (char)((w >> (29 - 7 * i)) & 0177u);
	out[ITS_ASCII_CHARS] = '\0';
}

void
its_ascii_printable(uint64_t w, char out[ITS_ASCII_CHARS + 1])
{
	its_ascii_word(w, out);

	for (unsigned i = 0; i < ITS_ASCII_CHARS; i++)
		if ((unsigned char)out[i] < 040 || (unsigned char)out[i] > 0176)
			out[i] = '.';
}
