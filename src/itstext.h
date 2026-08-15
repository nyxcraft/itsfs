/*
 * itstext.h -- what the bits in a word mean when they are characters.
 *
 * Three encodings, and ITS uses all three at once:
 *
 *   SIXBIT   six characters per word, six bits each, the character's ASCII code
 *            less 040.  File and directory names are SIXBIT, and so are the
 *            check words ITS writes into the MFD and the TUT.
 *   ASCII    five seven-bit characters per word, bit 35 left over.  Text files.
 *   6-bit    the UFD's own byte stream: not characters at all, but the same
 *            six-bit division of the word.  See its.h -- a UFD descriptor is
 *            read as six-bit bytes and every byte is a block-list opcode.
 *
 * This is deliberately NOT in itspack.c.  A packing says how a word is stored;
 * these say what a word means.  Keeping them apart is what lets `repack` rewrite
 * every byte of an image and no word of it.
 *
 * REFUSE WHAT THE FORMAT CANNOT REPRESENT.  Encoding never truncates and never
 * substitutes: a name with a lower-case letter in it is not a SIXBIT name, and
 * silently making it one produces two directory entries that look identical and
 * name different files.  s5fs shipped that bug; it is not being reproduced here.
 */
#ifndef ITSTEXT_H
#define ITSTEXT_H

#include <stdint.h>
#include <stddef.h>

#define ITS_SIXBIT_CHARS 6
#define ITS_ASCII_CHARS 5

/*
 * SIXBIT out of a word into `out[7]`, always six characters and a NUL.  Trailing
 * spaces are kept -- they are part of what is on the disk, and the caller is the
 * one that knows whether it wants them trimmed.
 */
void its_sixbit_word(uint64_t w, char out[ITS_SIXBIT_CHARS + 1]);

/* ...and with trailing spaces removed, which is what a name is. */
void its_sixbit_name(uint64_t w, char out[ITS_SIXBIT_CHARS + 1]);

/*
 * SIXBIT into a word, space padded.  Returns 0, or -1 if `s` is longer than six
 * characters or holds anything outside 040..137 -- lower case included, which is
 * the trap: SIXBIT has no lower case, and mapping it up silently is how a
 * round trip stops being one.
 */
int its_sixbit_make(const char *s, uint64_t *w);

/* Five 7-bit characters out of a word into `out[6]`.  Bit 35 is not a character
 * and is ignored here; see its.h for what ITS does with it. */
void its_ascii_word(uint64_t w, char out[ITS_ASCII_CHARS + 1]);

/* The i'th six-bit byte of a word, 0 <= i < 6.  This is the UFD descriptor
 * reader's primitive, and it is here so there is exactly one of it. */
unsigned its_byte6(uint64_t w, unsigned i);

/* A printable rendering of one word's five ASCII characters, with the
 * unprintable ones shown as `.` -- for dump output, never for extraction. */
void its_ascii_printable(uint64_t w, char out[ITS_ASCII_CHARS + 1]);

#endif /* ITSTEXT_H */
