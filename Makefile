# itsfs -- host tools for the ITS file system.
#
# Dependency-free: C99, POSIX, no libraries.  `make` builds bin/itsfs, `make
# test` runs the suite, `make test-san` runs it again under the sanitizers.
CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?=

# 36-bit words in 8-byte containers make a 300 MB pack ordinary, so 64-bit
# offsets are not optional even on a 32-bit host.
CPPFLAGS += -D_FILE_OFFSET_BITS=64

BIN := bin
SRC := src

BASE := itsfs.c cmd_dump.c cmd_pack.c cmd_word.c cmd_fs.c cmd_check.c cmd_manifest.c cmd_shell.c cmd_write.c cmd_tape.c cmd_saveset.c cmd_tar.c cmd_query.c \
        util.c image.c structure.c write.c itspack.c itsgeom.c itstext.c
HDRS := $(SRC)/cmds.h $(SRC)/util.h $(SRC)/image.h $(SRC)/itspack.h $(SRC)/itsgeom.h \
        $(SRC)/itstext.h $(SRC)/its.h $(SRC)/structure.h $(SRC)/write.h

# The one optional dependency.  `make FUSE=1` builds `mount`/`umount` against
# libfuse3; the default build has no libraries at all, and every file command
# works without a mount.
FUSE ?= 0
ifeq ($(FUSE),1)
  CPPFLAGS += -DHAVE_FUSE $(shell pkg-config --cflags fuse3)
  FUSELIBS := $(shell pkg-config --libs fuse3)
  BASE     += cmd_mount.c
endif

OBJSRC := $(addprefix $(SRC)/, $(BASE))

all: $(BIN)/itsfs

$(BIN)/itsfs: $(OBJSRC) $(HDRS) | $(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJSRC) $(FUSELIBS)

$(BIN):
	mkdir -p $(BIN)

#
# `make lint` -- the warning set the tree is actually clean under, plus the
# formatting rule.
#
# CI compiles with -Werror on `-Wall -Wextra -pedantic` and no more, because gcc,
# clang and macOS disagree about the rest and a build that fails on somebody
# else's compiler version is worse than a warning nobody sees.  This target is
# the local, larger set: the tree sits at ZERO under it, and the point of having
# it as a target is that it stays there.
#
LINTFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic -Wshadow -Wconversion \
             -Wsign-conversion -Wcast-qual -Wcast-align -Wstrict-prototypes \
             -Wmissing-prototypes -Wredundant-decls -Wundef -Wwrite-strings \
             -Wpointer-arith -Wformat=2 -Wswitch-enum -Wvla

# -Werror IS THE POINT.  Without it this compiled with -fsyntax-only, which
# fails on errors and not on warnings -- so the target printed "no warnings"
# whether or not there were any, and a warning could sit here indefinitely
# behind a passing check.  Found when `make lint FUSE=1` printed the pass line
# under six -Wcast-qual warnings.
lint: lint-format
	@rc=0; for f in $(OBJSRC); do \
	   $(CC) $(LINTFLAGS) -Werror $(CPPFLAGS) -fsyntax-only $$f || rc=1; done; \
	 test $$rc -eq 0 && echo "lint: no warnings under $$(echo $(LINTFLAGS) | tr ' ' '\n' | grep -c '^-W') warning options"; \
	 exit $$rc

# THE FORMAT HALF IS SEPARATE BECAUSE IT IS THE HALF CI CAN RUN.  Whether a file
# is clang-format clean is a decision one pinned binary makes the same way
# everywhere; whether a compiler emits -Wconversion here depends on which
# compiler and which version, which is why the warning set above is local-only
# and CI keeps -Werror on the portable three.
#
# The rule being enforced is in .clang-format's own header: hand-aligned tables
# must be fenced with `clang-format off`, because the tool cannot see that a
# construct is a table and collapses the columns that ARE the documentation.
# its.h is entirely one such table, and so are the drive and packing tables.
CLANG_FORMAT ?= clang-format
lint-format:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { \
	   echo "lint: no $(CLANG_FORMAT), skipping the format check"; exit 0; }
	@bad=; for f in $(OBJSRC) $(HDRS); do \
	   $(CLANG_FORMAT) --dry-run -Werror $$f >/dev/null 2>&1 || bad="$$bad $$f"; done; \
	 if [ -n "$$bad" ]; then \
	   echo "lint: not clang-format clean:$$bad"; \
	   echo "      fence hand-aligned tables with /* clang-format off */ ... on"; \
	   $(CLANG_FORMAT) --version; \
	   exit 1; \
	 fi; \
	 echo "lint: clang-format clean ($$($(CLANG_FORMAT) --version))"

