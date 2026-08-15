/*
 * cmd_word.c -- `itsfs sixbit`, the character encodings with no image involved.
 *
 * Small, and it earns its place twice: it is how a name is turned into the word
 * to search an image for, and it is the only command the test suite can check
 * against a value measured off a real pack without needing the pack.
 */

#define _POSIX_C_SOURCE 200809L

#include "cmds.h"
#include "util.h"
#include "itstext.h"
#include "itspack.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void
sixbit_usage(void)
{
	fprintf(stderr, "usage: itsfs sixbit [-d] value...\n"
			"       encode each value as SIXBIT, or with -d decode each word\n"
			"\n"
			"       itsfs sixbit M.F.D.        -> 551646164416\n"
			"       itsfs sixbit -d 551646164416  -> |M.F.D.|\n");
}

int
cmd_sixbit(int argc, char **argv)
{
	int c, decode = 0;

	while ((c = getopt(argc, argv, "d")) != -1) {
		switch (c) {
		case 'd':
			decode = 1;
			break;
		default:
			sixbit_usage();
			return 2;
		}
	}

	if (optind >= argc) {
		sixbit_usage();
		return 2;
	}

	for (int i = optind; i < argc; i++) {
		if (decode) {
			uint64_t w;
			char sb[ITS_SIXBIT_CHARS + 1], as[ITS_ASCII_CHARS + 1];

			if (parse_word(argv[i], &w) != 0)
				return 1;

			its_sixbit_word(w, sb);
			its_ascii_printable(w, as);
			printf("%012llo  %06llo,,%06llo  sixbit |%s|  ascii |%s|\n",
			       (unsigned long long)w, (unsigned long long)ITS_LH(w),
			       (unsigned long long)ITS_RH(w), sb, as);
		}
		else {
			uint64_t w;

			if (its_sixbit_make(argv[i], &w) != 0) {
				fprintf(stderr, "itsfs: '%s' is not SIXBIT: at most six characters, "
						"each one 040..137 (there is no lower case)\n",
					argv[i]);
				return 1;
			}

			printf("%012llo  %06llo,,%06llo\n", (unsigned long long)w,
			       (unsigned long long)ITS_LH(w), (unsigned long long)ITS_RH(w));
		}
	}

	return 0;
}
