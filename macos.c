/*
 * macOS package gateway for the ESP Package Manager (EPM).
 *
 * Copyright © 2020 by Jim Jagielski
 * Copyright © 2002-2020 by Michael R Sweet
 * Copyright © 2002-2010 by Easy Software Products.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Include necessary headers...
 */

#include "epm.h"

/*
 * Local functions...
 */

static int make_package(int format, const char *prodname, const char *directory,
                        dist_t *dist, const char *setup);
static int codesign_path(const char *path, const char *identity,
                         const char *entitlements, const char *keychain, int runtime);
static int is_bundle(const char *name, size_t len);
static int is_bundle_dir(const char *path);
static int is_macho(const char *path);
static int notarize_file(const char *path, const char *profile);
static int sign_payload(const char *directory, const char *prodfull, dist_t *dist,
                        const char *identity, const char *entitlements,
                        const char *keychain);
static int verify_signature(const char *path, int deep);
static void staged_path(char *buf, size_t bufsize, const char *directory,
                        const char *prodfull, const char *dst, int isdir);

/*
 * 'make_macos()' - Make a macOS disk image containing a macOS package.
 */

int                                  /* O - 0 = success, 1 = fail */
make_macos(int format,               /* I - Format */
           const char *prodname,     /* I - Product short name */
           const char *directory,    /* I - Directory for distribution files */
           const char *platname,     /* I - Platform name */
           dist_t *dist,             /* I - Distribution information */
           struct utsname *platform, /* I - Platform information */
           const char *setup)        /* I - Setup GUI image */
{
    char filename[1024], /* Destination filename */
        dmgname[1024];   /* Disk image filename */
    const char *identity, /* Developer ID Application identity */
        *profile;         /* notarytool keychain profile */

    REF(platname);
    REF(platform);

    /*
     * Create the main package and subpackages (if any)...
     */

    if (make_package(format, prodname, directory, dist, setup))
        return (1);

    /*
     * TODO: Copy uninstall application to disk image...
     */

    /*
     * Create a disk image of the package...
     */

    if (Verbosity)
        puts("Creating disk image...");

    if (dist->release[0])
        snprintf(filename, sizeof(filename), "%s-%s-%s", prodname, dist->version,
                 dist->release);
    else
        snprintf(filename, sizeof(filename), "%s-%s", prodname, dist->version);

    if (platname[0]) {
        strlcat(filename, "-", sizeof(filename));
        strlcat(filename, platname, sizeof(filename));
    }

    snprintf(dmgname, sizeof(dmgname), "%s/%s.dmg", directory, filename);

    {
        char srcfolder[1024],  /* Package directory to image */
            qsrcfolder[1024],  /* Quoted package directory */
            qdmgname[1024];    /* Quoted disk image filename */

        snprintf(srcfolder, sizeof(srcfolder), "%s/%s.pkg", directory, prodname);

        if (run_quote(qsrcfolder, sizeof(qsrcfolder), srcfolder) ||
            run_quote(qdmgname, sizeof(qdmgname), dmgname)) {
            fputs("epm: A disk image path is too long to quote safely.\n", stderr);
            return (1);
        }

        if (run_command(NULL, "hdiutil create -ov -srcfolder %s %s", qsrcfolder,
                        qdmgname)) {
            fputs("epm: Unable to create disk image.\n", stderr);
            return (1);
        }
    }

    /*
     * Sign and notarize the disk image...
     */

    if (format == PACKAGE_MACOS_SIGNED &&
        (identity = getenv("EPM_APPLICATION_IDENTITY")) != NULL) {
        if (Verbosity)
            puts("Signing disk image...");

        if (codesign_path(dmgname, identity, NULL, getenv("EPM_SIGNING_KEYCHAIN"), 0) ||
            verify_signature(dmgname, 0))
            return (1);

        if ((profile = getenv("EPM_NOTARY_KEYCHAIN_PROFILE")) != NULL &&
            notarize_file(dmgname, profile))
            return (1);
    }

    return (0);
}

/*
 * 'make_package()' - Make a macOS package.
 */