test: $(BIN)/itsfs
	@sh tests/run.sh

# The suite again under AddressSanitizer + UBSan.  This is not belt and braces:
# the whole job of this code is parsing a file nobody here wrote, and an
# out-of-bounds READ does not fault on a normal build -- it returns whatever was
# next in memory and the test passes.  Only a sanitizer turns that into a
# failure.
#
# abort_on_error is the point of the options: by default ASan prints and exits
# 1, which a test asking "did it fail?" reads as an ordinary error return.
SANFLAGS := -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
test-san: $(OBJSRC) $(HDRS) | $(BIN)
	$(CC) $(SANFLAGS) $(CPPFLAGS) -o $(BIN)/itsfs-san $(OBJSRC)
	@ITSFS=$(BIN)/itsfs-san \
	 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
	 UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
	 sh tests/run.sh

#
# THE ORACLE: the phase-1 and phase-3 acceptance tests, against a real pack
# rather than a fixture.
#
# Needs an ITS disk image, which is NOT in this repo and never will be.  Build
# one with the PDP-10/its Makefile (`make EMULATOR=simh`, which leaves it at
# out/simh/rp0.dsk) or point IMAGE at your own.
#
# It works on a copy.  `repack` opens its input read-only so the original is not
# at risk either way, but "work on a copy" is the rule for touching a reference
# pack and a target that quietly breaks it is not worth the convenience.
#
IMAGE ?= $(HOME)/its/out/simh/rp0.dsk
ORACLE_TMP ?= /tmp/itsfs-oracle
oracle: $(BIN)/itsfs
	@test -f "$(IMAGE)" || { echo "no image at $(IMAGE) -- set IMAGE=<path>"; exit 2; }
	@mkdir -p $(ORACLE_TMP)
	@echo "copying $(IMAGE) ..."
	@cp "$(IMAGE)" $(ORACLE_TMP)/ref.dsk
	@$(BIN)/itsfs info $(ORACLE_TMP)/ref.dsk
	@echo
	@echo "repacking le64 -> le64 ..."
	@$(BIN)/itsfs repack -f $(ORACLE_TMP)/ref.dsk $(ORACLE_TMP)/rt.dsk
	@cmp $(ORACLE_TMP)/ref.dsk $(ORACLE_TMP)/rt.dsk && echo "IDENTICAL: the word layer round-trips the real pack"
	@echo "repacking le64 -> core -> le64 ..."
	@$(BIN)/itsfs repack -f -P core $(ORACLE_TMP)/ref.dsk $(ORACLE_TMP)/core.dsk
	@$(BIN)/itsfs repack -f -p core $(ORACLE_TMP)/core.dsk $(ORACLE_TMP)/back.dsk
	@cmp $(ORACLE_TMP)/ref.dsk $(ORACLE_TMP)/back.dsk && echo "IDENTICAL: cross-packing round trip"
	@echo "repacking le64 -> dbd9 -> le64 ..."
	@$(BIN)/itsfs repack -f -P dbd9 $(ORACLE_TMP)/ref.dsk $(ORACLE_TMP)/d9.dsk
	@$(BIN)/itsfs repack -f -p dbd9 $(ORACLE_TMP)/d9.dsk $(ORACLE_TMP)/back9.dsk
	@cmp $(ORACLE_TMP)/ref.dsk $(ORACLE_TMP)/back9.dsk && echo "IDENTICAL: and through the packing that shares a byte"
	@echo
	@echo "reading the file system ..."
	@$(BIN)/itsfs free $(ORACLE_TMP)/ref.dsk
	@echo
	@n=`$(BIN)/itsfs dirs $(ORACLE_TMP)/ref.dsk | grep -vc '^#'`; \
	 echo "directories: $$n"; \
	 test "$$n" -gt 0 || { echo "no directories found -- the MFD read failed"; exit 1; }
	@sh tests/accounting.sh $(BIN)/itsfs $(ORACLE_TMP)/ref.dsk
	@echo
	@# THE SAME THREE QUESTIONS, ASKED BY CODE THAT SHARES NOTHING WITH THE
	@# READER.  accounting.sh above drives ls/dirs/free, so it is the reader's
	@# answer in a shell loop; `check` re-derives the geometry, the directory
	@# arithmetic, the descriptor bytecode and the TUT from its.h and walks the
	@# pack itself.  Two implementations agreeing on 30,940 is the point --
	@# either alone is one opinion.
	@echo "and again, with the checker that shares no code with the reader ..."
	@$(BIN)/itsfs check $(ORACLE_TMP)/ref.dsk
	@echo
	@# THE MANIFEST IS OF THE FILE SYSTEM, NOT OF THE CONTAINER.  Fingerprint
	@# the le64 pack, then verify that manifest against the SAME file system
	@# stored as dbd9 -- two words in nine bytes, so not one byte boundary in
	@# common.  Every byte differs and no word does, and a manifest that could
	@# not survive that would be fingerprinting the wrong thing.
	@echo "fingerprinting it, and verifying that through a different packing ..."
	@$(BIN)/itsfs manifest $(ORACLE_TMP)/ref.dsk > $(ORACLE_TMP)/ref.mf
	@echo "manifest: `grep -vc '^\#' $(ORACLE_TMP)/ref.mf` directories, files and links"
	@$(BIN)/itsfs verify -p dbd9 $(ORACLE_TMP)/d9.dsk $(ORACLE_TMP)/ref.mf
	@echo "IDENTICAL: the same file system, four and a half bytes per word instead of eight"
	@echo
	@# A THIRD READER, in another language, sharing no constants with either C
	@# one.  structure.c and cmd_check.c are already independent of each other,
	@# but both take their numbers from src/its.h -- so a wrong TRANSCRIPTION
	@# would be inherited by both and invisible to their agreement.  This one
	@# transcribes them again from the same ITS sources.  Optional: it needs
	@# python3, which `make test` deliberately does not.
	@if command -v python3 >/dev/null 2>&1; then \
	   echo; \
	   python3 tests/crosscount.py $(ORACLE_TMP)/ref.dsk; \
	   $(BIN)/itsfs check $(ORACLE_TMP)/ref.dsk 2>&1 | sed -n 's/^directories *//p' | \
	     sed 's/^/itsfs check: /'; \
	 fi
	@# ...and the one check here that is not the pack agreeing with itself.
	@# Extract files and compare them to the host files in the ITS source tree
	@# that they were built from -- something no part of this project has ever
	@# touched.  Skipped without a source tree; it is not in the repo either.
	@if [ -d "$(ITSSRC)" ]; then \
	   echo "comparing extracted files against the ITS source tree ..."; \
	   sh tests/crosscheck.sh $(BIN)/itsfs $(ORACLE_TMP)/ref.dsk $(ITSSRC) $(CROSSDIRS); \
	 else \
	   echo "no ITS source tree at $(ITSSRC) -- skipping the cross-check (set ITSSRC=)"; \
	 fi
	@rm -rf $(ORACLE_TMP)

