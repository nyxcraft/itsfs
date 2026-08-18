#!/bin/sh
# mount.sh -- the FUSE mount, checked against the commands that need no mount.
#
#   usage: sh tests/mount.sh <itsfs built with FUSE=1> [image] [scratch]
#
# Run by `make mount-test FUSE=1`.  Skips itself, loudly, on a machine with no
# libfuse or no permission to mount -- which is most CI.
#
# WHAT IT IS ACTUALLY ASKING.  A mount is a second reader of the same pack, so
# the question is not "does it mount" but "does reading a file through the
# kernel give the same bytes as `itsfs get`".  Every check below is that
# question in some form; the mount succeeding is only the setup.
#
# The fixture is built here by `mkfs` and `put`, so this needs no real pack.
# With one named on the command line it does the same comparisons against it,
# which is where the interesting names are -- a directory called `.`, names with
# spaces and percent signs.
set -u

ITSFS=${1:?usage: mount.sh <itsfs> [image] [scratch]}
IMAGE=${2:-}
T=${3:-$(mktemp -d)}

pass=0
rc=0
ok() { pass=$((pass + 1)); printf '  ok   %s\n' "$1"; }
no() { rc=1; printf '  FAIL %s\n' "$1"; }

[ -x "$ITSFS" ] || { echo "no itsfs at $ITSFS"; exit 1; }

"$ITSFS" 2>&1 | grep -aq "^  mount" || {
	echo "this itsfs has no mount -- build it with 'make FUSE=1'"
	exit 0
}

command -v fusermount3 >/dev/null 2>&1 || command -v fusermount >/dev/null 2>&1 || {
	echo "no fusermount -- skipping"
	exit 0
}

[ -e /dev/fuse ] || { echo "no /dev/fuse -- skipping"; exit 0; }

M=$T/mnt
mkdir -p "$M" || exit 1

cleanup() { "$ITSFS" umount "$M" >/dev/null 2>&1 || true; }
trap cleanup EXIT

echo "the mount, against the commands that need no mount"
echo

# ---------------------------------------------------------------- a fixture
echo "1. a pack made here"

"$ITSFS" mkfs "$T/m.dsk" MNTPAK >/dev/null 2>&1 || { echo "mkfs failed"; exit 1; }
"$ITSFS" mkdir "$T/m.dsk" KSHACK >/dev/null 2>&1
printf 'hello from ITS\r\n' > "$T/one.txt"
printf 'and a second file\r\n' > "$T/two.txt"
"$ITSFS" put "$T/m.dsk" 'KSHACK;ONE TXT' "$T/one.txt" >/dev/null 2>&1
"$ITSFS" put "$T/m.dsk" 'KSHACK;TWO TXT' "$T/two.txt" >/dev/null 2>&1

if "$ITSFS" mount "$T/m.dsk" "$M" >/dev/null 2>&1; then
	ok "it mounts"
else
	no "it mounts"
	exit 1
fi

# The mount is asynchronous: the process returns before the kernel has it.
i=0
while [ "$i" -lt 50 ]; do
	[ -d "$M/KSHACK" ] && break
	i=$((i + 1))
	sleep 0.1
done

[ -d "$M/KSHACK" ] && ok "...and an ITS directory is a directory" ||
	no "...and an ITS directory is a directory"

echo
echo "2. what is in it"

n=$(ls "$M" | wc -l | tr -d ' ')
[ "$n" = "1" ] && ok "the root lists the MFD: $n directory" ||
	no "the root lists the MFD (got $n)"

ls "$M/KSHACK" > "$T/ls.out" 2>&1

grep -aq "^ONE TXT$" "$T/ls.out" && ok "ITS DIR;FN1 FN2 is DIR/FN1 FN2" ||
	no "ITS DIR;FN1 FN2 is DIR/FN1 FN2"

# THE CHECK THAT MATTERS: the kernel's bytes against `get`'s bytes.
if cmp -s "$T/one.txt" "$M/KSHACK/ONE TXT"; then
	ok "a file read through the mount is the file that was put"
else
	no "a file read through the mount is the file that was put"
fi

"$ITSFS" get "$T/m.dsk" 'KSHACK;TWO TXT' "$T/two.get" >/dev/null 2>&1

if cmp -s "$T/two.get" "$M/KSHACK/TWO TXT"; then
	ok "...and is byte-identical to what \`get\` writes"
else
	no "...and is byte-identical to what \`get\` writes"
