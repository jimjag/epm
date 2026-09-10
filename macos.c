/*
 * macOS package gateway for the ESP Package Manager (EPM).
 *
 * Copyright © 2020 by Jim Jagielski
 * Copyright © 2002-2020 by Michael R Sweet
 * Copyright © 2002-2010 by Easy Software Products.
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

static int adhoc_identity(const char *identity);
static void assess_path(const char *path, const char *type);
static int codesign_path(const char *path, const char *identity,
                         const char *entitlements, const char *keychain, int runtime);
static int copy_resource(const char *resdir, const char *src, char *name,
                         size_t namesize);
static int hardened_runtime(void);
static int is_bundle(const char *name, size_t len);
static int is_bundle_dir(const char *path);
static int is_macho(const char *path);
static int make_app_image(const char *prodname, const char *directory,
                          const char *platname, dist_t *dist);
static int make_component(int format, const char *prodname, const char *directory,
                          dist_t *dist, const char *subpackage);
static int image_license(const char *dmgname, const char *license);
static int make_image(const char *srcfolder, const char *dmgname, const char *volname,
                      const char *license, const char *identity);
static int make_product(int format, const char *prodname, const char *directory,
                        dist_t *dist, const char *identity, const char *pkgname);
static int notarize_app(const char *app, const char *profile);
static int notarize_file(const char *path, const char *profile);
static int notarize_submit(const char *path, const char *profile);
static int pin_component_bundles(const char *plist);
static void product_fullname(const char *prodname, const char *subpackage, char *buf,
                             size_t bufsize);
static void product_identifier(const char *prodname, const char *subpackage, char *buf,
                               size_t bufsize);
static int sign_payload(const char *directory, const char *prodfull, dist_t *dist,
                        const char *subpackage, int all, const char *identity,
                        const char *entitlements, const char *keychain);
static int stage_files(const char *directory, const char *prodfull, dist_t *dist,
                       const char *subpackage, int all, const char *identifier);
static void staged_path(char *buf, size_t bufsize, const char *directory,
                        const char *prodfull, const char *dst, int isdir);
static int staple_path(const char *path);
static int verify_signature(const char *path, int deep);
static int write_component_plist(const char *rootdir, const char *plist);
static int write_distribution(const char *distfile, const char *prodname, dist_t *dist,
                              const char *license, const char *readme,
                              const char *welcome);
static int write_scripts(const char *scriptsdir, dist_t *dist, const char *subpackage,
                         const char *identifier);
static void xml_puts(FILE *fp, const char *s);

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
    int i;                /* Looping var */
    char filename[1024],  /* Destination filename */
        pkgname[1024],    /* Product package filename */
        dmgname[1024];    /* Disk image filename */
    const char *identity; /* Developer ID Application identity */

    REF(platform);
    REF(setup);

    if (format == PACKAGE_MACOS_APP)
        return (make_app_image(prodname, directory, platname, dist));

    identity = NULL;

    if (format == PACKAGE_MACOS_SIGNED &&
        (identity = getenv("EPM_APPLICATION_IDENTITY")) == NULL)
        fputs("epm: Warning - EPM_APPLICATION_IDENTITY is not set, so the package\n"
              "     payload will not be signed and the package cannot be notarized.\n",
              stderr);

    /*
     * Create the main package and subpackages (if any)...
     */

    if (make_component(format, prodname, directory, dist, NULL))
        return (1);

    for (i = 0; i < dist->num_subpackages; i++)
        if (make_component(format, prodname, directory, dist, dist->subpackages[i]))
            return (1);

    /*
     * Wrap them in a product archive...
     */

    snprintf(pkgname, sizeof(pkgname), "%s/%s.pkg", directory, prodname);

    if (make_product(format, prodname, directory, dist, identity, pkgname))
        return (1);

    /*
     * TODO: Copy uninstall application to disk image...
     */

    /*
     * Create a disk image of the package...
     */

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

    if (make_image(pkgname, dmgname, dist->product, NULL, identity))
        return (1);

    /*
     * Remove temporary files...
     */

    if (!KeepFiles) {
        if (Verbosity)
            puts("Removing temporary distribution files...");

        snprintf(filename, sizeof(filename), "%s/%s", directory, prodname);
        unlink_directory(filename);

        for (i = 0; i < dist->num_subpackages; i++) {
            char prodfull[256]; /* Full subpackage name */

            product_fullname(prodname, dist->subpackages[i], prodfull, sizeof(prodfull));
            snprintf(filename, sizeof(filename), "%s/%s", directory, prodfull);
            unlink_directory(filename);
        }
    }

    return (0);
}

/*
 * 'product_fullname()' - Get the full name of a package or subpackage.
 */

static void product_fullname(const char *prodname,   /* I - Product short name */
                             const char *subpackage, /* I - Subpackage or NULL */
                             char *buf,              /* O - Full name */
                             size_t bufsize)         /* I - Size of buffer */
{
    /*
     * OpenOffice builds have traditionally used '_' instead of '-'...
     */

    if (subpackage)
        snprintf(buf, bufsize, "%s%s%s", prodname, AooMode ? "_" : "-", subpackage);
    else
        strlcpy(buf, prodname, bufsize);
}

/*
 * 'product_identifier()' - Get the package identifier used for receipts.
 */

static void product_identifier(const char *prodname,   /* I - Product short name */
                               const char *subpackage, /* I - Subpackage or NULL */
                               char *buf,              /* O - Identifier */
                               size_t bufsize)         /* I - Size of buffer */
{
    const char *base = getenv("EPM_MACOS_IDENTIFIER"); /* Reverse-DNS base */

    if (!base || !base[0]) {
        product_fullname(prodname, subpackage, buf, bufsize);
        return;
    }

    if (subpackage)
        snprintf(buf, bufsize, "%s.%s", base, subpackage);
    else
        strlcpy(buf, base, bufsize);
}

/*
 * 'adhoc_identity()' - Is this the codesign ad-hoc identity?
 */

static int                          /* O - 1 if ad-hoc, 0 otherwise */
adhoc_identity(const char *identity) /* I - Signing identity or NULL */
{
    return (identity && !strcmp(identity, "-"));
}

/*
 * 'hardened_runtime()' - Should executables get the hardened runtime?
 */

static int hardened_runtime(void) /* O - 1 = yes, 0 = no */
{
    const char *value = getenv("EPM_SIGNING_HARDENED_RUNTIME");

    if (!value)
        return (1);

    return (strcasecmp(value, "no") && strcasecmp(value, "off") &&
            strcasecmp(value, "false") && strcmp(value, "0"));
}

/*
 * 'make_component()' - Make a component package for the main package or a
 *                      subpackage.
 */