# The ITS source tree the pack was built from, and which of its directories to
# compare.  These are directories that exist under both names -- the pack has
# many more that were never in the tree.
ITSSRC ?= $(HOME)/its/src
CROSSDIRS ?= kshack syseng sysen1 klh mrc lars gls

#
# `make nsalv` -- hand a pack to ITS'S OWN SALVAGER and compare it with ours.
#
# The one piece of evidence here that is not this project agreeing with itself.
# `itsfs check` and the reader both take their constants from src/its.h, so both
# inherit any misreading in it; NSALV is MIT's, from the 1980s, and knows this
# format because it was there.  Where the two agree, the agreement is about the
# format rather than about our reading of it.
#
# IT NEEDS NO WRITER.  A pack this project has only READ is enough to ask the
# question, which is why this runs at phase 5 rather than waiting for phase 8.
#
# Needs `expect`, an ITS salvager tape and an emulator with ITS support -- Open
# SIMH's pdp10, which the PDP-10/its Makefile builds at tools/simh/BIN/pdp10.
# The 3.8-1 pdp10 packaged by most distributions is NOT it: `set cpu its` is
# accepted but the KS10 paging ITS needs is not there.
#
# It works on copies and answers "no" to every offer NSALV makes to write, and
# then CHECKS that the pack came back byte-identical -- a salvager that quietly
# repaired the pack would destroy the evidence of what was wrong with it.
#
# Budget ten minutes: two full salvage runs over a 300 MB pack.
#
NSALV_PDP10 ?= $(HOME)/its/tools/simh/BIN/pdp10
NSALV_TAPE  ?= $(HOME)/its/out/simh/salv.tape
NSALV_TMP   ?= /tmp/itsfs-nsalv
nsalv: $(BIN)/itsfs
	@test -f "$(IMAGE)" || { echo "no image at $(IMAGE) -- set IMAGE=<path>"; exit 2; }
	@sh tests/nsalv.sh $(BIN)/itsfs "$(IMAGE)" "$(NSALV_PDP10)" "$(NSALV_TAPE)" $(NSALV_TMP)

