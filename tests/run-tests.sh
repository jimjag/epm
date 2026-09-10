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
LIBEPM="$ROOT/libepm.a"

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

if [ ! -x "$EPM" ] || [ ! -x "$EPMINSTALL" ] || [ ! -f "$LIBEPM" ]; then
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
    "$ROOT/tar.c" "$ROOT/file.c" "$ROOT/run.c" "$ROOT/qprintf.c" "$ROOT/string.c" \
    "$ROOT/snprintf.c"; then
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

# --- test_get_uid_gid --------------------------------------------------------
if compile_test "$SCRATCH/test_get_uid_gid" "$ROOT/tests/unit/test_get_uid_gid.c" \
    "$LIBEPM"; then
    if "$SCRATCH/test_get_uid_gid" >"$SCRATCH/out.log" 2>&1; then
        pass "test_get_uid_gid (numeric owner never resolves to (uid_t)-1)"
    else
        fail "test_get_uid_gid - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_get_uid_gid failed to compile"
    cat "$SCRATCH/cc.log"
fi

# --- test_run_quote -----------------------------------------------------------
if compile_test "$SCRATCH/test_run_quote" "$ROOT/tests/unit/test_run_quote.c" "$LIBEPM"; then
    if "$SCRATCH/test_run_quote" >"$SCRATCH/out.log" 2>&1; then
        pass "test_run_quote (quotes/backslashes/spaces round-trip through run_command's parser)"
    else
        fail "test_run_quote - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_run_quote failed to compile"
    cat "$SCRATCH/cc.log"
fi

# --- test_macos_signing -------------------------------------------------------
# macos.c has no platform #ifdefs of its own, so this runs on any host.
if compile_test "$SCRATCH/test_macos_signing" "$ROOT/tests/unit/test_macos_signing.c" \
    "$LIBEPM"; then
    if "$SCRATCH/test_macos_signing" >"$SCRATCH/out.log" 2>&1; then
        pass "test_macos_signing (Mach-O magic and filetype; /etc,/var staging; inside-out bundle signing plan)"
    else
        fail "test_macos_signing - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_macos_signing failed to compile"
    cat "$SCRATCH/cc.log"
fi

# --- test_bsd_freebsd_pkg -----------------------------------------------------
# make_freebsd_modern_pkg() only compiles under __FreeBSD__; the test defines
# that itself so this FreeBSD-only code path gets exercised even when running
# on another platform (there is no FreeBSD CI runner for this project).
if compile_test "$SCRATCH/test_bsd_freebsd_pkg" "$ROOT/tests/unit/test_bsd_freebsd_pkg.c" \
    "$LIBEPM"; then
    if "$SCRATCH/test_bsd_freebsd_pkg" >"$SCRATCH/out.log" 2>&1; then
        pass "test_bsd_freebsd_pkg (directory ownership, @exec escaping, and dep notes in the pkg(8) manifest)"
    else
        fail "test_bsd_freebsd_pkg - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_bsd_freebsd_pkg failed to compile"
    cat "$SCRATCH/cc.log"
fi

# --- test_bsd_legacy_pkg ------------------------------------------------------
# The legacy pkg_create(8) writer, compiled WITHOUT -D__FreeBSD__ so this
# covers the generic *BSD (NetBSD/OpenBSD) spelling of the packing list -
# the variant test_bsd_freebsd_pkg does not reach.
if compile_test "$SCRATCH/test_bsd_legacy_pkg" "$ROOT/tests/unit/test_bsd_legacy_pkg.c" \
    "$LIBEPM"; then
    if "$SCRATCH/test_bsd_legacy_pkg" >"$SCRATCH/out.log" 2>&1; then
        pass "test_bsd_legacy_pkg (directory ownership and @exec escaping in the legacy *BSD plist)"
    else
        fail "test_bsd_legacy_pkg - see output below"
        cat "$SCRATCH/out.log"
    fi
else
    fail "test_bsd_legacy_pkg failed to compile"
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

section "macOS backend (macos.c)"

# --- a real pkgbuild/hdiutil run through a path containing a space ---------
# macos.c builds whole command lines for pkgbuild(1) and hdiutil(1) and hands
# them to run_command(), which does its own argv splitting with no shell.  An
# unquoted path with a space in it therefore split into two arguments and the
# build failed outright.  Only a real run exercises those call sites, so this
# is skipped anywhere the tools are absent.
if [ "$(uname -s)" = "Darwin" ] && have pkgbuild && have hdiutil; then
    cat >macostest.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