static int                                     /* O - 0 = success, 1 = fail */
make_component(int format,                     /* I - Format */
               const char *prodname,           /* I - Product short name */
               const char *directory,          /* I - Directory for distribution files */
               dist_t *dist,                   /* I - Distribution information */
               const char *subpackage)         /* I - Subpackage or NULL */
{
    char prodfull[256],     /* Full product name */
        identifier[256],    /* Package identifier */
        rootdir[1024],      /* Staged payload directory */
        scriptsdir[1024],   /* Scripts directory */
        compdir[1024],      /* Component package directory */
        plist[1024],        /* Component property list */
        pkgname[1024],      /* Component package filename */
        qidentifier[1024],  /* Quoted package identifier */
        qversion[1024],     /* Quoted version */
        qscriptsdir[1024],  /* Quoted scripts directory */
        qrootdir[1024],     /* Quoted payload directory */
        qplist[1024],       /* Quoted component plist */
        plistopt[1100],     /* --component-plist option or empty */
        qpkgname[1024];     /* Quoted package filename */
    const char *ownership;  /* pkgbuild --ownership value */
    static int warned = 0;  /* Ownership warning shown? */

    product_fullname(prodname, subpackage, prodfull, sizeof(prodfull));
    product_identifier(prodname, subpackage, identifier, sizeof(identifier));

    if (Verbosity)
        printf("Creating %s macOS component package...\n", prodfull);

    snprintf(rootdir, sizeof(rootdir), "%s/%s/Package", directory, prodfull);
    snprintf(scriptsdir, sizeof(scriptsdir), "%s/%s/Scripts", directory, prodfull);

    make_directory(rootdir, 0755, 0, 0);
    make_directory(scriptsdir, 0755, 0, 0);

    if (write_scripts(scriptsdir, dist, subpackage, identifier))
        return (1);

    if (stage_files(directory, prodfull, dist, subpackage, 0, identifier))
        return (1);

    /*
     * Sign the staged payload before it is sealed into the package...
     */

    if (format == PACKAGE_MACOS_SIGNED) {
        const char *identity = getenv("EPM_APPLICATION_IDENTITY");

        if (identity && sign_payload(directory, prodfull, dist, subpackage, 0, identity,
                                     getenv("EPM_SIGNING_ENTITLEMENTS"),
                                     getenv("EPM_SIGNING_KEYCHAIN")))
            return (1);
    }

    /*
     * Pin every bundle to the path in the list file...
     */

    snprintf(plist, sizeof(plist), "%s/%s/Component.plist", directory, prodfull);

    plistopt[0] = '\0';

    if (!write_component_plist(rootdir, plist)) {
        if (run_quote(qplist, sizeof(qplist), plist) ||
            snprintf(plistopt, sizeof(plistopt), "--component-plist %s ", qplist) >=
                (int)sizeof(plistopt)) {
            fputs("epm: A package path is too long to quote safely.\n", stderr);
            return (1);
        }
    }

    /*
     * Build the component...
     */

    if (Verbosity)
        puts("Building macOS component package...");

    snprintf(compdir, sizeof(compdir), "%s/%s/Components", directory, prodname);
    make_directory(compdir, 0755, 0, 0);

    snprintf(pkgname, sizeof(pkgname), "%s/%s.pkg", compdir, prodfull);

    if (run_quote(qidentifier, sizeof(qidentifier), identifier) ||
        run_quote(qversion, sizeof(qversion), dist->version) ||
        run_quote(qscriptsdir, sizeof(qscriptsdir), scriptsdir) ||
        run_quote(qrootdir, sizeof(qrootdir), rootdir) ||
        run_quote(qpkgname, sizeof(qpkgname), pkgname)) {
        fputs("epm: A package path is too long to quote safely.\n", stderr);
        return (1);
    }

    /*
     * chown() fails silently for a non-root build, so the staged tree carries
     * the builder's uid; let pkgbuild assign ownership in that case.
     */

    if (getuid()) {
        ownership = "recommended";

        if (!warned) {
            fputs("epm: Warning - not running as root, so the list file's owners and\n"
                  "     groups are replaced by pkgbuild's recommended ownership.\n",
                  stderr);
            warned = 1;
        }
    } else
        ownership = "preserve";

    if (run_command(NULL,
                    "/usr/bin/pkgbuild --identifier %s --version %s --ownership %s "
                    "--scripts %s --root %s %s%s",
                    qidentifier, qversion, ownership, qscriptsdir, qrootdir, plistopt,
                    qpkgname)) {
        fputs("epm: Unable to build component package.\n", stderr);
        return (1);
    }

    if (access(pkgname, 0))
        return (1);

    return (0);
}

/*
 * 'write_scripts()' - Write the preinstall and postinstall scripts.
 */

static int                                   /* O - 0 = success, 1 = fail */
write_scripts(const char *scriptsdir,        /* I - Scripts directory */
              dist_t *dist,                  /* I - Distribution information */
              const char *subpackage,        /* I - Subpackage or NULL */
              const char *identifier)        /* I - Package identifier */
{
    int i;              /* Looping var */
    FILE *fp;           /* Script file */
    char filename[1024]; /* Script filename */
    command_t *c;       /* Current command */
    file_t *file;       /* Current distribution file */

    for (i = dist->num_commands, c = dist->commands; i > 0; i--, c++)
        if (c->type == COMMAND_PRE_INSTALL && c->subpackage == subpackage)
            break;

    if (i) {
        snprintf(filename, sizeof(filename), "%s/preinstall", scriptsdir);

        if ((fp = fopen(filename, "w")) == NULL) {
            fprintf(stderr, "epm: Unable to create preinstall script \"%s\": %s\n",
                    filename, strerror(errno));
            return (1);
        }

        fputs("#!/bin/sh\n", fp);

        for (; i > 0; i--, c++)
            if (c->type == COMMAND_PRE_INSTALL && c->subpackage == subpackage)
                fprintf(fp, "%s\n", c->command);

        fclose(fp);
        chmod(filename, 0755);
    }

    for (i = dist->num_commands, c = dist->commands; i > 0; i--, c++)
        if (c->type == COMMAND_POST_INSTALL && c->subpackage == subpackage)
            break;

    if (!i) {
        for (i = dist->num_files, file = dist->files; i > 0; i--, file++)
            if (tolower(file->type) == 'i' && file->subpackage == subpackage)
                break;
    }

    if (!i)
        return (0);

    snprintf(filename, sizeof(filename), "%s/postinstall", scriptsdir);

    if ((fp = fopen(filename, "w")) == NULL) {
        fprintf(stderr, "epm: Unable to create postinstall script \"%s\": %s\n",
                filename, strerror(errno));
        return (1);
    }

    fputs("#!/bin/sh\n", fp);

    for (i = dist->num_commands, c = dist->commands; i > 0; i--, c++)
        if (c->type == COMMAND_POST_INSTALL && c->subpackage == subpackage)
            fprintf(fp, "%s\n", c->command);

    /*
     * SystemStarter ran StartupItems through 10.9; later systems only have
     * launchd, so start the service whichever way this system supports.
     */

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++) {
        if (tolower(file->type) != 'i' || file->subpackage != subpackage)
            continue;

        fputs("if [ -x /sbin/SystemStarter ]; then\n", fp);
        qprintf(fp, "    /Library/StartupItems/%s/%s start\n", file->dst, file->dst);
        fputs("else\n", fp);
        qprintf(fp, "    launchctl bootout system /Library/LaunchDaemons/%s.%s.plist "
                    "2>/dev/null\n",
                identifier, file->dst);
        qprintf(fp, "    launchctl bootstrap system /Library/LaunchDaemons/%s.%s.plist "
                    "2>/dev/null || launchctl load -w /Library/LaunchDaemons/%s.%s.plist\n",
                identifier, file->dst, identifier, file->dst);
        fputs("fi\n", fp);
    }

    fclose(fp);
    chmod(filename, 0755);

    return (0);
}

