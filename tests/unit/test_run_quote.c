/*
 * Regression test for run_quote()/split_args() in run.c.
 *
 * Covers: codesign_path(), notarize_file(), and the FreeBSD pkg(8)
 * invocation used to interpolate signing identities, entitlements paths,
 * notary profiles, and build paths into run_command() format strings via a
 * bare '%s' with no escaping. run_command() has no shell of its own -
 * split_args() is its own whitespace/quote tokenizer - so an embedded '
 * (e.g. a Developer ID "Common Name" like "O'Brien's Software LLC") closed
 * the quoted argv token early and corrupted the rest of argv.
 *
 * This drives run_quote()'s output back through the *real* split_args()
 * parser (not a reimplementation) and checks the original value comes back
 * out unchanged, for exactly the kind of values that broke before the fix.
 */

#include "../../epm.h"

#include <stdio.h>
#include <string.h>

/* run.c references this extern global; provide it since we're not linking
 * epm.c in this test. */
int Verbosity = 0;

static int failures = 0;

static void check_roundtrip(const char *label, const char *input) {
    char quoted[512], buf[600], *argv[10];
    int argc;

    if (run_quote(quoted, sizeof(quoted), input)) {
        printf("    NOT OK - %s: run_quote() reported an error\n", label);
        failures++;
        return;
    }

    snprintf(buf, sizeof(buf), "prog %s tail", quoted);
    argc = split_args(buf, argv, 10);

    if (argc == 3 && !strcmp(argv[0], "prog") && !strcmp(argv[1], input) &&
        !strcmp(argv[2], "tail"))
        printf("    ok - %s round-trips through the real parser\n", label);
    else {
        printf("    NOT OK - %s: input=[%s] quoted=[%s] argc=%d argv[1]=[%s]\n", label,
               input, quoted, argc, argc > 1 ? argv[1] : "?");
        failures++;
    }
}

