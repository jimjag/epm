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
 *
 * And the inside-out bundle signing: macho_filetype() (thin, byte-swapped
 * and fat headers), the order and typing of plan_bundle()'s output on a
 * nested application, and - on macOS - a real ad-hoc sign_payload() run.
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

    /* macho_filetype(): thin headers in either byte order, and a fat slice. */
    {
        unsigned char exe_be[16] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
        unsigned char dylib_le[16] = {0xcf, 0xfa, 0xed, 0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0};
        unsigned char fat[48] = {0xca, 0xfe, 0xba, 0xbe, 0, 0, 0, 1, /* one slice */
                                 0, 0, 0, 7, 0, 0, 0, 3, 0, 0, 0, 32, 0, 0, 0, 16, 0, 0, 0, 12,
                                 0, 0, 0, 0,                                     /* pad to 32 */
                                 0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
        unsigned char shortfile[8] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0};

        snprintf(path, sizeof(path), "%s/exe_be", dir);
        write_file(path, exe_be, sizeof(exe_be));
        CHECK(macho_filetype(path) == 2, "a big-endian MH_EXECUTE header reads as filetype 2");

        snprintf(path, sizeof(path), "%s/dylib_le", dir);
        write_file(path, dylib_le, sizeof(dylib_le));
        CHECK(macho_filetype(path) == 6, "a little-endian MH_DYLIB header reads as filetype 6");

        snprintf(path, sizeof(path), "%s/fatexe", dir);
        write_file(path, fat, sizeof(fat));
        CHECK(macho_filetype(path) == 2, "a fat binary reports the filetype of its first slice");

        snprintf(path, sizeof(path), "%s/shortfile", dir);
        write_file(path, shortfile, sizeof(shortfile));
        CHECK(macho_filetype(path) == -1, "a truncated header yields -1");

        /* FAT_CIGAM_64: every field byte-swapped, one 32-byte fat_arch_64 at +8. */
        {
            unsigned char swapfat64[56] = {0xbf, 0xba, 0xfe, 0xca, 1, 0, 0, 0,
                                           7, 0, 0, 0, 3, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0, 0,
                                           16, 0, 0, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0,
                                           0xcf, 0xfa, 0xed, 0xfe, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0};

            snprintf(path, sizeof(path), "%s/swapfat64exe", dir);
            write_file(path, swapfat64, sizeof(swapfat64));
            CHECK(macho_filetype(path) == 2,
                  "a byte-swapped 64-bit fat binary locates its first slice");
        }
    }

    /*
     * plan_bundle(): an application with a helper executable, a dylib in
     * Resources, a nested framework (with the usual symlink farm), a plug-in
     * bundle, and a data directory that merely ends in .app.  Expect every
     * Mach-O before its bundle, nested bundles before the app, each bundle's
     * main executable left out, and entitlement-bearing kinds only on
     * executables and the .app.
     */
    {
        static const unsigned char exe[16] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0,
                                              0, 0, 0, 0, 0, 0, 0, 2};
        static const unsigned char dylib[16] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0,
                                                0, 0, 0, 0, 0, 0, 0, 6};
        static const unsigned char bundle[16] = {0xfe, 0xed, 0xfa, 0xcf, 0, 0, 0, 0,
                                                 0, 0, 0, 0, 0, 0, 0, 8};
        static const char *const plist_fmt =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
            "\t<key>CFBundleName</key>\n\t<string>%s</string>\n"
            "\t<key>CFBundleExecutable</key>\n\t<string>%s</string>\n</dict>\n</plist>\n";
        char app[1024], sub[1024], plist[2048], root[PATH_MAX];
        signplan_t plan = {NULL, 0};
        int i;

        snprintf(app, sizeof(app), "%s/Outer.app", dir);
        realpath(dir, root);

#define MKDIR(fmt) (snprintf(sub, sizeof(sub), fmt, app), make_directory(sub, 0755, 0, 0))
#define PUT(fmt, data) (snprintf(sub, sizeof(sub), fmt, app), write_file(sub, data, sizeof(data)))
#define PLIST(fmt, name)                                                                   \
    (snprintf(sub, sizeof(sub), fmt, app), snprintf(plist, sizeof(plist), plist_fmt, name, name), \
     write_file(sub, (const unsigned char *)plist, strlen(plist)))

        MKDIR("%s/Contents/MacOS");
        MKDIR("%s/Contents/Resources/data.app/lib");
        MKDIR("%s/Contents/Frameworks/Inner.framework/Versions/A/Resources");
        MKDIR("%s/Contents/PlugIns/P.bundle/Contents/MacOS");
        PLIST("%s/Contents/Info.plist", "Outer");
        PLIST("%s/Contents/Frameworks/Inner.framework/Versions/A/Resources/Info.plist", "Inner");
        PLIST("%s/Contents/PlugIns/P.bundle/Contents/Info.plist", "P");
        PUT("%s/Contents/MacOS/Outer", exe);
        PUT("%s/Contents/MacOS/helper", exe);
        PUT("%s/Contents/Resources/lib.dylib", dylib);
        PUT("%s/Contents/Resources/data.app/lib/data.dylib", dylib);
        PUT("%s/Contents/Frameworks/Inner.framework/Versions/A/Inner", dylib);
        PUT("%s/Contents/PlugIns/P.bundle/Contents/MacOS/P", bundle);
        snprintf(sub, sizeof(sub), "%s/Contents/Frameworks/Inner.framework/Versions/Current", app);
        symlink("A", sub);
        snprintf(sub, sizeof(sub), "%s/Contents/Frameworks/Inner.framework/Inner", app);
        symlink("Versions/Current/Inner", sub);
        snprintf(sub, sizeof(sub), "%s/Contents/Frameworks/Inner.framework/Resources", app);
        symlink("Versions/Current/Resources", sub);
