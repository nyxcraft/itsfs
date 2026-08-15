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

BASE := itsfs.c cmd_dump.c cmd_pack.c cmd_word.c cmd_fs.c cmd_check.c \
        util.c image.c structure.c itspack.c itsgeom.c itstext.c
HDRS := $(SRC)/cmds.h $(SRC)/util.h $(SRC)/image.h $(SRC)/itspack.h $(SRC)/itsgeom.h \
        $(SRC)/itstext.h $(SRC)/its.h $(SRC)/structure.h

OBJSRC := $(addprefix $(SRC)/, $(BASE))

all: $(BIN)/itsfs

$(BIN)/itsfs: $(OBJSRC) $(HDRS) | $(BIN)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(OBJSRC)

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

lint: lint-format
	@rc=0; for f in $(OBJSRC); do \
	   $(CC) $(LINTFLAGS) $(CPPFLAGS) -fsyntax-only $$f || rc=1; done; \
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

# Optional corruption fuzzer (needs python3).  NOT part of `make test` and CI
# does not run it -- the suite stays sh + coreutils.  Build sanitized first or a
# finding is invisible, for the reason above.
fuzz: $(OBJSRC) $(HDRS) | $(BIN)
	$(CC) $(SANFLAGS) $(CPPFLAGS) -o $(BIN)/itsfs-fuzz $(OBJSRC)
	@python3 tests/fuzz.py --bin $(BIN)/itsfs-fuzz --iters $(ITERS)

ITERS ?= 100

clean:
	rm -rf $(BIN)

.PHONY: all clean lint lint-format test test-san fuzz oracle nsalv version-diff