/*
 * 'stage_files()' - Copy the distribution files into the payload directory.
 */

static int                            /* O - 0 = success, 1 = fail */
stage_files(const char *directory,    /* I - Directory for distribution files */
            const char *prodfull,     /* I - Full product name */
            dist_t *dist,             /* I - Distribution information */
            const char *subpackage,   /* I - Subpackage or NULL */
            int all,                  /* I - Stage every subpackage? */
            const char *identifier)   /* I - Package identifier */
{
    int i;               /* Looping var */
    FILE *fp;            /* Plist file */
    char filename[1024]; /* Destination filename */
    file_t *file;        /* Current distribution file */
    uid_t uid;           /* Resolved owner ID */
    gid_t gid;           /* Resolved group ID */
    const char *option;  /* Init script option */

    if (Verbosity)
        puts("Copying temporary distribution files...");

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++) {
        if (!all && file->subpackage != subpackage)
            continue;

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

            /*
             * The launchd job runs the same StartupItems script once at boot,
             * for systems without SystemStarter.
             */

            snprintf(filename, sizeof(filename), "%s/%s/Package/Library/LaunchDaemons",
                     directory, prodfull);
            make_directory(filename, 0755, 0, 0);

            snprintf(filename, sizeof(filename),
                     "%s/%s/Package/Library/LaunchDaemons/%s.%s.plist", directory,
                     prodfull, identifier, file->dst);
            if ((fp = fopen(filename, "w")) == NULL) {
                fprintf(stderr, "epm: Unable to create launchd job file \"%s\": %s\n",
                        filename, strerror(errno));
                return (1);
            }

            fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", fp);
            fputs("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                  "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n",
                  fp);
            fputs("<plist version=\"1.0\">\n<dict>\n", fp);
            fputs("\t<key>Label</key>\n\t<string>", fp);
            xml_puts(fp, identifier);
            fputs(".", fp);
            xml_puts(fp, file->dst);
            fputs("</string>\n", fp);
            fputs("\t<key>ProgramArguments</key>\n\t<array>\n\t\t<string>"
                  "/Library/StartupItems/",
                  fp);
            xml_puts(fp, file->dst);
            fputs("/", fp);
            xml_puts(fp, file->dst);
            fputs("</string>\n\t\t<string>start</string>\n\t</array>\n", fp);
            fputs("\t<key>RunAtLoad</key>\n\t<true/>\n", fp);
            fputs("</dict>\n</plist>\n", fp);

            fclose(fp);
            chmod(filename, 0644);
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

    return (0);
}

/*
 * 'pin_component_bundles()' - Turn off relocation for every bundle in a
 *                             component property list.
 */

static int                                  /* O - 0 = success, 1 = fail */
pin_component_bundles(const char *plist)    /* I - Component plist to rewrite */
{
    FILE *fp;                 /* Plist file */
    char *data,               /* Plist contents */
        *ptr,                 /* Pointer into contents */
        *value;               /* Pointer to the key's value */
    long size;                /* Plist size */
    size_t len;               /* Bytes read */
    static const char key[] = "<key>BundleIsRelocatable</key>";

    if ((fp = fopen(plist, "rb")) == NULL)
        return (1);

    if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 || size > 4194304 ||
        fseek(fp, 0, SEEK_SET) || (data = malloc((size_t)size + 1)) == NULL) {
        fclose(fp);
        return (1);
    }

    len = fread(data, 1, (size_t)size, fp);
    fclose(fp);
    data[len] = '\0';

    if ((fp = fopen(plist, "wb")) == NULL) {
        free(data);
        return (1);
    }

    for (ptr = data; (value = strstr(ptr, key)) != NULL;) {
        value += sizeof(key) - 1;
        while (isspace(*value & 255))
            value++;

        fwrite(ptr, 1, (size_t)(value - ptr), fp);

        if (!strncmp(value, "<true/>", 7)) {
            fputs("<false/>", fp);
            ptr = value + 7;
        } else
            ptr = value;
    }

    fputs(ptr, fp);
    free(data);

    return (fclose(fp) != 0);
}

/*
 * 'write_component_plist()' - Analyze the payload and pin its bundles in place.
 *
 * pkgbuild marks every bundle relocatable by default, which lets Installer put
 * it wherever Spotlight finds an older copy with the same bundle id.
 */

static int                                  /* O - 0 = written, 1 = unavailable */
write_component_plist(const char *rootdir,  /* I - Staged payload directory */
                      const char *plist)    /* I - Component plist to write */
{
    char qrootdir[1024],      /* Quoted payload directory */
        qplist[1024];         /* Quoted plist filename */

    if (run_quote(qrootdir, sizeof(qrootdir), rootdir) ||
        run_quote(qplist, sizeof(qplist), plist))
        return (1);

    if (run_command(NULL, "/usr/bin/pkgbuild --analyze --root %s %s", qrootdir,
                    qplist)) {
        fputs("epm: Warning - unable to analyze the payload; bundles keep pkgbuild's\n"
              "     relocation defaults.\n",
              stderr);
        return (1);
    }

    return (pin_component_bundles(plist));
}

/*
 * 'copy_resource()' - Copy a license or readme into the product resources.
 */

static int                          /* O - 0 = success, 1 = fail */
copy_resource(const char *resdir,   /* I - Resources directory */
              const char *src,      /* I - Source file */
              char *name,           /* O - Resource name */
              size_t namesize)      /* I - Size of name buffer */
{
    const char *base;               /* Basename of source */
    char filename[1024];            /* Destination filename */

    if ((base = strrchr(src, '/')) != NULL)
        base++;
    else
        base = src;

    strlcpy(name, base, namesize);
    snprintf(filename, sizeof(filename), "%s/%s", resdir, name);

    return (copy_file(filename, src, 0444, (uid_t)-1, (gid_t)-1));
}

/*
 * 'make_product()' - Wrap the component packages in a product archive.
 */