f 0755 root root /usr/local/bin/t payload.txt
EOF

    SPACEDIR="$WORK/macos out dir"
    rm -rf "$SPACEDIR"
    mkdir -p "$SPACEDIR"

    run_with_timeout 120 out.log "$EPM" -f macos --output-dir "$SPACEDIR" \
        macostest macostest.list
    status=$?

    if [ "$status" = 124 ]; then
        fail "macOS package build hung"
    elif [ "$status" != 0 ]; then
        fail "epm -f macos failed with an output path containing a space"
        cat out.log
    elif [ ! -f "$SPACEDIR/macostest.pkg" ]; then
        fail "pkgbuild produced no .pkg for an output path containing a space"
        cat out.log
    elif ! find "$SPACEDIR" -name '*.dmg' | grep -q .; then
        fail "hdiutil produced no .dmg for an output path containing a space"
        cat out.log
    else
        pass "a macOS .pkg and .dmg build through an output path containing a space"
    fi

    rm -rf "$SPACEDIR"

    # --- the same hdiutil call in the portable backend --------------------
    SPACEDIR="$WORK/portable out dir"
    rm -rf "$SPACEDIR"
    mkdir -p "$SPACEDIR"

    run_with_timeout 120 out.log "$EPM" -f portable --output-dir "$SPACEDIR" \
        ptabletest macostest.list
    status=$?

    if [ "$status" != 0 ]; then
        fail "epm -f portable failed with an output path containing a space"
        cat out.log
    elif ! find "$SPACEDIR" -name '*.dmg' | grep -q .; then
        fail "portable backend produced no .dmg for an output path containing a space"
        cat out.log
    else
        pass "the portable backend's disk image builds through a path containing a space"
    fi

    rm -rf "$SPACEDIR"

    # --- signing command lines are quoted -----------------------------------
    # productbuild --sign and codesign need a real Developer ID to succeed,
    # which CI does not have.  run_command() echoes each command at -vv
    # *before* forking, so the quoting can still be checked: assert the paths
    # arrive as single quoted arguments even though the tools then fail.
    SPACEDIR="$WORK/signed out dir"
    rm -rf "$SPACEDIR"
    mkdir -p "$SPACEDIR"

    # No EPM_APPLICATION_IDENTITY, so payload signing is skipped and the run
    # reaches the "productbuild --sign" call.
    unset EPM_APPLICATION_IDENTITY
    EPM_SIGNING_IDENTITY="Some Signer (O'Brien)" \
        run_with_timeout 120 out.log "$EPM" -vv -f macos-signed \
        --output-dir "$SPACEDIR" signedtest macostest.list

    sign_pat="--sign 'Some Signer (O\\'Brien)'"
    if grep -Fq -- "--root '$SPACEDIR/signedtest/Package'" out.log &&
        grep -Fq -- "--scripts '$SPACEDIR/signedtest/Scripts'" out.log &&
        grep -Fq -- "--package-path '$SPACEDIR/signedtest/Components'" out.log &&
        grep -Fq -- "productbuild --distribution '$SPACEDIR/signedtest/Distribution'" out.log &&
        grep -Fq -- "$sign_pat" out.log; then
        pass "pkgbuild and productbuild receive quoted paths and a quoted identity"
    else
        fail "pkgbuild/productbuild command lines were not quoted as expected"
        grep -i "pkgbuild\|productbuild" out.log | head -3
    fi

    if grep -q "EPM_APPLICATION_IDENTITY is not set" out.log; then
        pass "macos-signed without EPM_APPLICATION_IDENTITY warns about the unsigned payload"
    else
        fail "no warning for macos-signed without EPM_APPLICATION_IDENTITY"
    fi

    # With an identity set and a Mach-O in the payload, codesign is reached.
    cat >machotest.list <<EOF