static int make_package(int format,            /* I - Format */
                        const char *prodname,  /* I - Product short name */
                        const char *directory, /* I - Directory for distribution files */
                        dist_t *dist,          /* I - Distribution  information */
                        const char *setup)     /* I - Setup GUI image */
{
    int i;               /* Looping var */
    FILE *fp;            /* Spec file */
    char prodfull[1024], /* Full product name */
        title[1024],     /* Software title */
        filename[1024],  /* Destination filename */
        pkgname[1024];   /* Package name */
    file_t *file;        /* Current distribution file */
    command_t *c;        /* Current command */
    uid_t uid;            /* Resolved owner ID */
    gid_t gid;            /* Resolved group ID */
    char current[1024];  /* Current directory */
    const char *option;  /* Init script option */

    strlcpy(prodfull, prodname, sizeof(prodfull));
    strlcpy(title, dist->product, sizeof(title));

    if (Verbosity)
        printf("Creating %s macOS package...\n", prodfull);

    getcwd(current, sizeof(current));

    /*
     * Copy the resources for the license, readme, and welcome (description)
     * stuff...
     */

    if (Verbosity)
        puts("Copying temporary resource files...");

    snprintf(filename, sizeof(filename), "%s/%s/Resources", directory, prodfull);
    make_directory(filename, 0755, 0, 0);

    /*
     * Do pre/post install commands...
     */

    for (i = dist->num_commands, c = dist->commands; i > 0; i--, c++)
        if (c->type == COMMAND_PRE_INSTALL)
            break;

    if (i) {
        snprintf(filename, sizeof(filename), "%s/%s/Resources/preinstall", directory,
                 prodfull);

        if ((fp = fopen(filename, "w")) == NULL) {
            fprintf(stderr, "epm: Unable to create preinstall script \"%s\": %s\n",
                    filename, strerror(errno));
            return (1);
        }

        fputs("#!/bin/sh\n", fp);

        for (; i > 0; i--, c++)
            if (c->type == COMMAND_PRE_INSTALL)
                fprintf(fp, "%s\n", c->command);

        fclose(fp);
        chmod(filename, 0755);
    }

    for (i = dist->num_commands, c = dist->commands; i > 0; i--, c++)
        if (c->type == COMMAND_POST_INSTALL)
            break;

    if (!i) {
        for (i = dist->num_files, file = dist->files; i > 0; i--, file++)
            if (tolower(file->type) == 'i')
                break;
    }

    if (i) {
        snprintf(filename, sizeof(filename), "%s/%s/Resources/postinstall", directory,
                 prodfull);

        if ((fp = fopen(filename, "w")) == NULL) {
            fprintf(stderr, "epm: Unable to create postinstall script \"%s\": %s\n",
                    filename, strerror(errno));
            return (1);
        }

        fputs("#!/bin/sh\n", fp);

        for (i = dist->num_commands, c = dist->commands; i > 0; i--, c++)
            if (c->type == COMMAND_POST_INSTALL)
                fprintf(fp, "%s\n", c->command);

        for (i = dist->num_files, file = dist->files; i > 0; i--, file++)
            if (tolower(file->type) == 'i')
                qprintf(fp, "/Library/StartupItems/%s/%s start\n", file->dst, file->dst);

        fclose(fp);
        chmod(filename, 0755);
    }

    /*
     * Copy the files over...
     */

    if (Verbosity)
        puts("Copying temporary distribution files...");

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++) {
        /*
         * Find the username and groupname IDs...
         */

        uid = get_uid(file->user);
        gid = get_gid(file->group);

        /*
         * Copy the file or make the directory or make the symlink as needed...
         */

        switch (tolower(file->type)) {
        case 'c':
        case 'f':
            staged_path(filename, sizeof(filename), directory, prodfull, file->dst, 0);

            if (Verbosity > 1)
                printf("%s -> %s...\n", file->src, filename);

            if (copy_file(filename, file->src, file->mode, uid, gid))
                return (1);
            break;
        case 'i':
            snprintf(filename, sizeof(filename),
                     "%s/%s/Package/Library/StartupItems/%s/%s", directory, prodfull,
                     file->dst, file->dst);

            if (Verbosity > 1)
                printf("%s -> %s...\n", file->src, filename);

            if (copy_file(filename, file->src, file->mode, uid, gid))
                return (1);

            snprintf(filename, sizeof(filename),
                     "%s/%s/Package/Library/StartupItems/%s/StartupParameters.plist",
                     directory, prodfull, file->dst);
            if ((fp = fopen(filename, "w")) == NULL) {
                fprintf(stderr, "epm: Unable to create init data file \"%s\": %s\n",
                        filename, strerror(errno));
                return (1);
            }

            fputs("{\n", fp);
            fprintf(fp, "  Description = \"%s\";\n", dist->product);
            qprintf(fp, "  Provides = (%s);\n", get_option(file, "provides", file->dst));
            if ((option = get_option(file, "requires", NULL)) != NULL)
                qprintf(fp, "  Requires = (%s);\n", option);
            if ((option = get_option(file, "uses", NULL)) != NULL)
                qprintf(fp, "  Uses = (%s);\n", option);
            if ((option = get_option(file, "order", NULL)) != NULL)
                qprintf(fp, "  OrderPreference = \"%s\";\n", option);
            fputs("}\n", fp);

            fclose(fp);

            snprintf(filename, sizeof(filename),
                     "%s/%s/Package/Library/StartupItems/%s/Resources/English.lproj",
                     directory, prodfull, file->dst);
            make_directory(filename, 0755, 0, 0);

            snprintf(filename, sizeof(filename),
                     "%s/%s/Package/Library/StartupItems/%s/Resources/English.lproj/"
                     "Localizable.strings",
                     directory, prodfull, file->dst);
            if ((fp = fopen(filename, "w")) == NULL) {
                fprintf(stderr, "epm: Unable to create init strings file \"%s\": %s\n",
                        filename, strerror(errno));
                return (1);
            }

            fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", fp);
            fputs("<!DOCTYPE plist SYSTEM "
                  "\"file://localhost/System/Library/DTDs/PropertyList.dtd\">\n",
                  fp);
            fputs("<plist version=\"0.9\">\n", fp);
            fputs("<dict>\n", fp);
            fprintf(fp, "        <key>Starting %s</key>\n", dist->product);
            fprintf(fp, "        <string>Starting %s</string>\n", dist->product);
            fputs("</dict>\n", fp);
            fputs("</plist>\n", fp);

            fclose(fp);
            break;
        case 'd':
            staged_path(filename, sizeof(filename), directory, prodfull, file->dst, 1);

            if (Verbosity > 1)
                printf("Directory %s...\n", filename);

            make_directory(filename, file->mode, uid, gid);
            break;
        case 'l':
            staged_path(filename, sizeof(filename), directory, prodfull, file->dst, 0);

            if (Verbosity > 1)
                printf("%s -> %s...\n", file->src, filename);

            make_link(filename, file->src);
            break;
        }
    }

    /*
     * Sign the staged payload before it is sealed into the package...
     */

    if (format == PACKAGE_MACOS_SIGNED) {
        const char *identity = getenv("EPM_APPLICATION_IDENTITY");

        if (identity && sign_payload(directory, prodfull, dist, identity,
                                     getenv("EPM_SIGNING_ENTITLEMENTS"),
                                     getenv("EPM_SIGNING_KEYCHAIN")))
            return (1);
    }

    /*
     * Build the distribution...
     */

    if (Verbosity)
        puts("Building macOS package...");

    if (directory[0] == '/')
        strlcpy(filename, directory, sizeof(filename));
    else
        snprintf(filename, sizeof(filename), "%s/%s", current, directory);

    /*
     * The package stands alone - just put it in the output directory...
     */

    snprintf(pkgname, sizeof(pkgname), "%s/%s.pkg", filename, prodfull);

    {
        char scriptsdir[1024],  /* Resources directory */
            rootdir[1024],      /* Staged payload directory */
            qidentifier[1024],  /* Quoted package identifier */
            qversion[1024],     /* Quoted version */
            qscriptsdir[1024],  /* Quoted resources directory */
            qrootdir[1024],     /* Quoted payload directory */
            qpkgname[1024];     /* Quoted package filename */

        snprintf(scriptsdir, sizeof(scriptsdir), "%s/%s/Resources", directory, prodfull);
        snprintf(rootdir, sizeof(rootdir), "%s/%s/Package", directory, prodfull);

        if (run_quote(qidentifier, sizeof(qidentifier), prodfull) ||
            run_quote(qversion, sizeof(qversion), dist->version) ||
            run_quote(qscriptsdir, sizeof(qscriptsdir), scriptsdir) ||
            run_quote(qrootdir, sizeof(qrootdir), rootdir) ||
            run_quote(qpkgname, sizeof(qpkgname), pkgname)) {
            fputs("epm: A package path is too long to quote safely.\n", stderr);
            return (1);
        }

        if (format == PACKAGE_MACOS_SIGNED) {
            const char *identity = getenv("EPM_SIGNING_IDENTITY"),
                *keychain = getenv("EPM_SIGNING_KEYCHAIN");
            char qidentity[1024], /* Quoted identity */
                keychainopt[1024]; /* --keychain option or empty */

            if (!identity) {
                fputs("epm: Using default 'Developer ID Installer' signing identity.\n"
                      "     Set the EPM_SIGNING_IDENTITY environment variable to "
                      "override.\n",
                      stderr);
                identity = "Developer ID Installer";
            }

            if (run_quote(qidentity, sizeof(qidentity), identity)) {
                fputs("epm: EPM_SIGNING_IDENTITY is too long.\n", stderr);
                return (1);
            }

            keychainopt[0] = '\0';

            if (keychain) {
                char qkeychain[1024]; /* Quoted keychain path */

                if (run_quote(qkeychain, sizeof(qkeychain), keychain) ||
                    snprintf(keychainopt, sizeof(keychainopt), "--keychain %s ",
                             qkeychain) >= (int)sizeof(keychainopt)) {
                    fputs("epm: EPM_SIGNING_KEYCHAIN is too long.\n", stderr);
                    return (1);
                }
            }

            if (run_command(
                    NULL,
                    "/usr/bin/pkgbuild --identifier %s --version %s --ownership preserve "
                    "--scripts %s --root %s %s--sign %s %s",
                    qidentifier, qversion, qscriptsdir, qrootdir, keychainopt, qidentity,
                    qpkgname)) {
                fputs("epm: Unable to build signed package.\n", stderr);
                return (1);
            }
        } else if (run_command(
                       NULL,
                       "/usr/bin/pkgbuild --identifier %s --version %s --ownership "
                       "preserve --scripts %s --root %s %s",
                       qidentifier, qversion, qscriptsdir, qrootdir, qpkgname)) {
            fputs("epm: Unable to build package.\n", stderr);
            return (1);
        }
    }

    /*
     * Verify that the package was created...
     */

    if (access(pkgname, 0))
        return (1);

    /*
     * Remove temporary files...
     */

    if (!KeepFiles) {
        if (Verbosity)
            puts("Removing temporary distribution files...");

        snprintf(filename, sizeof(filename), "%s/%s", directory, prodfull);
        unlink_directory(filename);
    }

    return (0);
}

