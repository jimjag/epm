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
static int add_unique(char ***list, int *count, const char *path);
static int codesign_path(const char *path, const char *identity,
                         const char *entitlements, int runtime);
static int compare_depth(const void *a, const void *b);
static void free_list(char **list, int count);
static int is_bundle(const char *name, size_t len);
static int is_bundle_dir(const char *path);
static int is_macho(const char *path);
static int notarize_file(const char *path, const char *profile);
static int sign_payload(const char *directory, const char *prodfull, dist_t *dist,
                        const char *identity, const char *entitlements);
static void staged_path(char *buf, size_t bufsize, const char *directory,
                        const char *prodfull, const char *dst);

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

    if (run_command(NULL, "hdiutil create -ov -srcfolder %s/%s.pkg %s", directory,
                    prodname, dmgname)) {
        fputs("epm: Unable to create disk image.\n", stderr);
        return (1);
    }

    /*
     * Sign and notarize the disk image...
     */

    if (format == PACKAGE_MACOS_SIGNED &&
        (identity = getenv("EPM_APPLICATION_IDENTITY")) != NULL) {
        if (Verbosity)
            puts("Signing disk image...");

        if (codesign_path(dmgname, identity, NULL, 0))
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
    struct passwd *pwd;  /* Pointer to user record */
    struct group *grp;   /* Pointer to group record */
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

        pwd = getpwnam(file->user);
        grp = getgrnam(file->group);

        endpwent();
        endgrent();

        /*
         * Copy the file or make the directory or make the symlink as needed...
         */

        switch (tolower(file->type)) {
        case 'c':
        case 'f':
            staged_path(filename, sizeof(filename), directory, prodfull, file->dst);

            if (Verbosity > 1)
                printf("%s -> %s...\n", file->src, filename);

            if (copy_file(filename, file->src, file->mode, pwd ? pwd->pw_uid : 0,
                          grp ? grp->gr_gid : 0))
                return (1);
            break;
        case 'i':
            snprintf(filename, sizeof(filename),
                     "%s/%s/Package/Library/StartupItems/%s/%s", directory, prodfull,
                     file->dst, file->dst);

            if (Verbosity > 1)
                printf("%s -> %s...\n", file->src, filename);

            if (copy_file(filename, file->src, file->mode, pwd ? pwd->pw_uid : 0,
                          grp ? grp->gr_gid : 0))
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
            staged_path(filename, sizeof(filename), directory, prodfull, file->dst);

            if (Verbosity > 1)
                printf("Directory %s...\n", filename);

            make_directory(filename, file->mode, pwd ? pwd->pw_uid : 0,
                           grp ? grp->gr_gid : 0);
            break;
        case 'l':
            staged_path(filename, sizeof(filename), directory, prodfull, file->dst);

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
                                     getenv("EPM_SIGNING_ENTITLEMENTS")))
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

    if (format == PACKAGE_MACOS_SIGNED) {
        const char *identity = getenv("EPM_SIGNING_IDENTITY");
        if (!identity) {
            fputs("epm: Using default 'Developer ID Installer' signing identity.\n"
                  "     Set the EPM_SIGNING_IDENTITY environment variable to override.\n",
                  stderr);
            identity = "Developer ID Installer";
        }

        if (run_command(
                NULL,
                "/usr/bin/pkgbuild --identifier %s --version %s --ownership preserve "
                "--scripts %s/%s/Resources --root %s/%s/Package --sign '%s' %s",
                prodfull, dist->version, directory, prodfull, directory, prodfull,
                identity, pkgname)) {
            fputs("epm: Unable to build signed package.\n", stderr);
            return (1);
        }
    } else {
        if (run_command(
                NULL,
                "/usr/bin/pkgbuild --identifier %s --version %s --ownership preserve "
                "--scripts %s/%s/Resources --root %s/%s/Package %s",
                prodfull, dist->version, directory, prodfull, directory, prodfull,
                pkgname)) {
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
                        const char *dst)       /* I - Destination path */
{
    if (!strncmp(dst, "/etc/", 5) || !strncmp(dst, "/var/", 5) ||
        !strcmp(dst, "/etc") || !strcmp(dst, "/var"))
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
    unsigned char header[4];     /* Magic number */
    unsigned magic;              /* Magic number as a big-endian value */

    if ((fp = fopen(path, "rb")) == NULL)
        return (0);

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return (0);
    }

    fclose(fp);

    magic = ((unsigned)header[0] << 24) | ((unsigned)header[1] << 16) |
            ((unsigned)header[2] << 8) | (unsigned)header[3];

    return (magic == 0xfeedfaceu || magic == 0xcefaedfeu || /* 32-bit */
            magic == 0xfeedfacfu || magic == 0xcffaedfeu || /* 64-bit */
            magic == 0xcafebabeu || magic == 0xbebafecau || /* universal */
            magic == 0xcafebabfu || magic == 0xbfbafecau);  /* universal 64-bit */
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
 * 'add_unique()' - Add a path to a list if it is not already present.
 */

static int                    /* O - 1 = added, 0 = duplicate, -1 = error */
add_unique(char ***list,      /* IO - List of paths */
           int *count,        /* IO - Number of paths */
           const char *path)  /* I - Path to add */
{
    int i;                    /* Looping var */
    char **temp;              /* New list */

    for (i = 0; i < *count; i++)
        if (!strcmp((*list)[i], path))
            return (0);

    if ((temp = realloc(*list, (size_t)(*count + 1) * sizeof(char *))) == NULL)
        return (-1);

    *list = temp;

    if ((temp[*count] = strdup(path)) == NULL)
        return (-1);

    (*count)++;

    return (1);
}

/*
 * 'free_list()' - Free a list of paths.
 */

static void free_list(char **list, /* I - List of paths */
                      int count)   /* I - Number of paths */
{
    int i; /* Looping var */

    for (i = 0; i < count; i++)
        free(list[i]);

    free(list);
}

/*
 * 'compare_depth()' - Order paths from deepest to shallowest.
 */

static int                       /* O - Result of comparison */
compare_depth(const void *a,     /* I - First path */
              const void *b)     /* I - Second path */
{
    size_t la = strlen(*(const char *const *)a),
           lb = strlen(*(const char *const *)b);

    if (la != lb)
        return (la < lb ? 1 : -1);

    return (strcmp(*(const char *const *)a, *(const char *const *)b));
}

/*
 * 'codesign_path()' - Sign a file or bundle with codesign.
 */

static int                             /* O - 0 = success, 1 = fail */
codesign_path(const char *path,        /* I - File or bundle to sign */
              const char *identity,    /* I - Signing identity */
              const char *entitlements,/* I - Entitlements plist or NULL */
              int runtime)             /* I - Enable the hardened runtime? */
{
    char options[1024]; /* Additional codesign options */

    options[0] = '\0';

    if (runtime)
        strlcat(options, "--options runtime ", sizeof(options));

    if (entitlements) {
        strlcat(options, "--entitlements '", sizeof(options));
        strlcat(options, entitlements, sizeof(options));
        strlcat(options, "' ", sizeof(options));
    }

    if (Verbosity > 1)
        printf("Signing %s...\n", path);

    if (run_command(NULL, "/usr/bin/codesign --force --timestamp %s--sign '%s' '%s'",
                    options, identity, path)) {
        fprintf(stderr, "epm: Unable to sign \"%s\".\n", path);
        return (1);
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
             const char *entitlements)  /* I - Entitlements plist or NULL */
{
    int i,                     /* Looping var */
        status = 0;            /* Return status */
    file_t *file;              /* Current distribution file */
    char **bundles = NULL,     /* Bundle directories to sign */
        **binaries = NULL;     /* Standalone Mach-O files to sign */
    int nbundles = 0,          /* Number of bundles */
        nbinaries = 0;         /* Number of binaries */
    char path[1024],           /* Staged path */
        dstprefix[512];        /* Destination path of a bundle */
    const char *p,             /* Pointer into destination path */
        *start;                /* Start of the current path component */
    size_t len;                /* Length of a destination path prefix */

    if (Verbosity)
        puts("Signing staged distribution files...");

    /*
     * Collect what needs signing.  A bundle is signed as a unit, so anything
     * staged inside one is left for codesign to seal.
     */

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++) {
        int inbundle = 0; /* Is this file inside a bundle? */

        if (tolower(file->type) != 'f' && tolower(file->type) != 'c' &&
            tolower(file->type) != 'd')
            continue;

        for (p = file->dst; *p; p++) {
            if (p[1] != '/' && p[1] != '\0')
                continue;

            for (start = p + 1; start > file->dst && start[-1] != '/'; start--)
                ;

            if (!is_bundle(start, (size_t)(p + 1 - start)))
                continue;

            len = (size_t)(p + 1 - file->dst);

            if (len >= sizeof(dstprefix))
                continue;

            memcpy(dstprefix, file->dst, len);
            dstprefix[len] = '\0';

            staged_path(path, sizeof(path), directory, prodfull, dstprefix);

            if (!is_bundle_dir(path))
                continue;

            inbundle = 1;

            if (add_unique(&bundles, &nbundles, path) < 0)
                goto nomem;
        }

        if (inbundle || tolower(file->type) == 'd')
            continue;

        staged_path(path, sizeof(path), directory, prodfull, file->dst);

        if (is_macho(path) && add_unique(&binaries, &nbinaries, path) < 0)
            goto nomem;
    }

    /*
     * Sign inside-out: standalone binaries, then bundles deepest-first so a
     * nested bundle is sealed before the bundle that contains it.
     */

    if (nbundles > 1)
        qsort(bundles, (size_t)nbundles, sizeof(char *), compare_depth);

    for (i = 0; i < nbinaries && !status; i++)
        status = codesign_path(binaries[i], identity, entitlements, 1);

    for (i = 0; i < nbundles && !status; i++)
        status = codesign_path(bundles[i], identity, entitlements, 1);

    free_list(binaries, nbinaries);
    free_list(bundles, nbundles);

    return (status);

nomem:

    fputs("epm: Out of memory collecting files to sign.\n", stderr);

    free_list(binaries, nbinaries);
    free_list(bundles, nbundles);

    return (1);
}

/*
 * 'notarize_file()' - Submit a file for notarization and staple the ticket.
 */

static int                          /* O - 0 = success, 1 = fail */
notarize_file(const char *path,     /* I - File to notarize */
              const char *profile)  /* I - notarytool keychain profile */
{
    if (Verbosity)
        puts("Submitting for notarization...");

    if (run_command(NULL,
                    "/usr/bin/xcrun notarytool submit --wait --keychain-profile '%s' '%s'",
                    profile, path)) {
        fputs("epm: Unable to submit for notarization.\n", stderr);
        return (1);
    }

    if (Verbosity)
        puts("Stapling notarization ticket...");

    /*
     * notarytool exits successfully even when the Notary service rejects the
     * submission, so stapling is what actually proves the file was notarized.
     */

    if (run_command(NULL, "/usr/bin/xcrun stapler staple '%s'", path)) {
        fputs("epm: Unable to staple notarization ticket - the submission was\n"
              "     probably rejected; run notarytool log for details.\n",
              stderr);
        return (1);
    }

    return (0);
}