static int                              /* O - 0 = success, 1 = fail */
make_product(int format,                /* I - Format */
             const char *prodname,      /* I - Product short name */
             const char *directory,     /* I - Directory for distribution files */
             dist_t *dist,              /* I - Distribution information */
             const char *identity,      /* I - Application identity or NULL */
             const char *pkgname)       /* I - Product package filename */
{
    int i;                  /* Looping var */
    FILE *fp;               /* Welcome file */
    char resdir[1024],      /* Resources directory */
        compdir[1024],      /* Components directory */
        distfile[1024],     /* Distribution filename */
        filename[1024],     /* Welcome filename */
        license[256],       /* License resource name */
        readme[256],        /* Readme resource name */
        qdistfile[1024],    /* Quoted distribution filename */
        qcompdir[1024],     /* Quoted components directory */
        qresdir[1024],      /* Quoted resources directory */
        qpkgname[1024],     /* Quoted package filename */
        signopts[2200];     /* --sign/--keychain options or empty */
    const char *welcome,    /* Welcome resource name or NULL */
        *profile;           /* notarytool keychain profile */
    int sign;               /* Sign the product? */

    if (Verbosity)
        puts("Building macOS product package...");

    snprintf(resdir, sizeof(resdir), "%s/%s/Resources", directory, prodname);
    snprintf(compdir, sizeof(compdir), "%s/%s/Components", directory, prodname);
    snprintf(distfile, sizeof(distfile), "%s/%s/Distribution", directory, prodname);

    make_directory(resdir, 0755, 0, 0);

    license[0] = '\0';
    readme[0] = '\0';
    welcome = NULL;

    if (dist->license[0] && copy_resource(resdir, dist->license, license, sizeof(license)))
        return (1);

    if (dist->readme[0] && copy_resource(resdir, dist->readme, readme, sizeof(readme)))
        return (1);

    for (i = 0; i < dist->num_descriptions; i++)
        if (dist->descriptions[i].subpackage == NULL)
            break;

    if (i < dist->num_descriptions) {
        snprintf(filename, sizeof(filename), "%s/Welcome.txt", resdir);

        if ((fp = fopen(filename, "w")) == NULL) {
            fprintf(stderr, "epm: Unable to create welcome file \"%s\": %s\n", filename,
                    strerror(errno));
            return (1);
        }

        fprintf(fp, "%s %s\n\n", dist->product, dist->version);

        for (; i < dist->num_descriptions; i++)
            if (dist->descriptions[i].subpackage == NULL)
                fprintf(fp, "%s\n", dist->descriptions[i].description);

        fclose(fp);

        welcome = "Welcome.txt";
    }

    if (write_distribution(distfile, prodname, dist, license[0] ? license : NULL,
                           readme[0] ? readme : NULL, welcome))
        return (1);

    if (run_quote(qdistfile, sizeof(qdistfile), distfile) ||
        run_quote(qcompdir, sizeof(qcompdir), compdir) ||
        run_quote(qresdir, sizeof(qresdir), resdir) ||
        run_quote(qpkgname, sizeof(qpkgname), pkgname)) {
        fputs("epm: A package path is too long to quote safely.\n", stderr);
        return (1);
    }

    signopts[0] = '\0';
    sign = (format == PACKAGE_MACOS_SIGNED);

    if (sign && adhoc_identity(identity)) {
        fputs("epm: Ad-hoc application identity - the product package is not signed.\n",
              stderr);
        sign = 0;
    }

    if (sign) {
        const char *signid = getenv("EPM_SIGNING_IDENTITY"),
            *keychain = getenv("EPM_SIGNING_KEYCHAIN");
        char qvalue[1024]; /* Quoted identity/keychain */

        if (!signid) {
            fputs("epm: Using default 'Developer ID Installer' signing identity.\n"
                  "     Set the EPM_SIGNING_IDENTITY environment variable to "
                  "override.\n",
                  stderr);
            signid = "Developer ID Installer";
        }

        if (run_quote(qvalue, sizeof(qvalue), signid) ||
            snprintf(signopts, sizeof(signopts), "--sign %s ", qvalue) >=
                (int)sizeof(signopts)) {
            fputs("epm: EPM_SIGNING_IDENTITY is too long.\n", stderr);
            return (1);
        }

        if (keychain) {
            if (run_quote(qvalue, sizeof(qvalue), keychain) ||
                strlcat(signopts, "--keychain ", sizeof(signopts)) >= sizeof(signopts) ||
                strlcat(signopts, qvalue, sizeof(signopts)) >= sizeof(signopts) ||
                strlcat(signopts, " ", sizeof(signopts)) >= sizeof(signopts)) {
                fputs("epm: EPM_SIGNING_KEYCHAIN is too long.\n", stderr);
                return (1);
            }
        }
    }

    if (run_command(NULL,
                    "/usr/bin/productbuild --distribution %s --package-path %s "
                    "--resources %s %s%s",
                    qdistfile, qcompdir, qresdir, signopts, qpkgname)) {
        fprintf(stderr, "epm: Unable to build %sproduct package.\n", sign ? "signed " : "");
        return (1);
    }

    if (access(pkgname, 0))
        return (1);

    if (!sign)
        return (0);

    if (Verbosity > 1)
        printf("Verifying %s...\n", pkgname);

    if (run_command(NULL, "/usr/sbin/pkgutil --check-signature %s", qpkgname)) {
        fprintf(stderr, "epm: Signature of \"%s\" does not verify.\n", pkgname);
        return (1);
    }

    /*
     * Staple the package itself so a copy taken out of the disk image still
     * passes Gatekeeper offline...
     */

    if (identity && (profile = getenv("EPM_NOTARY_KEYCHAIN_PROFILE")) != NULL &&
        notarize_file(pkgname, profile))
        return (1);

    assess_path(pkgname, "install");

    return (0);
}

/*
 * 'xml_puts()' - Write a string with XML special characters escaped.
 */

static void xml_puts(FILE *fp,       /* I - File */
                     const char *s)  /* I - String */
{
    for (; *s; s++) {
        switch (*s) {
        case '&':
            fputs("&amp;", fp);
            break;
        case '<':
            fputs("&lt;", fp);
            break;
        case '>':
            fputs("&gt;", fp);
            break;
        case '"':
            fputs("&quot;", fp);
            break;
        default:
            putc(*s, fp);
            break;
        }
    }
}

/*
 * 'write_distribution()' - Write the Distribution file for productbuild.
 */

static int                                  /* O - 0 = success, 1 = fail */
write_distribution(const char *distfile,    /* I - Distribution filename */
                   const char *prodname,    /* I - Product short name */
                   dist_t *dist,            /* I - Distribution information */
                   const char *license,     /* I - License resource or NULL */
                   const char *readme,      /* I - Readme resource or NULL */
                   const char *welcome)     /* I - Welcome resource or NULL */
{
    int i, j;               /* Looping vars */
    FILE *fp;               /* Distribution file */
    char identifier[256],   /* Package identifier */
        prodfull[256];      /* Full package name */
    const char *subpackage; /* Current subpackage */

    if ((fp = fopen(distfile, "w")) == NULL) {
        fprintf(stderr, "epm: Unable to create distribution file \"%s\": %s\n", distfile,
                strerror(errno));
        return (1);
    }

    fputs("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n", fp);
    fputs("<installer-gui-script minSpecVersion=\"1\">\n", fp);
    fputs("    <title>", fp);
    xml_puts(fp, dist->product);
    fputs("</title>\n", fp);
    fprintf(fp, "    <options customize=\"%s\"/>\n",
            dist->num_subpackages ? "allow" : "never");

    if (welcome) {
        fputs("    <welcome file=\"", fp);
        xml_puts(fp, welcome);
        fputs("\"/>\n", fp);
    }

    if (license) {
        fputs("    <license file=\"", fp);
        xml_puts(fp, license);
        fputs("\"/>\n", fp);
    }

    if (readme) {
        fputs("    <readme file=\"", fp);
        xml_puts(fp, readme);
        fputs("\"/>\n", fp);
    }

    fputs("    <choices-outline>\n        <line choice=\"default\">\n", fp);

    for (i = -1; i < dist->num_subpackages; i++) {
        product_identifier(prodname, i < 0 ? NULL : dist->subpackages[i], identifier,
                           sizeof(identifier));
        fputs("            <line choice=\"", fp);
        xml_puts(fp, identifier);
        fputs("\"/>\n", fp);
    }

    fputs("        </line>\n    </choices-outline>\n    <choice id=\"default\"/>\n", fp);

    for (i = -1; i < dist->num_subpackages; i++) {
        subpackage = i < 0 ? NULL : dist->subpackages[i];

        product_identifier(prodname, subpackage, identifier, sizeof(identifier));

        fputs("    <choice id=\"", fp);
        xml_puts(fp, identifier);

        if (!subpackage)
            fputs("\" visible=\"false\"", fp);
        else {
            /*
             * The first description line names the choice, the rest describe it.
             */

            for (j = 0; j < dist->num_descriptions; j++)
                if (dist->descriptions[j].subpackage == subpackage)
                    break;

            fputs("\" title=\"", fp);
            xml_puts(fp, j < dist->num_descriptions ? dist->descriptions[j].description
                                                    : subpackage);
            fputs("\"", fp);

            if (j < dist->num_descriptions && j + 1 < dist->num_descriptions) {
                int first = 1; /* First description line? */

                for (j++; j < dist->num_descriptions; j++) {
                    if (dist->descriptions[j].subpackage != subpackage)
                        continue;

                    fputs(first ? " description=\"" : " ", fp);
                    xml_puts(fp, dist->descriptions[j].description);
                    first = 0;
                }

                if (!first)
                    fputs("\"", fp);
            }
        }

        fputs(">\n        <pkg-ref id=\"", fp);
        xml_puts(fp, identifier);
        fputs("\"/>\n    </choice>\n", fp);
    }

    for (i = -1; i < dist->num_subpackages; i++) {
        subpackage = i < 0 ? NULL : dist->subpackages[i];

        product_identifier(prodname, subpackage, identifier, sizeof(identifier));
        product_fullname(prodname, subpackage, prodfull, sizeof(prodfull));

        fputs("    <pkg-ref id=\"", fp);
        xml_puts(fp, identifier);
        fputs("\" version=\"", fp);
        xml_puts(fp, dist->version);
        fputs("\" onConclusion=\"none\">", fp);
        xml_puts(fp, prodfull);
        fputs(".pkg</pkg-ref>\n", fp);
    }

    fputs("</installer-gui-script>\n", fp);

    fclose(fp);

    return (0);
}