/*
 * 'staged_path()' - Get the staged location of a destination path.
 */

static void staged_path(char *buf,             /* O - Staged path buffer */
                        size_t bufsize,        /* I - Size of buffer */
                        const char *directory, /* I - Distribution directory */
                        const char *prodfull,  /* I - Full product name */
                        const char *dst,       /* I - Destination path */
                        int isdir)             /* I - Is dst a directory entry? */
{
    if (!strncmp(dst, "/etc/", 5) || !strncmp(dst, "/var/", 5) ||
        (isdir && (!strcmp(dst, "/etc") || !strcmp(dst, "/var"))))
        snprintf(buf, bufsize, "%s/%s/Package/private%s", directory, prodfull, dst);
    else
        snprintf(buf, bufsize, "%s/%s/Package%s", directory, prodfull, dst);
}

/*
 * 'is_macho()' - Determine whether a file is a Mach-O binary.
 */

static int                       /* O - 1 if Mach-O, 0 otherwise */
is_macho(const char *path)       /* I - File to check */
{
    FILE *fp;                    /* File pointer */
    unsigned char header[8];     /* Magic number + fat_header.nfat_arch */
    unsigned magic,               /* Magic number as a big-endian value */
        nfat;                     /* Fat header architecture count */

    if ((fp = fopen(path, "rb")) == NULL)
        return (0);

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return (0);
    }

    fclose(fp);

    magic = ((unsigned)header[0] << 24) | ((unsigned)header[1] << 16) |
            ((unsigned)header[2] << 8) | (unsigned)header[3];

    if (magic == 0xfeedfaceu || magic == 0xcefaedfeu || /* 32-bit */
        magic == 0xfeedfacfu || magic == 0xcffaedfeu)   /* 64-bit */
        return (1);

    if (magic == 0xcafebabeu || magic == 0xbebafecau || /* universal */
        magic == 0xcafebabfu || magic == 0xbfbafecau) {  /* universal 64-bit */
        /*
         * 0xCAFEBABE is also the Java .class file magic.  A fat Mach-O's
         * next 4 bytes are always its architecture count, which in practice
         * is a small number (1-10ish); a .class file's next 4 bytes are its
         * minor/major version, and major version has never been below 45 -
         * use that gap to tell the two apart.  The 0xBEBAFECA spellings are
         * the same header byte-swapped, so nfat_arch is swapped there too.
         */

        if (magic == 0xcafebabeu || magic == 0xcafebabfu)
            nfat = ((unsigned)header[4] << 24) | ((unsigned)header[5] << 16) |
                   ((unsigned)header[6] << 8) | (unsigned)header[7];
        else
            nfat = ((unsigned)header[7] << 24) | ((unsigned)header[6] << 16) |
                   ((unsigned)header[5] << 8) | (unsigned)header[4];

        return (nfat > 0 && nfat <= 20);
    }

    return (0);
}

