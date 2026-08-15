/*
 * its.h -- on-disk layout of the ITS file system.
 *
 * EVERY CONSTANT HERE IS A TRANSCRIPTION, NOT A RECONSTRUCTION.
 *
 * The source is SYSTEM;FSDEFS 43 from the ITS monitor, which defines each
 * on-disk word offset, byte pointer and descriptor opcode as a commented MIDAS
 * symbol, under the heading "FILE SYSTEM PARAMETERS - APPLIES TO ALL ITS
 * MACHINES".  The copy read is the one in the PDP-10/its repository, whose file
 * header reads:
 *
 *     ;;; Copyright (c) 1999 Massachusetts Institute of Technology
 *     ;;; See the COPYING file at the top-level directory of this project.
 *
 * That tree is GPL and is NEVER vendored, copied or committed here.  What is
 * taken from it is what a file format IS -- offsets, widths and opcodes -- which
 * is the same thing t10fs takes from DEC's COMMOD.MAC and s5fs from the 2.9BSD
 * headers.  No line of ITS code, and no line of anybody's C, appears in this
 * project.  See docs/sources.md.
 *
 * Offsets are written in the base FSDEFS writes them in, and marked: a MIDAS
 * constant with a trailing dot is decimal (`UDDESC==11.`) and one without is
 * OCTAL (`LTIBLK==20`).  Getting that backwards is the single easiest way to
 * read this format wrongly, so each line below names the symbol and gives the
 * value the way the source gives it.
 *
 * STATUS MARKERS in the comments:
 *     [v]   read in FSDEFS *and* seen to decode correctly on a real ITS pack
 *     [s]   read in FSDEFS, not yet exercised against a pack
 * A field marked [s] is not less trustworthy as a transcription -- it is less
 * trustworthy as an understanding, and a reader should not lean on one.
 *
 * WHAT "A REAL PACK" MEANS HERE: an RP06 image built by the PDP-10/its
 * Makefile, booted, and run.  It is not a museum artifact recovered from MIT,
 * and until this project reads one of those, every [v] carries that asterisk.
 * See docs/validation.md.
 *
 * NOTHING IN THIS FILE IS A C STRUCT, and nothing ever will be.  A PDP-10 word
 * is 36 bits; there is no host type that is one.  Every field is fetched by word
 * index out of a uint64_t[] that came through itspack.  See docs/design.md.
 */
#ifndef ITS_H
#define ITS_H

#include "itspack.h"

/*
 * clang-format off -- EVERYTHING BELOW IS ONE TABLE.
 *
 * A column of symbols, a column of values, and a column giving the monitor's
 * own name with its evidence marker at the right margin.  Those columns ARE the
 * documentation: reading a value against its FSDEFS symbol by eye is the check
 * this file exists to make possible, and clang-format collapses them.
 */
/* clang-format off */

/* ------------------------------------------------------------------ the MFD
 *
 * One block, at NBLKS/2-1 (see itsgeom.h).  Seven words of header, then nothing
 * until the user-name area, which C(MDNAMP) points at and which runs to the end
 * of the block, two words per directory.
 *
 * THE NAME AREA IS FILLED FROM THE TOP DOWN, and that is not decoration: the
 * position of an entry IS the address of the directory it names.  FSDEFS says
 * it in a comment and disk.1228's QFL2 does it in two instructions --
 *
 *      QFL2:  SUBI J,2000-LMNBLK*NUDSL     ;J <= TRACK ADDR OF USER DIR
 *             LSH J,-1
 *
 * -- so a directory whose MFD entry sits at word A of the MFD block lives in
 * block (A - 2000 + 2*NUDSL) / 2, where 2000 is octal (the block size) and
 * NUDSL is C(MDNUDS).  There is no pointer.  [v]
 */