int main(void) {
    printf("test_run_quote:\n");

    check_roundtrip("plain value", "hello");
    check_roundtrip("embedded space", "hello world");
    check_roundtrip("Developer ID with apostrophes", "O'Brien's Software LLC");
    check_roundtrip("Windows-style backslash path", "C:\\Users\\bob");
    check_roundtrip("quote and backslash together", "it's a \\test\\");
    check_roundtrip("empty string", "");
    check_roundtrip("only a quote", "'");
    check_roundtrip("only a backslash", "\\");
    check_roundtrip("embedded double quote", "he said \"hi\"");

    /*
     * The macOS backend builds whole command lines out of an output
     * directory chosen with -o; an unquoted path with a space in it used to
     * split into two argv entries and silently corrupt the rest of the
     * command.  Check the real format string, not just an isolated value.
     */
    {
        char qscripts[512], qroot[512], qpkg[512], buf[2048], *argv[20];
        int argc;

        if (run_quote(qscripts, sizeof(qscripts), "/out dir/prod/Resources") ||
            run_quote(qroot, sizeof(qroot), "/out dir/prod/Package") ||
            run_quote(qpkg, sizeof(qpkg), "/out dir/prod.pkg")) {
            printf("    NOT OK - pkgbuild paths could not be quoted\n");
            failures++;
        } else {
            snprintf(buf, sizeof(buf),
                     "/usr/bin/pkgbuild --identifier prod --version 1.0 --ownership "
                     "preserve --scripts %s --root %s %s",
                     qscripts, qroot, qpkg);
            argc = split_args(buf, argv, 20);

            if (argc == 12 && !strcmp(argv[8], "/out dir/prod/Resources") &&
                !strcmp(argv[10], "/out dir/prod/Package") &&
                !strcmp(argv[11], "/out dir/prod.pkg"))
                printf("    ok - a pkgbuild command line survives spaces in -o "
                       "output paths\n");
            else {
                printf("    NOT OK - pkgbuild command line split into %d args, "
                       "argv[8]=[%s]\n",
                       argc, argc > 8 ? argv[8] : "?");
                failures++;
            }
        }
    }

    /*
     * split_args() is exported, so its argv bound has to hold for any
     * maxargs a caller passes.  One slot is reserved for the terminating
     * NULL, so maxargs < 2 cannot hold an argument at all and must write
     * nothing - a canary past the array catches the off-by-one that used to
     * store argv[1] into a one-element array.
     */
    {
        struct {
            char *argv[1];
            char *canary;
        } one;
        char buf[] = "alpha beta gamma";
        int argc;

        one.canary = (char *)0xAAAA;
        argc = split_args(buf, one.argv, 1);

        if (argc == 0 && one.canary == (char *)0xAAAA)
            printf("    ok - split_args(maxargs=1) writes nothing past the array\n");
        else {
            printf("    NOT OK - split_args(maxargs=1) argc=%d canary=%p\n", argc,
                   (void *)one.canary);
            failures++;
        }
    }

    {
        struct {
            char *argv[2];
            char *canary;
        } two;
        char buf[] = "alpha beta gamma";
        int argc;

        two.canary = (char *)0xAAAA;
        argc = split_args(buf, two.argv, 2);

        if (argc == 1 && two.argv[1] == NULL && two.canary == (char *)0xAAAA &&
            !strcmp(two.argv[0], "alpha beta gamma"))
            printf("    ok - split_args(maxargs=2) yields one argument plus the "
                   "NULL terminator\n");
        else {
            printf("    NOT OK - split_args(maxargs=2) argc=%d canary=%p\n", argc,
                   (void *)two.canary);
            failures++;
        }
    }

    {
        /* More arguments than fit: split what we can, terminate, stay in bounds. */
        struct {
            char *argv[4];
            char *canary;
        } four;
        char buf[] = "a b c d e f";
        int argc;

        four.canary = (char *)0xAAAA;
        argc = split_args(buf, four.argv, 4);

        if (argc == 3 && four.argv[3] == NULL && four.canary == (char *)0xAAAA)
            printf("    ok - split_args stops at maxargs-1 arguments and stays in "
                   "bounds\n");
        else {
            printf("    NOT OK - split_args(maxargs=4) argc=%d canary=%p\n", argc,
                   (void *)four.canary);
            failures++;
        }
    }

    /*
     * split_args() also honors "..." spans.  run_quote() never emits them,
     * but format strings elsewhere contain literal double quotes, so the
     * branch has to keep working.
     */
    {
        char buf[] = "prog \"two words\" \"esc\\\"aped\" tail";
        char *argv[10];
        int argc = split_args(buf, argv, 10);

        if (argc == 4 && !strcmp(argv[1], "two words") &&
            !strcmp(argv[2], "esc\"aped") && !strcmp(argv[3], "tail"))
            printf("    ok - double-quoted spans and their \\ escapes still parse\n");
        else {
            printf("    NOT OK - double-quoted span: argc=%d argv[1]=[%s] argv[2]=[%s]\n",
                   argc, argc > 1 ? argv[1] : "?", argc > 2 ? argv[2] : "?");
            failures++;
        }
    }

    /* A buffer too small to hold even '' must be refused, not written to. */
    {
        char two[2];

        two[0] = 'Z';
        two[1] = 'Z';

        if (run_quote(two, sizeof(two), "") && two[0] == 'Z' && two[1] == 'Z')
            printf("    ok - run_quote refuses a buffer smaller than the quotes\n");
        else {
            printf("    NOT OK - run_quote wrote into a 2-byte buffer\n");
            failures++;
        }
    }

    /*
     * run_quote()'s exact buffer boundary: "ab" needs 5 bytes ('a', 'b', two
     * quotes, NUL), so 5 must succeed and 4 must be refused.
     */
    {
        char exact[5], tight[4];

        if (!run_quote(exact, sizeof(exact), "ab") && !strcmp(exact, "'ab'"))
            printf("    ok - run_quote fills an exactly-sized buffer\n");
        else {
            printf("    NOT OK - run_quote failed on an exactly-sized buffer\n");
            failures++;
        }

        if (run_quote(tight, sizeof(tight), "ab"))
            printf("    ok - run_quote refuses a buffer one byte too small\n");
        else {
            printf("    NOT OK - run_quote accepted a too-small buffer\n");
            failures++;
        }
    }

    /* A value that can't fit even with quoting must be reported, not truncated. */
    {
        char tiny[4];
        int rc = run_quote(tiny, sizeof(tiny), "way too long for this buffer");

        if (rc)
            printf("    ok - an oversized value is reported, not silently truncated\n");
        else {
            printf("    NOT OK - an oversized value was not reported as an error\n");
            failures++;
        }
    }

    if (failures) {
        printf("test_run_quote: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_run_quote: ALL OK\n");
    return (0);
}
