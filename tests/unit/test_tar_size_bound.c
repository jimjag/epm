/*
 * Regression test for tar_header()'s file-size bound in tar.c.
 *
 * tar_header() is public API (declared in epm.h, not static), so this test
 * links tar.c directly and drives it through a real tarf_t, rather than
 * needing a many-gigabyte file on disk to prove the point - only the
 * declared `size` argument matters to tar_header() itself; tar_file() is
 * what actually streams file content, and isn't exercised here.
 *
 * Covers: review finding #18 - a file size that doesn't fit in the 11-octal-
 * digit tar header field was silently truncated via `(unsigned)size` instead
 * of being rejected, corrupting the archive with no error.
 */

#include "../../epm.h"

#include <stdio.h>

/* tar.c and file.c reference these extern globals; provide them since we're
 * not linking epm.c in this test. */
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
    tarf_t *tar;
    int result;

    printf("test_tar_size_bound:\n");

    tar = tar_open("/tmp/epm-test-size-bound.tar", 0);
    CHECK(tar != NULL, "tar_open() succeeds");

    if (!tar) {
        printf("test_tar_size_bound: %d FAILURE(S)\n", failures + 1);
        return (1);
    }

    /* A size that fits in 11 octal digits (8^11 - 1 = 8589934591) must still
     * work normally. */
    result = tar_header(tar, TAR_NORMAL, 0644, (off_t)1024, 0, "root", "root",
                        "small-file.txt", NULL);
    CHECK(result == 0, "a normal, small size is accepted");

    /* A size that does NOT fit must be rejected, not silently truncated. */
    result = tar_header(tar, TAR_NORMAL, 0644, (off_t)8589934592LL, 0, "root", "root",
                        "too-big-file.bin", NULL);
    CHECK(result == -1, "a size exceeding the tar header field's capacity is rejected");

    /* The largest representable size must still be accepted (boundary check). */
    result = tar_header(tar, TAR_NORMAL, 0644, (off_t)8589934591LL, 0, "root", "root",
                        "max-size-file.bin", NULL);
    CHECK(result == 0, "the largest representable size (8^11 - 1) is accepted");

    /* A negative size (e.g. from an off_t overflow upstream) must be
     * rejected too. */
    result = tar_header(tar, TAR_NORMAL, 0644, (off_t)-1, 0, "root", "root",
                        "negative-size-file.bin", NULL);
    CHECK(result == -1, "a negative size is rejected");

    tar_close(tar);
    unlink("/tmp/epm-test-size-bound.tar");

    if (failures) {
        printf("test_tar_size_bound: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_tar_size_bound: ALL OK\n");
    return (0);
}