%product Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
f 0755 root root /usr/local/bin/epm $ROOT/epm
EOF

    rm -rf "$SPACEDIR"
    mkdir -p "$SPACEDIR"

    EPM_APPLICATION_IDENTITY="Some Signer (O'Brien)" \
        run_with_timeout 120 out.log "$EPM" -vv -f macos-signed \
        --output-dir "$SPACEDIR" machotest machotest.list

    codesign_pat="--sign 'Some Signer (O\\'Brien)' '$SPACEDIR/machotest/Package/usr/local/bin/epm'"
    if grep -Fq -- "$codesign_pat" out.log; then
        pass "codesign receives a quoted identity and a quoted path"
    else
        fail "codesign command line was not quoted as expected"
        grep -i codesign out.log | head -2
    fi

    # --- ad-hoc identity builds end to end -----------------------------------
    # "-" needs no certificate, so this is the one signed build CI can finish:
    # the payload is signed and verified, the package and image are left
    # unsigned, and nothing asks Apple for a timestamp.
    rm -rf "$SPACEDIR"
    mkdir -p "$SPACEDIR"

    EPM_APPLICATION_IDENTITY=- \
        run_with_timeout 120 out.log "$EPM" -vv -k -f macos-signed \
        --output-dir "$SPACEDIR" machotest machotest.list
    status=$?

    if [ "$status" != 0 ]; then
        fail "ad-hoc macos-signed build failed"
        cat out.log
    elif grep -q -- "--timestamp" out.log; then
        fail "ad-hoc signing still asked for a secure timestamp"
    elif ! grep -q "product package is not signed" out.log ||
        ! grep -q "disk image is not signed" out.log; then
        fail "ad-hoc build did not report the unsigned package and image"
        cat out.log
    elif ! codesign --verify --strict "$SPACEDIR/machotest/Package/usr/local/bin/epm" \
        >/dev/null 2>&1; then
        fail "ad-hoc signed payload does not verify"
    elif [ ! -f "$SPACEDIR/machotest.pkg" ]; then
        fail "ad-hoc build produced no product package"
    else
        pass "an ad-hoc macos-signed build signs the payload and skips package and image signing"
    fi

    # --- the product archive: ownership, resources, choices ----------------
    # A non-root build cannot chown the staging tree, so the Bom must show
    # pkgbuild's recommended owners rather than the builder's uid.
    EXPAND="$WORK/expanded"
    rm -rf "$EXPAND"

    if [ "$(id -u)" != 0 ] && pkgutil --expand "$SPACEDIR/machotest.pkg" "$EXPAND" >/dev/null 2>&1; then
        if lsbom -p UGf "$EXPAND/machotest.pkg/Bom" 2>/dev/null | grep -q "^$(id -un)"; then
            fail "a non-root build baked the builder's uid into the payload"
            lsbom -p UGf "$EXPAND/machotest.pkg/Bom" | head -3
        else
            pass "a non-root build gets pkgbuild's recommended ownership"
        fi

        if grep -q '<readme file="payload.txt"/>' "$EXPAND/Distribution" &&
            [ -f "$EXPAND/Resources/payload.txt" ]; then
            pass "the product archive carries the %readme resource"
        else
            fail "the product archive lacks the %readme resource"
            cat "$EXPAND/Distribution"
        fi
    else
        skip "product archive inspection (needs a non-root build and pkgutil)"
    fi

    # --- subpackages, init scripts, identifiers and bundle relocation --------
    printf '#!/bin/sh\necho "$1"\n' >svc.sh
    mkdir -p Fake.app/Contents/MacOS
    cp /bin/ls Fake.app/Contents/MacOS/Fake
    printf '<?xml version="1.0"?><plist version="1.0"><dict><key>CFBundleIdentifier</key><string>org.example.fake</string><key>CFBundleExecutable</key><string>Fake</string><key>CFBundlePackageType</key><string>APPL</string></dict></plist>' \
        >Fake.app/Contents/Info.plist

    cat >subtest.list <<EOF
\$EPM_MACOS_IDENTIFIER=org.example.sub
%product Sub Test
%copyright 2026
%vendor Me
%readme payload.txt
%version 1.0
%description Main package
d 0755 root admin /Applications/Fake.app -
f 0644 root admin /Applications/Fake.app/Contents/Info.plist Fake.app/Contents/Info.plist
f 0755 root admin /Applications/Fake.app/Contents/MacOS/Fake Fake.app/Contents/MacOS/Fake
i 0755 root wheel fakesvc svc.sh
%subpackage extras
%description Extras
%description Extra things
f 0644 root wheel /usr/local/share/subtest/extra.txt payload.txt
%postinstall echo extras
EOF

    rm -rf "$SPACEDIR"
    mkdir -p "$SPACEDIR"

    run_with_timeout 120 out.log "$EPM" -k -f macos --output-dir "$SPACEDIR" \
        subtest subtest.list
    status=$?

    if [ "$status" != 0 ]; then
        fail "macos build with a subpackage, an init script and an application failed"
        cat out.log
    else
        pass "a macos build with a subpackage, an init script and an application succeeds"

        if [ -f "$SPACEDIR/subtest/Components/subtest.pkg" ] &&
            [ -f "$SPACEDIR/subtest/Components/subtest-extras.pkg" ] &&
            grep -q '<choice id="org.example.sub.extras" title="Extras" description="Extra things">' \
                "$SPACEDIR/subtest/Distribution" &&
            grep -q '<pkg-ref id="org.example.sub" version="1.0"' "$SPACEDIR/subtest/Distribution"; then
            pass "each subpackage is a component with its own installer choice and identifier"
        else
            fail "subpackage components or Distribution choices are missing"
            cat "$SPACEDIR/subtest/Distribution"
        fi

        if grep -q "echo extras" "$SPACEDIR/subtest-extras/Scripts/postinstall" 2>/dev/null &&
            ! grep -q "echo extras" "$SPACEDIR/subtest/Scripts/postinstall"; then
            pass "%postinstall lines stay with their subpackage"
        else
            fail "%postinstall lines were not split by subpackage"
        fi

        LAUNCHD="$SPACEDIR/subtest/Package/Library/LaunchDaemons/org.example.sub.fakesvc.plist"
        if [ -f "$SPACEDIR/subtest/Package/Library/StartupItems/fakesvc/fakesvc" ] &&
            [ -f "$LAUNCHD" ] && plutil -lint "$LAUNCHD" >/dev/null 2>&1 &&
            grep -q "SystemStarter" "$SPACEDIR/subtest/Scripts/postinstall" &&
            grep -q "launchctl bootstrap system /Library/LaunchDaemons/org.example.sub.fakesvc.plist" \
                "$SPACEDIR/subtest/Scripts/postinstall"; then
            pass "an init script gets both a StartupItem and a launchd job"
        else
            fail "init script staging is incomplete"
            cat "$SPACEDIR/subtest/Scripts/postinstall" 2>/dev/null
        fi

        if grep -A1 BundleIsRelocatable "$SPACEDIR/subtest/Component.plist" 2>/dev/null |
            grep -q '<false/>' &&
            ! grep -A1 BundleIsRelocatable "$SPACEDIR/subtest/Component.plist" |
            grep -q '<true/>'; then
            pass "application bundles are pinned to their list-file path"
        else
            fail "the component plist does not pin the application bundle"
            cat "$SPACEDIR/subtest/Component.plist" 2>/dev/null
        fi
    fi

    # --- the drag-install image ------------------------------------------
    cat >apptest.list <<EOF
