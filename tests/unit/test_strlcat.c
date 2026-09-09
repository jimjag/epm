/*
 * Regression test for epm_strlcat()/epm_strlcpy() in string.c.
 *
 * NOTE: this file is compiled from a *staged copy* of string.c and
 * epmstring.h (see run-tests.sh), sitting next to a stub config.h that
 * leaves HAVE_STRLCAT and HAVE_STRLCPY undefined - config.h has no include
 * guard and is found by an unqualified "config.h" search relative to
 * epmstring.h's own location, so the only reliable way to force the
 * fallback implementations to compile (instead of silently deferring to
 * whatever strlcat()/strlcpy() the host libc provides) is to stage
 * unmodified copies of string.c/epmstring.h next to our own config.h.
 *
 * Covers: an epm_strlcat() destination that already fills (or, pre-fix,
 * overflows) the buffer used to underflow `size` as a size_t and fall
 * through to an unbounded memcpy() - see review finding #3.
 */

#include "string.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    /*
     * Canaries bracket buf[] so the overflow this test targets is caught
     * even on a plain (non-ASan) build, on any platform this project
     * supports - not every host here has a sanitizer runtime available.
     */
    struct {
        unsigned char front_canary[64];
        char buf[8];
        unsigned char back_canary[64];
    } guarded;
    char *buf = guarded.buf;
    size_t ret;

    printf("test_strlcat:\n");

    memset(guarded.front_canary, 0xA5, sizeof(guarded.front_canary));
    memset(guarded.back_canary, 0xA5, sizeof(guarded.back_canary));

    /*
     * Destination already fills the buffer (dstlen == size - 1).  Before the
     * fix, `size -= dstlen + 1` underflowed to a huge size_t and the
     * subsequent memcpy() ran off the end of buf[].  ASan (when the test is
     * built with -fsanitize=address) will abort the process outright if this
     * regresses; the assertions below additionally check the *values* and
     * the canaries either side of buf[].
     */

    strcpy(buf, "AAAAAAA"); /* dstlen = 7, sizeof(buf) = 8 -> already full */
    ret = epm_strlcat(buf, "hello", sizeof(guarded.buf));
    CHECK(ret == 7 + 5, "full destination: returns would-be total length");
    CHECK(!strcmp(buf, "AAAAAAA"), "full destination: buffer left untouched");

    /* Ordinary truncating concatenation still works as before. */
    strcpy(buf, "AB");
    ret = epm_strlcat(buf, "CDEFGHIJK", sizeof(guarded.buf));
    CHECK(ret == 2 + 5, "partial room: copies only what fits");
    CHECK(!strcmp(buf, "ABCDEFG"), "partial room: buffer holds the truncated result");

    /* Ordinary non-truncating case. */
    buf[0] = '\0';
    ret = epm_strlcat(buf, "hi", sizeof(guarded.buf));
    CHECK(ret == 2 && !strcmp(buf, "hi"), "plenty of room: normal concatenation");

    /* size == 0 must not underflow either. */
    ret = epm_strlcat(buf, "x", 0);
    CHECK(ret == 1, "zero-size buffer: reports source length, does not crash");

    {
        size_t k;
        int canaries_ok = 1;

        for (k = 0; k < sizeof(guarded.front_canary); k++)
            if (guarded.front_canary[k] != 0xA5)
                canaries_ok = 0;
        for (k = 0; k < sizeof(guarded.back_canary); k++)
            if (guarded.back_canary[k] != 0xA5)
                canaries_ok = 0;

        CHECK(canaries_ok, "canaries either side of buf[] are untouched");
    }

    /*
     * epm_strlcpy() truncates correctly and always NUL-terminates.  Unlike
     * BSD's strlcpy(3), this codebase's fallback returns the *truncated*
     * length rather than the source's full length when it truncates - that
     * is pre-existing, unrelated behavior this test isn't asserting is
     * "correct" in the abstract, only that this fix didn't change it.
     */
    {
        char small[4];

        ret = epm_strlcpy(small, "hello", sizeof(small));
        CHECK(ret == 3, "strlcpy: returns the truncated length (pre-existing behavior)");
        CHECK(!strcmp(small, "hel"), "strlcpy: truncates to fit with NUL");
    }

    if (failures) {
        printf("test_strlcat: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_strlcat: ALL OK\n");
    return (0);
}
