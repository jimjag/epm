/*
 * Regression test for get_uid()/get_gid() in file.c.
 *
 * file.c is linked directly rather than through libepm.a so this can run
 * without a full epm build; it references AooMode, which normally lives in
 * epm.c, so that's stubbed here too.
 *
 * Covers: a numeric owner/group whose value happens to equal (uid_t)-1 /
 * (gid_t)-1 must NOT be returned as-is - chown(2) treats -1 as "leave this
 * ID unchanged", so returning it silently defeats the fallback entirely
 * (the file keeps its build-time owner instead of resetting to root).
 */

#include "../../epm.h"
#include "../../file.c"

#include <stdio.h>
#include <limits.h>

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
    printf("test_get_uid_gid:\n");

    /* A real account name still resolves normally. */
    CHECK(get_uid("root") == 0, "get_uid(\"root\") resolves to 0");

    /* An ordinary numeric UID with no matching account is used literally. */
    CHECK(get_uid("801") == 801, "get_uid(\"801\") falls back to the literal value");
    CHECK(get_gid("802") == 802, "get_gid(\"802\") falls back to the literal value");

    /* A name that isn't numeric and doesn't resolve falls back to root. */
    CHECK(get_uid("no_such_user_xyz") == 0,
          "get_uid() of an unknown non-numeric name falls back to 0");

    /*
     * 2^32-1: fits easily in `long`/`unsigned long` (no strtoul overflow),
     * but truncating straight to a 32-bit uid_t lands on (uid_t)-1, which
     * chown(2) treats as "don't change this ID" - the fix must reject this,
     * not return it.
     */
    CHECK(get_uid("4294967295") == 0,
          "get_uid(\"4294967295\") is rejected, not returned as (uid_t)-1");
    CHECK(get_gid("4294967295") == 0,
          "get_gid(\"4294967295\") is rejected, not returned as (gid_t)-1");

    /* A value that overflows even `unsigned long` (ERANGE) must also be rejected. */
    CHECK(get_uid("999999999999999999999999") == 0,
          "get_uid() of an overflowing numeric string falls back to 0");

    /* A negative-looking numeric string must not wrap around via strtoul. */
    CHECK(get_uid("-1") == 0, "get_uid(\"-1\") does not wrap to a huge UID");

    /*
     * The numeric fallback is gated on a leading digit, so the forms strtol()
     * used to accept - a sign, leading whitespace - are no longer treated as
     * numeric and fall back to root rather than resolving to something the
     * list file did not literally say.
     */
    CHECK(get_uid("+5") == 0, "get_uid(\"+5\") is not treated as numeric");
    CHECK(get_uid(" 5") == 0, "get_uid(\" 5\") is not treated as numeric");
    CHECK(get_uid("5x") == 0, "get_uid(\"5x\") with trailing junk is rejected");
    CHECK(get_uid("") == 0, "get_uid(\"\") falls back to 0");
    CHECK(get_uid(NULL) == 0, "get_uid(NULL) falls back to 0");
    CHECK(get_gid("+5") == 0, "get_gid(\"+5\") is not treated as numeric");
    CHECK(get_gid(NULL) == 0, "get_gid(NULL) falls back to 0");

    /* Boundary: the largest value that's still safely representable and
     * distinct from the sentinel is preserved as a literal ID. */
    CHECK(get_uid("4294967294") == 4294967294u,
          "get_uid(\"4294967294\") (2^32-2) is still accepted");

    /*
     * The sentinel guard has to hold whether uid_t/gid_t are signed or
     * unsigned.  This host's uid_t is unsigned, so a plain get_uid() call
     * cannot tell a signedness-independent guard from one that only works
     * for unsigned types - instantiate the predicate over both spellings
     * instead.  A "val < (unsigned long)(type)-1" bound passes the unsigned
     * column and fails the signed one, because (unsigned long)(int)-1 is
     * ULONG_MAX and admits everything.
     */
    {
        unsigned long sentinel32 = 4294967295UL; /* truncates to (int32)-1 */

        CHECK(!ID_IS_USABLE(unsigned int, sentinel32),
              "the id guard rejects the sentinel for an unsigned 32-bit id type");
        CHECK(!ID_IS_USABLE(int, sentinel32),
              "the id guard rejects the sentinel for a SIGNED 32-bit id type");
        CHECK(ID_IS_USABLE(unsigned int, 4294967294UL),
              "the id guard still accepts 2^32-2 for an unsigned id type");
        CHECK(ID_IS_USABLE(int, 1000UL),
              "the id guard still accepts an ordinary id for a signed id type");
        CHECK(!ID_IS_USABLE(int, 3000000000UL),
              "the id guard rejects a value that does not fit a signed id type");
        CHECK(!ID_IS_USABLE(unsigned short, 70000UL),
              "the id guard rejects a value that does not fit a narrow id type");
    }

    if (failures) {
        printf("test_get_uid_gid: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_get_uid_gid: ALL OK\n");
    return (0);
}