#undef MKDIR
#undef PUT
#undef PLIST

        CHECK(!plan_bundle(&plan, app, NULL), "plan_bundle() succeeds on a nested application");

        {
            static const struct {
                const char *suffix;
                int kind;
            } expected[] = {
                {"/Outer.app/Contents/Frameworks/Inner.framework", SIGN_BUNDLE},
                {"/Outer.app/Contents/MacOS/helper", SIGN_EXECUTABLE},
                {"/Outer.app/Contents/PlugIns/P.bundle", SIGN_BUNDLE},
                {"/Outer.app/Contents/Resources/data.app/lib/data.dylib", SIGN_LIBRARY},
                {"/Outer.app/Contents/Resources/lib.dylib", SIGN_LIBRARY},
                {"/Outer.app", SIGN_APP},
            };
            int n = (int)(sizeof(expected) / sizeof(expected[0]));
            int ok = (plan.count == n);

            for (i = 0; ok && i < n; i++) {
                snprintf(sub, sizeof(sub), "%s%s", root, expected[i].suffix);
                ok = !strcmp(plan.items[i].path, sub) && plan.items[i].kind == expected[i].kind;
            }

            CHECK(ok, "the plan is inside-out, skips main executables and types each item");

            if (!ok)
                for (i = 0; i < plan.count; i++)
                    printf("      plan[%d] = %d %s\n", i, plan.items[i].kind, plan.items[i].path);
        }

        plan_free(&plan);

#ifdef __APPLE__
        /*
         * Real codesign run with the ad-hoc identity: the signed tree must
         * verify --deep --strict, and a Mach-O outside Contents/MacOS must
         * carry its own fresh signature rather than the linker's.
         */
        if (!access("/usr/bin/codesign", X_OK) && !access("/bin/ls", R_OK)) {
            char cmd[2048];
            dist_t dist;
            file_t files[1];

            snprintf(cmd, sizeof(cmd), "cp /bin/ls '%s/Contents/MacOS/Outer' && "
                     "cp /bin/ls '%s/Contents/MacOS/helper' && "
                     "cp /bin/ls '%s/Contents/Resources/lib.dylib' && "
                     "cp /bin/ls '%s/Contents/Frameworks/Inner.framework/Versions/A/Inner' && "
                     "cp /bin/ls '%s/Contents/PlugIns/P.bundle/Contents/MacOS/P' && "
                     "rm -rf '%s/Contents/Resources/data.app'",
                     app, app, app, app, app, app);
            system(cmd);

            memset(&dist, 0, sizeof(dist));
            memset(files, 0, sizeof(files));
            files[0].type = 'f';
            strlcpy(files[0].dst, "/Outer.app/Contents/MacOS/Outer", sizeof(files[0].dst));
            dist.num_files = 1;
            dist.files = files;

            /* Stage as <dir>/prod/Package/Outer.app by moving the tree. */
            snprintf(cmd, sizeof(cmd), "mkdir -p '%s/prod/Package' && mv '%s' '%s/prod/Package/'",
                     dir, app, dir);
            system(cmd);

            CHECK(!sign_payload(dir, "prod", &dist, NULL, 1, "-", NULL, NULL),
                  "sign_payload() ad-hoc signs and verifies a nested application");

            snprintf(cmd, sizeof(cmd),
                     "/usr/bin/codesign --verify --deep --strict '%s/prod/Package/Outer.app' "
                     ">/dev/null 2>&1", dir);
            CHECK(!system(cmd), "the signed application passes codesign --verify --deep --strict");

            snprintf(cmd, sizeof(cmd),
                     "/usr/bin/codesign -d --verbose=2 '%s/prod/Package/Outer.app/Contents/Resources/lib.dylib' "
                     "2>&1 | grep -q linker-signed", dir);
            CHECK(system(cmd) != 0, "a Mach-O under Resources no longer carries only the linker signature");

            snprintf(cmd, sizeof(cmd),
                     "/usr/bin/codesign -d --verbose=2 '%s/prod/Package/Outer.app/Contents/Frameworks/Inner.framework' "
                     "2>&1 | grep -q 'Sealed Resources'", dir);
            CHECK(!system(cmd), "the nested framework is sealed");
        } else
            printf("    skipped - codesign not available\n");
