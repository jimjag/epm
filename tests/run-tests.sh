#!/bin/sh
#
# Regression test suite for the ESP Package Manager (EPM).
#
# Run via "make test" (which builds epm/epminstall/mkepmlist first), or
# directly as "sh tests/run-tests.sh" from a tree that has already been
# built.  Written in POSIX sh - no bashisms - since this is expected to run
# on every platform EPM itself targets.
#
# Exits 0 if every check passes, 1 if any check fails or is misconfigured.
#

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

EPM="$ROOT/epm"
EPMINSTALL="$ROOT/epminstall"
MKEPMLIST="$ROOT/mkepmlist"

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/epm-tests.XXXXXX") || exit 1
cleanup() {
    [ -n "${KEEP_SCRATCH:-}" ] || rm -rf "$SCRATCH"
}
trap cleanup EXIT INT TERM

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# --------------------------------------------------------------------------
# Small test-reporting helpers
# --------------------------------------------------------------------------

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "  ok - $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "  NOT OK - $1"
}

skip() {
    SKIP_COUNT=$((SKIP_COUNT + 1))
    echo "  skip - $1"
}

section() {
    echo ""
    echo "== $1 =="
}

have() {
    command -v "$1" >/dev/null 2>&1
}

# Run "$@" in the background, kill it if it hasn't finished within roughly
# $to_secs seconds; combined stdout/stderr goes to $log_file.  Portable
# stand-in for GNU coreutils' timeout(1), which several of the platforms EPM
# supports do not ship.
run_with_timeout() {
    to_secs=$1
    log_file=$2
    shift 2
    "$@" >"$log_file" 2>&1 &
    bgpid=$!
    i=0
    while kill -0 "$bgpid" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -ge "$to_secs" ]; then
            kill -9 "$bgpid" 2>/dev/null
            wait "$bgpid" 2>/dev/null
            return 124
        fi
        sleep 1
    done
    wait "$bgpid"
    return $?
}

# --------------------------------------------------------------------------
# Preconditions
# --------------------------------------------------------------------------

if [ ! -x "$EPM" ] || [ ! -x "$EPMINSTALL" ]; then
    echo "tests/run-tests.sh: epm and epminstall must be built first (run 'make')."
    exit 1
fi

CC=${CC:-cc}
if ! have "$CC"; then
    echo "tests/run-tests.sh: no C compiler ('$CC') found; cannot build unit tests."
    exit 1
fi

# Try to compile with -fsanitize=address; fall back to a plain build if the
# toolchain doesn't have a sanitizer runtime available.
compile_test() {
    out=$1
    shift
    if "$CC" -fsanitize=address -g -O0 -Wall -o "$out" "$@" >"$SCRATCH/cc.log" 2>&1; then
        return 0
    fi
    "$CC" -g -O0 -Wall -o "$out" "$@" >"$SCRATCH/cc.log" 2>&1
}

# ==========================================================================
# Unit tests - link directly against the real source files
# ==========================================================================

section "Unit tests"

# --- test_update_architecture ---------------------------------------------
if compile_test "$SCRATCH/test_update_architecture" \
    "$ROOT/tests/unit/test_update_architecture.c" "$ROOT/support.c"; then
    if "$SCRATCH/test_update_architecture" >"$SCRATCH/out.log" 2>&1; then
        pass "test_update_architecture (arm64/ppc64le no longer collapsed - finding #17)"
    else
        fail "test_update_architecture - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_update_architecture failed to compile"
    cat "$SCRATCH/cc.log"
fi

# --- test_tar_size_bound ---------------------------------------------------
if compile_test "$SCRATCH/test_tar_size_bound" "$ROOT/tests/unit/test_tar_size_bound.c" \
    "$ROOT/tar.c"; then
    if "$SCRATCH/test_tar_size_bound" >"$SCRATCH/out.log" 2>&1; then
        pass "test_tar_size_bound (oversized tar entries rejected - finding #18)"
    else
        fail "test_tar_size_bound - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_tar_size_bound failed to compile"
    cat "$SCRATCH/cc.log"
fi

# --- test_run_command -------------------------------------------------------
if compile_test "$SCRATCH/test_run_command" "$ROOT/tests/unit/test_run_command.c" \
    "$ROOT/run.c"; then
    if "$SCRATCH/test_run_command" >"$SCRATCH/out.log" 2>&1; then
        pass "test_run_command (failed chdir() no longer runs anyway - finding #21)"
    else
        fail "test_run_command - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_run_command failed to compile"
    cat "$SCRATCH/cc.log"
