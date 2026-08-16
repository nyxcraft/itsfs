/*
 * itsfs -- a single multi-tool for the ITS file system.
 *
 * Git-style dispatch: `itsfs <command> [args...]`.  This file is the only
 * main() and does nothing but route.
 *
 * Three layers, and the split is the design.  At the bottom: words, and the
 * packings that store them.  Above that, and unlike either of this project's
 * siblings: GEOMETRY, because an ITS block number is not a linear offset --
 * blocks are numbered within a cylinder and four sectors per cylinder are
 * unreachable.  Above that: the structure -- MFD, UFD, TUT, descriptors -- read
 * only, with every field offset transcribed from SYSTEM;FSDEFS 43 and cited in
 * its.h.  There is no writer.
 *
 *   itsfs info      what an image is: packing, drive, blocks, and the MFD
 *   itsfs dump      print blocks as 36-bit words
 *   itsfs packings  the word packings and how well each is established
 *   itsfs drives    the drives, and the geometry each implies
 *   itsfs repack    rewrite an image word for word in another packing
 *   itsfs sixbit    encode and decode SIXBIT, with no image involved
 *   itsfs dirs      list the directories in the MFD
 *   itsfs ls        list a directory
 *   itsfs cat       print a file
 *   itsfs get       copy a file out to the host
 *   itsfs free      what the TUT says about the pack
 *   itsfs check     check a pack -- shares no code with the reader
 *   itsfs manifest  fingerprint a pack: one line per file
 *   itsfs verify    diff a pack against a manifest
 *   itsfs shell     interactive explorer
 *   itsfs put       write a host file into a directory -- DESTRUCTIVE
 *   itsfs del       remove a file -- DESTRUCTIVE
 */

#include "cmds.h"

#include <stdio.h>
#include <string.h>

/* clang-format off */		/* the column layout IS the usage message */
static const struct subcmd {
	const char *name;
	int       (*fn)(int, char **);
	const char *help;
} subcmds[] = {
	{ "info",     cmd_info,     "describe an image: packing, drive, blocks" },
	{ "dump",     cmd_dump,     "print blocks as 36-bit words"              },
	{ "packings", cmd_packings, "list the word packings"                    },
	{ "drives",   cmd_drives,   "list the drives and their geometry"        },
	{ "repack",   cmd_repack,   "rewrite an image in another word packing"  },
	{ "sixbit",   cmd_sixbit,   "encode/decode SIXBIT"                      },
	{ "dirs",     cmd_dirs,     "list the directories in the MFD"           },
	{ "ls",       cmd_ls,       "list a directory: ls img KSHACK"           },
	{ "cat",      cmd_cat,      "print a file as text"                      },
	{ "get",      cmd_get,      "copy a file out to the host"               },
	{ "free",     cmd_free,     "what the TUT says about the pack"          },
	{ "check",    cmd_check,    "check a pack (shares no code with the reader)" },
	{ "manifest", cmd_manifest, "fingerprint a pack: one line per file"     },
	{ "verify",   cmd_verify,   "diff a pack against a manifest"            },
	{ "shell",    cmd_shell,    "interactive explorer (read-only)"          },
	{ "put",      cmd_put,      "write a host file into a directory (destructive)" },
	{ "del",      cmd_del,      "remove a file (destructive)"               },
	{ "rm",       cmd_del,      "remove a file -- the same command as del"  },
};

/* clang-format on */

#define NSUB ((int)(sizeof subcmds / sizeof subcmds[0]))

static int
usage(int rc)
{
	fprintf(stderr, "usage: itsfs <command> [args...]\n\ncommands:\n");

	for (int i = 0; i < NSUB; i++)
		fprintf(stderr, "  %-9s %s\n", subcmds[i].name, subcmds[i].help);
	fprintf(stderr, "\nRun a command with no arguments for its own usage.\n");
	return rc;
}

int
main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2)
		return usage(2);
	cmd = argv[1];

	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0)
		return usage(0);

	for (int i = 0; i < NSUB; i++)
		if (strcmp(cmd, subcmds[i].name) == 0)
			return subcmds[i].fn(argc - 1, argv + 1);

	fprintf(stderr, "itsfs: unknown command '%s'\n", cmd);
	return usage(2);
}