#define ITS_MD_NUM	0	/* MDNUM   ascending directory number         [v] */
#define ITS_MD_NAMP	1	/* MDNAMP  origin of the user-name area       [v] */
#define ITS_MD_YEAR	2	/* MDYEAR  current year                       [v] */
#define ITS_MD_PDOFF	3	/* MPDOFF  de-Coriolis clock offset           [s] */
#define ITS_MD_PDWDK	4	/* MPDWDK  preferred writing disk             [s] */
#define ITS_MD_CHK	5	/* MDCHK   must be SIXBIT "M.F.D."            [v] */
#define ITS_MD_NUDS	6	/* MDNUDS  number of user directories         [v] */
#define ITS_LMIBLK	7	/* LMIBLK  words of MFD header                [v] */
#define ITS_LMNBLK	2	/* LMNBLK  words per user-name block          [v] */
#define ITS_MN_UNAM	0	/* MNUNAM  SIXBIT user name; word 1 is zero   [v] */

/* The check word itself: SIXBIT "M.F.D.", which is what makes finding the MFD
 * a measurement rather than an assumption. */
#define ITS_MFD_MAGIC	UINT64_C(0551646164416)		/*                    [v] */

/* ------------------------------------------------------------------ the TUT
 *
 * "TRACK (BLOCK) UTILIZATION TABLE" -- disk.1228 line 48, and note the
 * parenthesis: a TRACK IN ITS SOURCE IS A BLOCK, not a surface track.
 *
 * NTUTBL blocks ending just below the MFD.  Three bits per block of the pack,
 * twelve to a word, starting at word LTIBLK.  It is a REFERENCE COUNT, not a
 * bitmap: 0 free, 1..TUTMNY-1 that many references, TUTMNY "many or more",
 * TUTLK locked out.  A block can legitimately be referenced by more than one
 * directory entry, which is how ITS does links to file contents.
 */
#define ITS_Q_PKNUM	0	/* QPKNUM  pack number                        [v] */
#define ITS_Q_PAKID	1	/* QPAKID  SIXBIT pack ID                     [v] */
#define ITS_Q_TUTP	2	/* QTUTP   free-space search pointer          [v] */
#define ITS_Q_SWAPA	3	/* QSWAPA  first block of the non-swap area   [v] */
#define ITS_Q_FRSTB	4	/* QFRSTB  first block the TUT maps           [v] */
#define ITS_Q_LASTB	5	/* QLASTB  last block the TUT maps            [v] */
#define ITS_Q_TRSRV	6	/* QTRSRV  -1: allocated dirs only            [s] */
#define ITS_LTIBLK	020	/* LTIBLK  (octal) first word of the map      [v] */

#define ITS_TUTBYT	3	/* TUTBYT  bits per entry (was 4 once)        [v] */
#define ITS_TUTEPW	12	/* TUTEPW  36./TUTBYT entries per word        [v] */
#define ITS_TUTMAX	8	/* TUTMAX  1_TUTBYT                           [v] */
#define ITS_TUTLK	7	/* TUTLK   TUTMAX-1: locked out               [v] */
#define ITS_TUTMNY	6	/* TUTMNY  TUTLK-1: many or more references   [v] */

/* ------------------------------------------------------------------ a UFD
 *
 * One block per directory, in the block the MFD's arithmetic gives.  Five words
 * of header, then TWO AREAS THAT GROW TOWARDS EACH OTHER: descriptor bytes up
 * from UDDESC, and five-word name blocks down from the end.  C(UDNAMP) is where
 * the name area currently starts, so the free space is the gap between them and
 * a directory is full when they meet.
 */
#define ITS_UD_ESCP	0	/* UDESCP  free pointer into the desc area    [v] */
#define ITS_UD_NAMP	1	/* UDNAMP  origin of the name area            [v] */
#define ITS_UD_NAME	2	/* UDNAME  SIXBIT user name, for checking     [v] */
#define ITS_UD_BLKS	3	/* UDBLKS  lh allocated, rh blocks used       [v] */
#define ITS_UD_ALLO	4	/* UDALLO  lh disk number, rh allocation      [s] */
#define ITS_UD_DESC	11	/* UDDESC==11.  (decimal) first desc word     [v] */

#define ITS_UFDBYT	6	/* UFDBYT  bits per descriptor byte           [v] */
#define ITS_UFDBPW	6	/* UFDBPW  36./UFDBYT bytes per word          [v] */