/*
 * 'read_u32()' - Read a 32-bit value in the given byte order.
 */

static unsigned                       /* O - Value */
read_u32(const unsigned char *p,      /* I - Bytes */
         int bigendian)               /* I - Big-endian? */
{
    if (bigendian)
        return (((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
                ((unsigned)p[2] << 8) | (unsigned)p[3]);
    else
        return (((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) |
                ((unsigned)p[1] << 8) | (unsigned)p[0]);
}

/*
 * 'macho_filetype()' - Get the Mach-O header filetype of a binary.
 *
 * Returns the filetype of a thin binary or of the first slice of a fat one,
 * or -1 when it cannot be read.
 */

static int                          /* O - MH_* filetype or -1 */
macho_filetype(const char *path)    /* I - Mach-O file */
{
    FILE *fp;                       /* File pointer */
    unsigned char header[16];       /* Thin header through filetype */
    unsigned char arch[32];         /* First fat_arch entry */
    unsigned magic;                 /* Magic number as read big-endian */
    unsigned long long offset = 0;  /* Offset of the thin header */
    int bigendian;                  /* Byte order of the thin header */

    if ((fp = fopen(path, "rb")) == NULL)
        return (-1);

    if (fread(header, 1, 8, fp) != 8) {
        fclose(fp);
        return (-1);
    }

    magic = read_u32(header, 1);

    if (magic == 0xcafebabeu || magic == 0xbebafecau ||
        magic == 0xcafebabfu || magic == 0xbfbafecau) {
        /*
         * Fat headers are big-endian; the byte-swapped spellings mean the
         * whole header is swapped.  A 64-bit fat_arch is 32 bytes with a
         * 64-bit offset, the classic one 20 bytes with a 32-bit offset.
         */

        int fatbig = (magic == 0xcafebabeu || magic == 0xcafebabfu);
        int fat64 = (magic == 0xcafebabfu || magic == 0xbfbafecau);
        size_t archlen = fat64 ? 32 : 20;

        if (fread(arch, 1, archlen, fp) != archlen) {
            fclose(fp);
            return (-1);
        }

        if (fat64 && fatbig)
            offset = ((unsigned long long)read_u32(arch + 8, 1) << 32) |
                     read_u32(arch + 12, 1);
        else if (fat64)
            offset = ((unsigned long long)read_u32(arch + 12, 0) << 32) |
                     read_u32(arch + 8, 0);
        else
            offset = read_u32(arch + 8, fatbig);

        if (fseek(fp, (long)offset, SEEK_SET) || fread(header, 1, 8, fp) != 8) {
            fclose(fp);
            return (-1);
        }

        magic = read_u32(header, 1);
    }

    if (magic == 0xfeedfaceu || magic == 0xfeedfacfu)
        bigendian = 1;
    else if (magic == 0xcefaedfeu || magic == 0xcffaedfeu)
        bigendian = 0;
    else {
        fclose(fp);
        return (-1);
    }

    /*
     * mach_header: magic, cputype, cpusubtype, filetype, ...
     */

    if (fread(header + 8, 1, 8, fp) != 8) {
        fclose(fp);
        return (-1);
    }

    fclose(fp);

    return ((int)read_u32(header + 12, bigendian));
}

/*
 * 'is_bundle()' - Determine whether a path component names a code bundle.
 */

static int                    /* O - 1 if a bundle, 0 otherwise */
is_bundle(const char *name,   /* I - Path component */
          size_t len)         /* I - Length of component */
{
    size_t i,                 /* Looping var */
        slen;                 /* Length of suffix */
    static const char *const suffixes[] = {".app", ".framework", ".bundle"};

    for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        slen = strlen(suffixes[i]);

        if (len > slen && !strncasecmp(name + len - slen, suffixes[i], slen))
            return (1);
    }

    return (0);
}

/*
 * 'is_app_bundle()' - Determine whether a bundle path is an application.
 */

static int                          /* O - 1 if a .app, 0 otherwise */
is_app_bundle(const char *path)     /* I - Bundle path */
{
    size_t len = strlen(path);      /* Length of path */

    return (len > 4 && !strncasecmp(path + len - 4, ".app", 4));
}

/*
 * 'is_bundle_dir()' - Determine whether a staged directory is really a bundle.
 */

static int                       /* O - 1 if a bundle, 0 otherwise */
is_bundle_dir(const char *path)  /* I - Staged directory */
{
    char marker[1024];    /* Path of a structural marker */
    struct stat fileinfo; /* Information about the marker */

    /*
     * A directory that merely ends in a bundle suffix is not necessarily a
     * bundle; codesign rejects anything without the expected layout, so look
     * for it rather than failing the build over a data directory.
     */

    snprintf(marker, sizeof(marker), "%s/Contents/Info.plist", path);
    if (!stat(marker, &fileinfo))
        return (1);

    snprintf(marker, sizeof(marker), "%s/Resources/Info.plist", path);
    if (!stat(marker, &fileinfo))
        return (1);

    snprintf(marker, sizeof(marker), "%s/Versions/Current", path);
    if (!lstat(marker, &fileinfo))
        return (1);

    return (0);
}

/*
 * 'plist_executable()' - Read CFBundleExecutable from an Info.plist.
 */

static int                            /* O - 0 = found, 1 = not found */
plist_executable(const char *plist,   /* I - Info.plist path */
                 char *buf,           /* O - Executable name */
                 size_t bufsize)      /* I - Size of buffer */
{
    FILE *fp;                         /* File pointer */
    char *data,                       /* File contents */
        *key,                         /* Location of the key */
        *start,                       /* Start of the value */
        *end;                         /* End of the value */
    long size;                        /* File size */
    size_t len;                       /* Length of the value */

    if ((fp = fopen(plist, "rb")) == NULL)
        return (1);

    if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) <= 0 || size > 1048576 ||
        fseek(fp, 0, SEEK_SET) || (data = malloc((size_t)size + 1)) == NULL) {
        fclose(fp);
        return (1);
    }

    len = fread(data, 1, (size_t)size, fp);
    fclose(fp);
    data[len] = '\0';

    /*
     * Try the XML form first; a binary plist needs PlistBuddy.
     */

    if (strncmp(data, "bplist", 6) &&
        (key = strstr(data, "<key>CFBundleExecutable</key>")) != NULL &&
        (start = strstr(key, "<string>")) != NULL &&
        (end = strstr(start += 8, "</string>")) != NULL &&
        (size_t)(end - start) < bufsize) {
        memcpy(buf, start, (size_t)(end - start));
        buf[end - start] = '\0';
        free(data);

        return (buf[0] == '\0');
    }

    free(data);

    {
        char command[2048],       /* PlistBuddy command line */
            qplist[1024];         /* Quoted plist path */

        if (run_quote(qplist, sizeof(qplist), plist))
            return (1);

        snprintf(command, sizeof(command),
                 "/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' %s 2>/dev/null",
                 qplist);

        if ((fp = popen(command, "r")) == NULL)
            return (1);

        if (!fgets(buf, (int)bufsize, fp))
            buf[0] = '\0';

        pclose(fp);

        buf[strcspn(buf, "\r\n")] = '\0';

        return (buf[0] == '\0');
    }
}

