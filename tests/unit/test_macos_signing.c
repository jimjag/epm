/*
 * Regression tests for macos.c's payload-signing helpers.
 *
 * is_macho(), staged_path(), etc. are all static, so this test #includes
 * macos.c directly (rather than linking it) to reach them - the same
 * pattern used to verify this code manually during development.
 *
 * Covers three review findings:
 *  - is_macho(): 0xCAFEBABE is both the Mach-O universal-binary magic and
 *    the Java .class file magic. A bundled .class file was misidentified
 *    as Mach-O, queued for codesign, and codesign's failure on a non-code
 *    file aborted the whole signed build.
 *  - staged_path(): a DRY refactor that unified the per-file-type staging
 *    logic accidentally widened the exact "/etc"/"/var" (no subpath) rule
 *    from directories only to files and symlinks too.
 *  - is_macho(): the byte-swapped fat magics (0xBEBAFECA) carry a
 *    byte-swapped architecture count, which must be decoded as such.
 */

#include "../../epm.h"
#include "../../macos.c"

#include <stdio.h>
#include <string.h>

int Verbosity = 0;
int KeepFiles = 0;
int AooMode = 0;
int CompressFiles = 0;
const char *DataDir = "";
const char *SetupProgram = "";
const char *SoftwareDir = "";
const char *UninstProgram = "";

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

static void write_file(const char *path, const unsigned char *data, size_t len) {
    FILE *fp = fopen(path, "wb");

    if (fp) {
        fwrite(data, 1, len, fp);
        fclose(fp);
    }
}

int main(void) {
    char dir[] = "/tmp/epm-macho-test.XXXXXX";
    char path[1024];

    printf("test_macos_signing:\n");

    if (!mkdtemp(dir)) {
        printf("test_macos_signing: could not create a scratch directory\n");
        return (1);
    }

    /* A real thin 64-bit Mach-O magic is still recognized. */
    {
        unsigned char thin[8] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0};

        snprintf(path, sizeof(path), "%s/thin", dir);
        write_file(path, thin, sizeof(thin));
        CHECK(is_macho(path), "a thin 64-bit Mach-O magic is recognized");
    }

    /* A real fat/universal Mach-O with a sane architecture count (2). */
    {
        unsigned char fat[8] = {0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 2};

        snprintf(path, sizeof(path), "%s/fat", dir);
        write_file(path, fat, sizeof(fat));
        CHECK(is_macho(path), "a fat Mach-O with nfat_arch=2 is recognized");
    }

    /*
     * Same leading 4 bytes as the fat case above, but the next 4 bytes are
     * a Java class file's (minor=0, major=52) - Java SE 8. Real class files
     * never have a major version below 45, which is what the fix uses to
     * tell the two apart.
     */
    {
        unsigned char javaclass[8] = {0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 52};

        snprintf(path, sizeof(path), "%s/Sample.class", dir);
        write_file(path, javaclass, sizeof(javaclass));
        CHECK(!is_macho(path),
              "a Java .class file (major version 52) is NOT misread as Mach-O");
    }

    /* A newer class file (major version 68, Java 24) is rejected too. */
    {
        unsigned char javaclass[8] = {0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 68};

        snprintf(path, sizeof(path), "%s/Newer.class", dir);
        write_file(path, javaclass, sizeof(javaclass));
        CHECK(!is_macho(path), "a newer .class file (major version 68) is rejected too");
    }

    /*
     * The 0xBEBAFECA spellings are the same fat header byte-swapped, so
     * nfat_arch is byte-swapped too and must be decoded that way - reading
     * it big-endian turns nfat=2 into 0x02000000 and rejects every one.
     */
    {
        unsigned char cigam[8] = {0xbe, 0xba, 0xfe, 0xca, 2, 0, 0, 0};
        unsigned char cigam64[8] = {0xbf, 0xba, 0xfe, 0xca, 2, 0, 0, 0};

        snprintf(path, sizeof(path), "%s/swapfat", dir);
        write_file(path, cigam, sizeof(cigam));
        CHECK(is_macho(path), "a byte-swapped fat Mach-O (FAT_CIGAM) is recognized");

        snprintf(path, sizeof(path), "%s/swapfat64", dir);
        write_file(path, cigam64, sizeof(cigam64));
        CHECK(is_macho(path), "a byte-swapped 64-bit fat Mach-O is recognized");
    }

    /* Arbitrary non-Mach-O data is rejected. */
    {
        unsigned char text[8] = {'p', 'l', 'a', 'i', 'n', ' ', 't', 'x'};

        snprintf(path, sizeof(path), "%s/plain.txt", dir);
        write_file(path, text, sizeof(text));
        CHECK(!is_macho(path), "plain text is not treated as Mach-O");
    }

    /* staged_path(): the exact "/etc"/"/var" rule applies to directories... */
    {
        char buf[1024];

        staged_path(buf, sizeof(buf), "/dist", "prod", "/etc", 1);
        CHECK(!strcmp(buf, "/dist/prod/Package/private/etc"),
              "a directory entry at exactly /etc stages under Package/private");

        staged_path(buf, sizeof(buf), "/dist", "prod", "/var", 1);
        CHECK(!strcmp(buf, "/dist/prod/Package/private/var"),
              "a directory entry at exactly /var stages under Package/private");
    }

    /* ...but NOT to files or symlinks at that exact path (no subpath). */
    {
        char buf[1024];

        staged_path(buf, sizeof(buf), "/dist", "prod", "/etc", 0);
        CHECK(!strcmp(buf, "/dist/prod/Package/etc"),
              "a file entry at exactly /etc stays under plain Package (not private)");

        staged_path(buf, sizeof(buf), "/dist", "prod", "/var", 0);
        CHECK(!strcmp(buf, "/dist/prod/Package/var"),
              "a symlink entry at exactly /var stays under plain Package (not private)");
    }

    /* The /etc/, /var/ PREFIX rule still applies to files regardless. */
    {
        char buf[1024];

        staged_path(buf, sizeof(buf), "/dist", "prod", "/etc/foo.conf", 0);
        CHECK(!strcmp(buf, "/dist/prod/Package/private/etc/foo.conf"),
              "a file under /etc/ still stages under Package/private");
    }

    unlink_directory(dir);

    if (failures) {
        printf("test_macos_signing: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_macos_signing: ALL OK\n");
    return (0);
}
