/*
 * Regression tests for bsd.c's legacy pkg_create(8) packing-list writer.
 *
 * make_subpackage() is the code path used on NetBSD and OpenBSD, and on
 * FreeBSD installations that still ship pkg_create(8).  It is static, so
 * this test #includes bsd.c directly.  Unlike test_bsd_freebsd_pkg.c it is
 * deliberately compiled WITHOUT -D__FreeBSD__, so it exercises the generic
 * *BSD spelling of the plist - the variant no other test touches.
 *
 * The directory-ownership @exec block here is the original that the newer
 * pkg(8) writer was copied from.  Both emit shell commands that run as root
 * at install time, so both have to escape whitespace and metacharacters in
 * the path (via qprintf) rather than writing it raw.
 *
 * make_subpackage() ends by shelling out to a real pkg_create(8), which
 * does not exist on this host - that failure is expected.  The plist is
 * written and closed before that point, which is what these checks read.
 */

#include "../../epm.h"
#include "../../bsd.c"

#include <stdio.h>
#include <string.h>

int Verbosity = 0;
int KeepFiles = 1; /* keep the generated files around so we can inspect them */
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

static int slurp(const char *path, char *buf, size_t bufsize) {
    FILE *fp = fopen(path, "r");
    size_t n;

    buf[0] = '\0';

    if (!fp)
        return (0);

    n = fread(buf, 1, bufsize - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    return (1);
}

int main(void) {
    dist_t dist;
    file_t *f;
    char dir[] = "/tmp/epm-bsd-legacy-test.XXXXXX";
    char plistpath[1024], plist[8192];

    printf("test_bsd_legacy_pkg:\n");

    if (!mkdtemp(dir)) {
        printf("test_bsd_legacy_pkg: could not create a scratch directory\n");
        return (1);
    }

    memset(&dist, 0, sizeof(dist));
    strcpy(dist.product, "Test Product");
    strcpy(dist.version, "1.0");
    strcpy(dist.vendor, "Test Vendor");
    strcpy(dist.packager, "someone@example.com");

    dist.num_files = 3;
    dist.files = calloc(3, sizeof(file_t));

    f = &dist.files[0];
    f->type = 'd';
    f->mode = 0750;
    strcpy(f->user, "daemon");
    strcpy(f->group, "wheel");
    strcpy(f->dst, "/var/db/testtool");

    f = &dist.files[1];
    f->type = 'd';
    f->mode = 0700;
    strcpy(f->user, "daemon");
    strcpy(f->group, "wheel");
    strcpy(f->dst, "/var/db/test tool; touch /tmp/pwned");

    f = &dist.files[2];
    f->type = 'f';
    f->mode = 0644;
    strcpy(f->user, "root");
    strcpy(f->group, "wheel");
    strcpy(f->dst, "/usr/local/share/thing.conf");
    strcpy(f->src, "/dev/null");

    /* Expected to fail: there is no pkg_create(8) on this host. */
    (void)make_subpackage("test", dir, "", &dist, NULL);

    snprintf(plistpath, sizeof(plistpath), "%s/test.plist", dir);
    CHECK(slurp(plistpath, plist, sizeof(plist)), "the legacy plist was written to disk");

    /* Directory ownership is applied explicitly, as @dir/@dirrm carry none. */
    CHECK(strstr(plist, "@exec mkdir -p /var/db/testtool\n") != NULL,
          "the directory is created via @exec mkdir");
    CHECK(strstr(plist, "@exec chown daemon:wheel /var/db/testtool\n") != NULL,
          "the directory's declared owner:group is applied via @exec chown");
    CHECK(strstr(plist, "@exec chmod 0750 /var/db/testtool\n") != NULL,
          "the directory's declared mode is applied via @exec chmod");

    /*
     * The escaping that keeps those root-run commands from being reparsed
     * by the install-time shell.
     */
    CHECK(strstr(plist, "/var/db/test tool; touch /tmp/pwned") == NULL,
          "a directory path with shell metacharacters is never emitted raw");
    CHECK(strstr(plist,
                 "@exec chown daemon:wheel /var/db/test\\ tool\\;\\ touch\\ "
                 "/tmp/pwned\n") != NULL,
          "@exec chown escapes whitespace and metacharacters in the path");
    CHECK(strstr(plist, "@exec chmod 0700 /var/db/test\\ tool\\;\\ touch\\ "
                        "/tmp/pwned\n") != NULL,
          "@exec chmod escapes whitespace and metacharacters in the path");

    /* Ordinary file entries still carry their own @mode/@owner/@group. */
    CHECK(strstr(plist, "@mode 0644\n") != NULL, "a file entry emits its @mode");
    CHECK(strstr(plist, "@owner root\n") != NULL, "a file entry emits its @owner");
    CHECK(strstr(plist, "usr/local/share/thing.conf\n") != NULL,
          "a file entry is listed relative to the package root");

    /* Directories are still torn down in reverse order via @dirrm. */
    CHECK(strstr(plist, "@dirrm var/db/testtool\n") != NULL,
          "the directory is still scheduled for removal via @dirrm");

    free(dist.files);
    unlink_directory(dir);

    if (failures) {
        printf("test_bsd_legacy_pkg: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_bsd_legacy_pkg: ALL OK\n");
    return (0);
}