/*
 * 'bundle_executable()' - Get the canonical path of a bundle's main executable.
 *
 * codesign signs a bundle's main executable as part of the bundle itself, so
 * it must not be signed on its own.
 */

static int                              /* O - 0 = found, 1 = not found */
bundle_executable(const char *bundle,   /* I - Canonical bundle path */
                  char *buf,            /* O - Canonical executable path */
                  size_t bufsize)       /* I - Size of buffer */
{
    char plist[1024],                   /* Info.plist path */
        name[256],                      /* CFBundleExecutable */
        candidate[1024],                /* Executable path before realpath */
        resolved[PATH_MAX];             /* Canonical executable path */
    struct stat fileinfo;               /* Information about the candidate */

    snprintf(plist, sizeof(plist), "%s/Contents/Info.plist", bundle);
    if (plist_executable(plist, name, sizeof(name))) {
        snprintf(plist, sizeof(plist), "%s/Resources/Info.plist", bundle);
        if (plist_executable(plist, name, sizeof(name)))
            return (1);
    }

    if (strchr(name, '/'))
        return (1);

    snprintf(candidate, sizeof(candidate), "%s/Contents/MacOS/%s", bundle, name);
    if (stat(candidate, &fileinfo))
        snprintf(candidate, sizeof(candidate), "%s/%s", bundle, name);

    if (!realpath(candidate, resolved) || strlen(resolved) >= bufsize)
        return (1);

    strlcpy(buf, resolved, bufsize);

    return (0);
}

