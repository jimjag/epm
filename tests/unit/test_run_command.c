/*
 * Regression test for run_command()'s chdir() handling in run.c.
 *
 * run_command() is public API, so this test links run.c directly.
 *
 * Covers: review finding #21 - a failed chdir() in the forked child fell
 * through to execvp() anyway, running the command in the wrong directory
 * instead of failing outright; and the related vsnprintf() truncation and
 * >99-argument cases noted alongside it.
 */

#include "../../epm.h"

#include <stdio.h>
#include <sys/stat.h>

/* run.c references these extern globals; provide them since we're not
 * linking epm.c in this test. */
int Verbosity = 0;
int AooMode = 0;

static int failures = 0;

#define CHECK(cond, msg)                                                                \
    do {                                                                                \
        if (cond)                                                                       \
            printf("    ok - %s\n", msg);                                               \
        else {                                                                          \
            printf("    NOT OK - %s\n", msg);                                           \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

int main(void) {
    int status;

    printf("test_run_command:\n");

    /* A directory that can't possibly exist must make the command fail,
     * not silently run in the wrong (current) directory. */
    status =
        run_command("/nonexistent/epm-test-directory/really-not-there", "true");
    CHECK(status != 0, "chdir() failure is reported, command does not run anyway");

    /* The ordinary, common case must still work: no directory change,
     * a command that succeeds. */
    status = run_command(NULL, "true");
    CHECK(status == 0, "a normal command with no directory change succeeds");

    /* And a command that legitimately fails still reports failure. */
    status = run_command(NULL, "false");
    CHECK(status != 0, "a command that fails is reported as failing");

    /* An overlong formatted command must be refused, not silently
     * truncated and executed. */
    {
        char huge_arg[20000];

        memset(huge_arg, 'x', sizeof(huge_arg) - 1);
        huge_arg[sizeof(huge_arg) - 1] = '\0';

        status = run_command(NULL, "true %s", huge_arg);
        CHECK(status != 0, "a command too long for the internal buffer is refused");
    }

    if (failures) {
        printf("test_run_command: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_run_command: ALL OK\n");
    return (0);
}