/*
 * 'make_image()' - Create, sign and notarize a disk image.
 */

static int                          /* O - 0 = success, 1 = fail */
make_image(const char *srcfolder,   /* I - File or directory to image */
           const char *dmgname,     /* I - Disk image filename */
           const char *volname,     /* I - Volume name */
           const char *license,     /* I - License to show on mount or NULL */
           const char *identity)    /* I - Application identity or NULL */
{
    char qsrcfolder[1024], /* Quoted source */
        qvolname[1024],    /* Quoted volume name */
        qdmgname[1024];    /* Quoted disk image filename */
    const char *profile;   /* notarytool keychain profile */

    if (Verbosity)
        puts("Creating disk image...");

    if (run_quote(qsrcfolder, sizeof(qsrcfolder), srcfolder) ||
        run_quote(qvolname, sizeof(qvolname), volname) ||
        run_quote(qdmgname, sizeof(qdmgname), dmgname)) {
        fputs("epm: A disk image path is too long to quote safely.\n", stderr);
        return (1);
    }

    /*
     * HFS+ so the image mounts on releases before APFS; UDZO is the default
     * on current systems but is spelled out so the output does not depend on
     * the host.
     */

    if (run_command(NULL,
                    "hdiutil create -ov -fs HFS+ -format UDZO -volname %s -srcfolder %s %s",
                    qvolname, qsrcfolder, qdmgname)) {
        fputs("epm: Unable to create disk image.\n", stderr);
        return (1);
    }

    if (license && image_license(dmgname, license))
        return (1);

    if (!identity)
        return (0);

    if (adhoc_identity(identity)) {
        fputs("epm: Ad-hoc application identity - the disk image is not signed.\n",
              stderr);
        return (0);
    }

    if (Verbosity)
        puts("Signing disk image...");

    if (codesign_path(dmgname, identity, NULL, getenv("EPM_SIGNING_KEYCHAIN"), 0) ||
        verify_signature(dmgname, 0))
        return (1);

    if ((profile = getenv("EPM_NOTARY_KEYCHAIN_PROFILE")) != NULL &&
        notarize_file(dmgname, profile))
        return (1);

    assess_path(dmgname, "open");

    return (0);
}

/*
 * 'base64_put()' - Write bytes to a file as base64.
 */

static void base64_put(FILE *fp,                 /* I - File */
                       const unsigned char *data, /* I - Bytes */
                       size_t len)               /* I - Number of bytes */
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    for (; len >= 3; data += 3, len -= 3) {
        putc(alphabet[data[0] >> 2], fp);
        putc(alphabet[((data[0] & 3) << 4) | (data[1] >> 4)], fp);
        putc(alphabet[((data[1] & 15) << 2) | (data[2] >> 6)], fp);
        putc(alphabet[data[2] & 63], fp);
    }

    if (len == 2) {
        putc(alphabet[data[0] >> 2], fp);
        putc(alphabet[((data[0] & 3) << 4) | (data[1] >> 4)], fp);
        putc(alphabet[(data[1] & 15) << 2], fp);
        putc('=', fp);
    } else if (len == 1) {
        putc(alphabet[data[0] >> 2], fp);
        putc(alphabet[(data[0] & 3) << 4], fp);
        fputs("==", fp);
    }
}

/*
 * 'resource_put()' - Write one resource of a udifrez XML file.
 */

static void resource_put(FILE *fp,                  /* I - File */
                         const char *type,          /* I - Resource type */
                         const unsigned char *data, /* I - Resource data */
                         size_t len)                /* I - Length of data */
{
    fprintf(fp, "\t<key>%s</key>\n\t<array>\n\t\t<dict>\n", type);
    fputs("\t\t\t<key>Attributes</key>\n\t\t\t<string>0x0000</string>\n", fp);
    fputs("\t\t\t<key>Data</key>\n\t\t\t<data>", fp);
    base64_put(fp, data, len);
    fputs("</data>\n", fp);
    fputs("\t\t\t<key>ID</key>\n\t\t\t<string>5000</string>\n", fp);
    fputs("\t\t\t<key>Name</key>\n\t\t\t<string>English</string>\n", fp);
    fputs("\t\t</dict>\n\t</array>\n", fp);
}

/*
 * 'image_license()' - Attach a license agreement to a disk image.
 *
 * The agreement is the classic LPic/STR#/TEXT resource set that the Finder
 * shows before mounting; udifrez takes it as an XML resource file.
 */