#
# `make version-diff` -- how far does the transcription reach?
#
# src/its.h comes from ONE file, SYSTEM;FSDEFS 43, and ITS ran for twenty-three
# years.  The PDP-10/its repository preserves an earlier version of the same file
# in its history -- SYSENG;FSDEFS 40, imported and then deleted in 2016 -- so the
# question has an answer rather than a shrug.
#
# Needs the ITS source tree WITH ITS HISTORY.  A shallow clone will not have the
# deleted file; `git fetch --unshallow` in that tree fixes it.
#
# It also checks the citation column: every symbol its.h names must actually be
# defined in FSDEFS.  That is the one kind of wrong citation a machine can catch.
#
version-diff:
	@sh tests/version-diff.sh $(ITSSRC)/..

#
# `make interop` -- write a file, then let ITS ITSELF look at the pack.
#
# `nsalv` above hands a pack to ITS's SALVAGER, a program whose job is to
# inspect a file system.  This hands it to the two that USE one: DSKDMP, the
# standalone loader, which reads the directory with its own code and is a THIRD
# implementation sharing nothing with the monitor or with NSALV; and the monitor,
# which runs a salvage pass over every directory before it will come up at all.
#
# Two emulator runs, because doing both in one leaves the first half's output in
# the expect buffer and the second half types into it.  Budget five minutes.
#
# It does NOT make the monitor open the file and print it -- see the header of
# tests/interop.sh for why, and docs/roadmap.md for what would.
#
INTEROP_TMP ?= /tmp/itsfs-interop
interop: $(BIN)/itsfs
	@test -f "$(IMAGE)" || { echo "no image at $(IMAGE) -- set IMAGE=<path>"; exit 2; }
	@sh tests/interop.sh $(BIN)/itsfs "$(IMAGE)" "$(NSALV_PDP10)" $(INTEROP_TMP)

#
# `make mkfs-test` -- build a file system from nothing and let ITS grade it.
#
# THE ONE PACK WITH NO ITS IN IT.  Everywhere else the starting point is a pack
# ITS built; here every word is `itsfs mkfs`'s.  Both graders boot FROM TAPE,
# because a pack this makes does not boot -- ITS starts from the front end's
# blocks at the bottom of the disk, which NSALV's own ZAP refuses to touch, and
# `mkfs` writes a file system rather than a bootable pack.
#
# Needs the salvager and dskdmp tapes, which the PDP-10/its build leaves in
# out/<emulator>/.  Budget five minutes.
#
NSALV_DSKDMP ?= $(HOME)/its/out/simh/dskdmp.tape
MKFS_TMP ?= /tmp/itsfs-mkfs
mkfs-test: $(BIN)/itsfs
	@sh tests/mkfs.sh $(BIN)/itsfs "$(NSALV_PDP10)" "$(NSALV_TAPE)" "$(NSALV_DSKDMP)" $(MKFS_TMP)

#
# `make tape-test` -- read a real ITS tape, and check the words against the
# host files it was built from.
#
# THE MEASUREMENT THAT PROMOTED `core` FROM corroborated TO confirmed.  The
# salvager tape is 79,890 bytes of program: a whole multiple of five and NOT of
# eight, so it cannot be one word per eight bytes.  Decoded here as five frames
# per word and read as seven-bit characters, it contains three strings this
# project has watched NSALV print on a console.  And extracting it and
# re-encoding reproduces both host files byte for byte.
#
# Needs the ITS tree for the tapes and the files they were made from; no
# emulator.
#
TAPE ?= $(HOME)/its/out/simh/salv.tape
TAPE_SRC ?= $(HOME)/its/bin/ks10/boot
TAPE_TMP ?= /tmp/itsfs-tape
tape-test: $(BIN)/itsfs
	@sh tests/tape.sh $(BIN)/itsfs "$(TAPE)" "$(TAPE_SRC)" $(TAPE_TMP)

