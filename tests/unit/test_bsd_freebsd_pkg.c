/*
 * Regression tests for bsd.c's FreeBSD pkg(8) manifest/plist writer.
 *
 * make_freebsd_modern_pkg() is static and only compiled under __FreeBSD__,
 * so this test #includes bsd.c directly and must be compiled with
 * -D__FreeBSD__ (this file is not itself FreeBSD-specific - it's testing
 * FreeBSD-only code from any host, the same way the code was developed and
 * manually verified without access to a real FreeBSD machine).
 *
 * Covers three review findings:
 *  - Directory entries' owner/group/mode were silently dropped: the plist
 *    writer skipped 'd' entries entirely when emitting @mode/@owner/@group,
 *    and later wrote them as a bare "@dir path" with no ownership of its
 *    own, so a directory just inherited whatever @owner/@group/@mode state
 *    the last regular file happened to leave active.
 *  - Those @exec lines become shell commands run as root at install time,
 *    so a path containing whitespace or shell metacharacters has to be
 *    escaped (via qprintf) rather than written raw.
 *  - An upper-bound-only dependency version (vernumber[0]==0,
 *    vernumber[1] set, e.g. "foo <= 2.0") was silently dropped from the
 *    UCL manifest instead of being reported as unexpressed.
 *  - The pkg(8) command line interpolated build paths with no quoting, so a
 *    space in the output directory split one argument into two.
 *
 * make_freebsd_modern_pkg() ends by shelling out to a real `pkg create`,
 * which doesn't exist on this (non-FreeBSD) test host - that's expected to
 * fail. Everything checked here is produced before that point: the manifest
 * and plist on disk, the notes on stderr, and the command line run_command()
 * echoes on stdout at -vv just before it forks.
 */

#include "../../epm.h"
#include "../../bsd.c"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

int Verbosity = 2; /* make run_command() echo the command it would run */
int KeepFiles = 1; /* keep the metadata dir around so we can inspect it */
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

static int count_occurrences(const char *haystack, const char *needle) {
    int n = 0;
    const char *p = haystack;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }

    return (n);
}

static void add_requires(depend_t *d, const char *product, int lower,
                         const char *lowerstr, int upper, const char *upperstr) {
    d->type = DEPEND_REQUIRES;
    strlcpy(d->product, product, sizeof(d->product));
    d->vernumber[0] = lower;
    d->vernumber[1] = upper;
    strlcpy(d->version[0], lowerstr, sizeof(d->version[0]));
    strlcpy(d->version[1], upperstr, sizeof(d->version[1]));
}

static void add_dir(file_t *f, const char *dst, mode_t mode, const char *user,
                    const char *group) {
    f->type = 'd';
    f->mode = mode;
    strlcpy(f->user, user, sizeof(f->user));
    strlcpy(f->group, group, sizeof(f->group));
    strlcpy(f->dst, dst, sizeof(f->dst));
}