fi

# --- test_strlcat ------------------------------------------------------------
# This one needs the fallback epm_strlcat()/epm_strlcpy() in string.c to
# actually be compiled, which only happens when HAVE_STRLCAT/HAVE_STRLCPY are
# undefined.  config.h has no include guard and is always found relative to
# epmstring.h's own directory, so we stage fresh copies of string.c and
# epmstring.h next to a stub config.h rather than fighting the preprocessor's
# search order.
STRLCAT_DIR="$SCRATCH/strlcat"
mkdir -p "$STRLCAT_DIR"
cp "$ROOT/string.c" "$ROOT/epmstring.h" "$STRLCAT_DIR/"
cp "$ROOT/tests/unit/stub-config/config.h" "$STRLCAT_DIR/config.h"
cp "$ROOT/tests/unit/test_strlcat.c" "$STRLCAT_DIR/test_strlcat.c"

if (cd "$STRLCAT_DIR" && compile_test "$STRLCAT_DIR/test_strlcat" "test_strlcat.c"); then
    if "$STRLCAT_DIR/test_strlcat" >"$SCRATCH/out.log" 2>&1; then
        pass "test_strlcat (fallback strlcat/strlcpy underflow fixed - finding #3)"
    else
        fail "test_strlcat - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_strlcat failed to compile"
    cat "$SCRATCH/cc.log"
fi

# ==========================================================================
# Integration tests - drive the real epm/epminstall binaries
# ==========================================================================

WORK="$SCRATCH/work"
mkdir -p "$WORK"
cd "$WORK" || exit 1

# A minimal, always-installable source file every test list can point at.
echo "hello" >payload.txt

section "Parser robustness (dist.c)"

# --- empty %description file ------------------------------------------------
: >empty.txt
cat >empty-desc.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme empty.txt
%version 1.0
%description <empty.txt
f 0755 root root /usr/bin/t payload.txt
EOF

if "$EPM" -f portable emptydesc empty-desc.list >out.log 2>&1; then
    pass "empty %description file no longer underflows the read buffer (finding #1)"
else
    fail "empty %description file: epm failed unexpectedly"
    cat out.log
fi

# --- %include nesting past the limit ---------------------------------------
n=1
while [ "$n" -le 14 ]; do
    next=$((n + 1))
    echo "%include inc$next.list" >"inc$n.list"
    n=$next
done
echo "%vendor Nested" >inc15.list

cat >deep-include.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
%include inc1.list
f 0755 root root /usr/bin/t payload.txt
EOF

if "$EPM" -f portable deepinclude deep-include.list >out.log 2>&1; then
    if grep -qi "too many nested" out.log; then
        pass "%include nesting past the limit is rejected cleanly (finding #2)"
    else
        fail "%include nesting past the limit: expected a warning, got none"
        cat out.log
    fi
else
    fail "%include nesting past the limit: epm failed unexpectedly"
    cat out.log
fi

# --- self-including list file (would have recursed/overflowed forever) ----
cat >self.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
%include self.list
f 0755 root root /usr/bin/t payload.txt
EOF

run_with_timeout 10 out.log "$EPM" -f portable selfinclude self.list
status=$?
if [ "$status" = 124 ]; then
    fail "self-including list file: epm hung (would have recursed forever)"
elif grep -qi "circular" out.log; then
    pass "self-including list file is detected and rejected (finding #2)"
else
    fail "self-including list file: expected a 'circular' warning"
    cat out.log
fi

# --- %literal() with no section name ---------------------------------------
cat >empty-literal.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
%literal() something
f 0755 root root /usr/bin/t payload.txt
EOF

"$EPM" -f deb emptyliteral empty-literal.list >out.log 2>&1
status=$?
if [ "$status" -gt 128 ]; then
    fail "%literal() with no section name: epm crashed (signal $((status - 128)))"
elif grep -qi "no section name" out.log; then
    pass "%literal() with no section name is rejected, not a NULL deref (finding #5/#6)"
else
    fail "%literal() with no section name: expected a clear error message"
    cat out.log
fi

section "write_dist() / epminstall round-tripping"