static int                              /* O - 0 = success, 1 = fail */
image_license(const char *dmgname,      /* I - Disk image */
              const char *license)      /* I - License text file */
{
    FILE *fp;                           /* License/resource file */
    char xmlname[1024],                 /* Resource file name */
        qxmlname[1024],                 /* Quoted resource file name */
        qdmgname[1024];                 /* Quoted image name */
    unsigned char *text;                /* License text */
    long size;                          /* License size */
    size_t i,                           /* Looping var */
        len;                            /* Length of text */
    int status;                         /* Return status */
    static const unsigned char lpic[] = /* One language entry: English, id 5000 + 0 */
        {0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    static const unsigned char strings[] = /* STR#: language and button names */
        "\000\006"
        "\007English"
        "\005Agree"
        "\010Disagree"
        "\005Print"
        "\007Save..."
        "\172" "If you agree with the terms of this license, click \"Agree\" to "
        "access the software.  If you do not agree, click \"Disagree\".";

    if ((fp = fopen(license, "rb")) == NULL) {
        fprintf(stderr, "epm: Unable to open license file \"%s\": %s\n", license,
                strerror(errno));
        return (1);
    }

    if (fseek(fp, 0, SEEK_END) || (size = ftell(fp)) < 0 || size > 1048576 ||
        fseek(fp, 0, SEEK_SET) || (text = malloc((size_t)size + 1)) == NULL) {
        fprintf(stderr, "epm: Unable to read license file \"%s\".\n", license);
        fclose(fp);
        return (1);
    }

    len = fread(text, 1, (size_t)size, fp);
    fclose(fp);

    /*
     * TEXT resources use classic Mac line endings...
     */

    for (i = 0; i < len; i++) {
        if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') {
            memmove(text + i, text + i + 1, len - i - 1);
            len--;
        }

        if (text[i] == '\n')
            text[i] = '\r';
    }

    snprintf(xmlname, sizeof(xmlname), "%s.sla.xml", dmgname);

    if ((fp = fopen(xmlname, "w")) == NULL) {
        fprintf(stderr, "epm: Unable to create \"%s\": %s\n", xmlname, strerror(errno));
        free(text);
        return (1);
    }

    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", fp);
    fputs("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
          "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n",
          fp);
    fputs("<plist version=\"1.0\">\n<dict>\n", fp);
    resource_put(fp, "LPic", lpic, sizeof(lpic));
    resource_put(fp, "STR#", strings, sizeof(strings) - 1);
    resource_put(fp, "TEXT", text, len);
    fputs("</dict>\n</plist>\n", fp);

    status = fclose(fp) != 0;
    free(text);

    if (status || run_quote(qxmlname, sizeof(qxmlname), xmlname) ||
        run_quote(qdmgname, sizeof(qdmgname), dmgname)) {
        fprintf(stderr, "epm: Unable to write \"%s\".\n", xmlname);
        unlink(xmlname);
        return (1);
    }

    if (Verbosity)
        puts("Adding license agreement to disk image...");

    status = run_command(NULL, "hdiutil udifrez %s -xml %s", qdmgname, qxmlname);

    if (!KeepFiles)
        unlink(xmlname);

    if (status) {
        fputs("epm: Unable to add the license agreement to the disk image.\n", stderr);
        return (1);
    }

    return (0);
}

/*
 * 'make_app_image()' - Make a drag-install disk image of application bundles.
 */

static int                              /* O - 0 = success, 1 = fail */
make_app_image(const char *prodname,    /* I - Product short name */
               const char *directory,   /* I - Directory for distribution files */
               const char *platname,    /* I - Platform name */
               dist_t *dist)            /* I - Distribution information */
{
    int i;                    /* Looping var */
    char identifier[256],     /* Package identifier */
        srcfolder[1024],      /* Staged /Applications directory */
        filename[1024],       /* Destination filename */
        dmgname[1024];        /* Disk image filename */
    file_t *file;             /* Current distribution file */
    const char *identity,     /* Application identity or NULL */
        *profile;             /* notarytool keychain profile */

    if (Verbosity)
        printf("Creating %s macOS application image...\n", prodname);

    /*
     * Everything on a drag-install image lands in /Applications...
     */

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++) {
        if (tolower(file->type) == 'i' ||
            (strcmp(file->dst, "/Applications") &&
             strncmp(file->dst, "/Applications/", 14))) {
            fprintf(stderr,
                    "epm: The macos-app format only packages files under /Applications;\n"
                    "     \"%s\" cannot be installed by dragging.\n",
                    file->dst);
            return (1);
        }

        if (!strcmp(file->dst, "/Applications/Applications")) {
            fputs("epm: \"/Applications/Applications\" collides with the drag-install "
                  "link.\n",
                  stderr);
            return (1);
        }
    }

    product_identifier(prodname, NULL, identifier, sizeof(identifier));

    if (stage_files(directory, prodname, dist, NULL, 1, identifier))
        return (1);

    snprintf(srcfolder, sizeof(srcfolder), "%s/%s/Package/Applications", directory,
             prodname);
    make_directory(srcfolder, 0755, 0, 0);

    identity = getenv("EPM_APPLICATION_IDENTITY");

    if (identity && sign_payload(directory, prodname, dist, NULL, 1, identity,
                                 getenv("EPM_SIGNING_ENTITLEMENTS"),
                                 getenv("EPM_SIGNING_KEYCHAIN")))
        return (1);

    /*
     * Staple each application before it goes on the image, so a copy dragged
     * out passes Gatekeeper offline...
     */

    if (identity && !adhoc_identity(identity) &&
        (profile = getenv("EPM_NOTARY_KEYCHAIN_PROFILE")) != NULL) {
        DIR *dir;            /* /Applications directory */
        struct dirent *dent; /* Directory entry */

        if ((dir = opendir(srcfolder)) == NULL) {
            fprintf(stderr, "epm: Unable to read directory \"%s\": %s\n", srcfolder,
                    strerror(errno));
            return (1);
        }

        while ((dent = readdir(dir)) != NULL) {
            if (!is_bundle(dent->d_name, strlen(dent->d_name)))
                continue;

            snprintf(filename, sizeof(filename), "%s/%s", srcfolder, dent->d_name);

            if (is_bundle_dir(filename) && notarize_app(filename, profile)) {
                closedir(dir);
                return (1);
            }
        }

        closedir(dir);
    }

    snprintf(filename, sizeof(filename), "%s/Applications", srcfolder);
    unlink(filename);

    if (symlink("/Applications", filename)) {
        fprintf(stderr, "epm: Unable to create link \"%s\": %s\n", filename,
                strerror(errno));
        return (1);
    }

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

    if (make_image(srcfolder, dmgname, dist->product,
                   dist->license[0] ? dist->license : NULL, identity))
        return (1);

    if (!KeepFiles) {
        if (Verbosity)
            puts("Removing temporary distribution files...");

        snprintf(filename, sizeof(filename), "%s/%s", directory, prodname);
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
    static const char *const suffixes[] = {
        ".app",     ".framework",  ".bundle",      ".plugin", ".xpc",
        ".appex",   ".mdimporter", ".qlgenerator", ".prefPane", ".saver"};

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
    char *path;         /* Canonical path */
    int kind;           /* SIGN_* */
    char *entitlements; /* Entitlements plist from the list file or NULL */
} signitem_t;

typedef struct {
    signitem_t *items; /* Items in signing order */
    int count;         /* Number of items */
} signplan_t;

/*
 * 'plan_find()' - Find a path in a signing plan.
 */

static signitem_t *              /* O - Item or NULL */
plan_find(signplan_t *plan,      /* I - Signing plan */
          const char *path)      /* I - Canonical path */
{
    int i;                       /* Looping var */

    for (i = 0; i < plan->count; i++)
        if (!strcmp(plan->items[i].path, path))
            return (plan->items + i);

    return (NULL);
}

/*
 * 'plan_add()' - Append a path to a signing plan unless already present.
 */