/*
 * Signing plan - what codesign is run on, in order.
 */

enum {
    SIGN_LIBRARY,    /* Library, plug-in or other non-executable Mach-O */
    SIGN_EXECUTABLE, /* MH_EXECUTE Mach-O */
    SIGN_BUNDLE,     /* Framework or plug-in bundle */
    SIGN_APP         /* Application bundle */
};

#define MH_EXECUTE_TYPE 2 /* mach_header filetype of an executable */

typedef struct {
    char *path;      /* Canonical path */
    int kind;        /* SIGN_* */
} signitem_t;

typedef struct {
    signitem_t *items; /* Items in signing order */
    int count;         /* Number of items */
} signplan_t;

/*
 * 'plan_add()' - Append a path to a signing plan unless already present.
 */

static int                     /* O - 0 = success, 1 = out of memory */
plan_add(signplan_t *plan,     /* IO - Signing plan */
         const char *path,     /* I - Canonical path */
         int kind)             /* I - SIGN_* */
{
    int i;                     /* Looping var */
    signitem_t *temp;          /* New list */

    for (i = 0; i < plan->count; i++)
        if (!strcmp(plan->items[i].path, path))
            return (0);

    if ((temp = realloc(plan->items, (size_t)(plan->count + 1) * sizeof(signitem_t))) ==
        NULL)
        return (1);

    plan->items = temp;

    if ((temp[plan->count].path = strdup(path)) == NULL)
        return (1);

    temp[plan->count].kind = kind;
    plan->count++;

    return (0);
}

/*
 * 'plan_free()' - Free a signing plan.
 */

static void plan_free(signplan_t *plan) /* I - Signing plan */
{
    int i; /* Looping var */

    for (i = 0; i < plan->count; i++)
        free(plan->items[i].path);

    free(plan->items);

    plan->items = NULL;
    plan->count = 0;
}

/*
 * 'compare_names()' - Sort directory entries by name.
 */

static int                        /* O - Result of comparison */
compare_names(const void *a,      /* I - First name */
              const void *b)      /* I - Second name */
{
    return (strcmp(*(const char *const *)a, *(const char *const *)b));
}

static int plan_bundle(signplan_t *plan, const char *bundle);

/*
 * 'plan_walk()' - Add the code inside a bundle directory to a signing plan.
 */

static int                          /* O - 0 = success, 1 = fail */
plan_walk(signplan_t *plan,         /* IO - Signing plan */
          const char *directory,    /* I - Canonical directory */
          const char *mainexe)      /* I - Main executable of the enclosing bundle */
{
    DIR *dir;                       /* Directory */
    struct dirent *dent;            /* Directory entry */
    char **names = NULL,            /* Entry names, sorted for a stable order */
        path[PATH_MAX];             /* Entry path */
    int i,                          /* Looping var */
        count = 0,                  /* Number of names */
        status = 0;                 /* Return status */
    struct stat fileinfo;           /* Information about the entry */

    if ((dir = opendir(directory)) == NULL) {
        fprintf(stderr, "epm: Unable to read directory \"%s\": %s\n", directory,
                strerror(errno));
        return (1);
    }

    while ((dent = readdir(dir)) != NULL) {
        char **temp;                /* New name list */

        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
            continue;

        if ((temp = realloc(names, (size_t)(count + 1) * sizeof(char *))) == NULL ||
            (temp[count] = strdup(dent->d_name)) == NULL) {
            if (temp)
                names = temp;
            status = 1;
            break;
        }

        names = temp;
        count++;
    }

    closedir(dir);

    if (count > 1)
        qsort(names, (size_t)count, sizeof(char *), compare_names);

    for (i = 0; i < count && !status; i++) {
        if (snprintf(path, sizeof(path), "%s/%s", directory, names[i]) >=
            (int)sizeof(path)) {
            fprintf(stderr, "epm: Path too long under \"%s\".\n", directory);
            status = 1;
            break;
        }

        /*
         * Symbolic links are skipped: codesign works on the real file, and a
         * framework's top-level links all resolve into Versions/.
         */

        if (lstat(path, &fileinfo))
            continue;

        if (S_ISDIR(fileinfo.st_mode)) {
            if (is_bundle(names[i], strlen(names[i])) && is_bundle_dir(path))
                status = plan_bundle(plan, path);
            else
                status = plan_walk(plan, path, mainexe);
        } else if (S_ISREG(fileinfo.st_mode) && is_macho(path) &&
                   (!mainexe || strcmp(path, mainexe))) {
            status = plan_add(plan, path,
                              macho_filetype(path) == MH_EXECUTE_TYPE ? SIGN_EXECUTABLE
                                                                        : SIGN_LIBRARY);
        }
    }

    for (i = 0; i < count; i++)
        free(names[i]);

    free(names);

    return (status);
}

/*
 * 'plan_bundle()' - Add a bundle to a signing plan, inside-out.
 *
 * Every Mach-O inside the bundle comes first, nested bundles are sealed as
 * they are met, and the bundle itself goes last, so each seal covers code
 * that is already signed.  The bundle's main executable is left out because
 * codesign signs it together with the bundle.
 */