# --- %literal(control) survives a write_dist() round-trip ------------------
cat >literal.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
%literal(control) X-Custom: yes
f 0755 root root /usr/bin/t payload.txt
EOF

mkdir -p installdir
if "$EPMINSTALL" --list-file literal.list -d installdir/some/dir >out.log 2>&1; then
    if grep -q '^%literal(control) X-Custom: yes$' literal.list; then
        pass "%literal(control) survives a write_dist() round-trip (finding #4)"
    else
        fail "%literal(control) was dropped or corrupted by write_dist()"
        cat literal.list
    fi
else
    fail "epminstall failed on a list file with a %literal(control) section"
    cat out.log
fi

# --- add_file() clears the new entry (no heap garbage in options) ---------
cat >noopts.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
f 0755 root root /usr/bin/t payload.txt
EOF

if "$EPMINSTALL" --list-file noopts.list -d installdir2 >out.log 2>&1; then
    # A clean entry's line ends right after the source path - nothing but
    # whitespace/EOL should follow "payload.txt" (or "-" for the directory
    # entries epminstall itself adds).
    if grep -Eq '^f 0755 root root /usr/bin/t payload\.txt[[:space:]]*$' noopts.list; then
        pass "add_file() zeroes new entries - no heap garbage in options (finding #14)"
    else
        fail "unexpected trailing content after the source path - possible garbage"
        cat noopts.list
    fi
else
    fail "epminstall failed on an options-less list file"
    cat out.log
fi

# --- options survive the write_dist() separator fix ------------------------
cat >withopts.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
f 0755 root root /usr/bin/t payload.txt nostrip()
EOF

if "$EPMINSTALL" --list-file withopts.list -d installdir3 >out.log 2>&1; then
    if grep -q 'payload\.txt nostrip()' withopts.list; then
        pass "file options round-trip with a proper separator (finding #15)"
    else
        fail "file options were glued onto the source path with no separator"
        cat withopts.list
    fi
else
    fail "epminstall failed on a list file with file options"
    cat out.log
fi

# --- symlinks are detected via lstat(), not stat() -------------------------
ln -sf payload.txt alink

cat >symlink2arg.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
EOF
cp symlink2arg.list symlink2arg.list.orig
if "$EPMINSTALL" --list-file symlink2arg.list alink /usr/bin/alink >out.log 2>&1; then
    if grep -Eq '^l 0[0-7]+ [^ ]+ [^ ]+ /usr/bin/alink' symlink2arg.list; then
        pass "epminstall detects a symlink given as a direct rename (finding #16)"
    else
        fail "symlink (2-arg form) was recorded as something other than type 'l'"
        cat symlink2arg.list
    fi
else
    fail "epminstall failed installing a symlink (2-arg form)"
    cat out.log
fi

cat >symlinkdir.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
EOF
# epminstall only takes the "install into a directory" path (as opposed to
# treating a lone 2nd argument as a literal destination filename) when more
# than one source file is given - so pass a second, ordinary source here to
# land in that code path rather than the direct-rename one exercised above.
if "$EPMINSTALL" --list-file symlinkdir.list alink payload.txt /usr/share/somedir \
    >out.log 2>&1; then
    if grep -Eq '^l 0[0-7]+ [^ ]+ [^ ]+ /usr/share/somedir/alink' symlinkdir.list; then
        pass "epminstall detects a symlink installed into a directory (finding #16)"
    else
        fail "symlink (directory form) was recorded as something other than type 'l'"
        cat symlinkdir.list
    fi
else
    fail "epminstall failed installing a symlink (directory form)"
    cat out.log
fi

section "Debian backend (deb.c)"

# --- multi-line description is properly escaped in the control file -------
cat >multiline-desc.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
%description <<END
First line of the description.

Provides: injected-virtual-package
END
f 0755 root root /usr/bin/t payload.txt
EOF

if "$EPM" -k -f deb multilinedesc multiline-desc.list >out.log 2>&1; then
    control=$(find . -path "*multilinedesc*/DEBIAN/control" | head -1)
    if [ -n "$control" ]; then
        # Everything from "Description:" to EOF must be either that line
        # itself or a continuation line starting with a space - including
        # the embedded blank line, which must become a literal " .", and the
        # embedded "Provides:" text, which must NOT appear as a bare,
        # unindented field (that would mean it injected a new control field).
        if awk '
            /^Description:/ { indesc = 1; next }
            indesc && $0 !~ /^ / { bad = 1 }
            END { exit bad }
        ' "$control" &&
            grep -qx ' \.' "$control" &&
            ! grep -qx 'Provides: injected-virtual-package' "$control"; then
            pass "multi-line %description is written as proper control continuation lines (finding #11)"
        else
            fail "control file has an unescaped continuation line - possible field injection"
            cat "$control"
        fi
    else
        fail "could not find the generated DEBIAN/control file"
    fi