fi

# stat has to agree with read, or every tool that trusts st_size is wrong.
a=$(wc -c < "$T/one.txt" | tr -d ' ')
b=$(stat -c %s "$M/KSHACK/ONE TXT" 2>/dev/null || stat -f %z "$M/KSHACK/ONE TXT")
[ "$a" = "$b" ] && ok "st_size is the number of bytes read() returns: $b" ||
	no "st_size is $b, read() gives $a"

echo
echo "3. read-only, and it means it"

# `:` is a special builtin, and a failed redirection on one is fatal in dash --
# which ended this script rather than failing this check.  `touch` is not.
if touch "$M/KSHACK/NEW FILE" 2>/dev/null; then
	no "creating a file is refused"
else
	ok "creating a file is refused"
fi

if rm -f "$M/KSHACK/ONE TXT" 2>/dev/null && [ ! -e "$M/KSHACK/ONE TXT" ]; then
	no "removing a file is refused"
else
	ok "removing a file is refused"
fi

# The pack must be untouched after all that.
out=$("$ITSFS" check "$T/m.dsk" 2>&1)
case "$out" in
*"no problems found"*) ok "...and the pack is unchanged underneath" ;;
*) no "...and the pack is unchanged underneath" ;;
esac

cleanup
trap - EXIT

# ------------------------------------------------------------- a real pack
if [ -n "$IMAGE" ] && [ -f "$IMAGE" ]; then
	echo
	echo "4. and a real pack, which has the names that are not path components"

	mkdir -p "$T/mnt2"
	M=$T/mnt2
	trap cleanup EXIT

	if "$ITSFS" mount "$IMAGE" "$M" >/dev/null 2>&1; then
		ok "a real pack mounts"
	else
		no "a real pack mounts"
		exit 1
	fi

	i=0
	while [ "$i" -lt 50 ]; do
		[ -n "$(ls -A "$M" 2>/dev/null)" ] && break
		i=$((i + 1))
		sleep 0.1
	done

	n=$(ls -A "$M" | wc -l | tr -d ' ')
	want=$("$ITSFS" dirs "$IMAGE" 2>/dev/null | sed -n 's/^# \([0-9]*\) directories$/\1/p')
	[ "$n" = "$want" ] && ok "every directory is there: $n" ||
		no "the root lists $n directories, the MFD has $want"

	# The pack has a directory named `.`, holding the monitor.  A bare `.`
	# is not a path component, so it is percent-encoded -- and if that were
	# wrong, this is the directory that would vanish.
	if [ -d "$M/%2E" ]; then
		ok "the directory named . is reachable as %2E"
	else
		no "the directory named . is reachable as %2E"
	fi

	# Compare a directory's worth of files with `get`.  A directory with real
	# files in it: the first one on the pack holds a single LINK, and `-f`
	# follows a symlink, so picking blind compares a link against `get` and
	# calls the mount wrong.
	d=
	for cand in $(ls -A "$M" | grep -av '%' | head -20); do
		if [ "$(find "$M/$cand" -maxdepth 1 -type f | head -1)" != "" ]; then
			d=$cand
			break
		fi
	done

	[ -n "$d" ] || d=$(ls -A "$M" | grep -av '%' | head -1)
	same=0
	bad=0

	for f in "$M/$d"/*; do
		[ -L "$f" ] && continue
		[ -f "$f" ] || continue
		b=${f##*/}
		f1=${b%% *}
		f2=${b#* }
		[ "$f2" = "$b" ] && f2=

		if "$ITSFS" get "$IMAGE" "$d;$f1 $f2" "$T/g.tmp" >/dev/null 2>&1 &&
			cmp -s "$T/g.tmp" "$f"; then
			same=$((same + 1))
		elif "$ITSFS" get -w "$IMAGE" "$d;$f1 $f2" "$T/g.tmp" >/dev/null 2>&1 &&
			cmp -s "$T/g.tmp" "$f"; then
			same=$((same + 1))
		else
			bad=$((bad + 1))
		fi
	done

	[ "$same" -gt 0 ] && [ "$bad" -eq 0 ] &&
		ok "all $same files in $d read the same through the mount as through \`get\`" ||
		no "$bad of $((same + bad)) files in $d differ"

	cleanup
	trap - EXIT
fi

echo
if [ "$rc" -eq 0 ]; then
	echo "$pass checks passed -- the mount reads what the commands read"
else
	echo "FAILED"
fi
exit $rc
