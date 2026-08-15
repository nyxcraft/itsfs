/*
 * cmds.h -- one prototype per subcommand.
 *
 * Each takes (argc, argv) with argv[0] the subcommand's own name, so a command
 * can print its own usage without knowing it is a subcommand, and returns an
 * exit status.
 */
#ifndef CMDS_H
#define CMDS_H

int cmd_info(int argc, char **argv);
int cmd_dump(int argc, char **argv);
int cmd_packings(int argc, char **argv);
int cmd_drives(int argc, char **argv);
int cmd_repack(int argc, char **argv);
int cmd_sixbit(int argc, char **argv);
int cmd_ls(int argc, char **argv);
int cmd_dirs(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_get(int argc, char **argv);
int cmd_free(int argc, char **argv);
int cmd_check(int argc, char **argv);
int cmd_manifest(int argc, char **argv);
int cmd_verify(int argc, char **argv);
int cmd_shell(int argc, char **argv);

#endif /* CMDS_H */