#
# `make klh10` -- dbd9 against KLH10'S OWN CONVERTER.
#
# This is the measurement that promoted dbd9 from `corroborated` to `confirmed`.
# It needs KLH10 built, which is worth doing for a reason that is not obvious:
# the build ships `vdkfmt`, KLH10's disk-format converter, so an artifact
# written by KLH10's code can be had WITHOUT running the emulator.
#
#   cd tools/klh10 && ./autogen.sh && mkdir -p tmp && cd tmp && \
#       ../configure && make -C bld-ks-its
#
VDKFMT ?= $(HOME)/its/tools/klh10/tmp/bld-ks-its/vdkfmt
KLH10_TMP ?= $(shell mktemp -d)

klh10: all
	@sh tests/klh10.sh $(BIN)/itsfs "$(VDKFMT)" "$(IMAGE)" $(KLH10_TMP)
	@rm -rf $(KLH10_TMP)

#
# `make interop-klh10` -- the same questions as `make interop`, on a SECOND
# emulator and a second packing.  KLH10 is an unrelated implementation of the
# same hardware and its ITS config reads dbd9, so this run shares with the SIMH
# one only the two things being tested: the file system, and ITS.
#
KLH10_BIN ?= $(HOME)/its/tools/klh10/tmp/bld-ks-its
ITSTREE ?= $(HOME)/its
INTEROP_K10_TMP ?= $(shell mktemp -d)

interop-klh10: all
	@sh tests/interop-klh10.sh $(BIN)/itsfs "$(IMAGE)" "$(KLH10_BIN)" \
	    "$(ITSTREE)" $(INTEROP_K10_TMP)
	@rm -rf $(INTEROP_K10_TMP)

#
# `make itsdump` -- ITS'S OWN DUMP WRITES A TAPE, and ours is compared with it.
#
# The only level-1 evidence here: build something with itsfs, build the same
# thing with ITS, cmp.  Needs KLH10 built and about twenty minutes.
#
ITSDUMP_TMP ?= $(shell mktemp -d)

itsdump: all
	@sh tests/itsdump.sh $(BIN)/itsfs "$(IMAGE)" "$(KLH10_BIN)" \
	    "$(ITSTREE)" $(ITSDUMP_TMP)
	@rm -rf $(ITSDUMP_TMP)

#
# `make itsload` -- ITS'S OWN DUMP READS A TAPE THIS WROTE.
#
# The other direction from `itsdump`, and the only grader for the one part of
# `save` that is not a copy of something ITS produced: ITS's DUMP omits links,
# so nothing it writes can be compared against what we write for one.  This
# hands ITS a tape with a file AND a link on it and looks at what comes back.
#
ITSLOAD_TMP ?= $(shell mktemp -d)

itsload: all
	@sh tests/itsload.sh $(BIN)/itsfs "$(IMAGE)" "$(KLH10_BIN)" \
	    "$(ITSTREE)" $(ITSLOAD_TMP)
	@rm -rf $(ITSLOAD_TMP)

# Optional corruption fuzzer (needs python3).  NOT part of `make test` and CI
# does not run it -- the suite stays sh + coreutils.  Build sanitized first or a
# finding is invisible, for the reason above.
#
# It runs the WRITERS as well as the readers, so it works entirely inside a
# mkdtemp of its own on packs it built itself.  It is never pointed at anything
# on disk here, and takes no argument that could be.
# The FUSE mount, against the commands that need no mount.  Needs `FUSE=1` and
# a machine that can actually mount; skips itself loudly otherwise.
mount-test: $(BIN)/itsfs
	@sh tests/mount.sh $(BIN)/itsfs "$(IMAGE)" 2>&1

fuzz: $(OBJSRC) $(HDRS) | $(BIN)
	$(CC) $(SANFLAGS) $(CPPFLAGS) -o $(BIN)/itsfs-fuzz $(OBJSRC)
	@python3 tests/fuzz.py --bin $(BIN)/itsfs-fuzz --iters $(ITERS)

ITERS ?= 100

clean:
	rm -rf $(BIN)

.PHONY: all clean lint lint-format test test-san fuzz oracle nsalv interop mkfs-test tape-test version-diff klh10 interop-klh10 itsdump itsload