#endif
    }

    /*
     * Bundle suffixes, identifiers, ad-hoc detection and the runtime switch.
     */

    CHECK(is_bundle("Foo.xpc", 7) && is_bundle("Foo.appex", 9) &&
              is_bundle("Foo.mdimporter", 14) && is_bundle("Foo.qlgenerator", 15) &&
              is_bundle("Foo.plugin", 10) && is_bundle("Foo.prefPane", 12) &&
              is_bundle("Foo.saver", 9),
          "XPC services, extensions, importers, generators and panes are bundles");
    CHECK(!is_bundle("Foo.kext", 8) && !is_bundle(".app", 4) && !is_bundle("app", 3),
          "kexts, a bare suffix and a plain name are not bundles");

    CHECK(adhoc_identity("-") && !adhoc_identity("Developer ID Application") &&
              !adhoc_identity(NULL),
          "only \"-\" is the ad-hoc identity");

    {
        char buf[256];

        unsetenv("EPM_MACOS_IDENTIFIER");
        product_identifier("prod", NULL, buf, sizeof(buf));
        CHECK(!strcmp(buf, "prod"), "the identifier defaults to the product name");
        product_identifier("prod", "docs", buf, sizeof(buf));
        CHECK(!strcmp(buf, "prod-docs"), "a subpackage identifier defaults to the full name");

        setenv("EPM_MACOS_IDENTIFIER", "org.example.prod", 1);
        product_identifier("prod", NULL, buf, sizeof(buf));
        CHECK(!strcmp(buf, "org.example.prod"), "EPM_MACOS_IDENTIFIER overrides the identifier");
        product_identifier("prod", "docs", buf, sizeof(buf));
        CHECK(!strcmp(buf, "org.example.prod.docs"),
              "a subpackage is appended to the overridden identifier");
        unsetenv("EPM_MACOS_IDENTIFIER");

        unsetenv("EPM_SIGNING_HARDENED_RUNTIME");
        CHECK(hardened_runtime(), "the hardened runtime is on by default");
        setenv("EPM_SIGNING_HARDENED_RUNTIME", "no", 1);
        CHECK(!hardened_runtime(), "EPM_SIGNING_HARDENED_RUNTIME=no turns it off");
        setenv("EPM_SIGNING_HARDENED_RUNTIME", "yes", 1);
        CHECK(hardened_runtime(), "any other value leaves it on");
        unsetenv("EPM_SIGNING_HARDENED_RUNTIME");
    }

    /*
     * notarytool JSON and the component plist rewrite.
     */

    {
        char value[64];
        static const char json[] =
            "{\n  \"id\" : \"abc-123\",\n  \"message\" : \"Processing complete\",\n"
            "  \"status\" : \"Invalid\"\n}\n";

        CHECK(!json_string(json, "status", value, sizeof(value)) && !strcmp(value, "Invalid"),
              "json_string() reads the submission status");
        CHECK(!json_string(json, "id", value, sizeof(value)) && !strcmp(value, "abc-123"),
              "json_string() reads the submission id");
        CHECK(json_string(json, "missing", value, sizeof(value)),
              "json_string() reports a missing key");
        CHECK(json_string("Error: bad request", "status", value, sizeof(value)),
              "json_string() reports plain-text output as missing");
    }

    {
        char plist[1024], cmd[2048];
        static const char before[] =
            "<plist version=\"1.0\">\n<array>\n\t<dict>\n"
            "\t\t<key>BundleIsRelocatable</key>\n\t\t<true/>\n"
            "\t\t<key>BundleIsVersionChecked</key>\n\t\t<true/>\n"
            "\t\t<key>RootRelativeBundlePath</key>\n\t\t<string>Applications/A.app</string>\n"
            "\t</dict>\n\t<dict>\n"
            "\t\t<key>BundleIsRelocatable</key>\n\t\t<false/>\n"
            "\t\t<key>RootRelativeBundlePath</key>\n\t\t<string>Applications/B.app</string>\n"
            "\t</dict>\n</array>\n</plist>\n";

        make_directory(dir, 0755, 0, 0);
        snprintf(plist, sizeof(plist), "%s/Component.plist", dir);
        write_file(plist, (const unsigned char *)before, sizeof(before) - 1);

        CHECK(!pin_component_bundles(plist), "pin_component_bundles() rewrites the plist");

        snprintf(cmd, sizeof(cmd),
                 "test \"$(grep -c '<true/>' '%s')\" = 1 && "
                 "! grep -A1 BundleIsRelocatable '%s' | grep -q '<true/>'",
                 plist, plist);
        CHECK(!system(cmd), "every BundleIsRelocatable is false and other keys are untouched");
    }

    unlink_directory(dir);

    if (failures) {
        printf("test_macos_signing: %d FAILURE(S)\n", failures);
        return (1);
    }

    printf("test_macos_signing: ALL OK\n");
    return (0);
}
