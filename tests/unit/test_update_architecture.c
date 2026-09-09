/*
 * Regression test for update_architecture() in dist.c.
 *
 * update_architecture() is static, so the only way to exercise it directly
 * (short of removing "static" from the shipped source) is to pull dist.c in
 * textually and call it from the same translation unit.  Unlike
 * test_strlcat.c, this test wants the project's *real* config.h - it isn't
 * forcing any alternate code path - so no staging is required; dist.c's own
 * quote-includes resolve normally against the real repo tree.
 *
 * Covers: review finding #17 - arm64/aarch64 and ppc64le were being
 * collapsed onto the same generic name as 32-bit arm/ppc.
 */

#include "../../dist.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* dist.c references this extern global; provide it since we're not linking
 * epm.c in this test. */
int Verbosity = 0;

static int failures = 0;

static void check_arch(const char *input, const char *expected) {
    char buf[64];

    strlcpy(buf, input, sizeof(buf));
    update_architecture(buf, sizeof(buf));

    if (!strcmp(buf, expected))
        printf("    ok - %-12s -> %s\n", input, buf);
    else {
        printf("    NOT OK - %-12s -> %-12s (expected %s)\n", input, buf, expected);
        failures++;
    }
}

int main(void) {
    printf("test_update_architecture:\n");

    /* 64-bit ARM must stay distinct from 32-bit ARM... */
    check_arch("arm64", "arm64");
    check_arch("aarch64", "arm64");
    check_arch("armv7l", "arm");
    check_arch("armv6", "arm");

    /* ...and 64-bit little-endian PowerPC distinct from 32-bit. */
    check_arch("ppc64le", "ppc64le");
    check_arch("powerpc64le", "ppc64le");
    check_arch("ppc", "powerpc");
    check_arch("powerpc", "powerpc");

    /* Pre-existing synonyms must still work. */
    check_arch("x86_64", "x86_64");
    check_arch("i686", "intel");
    check_arch("i386", "intel");
    check_arch("sun4u", "sparc");

    if (failures) {
        printf("test_update_architecture: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_update_architecture: ALL OK\n");
    return (0);
}