/* A name block: five words, and every one of them is packed. */
#define ITS_LUNBLK	5	/* LUNBLK  words per name block               [v] */
#define ITS_UN_FN1	0	/* UNFN1   first name, SIXBIT                 [v] */
#define ITS_UN_FN2	1	/* UNFN2   second name, SIXBIT                [v] */
#define ITS_UN_RNDM	2	/* UNRNDM  "all kinds of random info"         [v] */
#define ITS_UN_DATE	3	/* UNDATE  creation date and time             [v] */
#define ITS_UN_REF	4	/* UNREF   reference date, author, byte size  [v] */

/*
 * The fields inside those words are given in FSDEFS as PDP-10 BYTE POINTERS,
 * whose left half is <position,,size> -- P is how many bits lie to the RIGHT of
 * the byte and S is its width, so the value is (word >> P) & ((1<<S)-1).
 *
 * They are transcribed here as (P, S) pairs next to the pointer FSDEFS writes,
 * so a line can be checked against the source without decoding anything, and
 * fetched with ITS_FIELD below.  Writing out the shifts by hand instead is how
 * a project ends up with four subtly different readings of one word.
 */
#define ITS_FIELD(w, p, s)	(((uint64_t)(w) >> (p)) & ((UINT64_C(1) << (s)) - 1))

/* in UNRNDM */
#define ITS_UN_DSCP_P	0
#define ITS_UN_DSCP_S	13	/* UNDSCP==1500,,   pointer to the descriptor [v] */
#define ITS_UN_PKN_P	13
#define ITS_UN_PKN_S	5	/* UNPKN==150500,,  pack number               [v] */
#define ITS_UN_LNK_P	18
#define ITS_UN_LNK_S	1	/* UNLNKB==220100,, link bit                  [v] */
#define ITS_UN_WRDC_P	24
#define ITS_UN_WRDC_S	10	/* UNWRDC==301200,, words in the last block   [v] */

/*
 * The flag bits.  FSDEFS lists these as bare values immediately after UNLNKB,
 * and UNLINK==1 is the bit UNLNKB reaches -- so they are values IN THAT FIELD,
 * not in the word: the field starts where the link bit is and runs up from it.
 * UNIGFL==024 is the pair that together mean "pretend this entry is not here".
 *
 * The field's WIDTH is not given anywhere; six is what the values in FSDEFS
 * need (UNCDEL==020 is the highest) and it does not collide with UNWRDC above
 * it.  That is a deduction, and it is marked as one.
 */
#define ITS_UN_FLAGS_P	18
#define ITS_UN_FLAGS_S	6	/* width deduced, not transcribed             [s] */
#define ITS_UN_LINK	1	/* UNLINK  this entry is a link               [v] */
#define ITS_UN_REAP	2	/* UNREAP  do not reap                        [s] */
#define ITS_UN_WRIT	4	/* UNWRIT  open for writing                   [s] */
#define ITS_UN_MARK	010	/* UNMARK  GC mark bit                        [s] */
#define ITS_UN_CDEL	020	/* UNCDEL  delete when closed                 [s] */
#define ITS_UN_IGFL	024	/* UNIGFL  bits that mean "ignore this file"  [s] */

/* in UNDATE */
#define ITS_UN_TIM_P	0
#define ITS_UN_TIM_S	18	/* UNTIM==2200,,    compacted creation time   [s] */
#define ITS_UN_DAY_P	18
#define ITS_UN_DAY_S	5	/* UNDAY==220500,,  day                       [v] */
#define ITS_UN_MON_P	23
#define ITS_UN_MON_S	4	/* UNMON==270400,,  month                     [v] */
#define ITS_UN_YRB_P	27
#define ITS_UN_YRB_S	7	/* UNYRB==330700,,  year, less 1900           [v] */

/* in UNREF */
#define ITS_UN_REFD_P	18
#define ITS_UN_REFD_S	16	/* UNREFD==222000,, reference date            [s] */
#define ITS_UN_AUTH_P	9
#define ITS_UN_AUTH_S	9	/* UNAUTH==111100,, author; all ones = none   [v] */
#define ITS_UN_BYTE_P	0
#define ITS_UN_BYTE_S	9	/* UNBYTE==001100,, byte size and odd count   [s] */