static int                          /* O - 0 = success, 1 = out of memory */
plan_add(signplan_t *plan,          /* IO - Signing plan */
         const char *path,          /* I - Canonical path */
         int kind,                  /* I - SIGN_* */
         const char *entitlements)  /* I - Entitlements plist or NULL */
{
    signitem_t *temp;               /* New list */

    if ((temp = plan_find(plan, path)) != NULL) {
        if (entitlements && !temp->entitlements &&
            (temp->entitlements = strdup(entitlements)) == NULL)
            return (1);

        return (0);
    }

    if ((temp = realloc(plan->items, (size_t)(plan->count + 1) * sizeof(signitem_t))) ==
        NULL)
        return (1);

    plan->items = temp;

    if ((temp[plan->count].path = strdup(path)) == NULL)
        return (1);

    temp[plan->count].kind = kind;
    temp[plan->count].entitlements = NULL;

    if (entitlements && (temp[plan->count].entitlements = strdup(entitlements)) == NULL)
        return (1);

    plan->count++;

    return (0);
}

/*
 * 'plan_free()' - Free a signing plan.
 */

static void plan_free(signplan_t *plan) /* I - Signing plan */
{
    int i; /* Looping var */

    for (i = 0; i < plan->count; i++) {
        free(plan->items[i].path);
        free(plan->items[i].entitlements);
    }

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

static int plan_bundle(signplan_t *plan, const char *bundle, const char *entitlements);

/*
 * 'plan_walk()' - Add the code inside a bundle directory to a signing plan.
 */

static int                          /* O - 0 = success, 1 = fail */
plan_walk(signplan_t *plan,         /* IO - Signing plan */
          const char *directory,    /* I - Canonical directory */
          const char *mainexe,      /* I - Main executable of the enclosing bundle */
          const char *entitlements) /* I - Entitlements of the enclosing bundle */
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
                status = plan_bundle(plan, path, entitlements);
            else
                status = plan_walk(plan, path, mainexe, entitlements);
        } else if (S_ISREG(fileinfo.st_mode) && is_macho(path) &&
                   (!mainexe || strcmp(path, mainexe))) {
            status = plan_add(plan, path,
                              macho_filetype(path) == MH_EXECUTE_TYPE ? SIGN_EXECUTABLE
                                                                        : SIGN_LIBRARY,
                              entitlements);
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

static int                          /* O - 0 = success, 1 = fail */
plan_bundle(signplan_t *plan,       /* IO - Signing plan */
            const char *bundle,     /* I - Bundle directory */
            const char *entitlements) /* I - Entitlements plist or NULL */
{
    char root[PATH_MAX],            /* Canonical bundle path */
        mainexe[PATH_MAX];          /* Canonical main executable path */

    if (!realpath(bundle, root)) {
        fprintf(stderr, "epm: Unable to resolve bundle \"%s\": %s\n", bundle,
                strerror(errno));
        return (1);
    }

    if (bundle_executable(root, mainexe, sizeof(mainexe)))
        mainexe[0] = '\0';

    if (plan_walk(plan, root, mainexe[0] ? mainexe : NULL, entitlements))
        return (1);

    return (plan_add(plan, root, is_app_bundle(root) ? SIGN_APP : SIGN_BUNDLE,
                     entitlements));
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

    /*
     * A secure timestamp needs Apple's server; an ad-hoc signature carries
     * no certificate, so there is nothing to timestamp.
     */

    strlcpy(options, adhoc_identity(identity) ? "" : "--timestamp ", sizeof(options));

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

    if (run_command(NULL, "/usr/bin/codesign --force %s--sign %s %s", options,
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
 * 'verify_signature()' - Check a signature with codesign.
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

    return (0);
}

/*
 * 'assess_path()' - Show the Gatekeeper assessment of a signed item.
 *
 * Advisory only: it fails for an ad-hoc identity and for anything not yet
 * notarized, so it runs after notarization and only at -vv.
 */

static void assess_path(const char *path, /* I - Signed file, bundle, package or image */
                        const char *type) /* I - spctl --type value */
{
    char qpath[1024]; /* Quoted path */

    if (Verbosity < 2 || run_quote(qpath, sizeof(qpath), path))
        return;

    if (!strcmp(type, "open"))
        run_command(NULL,
                    "/usr/sbin/spctl --assess --type open "
                    "--context context:primary-signature --verbose=4 %s",
                    qpath);
    else
        run_command(NULL, "/usr/sbin/spctl --assess --type %s --verbose=4 %s", type,
                    qpath);
}

/*
 * 'sign_payload()' - Sign the Mach-O files and bundles staged for a package.
 */

static int                              /* O - 0 = success, 1 = fail */
sign_payload(const char *directory,     /* I - Distribution directory */
             const char *prodfull,      /* I - Full product name */
             dist_t *dist,              /* I - Distribution information */
             const char *subpackage,    /* I - Subpackage or NULL */
             int all,                   /* I - Sign every subpackage? */
             const char *identity,      /* I - Signing identity */
             const char *entitlements,  /* I - Default entitlements plist or NULL */
             const char *keychain)      /* I - Keychain path or NULL */
{
    int i,                     /* Looping var */
        runtime,               /* Hardened runtime? */
        status = 0;            /* Return status */
    file_t *file;              /* Current distribution file */
    signplan_t plan = {NULL, 0}, /* Everything to sign, in order */
        roots = {NULL, 0};     /* Outermost items, for verification */
    char path[1024],                 /* Staged path */
        dstprefix[sizeof(file->dst)], /* Destination path of a bundle */
        fileent[256];          /* Entitlements option of the current file */
    const char *p,             /* Pointer into destination path */
        *start,                /* Start of the current path component */
        *option;               /* Option value */
    size_t len;                /* Length of a destination path prefix */

    if (Verbosity)
        puts("Signing staged distribution files...");

    runtime = hardened_runtime();

    /*
     * Collect the outermost bundles and the standalone Mach-O files; what is
     * inside a bundle is found by walking it.
     */

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++) {
        int inbundle = 0; /* Is this file inside a bundle? */

        if (tolower(file->type) != 'f' && tolower(file->type) != 'c' &&
            tolower(file->type) != 'd')
            continue;

        if (!all && file->subpackage != subpackage)
            continue;

        /*
         * get_option() returns a static buffer, so copy the value out...
         */

        if ((option = get_option(file, "entitlements", NULL)) != NULL)
            strlcpy(fileent, option, sizeof(fileent));
        else
            fileent[0] = '\0';

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

            if (plan_add(&roots, path, is_app_bundle(path) ? SIGN_APP : SIGN_BUNDLE,
                         fileent[0] ? fileent : NULL))
                goto nomem;
        }

        if (inbundle || tolower(file->type) == 'd')
            continue;

        staged_path(path, sizeof(path), directory, prodfull, file->dst, 0);

        if (is_macho(path) &&
            plan_add(&roots, path,
                     macho_filetype(path) == MH_EXECUTE_TYPE ? SIGN_EXECUTABLE
                                                             : SIGN_LIBRARY,
                     fileent[0] ? fileent : NULL))
            goto nomem;
    }

    for (i = 0; i < roots.count && !status; i++) {
        if (roots.items[i].kind == SIGN_APP || roots.items[i].kind == SIGN_BUNDLE)
            status = plan_bundle(&plan, roots.items[i].path, roots.items[i].entitlements);
        else
            status = plan_add(&plan, roots.items[i].path, roots.items[i].kind,
                              roots.items[i].entitlements);
    }

    for (i = 0; i < roots.count && !status; i++)
        strip_xattrs(roots.items[i].path, roots.items[i].kind == SIGN_APP ||
                                              roots.items[i].kind == SIGN_BUNDLE);

    /*
     * Process entitlements belong on executables and applications; libraries
     * and plug-ins run under their host's.
     */

    for (i = 0; i < plan.count && !status; i++) {
        int kind = plan.items[i].kind;
        const char *ent = plan.items[i].entitlements ? plan.items[i].entitlements
                                                      : entitlements;

        status = codesign_path(plan.items[i].path, identity,
                               (kind == SIGN_EXECUTABLE || kind == SIGN_APP) ? ent : NULL,
                               keychain, runtime);
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
 * 'json_string()' - Find a top-level string value in notarytool's JSON output.
 */

static int                          /* O - 0 = found, 1 = not found */
json_string(const char *json,       /* I - JSON text */
            const char *key,        /* I - Key name */
            char *buf,              /* O - Value */
            size_t bufsize)         /* I - Size of buffer */
{
    char quoted[64];                /* Key with quotes */
    const char *ptr,                /* Pointer into JSON */
        *end;                       /* End of value */

    snprintf(quoted, sizeof(quoted), "\"%s\"", key);

    for (ptr = json; (ptr = strstr(ptr, quoted)) != NULL; ptr++) {
        ptr += strlen(quoted);
        while (isspace(*ptr & 255))
            ptr++;

        if (*ptr != ':')
            continue;

        ptr++;
        while (isspace(*ptr & 255))
            ptr++;

        if (*ptr != '"' || (end = strchr(ptr + 1, '"')) == NULL ||
            (size_t)(end - ptr - 1) >= bufsize)
            return (1);

        memcpy(buf, ptr + 1, (size_t)(end - ptr - 1));
        buf[end - ptr - 1] = '\0';

        return (0);
    }

    return (1);
}

/*
 * 'notarize_submit()' - Submit a file to the notary service and wait for it.
 */

static int                          /* O - 0 = accepted, 1 = fail */
notarize_submit(const char *path,   /* I - File to notarize */
                const char *profile) /* I - notarytool keychain profile */
{
    FILE *fp;                       /* Pipe from notarytool */
    char qpath[1024],               /* Quoted path */
        qprofile[1024],             /* Quoted profile */
        command[3072],              /* Command line */
        output[65536],              /* notarytool output */
        status[64],                 /* Submission status */
        id[128];                    /* Submission id */
    size_t len;                     /* Length of output */
    int exitcode;                   /* notarytool exit status */

    if (run_quote(qpath, sizeof(qpath), path) ||
        run_quote(qprofile, sizeof(qprofile), profile)) {
        fprintf(stderr, "epm: Unable to notarize \"%s\" - path or profile too long.\n",
                path);
        return (1);
    }

    if (Verbosity)
        puts("Submitting for notarization...");

    snprintf(command, sizeof(command),
             "/usr/bin/xcrun notarytool submit --wait --output-format json "
             "--keychain-profile %s %s 2>&1",
             qprofile, qpath);

    if (Verbosity > 1)
        puts(command);

    if ((fp = popen(command, "r")) == NULL) {
        fputs("epm: Unable to run notarytool.\n", stderr);
        return (1);
    }

    len = fread(output, 1, sizeof(output) - 1, fp);
    output[len] = '\0';
    exitcode = pclose(fp);

    if (Verbosity > 1)
        fputs(output, stdout);

    if (!json_string(output, "status", status, sizeof(status))) {
        if (!strcasecmp(status, "Accepted"))
            return (0);

        fprintf(stderr, "epm: Notarization of \"%s\" was not accepted (%s).\n", path,
                status);

        /*
         * The rejection reasons only come from the log...
         */

        if (!json_string(output, "id", id, sizeof(id))) {
            snprintf(command, sizeof(command),
                     "/usr/bin/xcrun notarytool log --keychain-profile %s '%s' 2>&1",
                     qprofile, id);

            if ((fp = popen(command, "r")) != NULL) {
                while ((len = fread(output, 1, sizeof(output) - 1, fp)) > 0)
                    fwrite(output, 1, len, stderr);

                pclose(fp);
            }
        }

        return (1);
    }

    /*
     * No status in the output: a notarytool without --output-format, so fall
     * back to the plain submission and let stapling prove the result.
     */

    if (exitcode == 0)
        return (0);

    if (run_command(NULL,
                    "/usr/bin/xcrun notarytool submit --wait --keychain-profile %s %s",
                    qprofile, qpath)) {
        fputs("epm: Unable to submit for notarization.\n", stderr);
        return (1);
    }

    return (0);
}

/*
 * 'staple_path()' - Staple a notarization ticket to a file or bundle.
 */

static int                        /* O - 0 = success, 1 = fail */
staple_path(const char *path)     /* I - Notarized file or bundle */
{
    char qpath[1024];             /* Quoted path */

    if (run_quote(qpath, sizeof(qpath), path)) {
        fprintf(stderr, "epm: Unable to staple \"%s\" - path too long.\n", path);
        return (1);
    }

    if (Verbosity)
        puts("Stapling notarization ticket...");

    /*
     * An older notarytool exits successfully even when the Notary service
     * rejects the submission, so stapling is what proves the file was
     * notarized.
     */

    if (run_command(NULL, "/usr/bin/xcrun stapler staple %s", qpath)) {
        fputs("epm: Unable to staple notarization ticket - the submission was\n"
              "     probably rejected; run notarytool log for details.\n",
              stderr);
        return (1);
    }

    return (0);
}

/*
 * 'notarize_file()' - Submit a file for notarization and staple the ticket.
 */

static int                          /* O - 0 = success, 1 = fail */
notarize_file(const char *path,     /* I - File to notarize */
              const char *profile)  /* I - notarytool keychain profile */
{
    if (notarize_submit(path, profile))
        return (1);

    return (staple_path(path));
}

/*
 * 'notarize_app()' - Notarize an application bundle and staple the ticket.
 *
 * The notary service takes bundles only inside a zip archive; the ticket is
 * stapled to the bundle itself.
 */

static int                          /* O - 0 = success, 1 = fail */
notarize_app(const char *app,       /* I - Application bundle */
             const char *profile)   /* I - notarytool keychain profile */
{
    char zipname[1024],             /* Archive filename */
        qapp[1024],                 /* Quoted bundle path */
        qzipname[1024];             /* Quoted archive filename */
    int status;                     /* Return status */

    if (snprintf(zipname, sizeof(zipname), "%s.notarize.zip", app) >= (int)sizeof(zipname) ||
        run_quote(qapp, sizeof(qapp), app) ||
        run_quote(qzipname, sizeof(qzipname), zipname)) {
        fprintf(stderr, "epm: Unable to notarize \"%s\" - path too long.\n", app);
        return (1);
    }

    if (run_command(NULL, "/usr/bin/ditto -c -k --keepParent %s %s", qapp, qzipname)) {
        fprintf(stderr, "epm: Unable to archive \"%s\" for notarization.\n", app);
        return (1);
    }

    status = notarize_submit(zipname, profile);

    unlink(zipname);

    if (status)
        return (1);

    return (staple_path(app));
}