static int                        /* O - 0 = success, 1 = fail */
plan_bundle(signplan_t *plan,     /* IO - Signing plan */
            const char *bundle)   /* I - Bundle directory */
{
    char root[PATH_MAX],          /* Canonical bundle path */
        mainexe[PATH_MAX];        /* Canonical main executable path */

    if (!realpath(bundle, root)) {
        fprintf(stderr, "epm: Unable to resolve bundle \"%s\": %s\n", bundle,
                strerror(errno));
        return (1);
    }

    if (bundle_executable(root, mainexe, sizeof(mainexe)))
        mainexe[0] = '\0';

    if (plan_walk(plan, root, mainexe[0] ? mainexe : NULL))
        return (1);

    return (plan_add(plan, root, is_app_bundle(root) ? SIGN_APP : SIGN_BUNDLE));
}

/*
 * 'codesign_path()' - Sign a file or bundle with codesign.
 */

static int                             /* O - 0 = success, 1 = fail */
codesign_path(const char *path,        /* I - File or bundle to sign */
              const char *identity,    /* I - Signing identity */
              const char *entitlements,/* I - Entitlements plist or NULL */
              const char *keychain,    /* I - Keychain holding the identity or NULL */
              int runtime)             /* I - Enable the hardened runtime? */
{
    char options[2048],     /* Additional codesign options */
        qpath[1024],         /* Quoted path */
        qidentity[1024],     /* Quoted identity */
        qvalue[1024];        /* Quoted option value */

    if (run_quote(qpath, sizeof(qpath), path) ||
        run_quote(qidentity, sizeof(qidentity), identity)) {
        fprintf(stderr, "epm: Unable to sign \"%s\" - path or identity too long.\n", path);
        return (1);
    }

    options[0] = '\0';

    if (runtime && strlcat(options, "--options runtime ", sizeof(options)) >= sizeof(options)) {
        fprintf(stderr, "epm: Unable to sign \"%s\" - options too long.\n", path);
        return (1);
    }

    if (entitlements) {
        if (run_quote(qvalue, sizeof(qvalue), entitlements) ||
            strlcat(options, "--entitlements ", sizeof(options)) >= sizeof(options) ||
            strlcat(options, qvalue, sizeof(options)) >= sizeof(options) ||
            strlcat(options, " ", sizeof(options)) >= sizeof(options)) {
            fprintf(stderr,
                    "epm: Unable to sign \"%s\" - entitlements path too long.\n", path);
            return (1);
        }
    }

    if (keychain) {
        if (run_quote(qvalue, sizeof(qvalue), keychain) ||
            strlcat(options, "--keychain ", sizeof(options)) >= sizeof(options) ||
            strlcat(options, qvalue, sizeof(options)) >= sizeof(options) ||
            strlcat(options, " ", sizeof(options)) >= sizeof(options)) {
            fprintf(stderr, "epm: Unable to sign \"%s\" - keychain path too long.\n",
                    path);
            return (1);
        }
    }

    if (Verbosity > 1)
        printf("Signing %s...\n", path);

    if (run_command(NULL, "/usr/bin/codesign --force --timestamp %s--sign %s %s", options,
                    qidentity, qpath)) {
        fprintf(stderr, "epm: Unable to sign \"%s\".\n", path);
        return (1);
    }

    return (0);
}

/*
 * 'strip_xattrs()' - Remove extended attributes before signing.
 *
 * A quarantine or Finder attribute makes codesign fail or seal a bundle it
 * later rejects as modified.
 */

static void strip_xattrs(const char *path, /* I - File or bundle */
                         int recursive)    /* I - Is path a directory? */
{
    char qpath[1024]; /* Quoted path */

    if (run_quote(qpath, sizeof(qpath), path))
        return;

    run_command(NULL, "/usr/bin/xattr %s %s", recursive ? "-cr" : "-c", qpath);
}

/*
 * 'verify_signature()' - Check a signature the way Gatekeeper will.
 */

static int                              /* O - 0 = valid, 1 = invalid */
verify_signature(const char *path,      /* I - Signed file, bundle or image */
                 int deep)              /* I - Verify nested code too? */
{
    char qpath[1024]; /* Quoted path */

    if (run_quote(qpath, sizeof(qpath), path)) {
        fprintf(stderr, "epm: Unable to verify \"%s\" - path too long.\n", path);
        return (1);
    }

    if (Verbosity > 1)
        printf("Verifying %s...\n", path);

    if (run_command(NULL, "/usr/bin/codesign --verify --strict %s--verbose=2 %s",
                    deep ? "--deep " : "", qpath)) {
        fprintf(stderr, "epm: Signature of \"%s\" does not verify.\n", path);
        return (1);
    }

    /*
     * The Gatekeeper assessment is advisory: it also fails for an ad-hoc
     * identity and for anything not yet notarized, so only show it.
     */

    if (Verbosity > 1) {
        size_t len = strlen(path);

        if (len > 4 && !strcasecmp(path + len - 4, ".dmg"))
            run_command(NULL,
                        "/usr/sbin/spctl --assess --type open "
                        "--context context:primary-signature --verbose=4 %s",
                        qpath);
        else
            run_command(NULL, "/usr/sbin/spctl --assess --type exec --verbose=4 %s",
                        qpath);
    }

    return (0);
}

/*
 * 'sign_payload()' - Sign the Mach-O files and bundles staged for a package.
 */