int main(void) {
    dist_t dist;
    char dir[] = "/tmp/epm-freebsd-pkg-test.XXXXXX";
    char builddir[1024];
    char plistpath[1024], manifestpath[1024], errpath[1024], outpath[1024];
    char plist[8192], manifest[8192], errlog[8192], outlog[8192];
    int rc, savederr, errfd, savedout, outfd;

    printf("test_bsd_freebsd_pkg:\n");

    if (!mkdtemp(dir)) {
        printf("test_bsd_freebsd_pkg: could not create a scratch directory\n");
        return (1);
    }

    memset(&dist, 0, sizeof(dist));
    strcpy(dist.product, "Test Product");
    strcpy(dist.version, "1.0");
    strcpy(dist.vendor, "Test Vendor");
    strcpy(dist.packager, "someone@example.com");

    /*
     * Four dependencies: two upper-bound-only ones (so the "warn once"
     * de-duplication is actually exercised), one ordinary lower-bound one
     * that must still get a version constraint, and one conflict.
     */

    dist.num_depends = 4;
    dist.depends = calloc(4, sizeof(depend_t));
    add_requires(&dist.depends[0], "libfoo", 0, "0.0", 200, "2.0");
    add_requires(&dist.depends[1], "libqux", 0, "0.0", 300, "3.0");
    add_requires(&dist.depends[2], "libbar", 100, "1.0", INT_MAX, "999.99.99p99");
    dist.depends[3].type = DEPEND_INCOMPAT;
    strcpy(dist.depends[3].product, "oldthing");

    /*
     * Two directories: an ordinary one, and one whose path needs shell
     * escaping in the @exec lines.
     */

    dist.num_files = 2;
    dist.files = calloc(2, sizeof(file_t));
    add_dir(&dist.files[0], "/var/db/testtool", 0750, "daemon", "wheel");
    add_dir(&dist.files[1], "/var/db/test tool; rm -rf /", 0700, "daemon", "wheel");

    /*
     * Capture stderr so the "not expressed in the manifest" notes can be
     * checked - they are the entire observable effect of that fix.
     */

    snprintf(errpath, sizeof(errpath), "%s/stderr.log", dir);
    snprintf(outpath, sizeof(outpath), "%s/stdout.log", dir);

    /*
     * Build into a directory whose name contains a space, so the pkg(8)
     * command line that run_command() echoes at -vv shows whether the build
     * paths were quoted into single arguments.
     */

    snprintf(builddir, sizeof(builddir), "%s/build out", dir);
    make_directory(builddir, 0755, 0, 0);

    fflush(stderr);
    fflush(stdout);
    savederr = dup(2);
    savedout = dup(1);
    errfd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    outfd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (savederr < 0 || errfd < 0 || savedout < 0 || outfd < 0) {
        printf("test_bsd_freebsd_pkg: could not redirect stdout/stderr\n");
        return (1);
    }

    dup2(errfd, 2);
    dup2(outfd, 1);
    close(errfd);
    close(outfd);

    rc = make_freebsd_modern_pkg("test", builddir, "amd64", &dist, NULL, "test");

    fflush(stderr);
    fflush(stdout);
    dup2(savederr, 2);
    dup2(savedout, 1);
    close(savederr);
    close(savedout);

    Verbosity = 0; /* keep the teardown below from narrating itself */

    CHECK(rc == 1, "make_freebsd_modern_pkg() fails cleanly (no real pkg(8) here)");

    /*
     * Packing list: directory ownership, and shell escaping of the @exec
     * commands that apply it.
     */

    snprintf(plistpath, sizeof(plistpath), "%s/test.metadata/plist", builddir);
    CHECK(slurp(plistpath, plist, sizeof(plist)), "the packing list was written to disk");

    CHECK(strstr(plist, "@exec chown daemon:wheel /var/db/testtool\n") != NULL,
          "the directory's declared owner:group is applied via @exec chown");
    CHECK(strstr(plist, "@exec chmod 0750 /var/db/testtool\n") != NULL,
          "the directory's declared mode is applied via @exec chmod");
    CHECK(strstr(plist, "@dir var/db/testtool\n") != NULL,
          "the directory is still declared as a package member via @dir");

    CHECK(strstr(plist, "/var/db/test tool; rm -rf /") == NULL,
          "a directory path with shell metacharacters is never emitted raw");
    CHECK(strstr(plist, "@exec mkdir -p /var/db/test\\ tool\\;\\ rm\\ -rf\\ /\n") != NULL,
          "@exec mkdir escapes whitespace and metacharacters in the path");
    CHECK(strstr(plist,
                 "@exec chown daemon:wheel /var/db/test\\ tool\\;\\ rm\\ -rf\\ /\n") !=
              NULL,
          "@exec chown escapes whitespace and metacharacters in the path");
    CHECK(strstr(plist, "@exec chmod 0700 /var/db/test\\ tool\\;\\ rm\\ -rf\\ /\n") !=
              NULL,
          "@exec chmod escapes whitespace and metacharacters in the path");

    /*
     * UCL manifest: an upper-bound-only dependency must not be given a
     * fabricated version, while an ordinary lower bound still gets one.
     */

    snprintf(manifestpath, sizeof(manifestpath), "%s/test.metadata/+MANIFEST",
             builddir);
    CHECK(slurp(manifestpath, manifest, sizeof(manifest)),
          "the UCL manifest was written to disk");

    CHECK(strstr(manifest, "\"libfoo\": {origin: \"local/libfoo\"};") != NULL,
          "an upper-bound-only dependency gets no fabricated version constraint");
    CHECK(strstr(manifest, "\"libqux\": {origin: \"local/libqux\"};") != NULL,
          "a second upper-bound-only dependency is likewise left unconstrained");
    CHECK(strstr(manifest,
                 "\"libbar\": {origin: \"local/libbar\", version: \"1.0\"};") != NULL,
          "an ordinary lower-bound dependency still gets its version constraint");
    CHECK(strstr(manifest, "oldthing") == NULL,
          "a conflict is not written into the deps block");

    /*
     * stderr notes: reported, and reported once each rather than per entry.
     */

    CHECK(slurp(errpath, errlog, sizeof(errlog)), "stderr was captured");

    CHECK(count_occurrences(errlog, "upper-bound-only dependency versions") == 1,
          "upper-bound-only dependencies are reported, exactly once");
    CHECK(count_occurrences(errlog, "dependency incompatibilities/conflicts") == 1,
          "conflicts are reported, exactly once");

    /*
     * The pkg(8) invocation itself: every build path must arrive as one
     * quoted argument despite the space in the build directory.
     */

    CHECK(slurp(outpath, outlog, sizeof(outlog)), "stdout was captured");

    {
        char expect[2048];

        snprintf(expect, sizeof(expect),
                 "/usr/sbin/pkg create -m '%s/test.metadata' -p "
                 "'%s/test.metadata/plist' -r '%s/test.buildroot' -o '%s' -f txz",
                 builddir, builddir, builddir, builddir);

        CHECK(strstr(outlog, expect) != NULL,
              "pkg create receives quoted build paths despite a space in them");
    }

    free(dist.depends);
    free(dist.files);
    unlink_directory(dir);

    if (failures) {
        printf("test_bsd_freebsd_pkg: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_bsd_freebsd_pkg: ALL OK\n");
    return (0);
}