else
    fail "epm -f deb failed on a multi-line %description"
    cat out.log
fi

# --- arm64/ppc64le get real Debian architecture names ----------------------
cat >archtest.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
f 0755 root root /usr/bin/t payload.txt
EOF

if "$EPM" -k -a ppc64le -f deb archtest archtest.list >out.log 2>&1; then
    control=$(find . -path "*archtest*/DEBIAN/control" | head -1)
    if [ -n "$control" ] && grep -q '^Architecture: ppc64el$' "$control"; then
        pass "ppc64le maps to Debian's real 'ppc64el' architecture name (finding #17)"
    else
        fail "expected 'Architecture: ppc64el' in the control file"
        [ -n "$control" ] && cat "$control"
    fi
else
    fail "epm -f deb failed building for -a ppc64le"
    cat out.log
fi

# --- --aoo-mode's Installed-Size: doesn't crash or read garbage ------------
if "$EPM" --aoo-mode -k -f deb aootest archtest.list >out.log 2>&1; then
    control=$(find . -path "*aootest*/DEBIAN/control" | head -1)
    if [ -n "$control" ] && grep -Eq '^Installed-Size: [0-9]+$' "$control"; then
        pass "--aoo-mode Installed-Size: is a clean integer (finding #9)"
    else
        fail "Installed-Size: missing or not a clean integer"
        [ -n "$control" ] && cat "$control"
    fi
else
    fail "epm --aoo-mode -f deb failed"
    cat out.log
fi

section "Portable backend (portable.c / tar.c)"

# --- shell metacharacters in the output directory do not get executed -----
rm -f "$SCRATCH/PWNED"
INJECT_DIR='evil$(touch '"$SCRATCH"'/PWNED)dir'
if "$EPM" -z -f portable --output-dir "$INJECT_DIR" injecttest noopts.list \
    >out.log 2>&1; then
    :
fi
if [ -e "$SCRATCH/PWNED" ]; then
    fail "shell metacharacters in --output-dir were executed via popen() (finding #12)"
else
    pass "shell metacharacters in a directory name are not executed (finding #12)"
fi

# --- .pss is cleaned up alongside .psw for patch-capable builds -----------
cat >patchcapable.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
F 0755 root root /usr/bin/t payload.txt
EOF

mkdir -p patchout
if "$EPM" -f portable --output-dir patchout patchcapable patchcapable.list \
    >out.log 2>&1; then
    if find patchout -name '*.pss' | grep -q . || find patchout -name '*.psw' | grep -q .; then
        fail "stray .pss/.psw file left behind after a patch-capable build (finding #19)"
        find patchout -name '*.ps?'
    else
        pass "no stray .pss/.psw files left behind (finding #19)"
    fi
else
    fail "epm -f portable failed on a patch-capable list file"
    cat out.log
fi

section "Command-line validation (epm.c)"

# --- an over-length --output-dir is rejected, not overflowed ---------------
longdir=$(awk 'BEGIN { s = ""; for (i = 0; i < 9000; i++) s = s "a"; print s }')
"$EPM" -f portable --output-dir "$longdir" longdirtest noopts.list >out.log 2>&1
status=$?
if [ "$status" -gt 128 ]; then
    fail "an over-length --output-dir crashed epm (signal $((status - 128))) (finding #8)"
else
    pass "an over-length --output-dir does not crash epm (finding #8)"
fi

# --- product names with invalid characters are now rejected outright ------
"$EPM" -f portable 'bad name!' noopts.list >out.log 2>&1
status=$?
if [ "$status" = 0 ]; then
    fail "a product name with invalid characters was accepted (finding #13)"
else
    pass "a product name with invalid characters is rejected (finding #13)"
fi

# ==========================================================================
# Summary
# ==========================================================================

section "Summary"
echo "  $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"

if [ "$FAIL_COUNT" -gt 0 ]; then
    exit 1
fi

exit 0