%product App Test
%copyright 2026
%vendor Me
%readme payload.txt
%license payload.txt
%version 1.0
d 0755 root admin /Applications/Fake.app -
f 0644 root admin /Applications/Fake.app/Contents/Info.plist Fake.app/Contents/Info.plist
f 0755 root admin /Applications/Fake.app/Contents/MacOS/Fake Fake.app/Contents/MacOS/Fake
EOF

    rm -rf "$SPACEDIR"
    mkdir -p "$SPACEDIR"

    EPM_APPLICATION_IDENTITY=- \
        run_with_timeout 120 out.log "$EPM" -f macos-app --output-dir "$SPACEDIR" \
        apptest apptest.list
    status=$?

    APPDMG=$(find "$SPACEDIR" -name 'apptest-1.0*.dmg' | head -1)
    if [ "$status" != 0 ] || [ -z "$APPDMG" ]; then
        fail "macos-app build failed"
        cat out.log
    else
        # %license becomes a license agreement the Finder shows before
        # mounting; hdiutil asks for it on stdin.
        if echo N | hdiutil attach -noverify -nobrowse -readonly -mountrandom "$WORK" \
            "$APPDMG" >/dev/null 2>&1; then
            fail "macos-app image mounted without agreeing to the license"
        else
            pass "macos-app images carry the %license agreement"
        fi

        MOUNT=$(echo Y | hdiutil attach -noverify -nobrowse -readonly -mountrandom "$WORK" \
            "$APPDMG" 2>/dev/null | awk -F'\t' '/^\/dev\/.*Apple_HFS/ {sub(/^[ \t]+/, "", $NF); print $NF}')

        if [ -z "$MOUNT" ]; then
            fail "macos-app image does not mount after agreeing to the license"
        elif [ -d "$MOUNT/Fake.app" ] && [ -L "$MOUNT/Applications" ] &&
            [ "$(readlink "$MOUNT/Applications")" = /Applications ] &&
            codesign --verify --strict "$MOUNT/Fake.app" >/dev/null 2>&1; then
            pass "macos-app images hold the signed application and an /Applications link"
            hdiutil detach "$MOUNT" -force -quiet >/dev/null 2>&1
        else
            fail "macos-app image content is wrong"
            ls -l "$MOUNT"
            hdiutil detach "$MOUNT" -force -quiet >/dev/null 2>&1
        fi
    fi

    run_with_timeout 120 out.log "$EPM" -f macos-app --output-dir "$SPACEDIR" \
        apptest machotest.list
    if [ $? != 0 ] && grep -q "only packages files under /Applications" out.log; then
        pass "macos-app refuses files outside /Applications"
    else
        fail "macos-app accepted a file outside /Applications"
    fi

    unset EPM_APPLICATION_IDENTITY
    rm -rf "$SPACEDIR" "$EXPAND" Fake.app
else
    skip "macOS backend end-to-end build (needs Darwin with pkgbuild and hdiutil)"
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