static int                              /* O - 0 = success, 1 = fail */
sign_payload(const char *directory,     /* I - Distribution directory */
             const char *prodfull,      /* I - Full product name */
             dist_t *dist,              /* I - Distribution information */
             const char *identity,      /* I - Signing identity */
             const char *entitlements,  /* I - Entitlements plist or NULL */
             const char *keychain)      /* I - Keychain path or NULL */
{
    int i,                     /* Looping var */
        status = 0;            /* Return status */
    file_t *file;              /* Current distribution file */
    signplan_t plan = {NULL, 0}, /* Everything to sign, in order */
        roots = {NULL, 0};     /* Outermost items, for verification */
    char path[1024],                 /* Staged path */
        dstprefix[sizeof(file->dst)]; /* Destination path of a bundle */
    const char *p,             /* Pointer into destination path */
        *start;                /* Start of the current path component */
    size_t len;                /* Length of a destination path prefix */

    if (Verbosity)
        puts("Signing staged distribution files...");

    /*
     * Collect the outermost bundles and the standalone Mach-O files; what is
     * inside a bundle is found by walking it.
     */

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++) {
        int inbundle = 0; /* Is this file inside a bundle? */

        if (tolower(file->type) != 'f' && tolower(file->type) != 'c' &&
            tolower(file->type) != 'd')
            continue;

        for (p = file->dst; *p && !inbundle; p++) {
            if (p[1] != '/' && p[1] != '\0')
                continue;

            for (start = p + 1; start > file->dst && start[-1] != '/'; start--)
                ;

            if (!is_bundle(start, (size_t)(p + 1 - start)))
                continue;

            len = (size_t)(p + 1 - file->dst);

            memcpy(dstprefix, file->dst, len);
            dstprefix[len] = '\0';

            staged_path(path, sizeof(path), directory, prodfull, dstprefix, 1);

            if (!is_bundle_dir(path))
                continue;

            inbundle = 1;

            if (plan_add(&roots, path, is_app_bundle(path) ? SIGN_APP : SIGN_BUNDLE))
                goto nomem;
        }

        if (inbundle || tolower(file->type) == 'd')
            continue;

        staged_path(path, sizeof(path), directory, prodfull, file->dst, 0);

        if (is_macho(path) &&
            plan_add(&roots, path,
                     macho_filetype(path) == MH_EXECUTE_TYPE ? SIGN_EXECUTABLE
                                                             : SIGN_LIBRARY))
            goto nomem;
    }

    for (i = 0; i < roots.count && !status; i++) {
        if (roots.items[i].kind == SIGN_APP || roots.items[i].kind == SIGN_BUNDLE)
            status = plan_bundle(&plan, roots.items[i].path);
        else
            status = plan_add(&plan, roots.items[i].path, roots.items[i].kind);
    }

    /*
     * Process entitlements belong on executables and applications; libraries
     * and plug-ins run under their host's.
     */

    for (i = 0; i < roots.count && !status; i++)
        strip_xattrs(roots.items[i].path, roots.items[i].kind == SIGN_APP ||
                                              roots.items[i].kind == SIGN_BUNDLE);

    for (i = 0; i < plan.count && !status; i++) {
        int kind = plan.items[i].kind;

        status = codesign_path(plan.items[i].path, identity,
                               (kind == SIGN_EXECUTABLE || kind == SIGN_APP) ? entitlements
                                                                             : NULL,
                               keychain, 1);
    }

    if (Verbosity && !status)
        printf("Signed %d code object(s).\n", plan.count);

    for (i = 0; i < roots.count && !status; i++)
        status = verify_signature(roots.items[i].path, roots.items[i].kind == SIGN_APP ||
                                                            roots.items[i].kind == SIGN_BUNDLE);

    plan_free(&plan);
    plan_free(&roots);

    return (status);

nomem:

    fputs("epm: Out of memory collecting files to sign.\n", stderr);

    plan_free(&plan);
    plan_free(&roots);

    return (1);
}

/*
 * 'notarize_file()' - Submit a file for notarization and staple the ticket.
 */

static int                          /* O - 0 = success, 1 = fail */
notarize_file(const char *path,     /* I - File to notarize */
              const char *profile)  /* I - notarytool keychain profile */
{
    char qpath[1024], qprofile[1024]; /* Quoted path/profile */

    if (run_quote(qpath, sizeof(qpath), path) ||
        run_quote(qprofile, sizeof(qprofile), profile)) {
        fprintf(stderr, "epm: Unable to notarize \"%s\" - path or profile too long.\n",
                path);
        return (1);
    }

    if (Verbosity)
        puts("Submitting for notarization...");

    if (run_command(NULL,
                    "/usr/bin/xcrun notarytool submit --wait --keychain-profile %s %s",
                    qprofile, qpath)) {
        fputs("epm: Unable to submit for notarization.\n", stderr);
        return (1);
    }

    if (Verbosity)
        puts("Stapling notarization ticket...");

    /*
     * notarytool exits successfully even when the Notary service rejects the
     * submission, so stapling is what actually proves the file was notarized.
     */

    if (run_command(NULL, "/usr/bin/xcrun stapler staple %s", qpath)) {
        fputs("epm: Unable to staple notarization ticket - the submission was\n"
              "     probably rejected; run notarytool log for details.\n",
              stderr);
        return (1);
    }

    return (0);
}