/* ------------------------------------------------- the UFD descriptor opcodes
 *
 * A file's block list is a run-length program in six-bit bytes.  B is the
 * current block number, undefined until a load address sets it.
 *
 *      0                    end of the description
 *      1..UDTKMX            take N blocks: B .. B+N-1, then B += N
 *      UDTKMX+1..UDWPH-1    skip N-UDTKMX and take one
 *      UDWPH                write place holder: a no-op, skip it
 *      040..077             load address: B = (N&037)<<12 | N2<<6 | N3,
 *                           take B, then B += 1.  NXLBYT=2 extra bytes.
 *
 * A ZERO-LENGTH FILE IS UDWPH THEN 0, and a legal description must load an
 * address before it may use any other code.  FSDEFS says the skip codes have
 * been unreachable for years ("ITS has been broken for years such that it never
 * uses this UFD descriptor code!") -- they are implemented anyway, because a
 * reader's job is to read what is there rather than what the writer meant.
 *
 * THE "FUNNY" BIT IS GONE.  FSDEFS dates its removal -- 8/19/90, to make room
 * for RP07 block numbers -- and says outright that any program interpreting UFD
 * descriptors "needs to be fixed to not mask that bit out (as most of them
 * currently do)".  Nothing here masks it.  17 bits of block number is what the
 * three bytes carry, which is 131,072 blocks; an RP07 has 108,360.
 */
#define ITS_UD_TKMX	12	/* UDTKMX==12.  highest "take N" code         [v] */
#define ITS_UD_WPH	31	/* UDWPH==31.   write place holder            [v] */
#define ITS_UD_LOADAD	040	/* (octal) load-address codes start here      [v] */
#define ITS_NXLBYT	2	/* NXLBYT  extra bytes after a load address   [v] */

/*
 * A link's "descriptor" is not a block list at all: it is the target's name as
 * SIXBIT characters, terminated by a zero byte, with the directory, first and
 * second names in order.  The first two are ended by ";" when they are shorter
 * than six characters, and ";" (73), ":" (72) and " " (0) are quoted by a
 * preceding ":".  FSDEFS gives five worked examples, and observes that the
 * encoding admits a lot of illegal or wasteful spellings of the same name.
 */
/*
 * AND A TRAP, WHICH COST AN AFTERNOON.  FSDEFS writes those two characters as
 * `";" (73)` and `":" (72)`, which are their ASCII codes -- but the bytes on the
 * disk are SIXBIT, so what is actually stored is 033 and 032.  Taking the
 * numbers in the comment at face value finds no separator anywhere and renders
 * every link as one run-on string.  The real bytes of a link on a real pack:
 *
 *      16 33 40 33 44 44 64 00      ".;@;DDT" -> the file .;@ DDT
 *
 * Note also that the space FSDEFS lists as quoted is `(0)`, which IS its SIXBIT
 * value -- so the comment mixes the two encodings in one sentence.
 *
 * THE TWO BELOW ARE THE ONLY CONSTANTS HERE THAT CITE NO DEFSYM, because FSDEFS
 * gives these characters in prose and defines no symbol for either.  What they
 * cite instead is ITS's own CODE -- NSALV's link parser, which compares against
 * MIDAS character constants:
 *
 *      LTYPE:	MOVEI B,6
 *      LTYPE2:	IDPB Z,E		;Z accumulates the link.
 *      	ILDB A,N		;Get a byte.
 *      	JUMPE A,CPOPJ		;Not expecting zeros in the link.
 *      	CAIN A,':		;Quoting character?
 *
 * and MIDAS assembles `'X` as SIXBIT (midas.458, SQUOTE: `ADDI T,-40(A)`).  So
 * `';` is 033 and `':` is 032, and ITS's code contradicts ITS's comment.
 * tests/version-diff.sh knows these two have no DEFSYM and says so.
 */
#define ITS_LNK_SEP	033	/* no DEFSYM; NSALV LTYPE's `';`              [v] */
#define ITS_LNK_QUOTE	032	/* no DEFSYM; NSALV LTYPE's `':`              [s] */

/* clang-format on */

#endif /* ITS_H */
