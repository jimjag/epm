/*
 * Free/Net/OpenBSD package gateway for the ESP Package Manager (EPM).
 *
 * Copyright 2020 by Jim Jagielski
 * Copyright 1999-2017 by Michael R Sweet
 * Copyright 1999-2010 by Easy Software Products.
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

static void cr2semicolon(char *command) {
    int len, i;

    len = strlen(command);
    for (i = 0; i < len; i++)
        if (*(command + i) == '\n')
            *(command + i) = ';';
}

static int make_subpackage(const char *prodname, const char *directory,
                           const char *platname, dist_t *dist, const char *subpackage);
static int copy_bsd_files(const char *directory, const char *prodfull, dist_t *dist,
                          const char *subpackage);
#ifdef __FreeBSD__
static void write_ucl_string(FILE *fp, const char *s);
static int make_freebsd_modern_pkg(const char *prodname, const char *directory,
                                   const char *platname, dist_t *dist,
                                   const char *subpackage, const char *prodfull);
#endif /* __FreeBSD__ */

/*
 * 'make_bsd()' - Make a Free/Net/OpenBSD software distribution package.
 */

int                                /* O - 0 = success, 1 = fail */
make_bsd(const char *prodname,     /* I - Product short name */
         const char *directory,    /* I - Directory for distribution files */
         const char *platname,     /* I - Platform name */
         dist_t *dist,             /* I - Distribution information */
         struct utsname *platform) /* I - Platform information */
{
    int i; /* Looping var */

    if (make_subpackage(prodname, directory, platname, dist, NULL))
        return (1);

    for (i = 0; i < dist->num_subpackages; i++)
        if (make_subpackage(prodname, directory, platname, dist, dist->subpackages[i]))
            return (1);

    return (0);
}

/*
 * 'copy_bsd_files()' - Copy the distribution files into the buildroot.
 */

static int                             /* O - 0 = success, 1 = fail */
copy_bsd_files(const char *directory,  /* I - Directory for distribution files */
              const char *prodfull,   /* I - Full subpackage name */
              dist_t *dist,           /* I - Distribution information */
              const char *subpackage) /* I - Subpackage name */
{
    int i;                /* Looping var */
    file_t *file;         /* Current distribution file */
    char filename[1024];  /* Destination filename */
    uid_t uid;             /* Resolved owner ID */
    gid_t gid;             /* Resolved group ID */

    if (Verbosity)
        puts("Copying temporary distribution files...");

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++) {
        if (file->subpackage != subpackage)
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
            snprintf(filename, sizeof(filename), "%s/%s.buildroot%s", directory, prodfull,
                     file->dst);

            if (Verbosity > 1)
                printf("%s -> %s...\n", file->src, filename);

            if (copy_file(filename, file->src, file->mode, uid, gid))
                return (1);
            break;
        case 'i':
            snprintf(filename, sizeof(filename), "%s/%s.buildroot/usr/local/etc/rc.d/%s",
                     directory, prodfull, file->dst);

            if (Verbosity > 1)
                printf("%s -> %s...\n", file->src, filename);

            if (copy_file(filename, file->src, file->mode, uid, gid))
                return (1);
            break;
        case 'd':
            snprintf(filename, sizeof(filename), "%s/%s.buildroot%s", directory, prodfull,
                     file->dst);

            if (Verbosity > 1)
                printf("Directory %s...\n", filename);

            make_directory(filename, file->mode, uid, gid);
            break;
        case 'l':
            snprintf(filename, sizeof(filename), "%s/%s.buildroot%s", directory, prodfull,
                     file->dst);

            if (Verbosity > 1)
                printf("%s -> %s...\n", file->src, filename);

            make_link(filename, file->src);
            break;
        }
    }

    return (0);
}

#ifdef __FreeBSD__
/*
 * 'write_ucl_string()' - Write a quoted, escaped UCL string value.
 */

static void write_ucl_string(FILE *fp,   /* I - Manifest file */
                             const char *s) /* I - String to write */
{
    putc('"', fp);

    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            putc('\\', fp);
        if (*s == '\n')
            fputs("\\n", fp);
        else
            putc(*s, fp);
    }

    putc('"', fp);
}

/*
 * 'make_freebsd_modern_pkg()' - Build a package with pkg(8), for FreeBSD
 *                                releases where pkg_create(8) no longer
 *                                exists (issue #14).
 *
 * Pre-install and post-remove commands aren't supported here, same as the
 * legacy BSD packager.
 */

static int                                /* O - 0 = success, 1 = fail */
make_freebsd_modern_pkg(const char *prodname,     /* I - Product short name */
                        const char *directory,    /* I - Distribution directory */
                        const char *platname,     /* I - Platform name */
                        dist_t *dist,              /* I - Distribution information */
                        const char *subpackage,    /* I - Subpackage name */
                        const char *prodfull)       /* I - Full subpackage name */
{
    int i;                  /* Looping var */
    FILE *fp;                /* Manifest/plist file */
    char metadir[1024],      /* Metadata directory */
        manifestname[1024],  /* +MANIFEST filename */
        plistname[1024],     /* Packing list filename */
        rootdir[1024];       /* Staged root directory */
    char *old_user,          /* Old owner */
        *old_group;          /* Old group */
    int old_mode;            /* Old permissions */
    file_t *file;             /* Current distribution file */
    command_t *c;             /* Current command */
    depend_t *d;               /* Current dependency */

    REF(prodname);

    if (Verbosity)
        printf("Creating %s pkg(8) manifest...\n", prodfull);

    snprintf(metadir, sizeof(metadir), "%s/%s.metadata", directory, prodfull);
    make_directory(metadir, 0755, 0, 0);

    snprintf(manifestname, sizeof(manifestname), "%s/+MANIFEST", metadir);

    if ((fp = fopen(manifestname, "w")) == NULL) {
        fprintf(stderr, "epm: Unable to create manifest file \"%s\": %s\n", manifestname,
                strerror(errno));
        return (1);
    }

    fputs("name: ", fp);
    write_ucl_string(fp, prodfull);
    fputs(";\n", fp);

    fputs("version: ", fp);
    if (dist->release[0]) {
        char version[512];

        snprintf(version, sizeof(version), "%s_%s", dist->version, dist->release);
        write_ucl_string(fp, version);
    } else
        write_ucl_string(fp, dist->version);
    fputs(";\n", fp);

    fputs("origin: ", fp);
    {
        char origin[512];

        snprintf(origin, sizeof(origin), "local/%s", prodfull);
        write_ucl_string(fp, origin);
    }
    fputs(";\n", fp);

    fputs("comment: ", fp);
    write_ucl_string(fp, dist->product);
    fputs(";\n", fp);

    fputs("www: \"\";\n", fp);

    fputs("maintainer: ", fp);
    write_ucl_string(fp, dist->packager[0] ? dist->packager : dist->vendor);
    fputs(";\n", fp);

    fputs("prefix: \"/\";\n", fp);

    if (platname[0]) {
        fputs("arch: ", fp);
        write_ucl_string(fp, platname);
        fputs(";\n", fp);
    }

    fputs("desc: ", fp);
    {
        char desc[4096];

        desc[0] = '\0';
        for (i = 0; i < dist->num_descriptions; i++)
            if (dist->descriptions[i].subpackage == subpackage) {
                strlcat(desc, dist->descriptions[i].description, sizeof(desc));
                strlcat(desc, "\n", sizeof(desc));
            }
        if (!desc[0])
            strlcpy(desc, dist->product, sizeof(desc));
        write_ucl_string(fp, desc);
    }
    fputs(";\n", fp);

    {
        int warned_incompat = 0, warned_upper = 0;

        for (i = dist->num_depends, d = dist->depends; i > 0; i--, d++) {
            if (d->subpackage != subpackage)
                continue;

            if (d->type == DEPEND_INCOMPAT && !warned_incompat) {
                fputs("epm: NOTE - dependency incompatibilities/conflicts are not "
                      "currently\n"
                      "     expressed in the pkg(8) manifest for FreeBSD; ignoring "
                      "requirement\n",
                      stderr);
                warned_incompat = 1;
            } else if (d->type == DEPEND_REQUIRES && d->vernumber[0] == 0 &&
                      d->vernumber[1] < INT_MAX && !warned_upper) {
                fputs("epm: NOTE - upper-bound-only dependency versions are not "
                      "currently\n"
                      "     expressed in the pkg(8) manifest for FreeBSD; ignoring "
                      "constraint\n",
                      stderr);
                warned_upper = 1;
            }
        }
    }

    {
        int wrote_deps = 0;

        for (i = dist->num_depends, d = dist->depends; i > 0; i--, d++) {
            if (d->type != DEPEND_REQUIRES || d->subpackage != subpackage)
                continue;

            if (!wrote_deps) {
                fputs("deps: {\n", fp);
                wrote_deps = 1;
            }

            fputs("  ", fp);
            write_ucl_string(fp, d->product);
            fputs(": {origin: ", fp);
            {
                char deporigin[512];

                snprintf(deporigin, sizeof(deporigin), "local/%s", d->product);
                write_ucl_string(fp, deporigin);
            }
            if (d->vernumber[0] > 0) {
                fputs(", version: ", fp);
                write_ucl_string(fp, d->version[0]);
            }
            fputs("};\n", fp);
        }

        if (wrote_deps)
            fputs("};\n", fp);
    }

    fclose(fp);

    /*
     * Write the packing list...
     */

    snprintf(plistname, sizeof(plistname), "%s/plist", metadir);

    if ((fp = fopen(plistname, "w")) == NULL) {
        fprintf(stderr, "epm: Unable to create plist file \"%s\": %s\n", plistname,
                strerror(errno));
        return (1);
    }

    fputs("@cwd /\n", fp);

    for (i = dist->num_commands, c = dist->commands; i > 0; i--, c++)
        if (c->subpackage == subpackage)
            switch (c->type) {
            case COMMAND_PRE_INSTALL:
                fputs("epm: WARNING - Package contains pre-install commands which are "
                      "not\n"
                      "     supported by the BSD packager.\n",
                      stderr);
                break;
            case COMMAND_POST_INSTALL:
                if (AooMode)
                    cr2semicolon(c->command);
                fprintf(fp, "@exec %s\n", c->command);
                break;
            case COMMAND_PRE_REMOVE:
                if (AooMode)
                    cr2semicolon(c->command);
                fprintf(fp, "@unexec %s\n", c->command);
                break;
            case COMMAND_POST_REMOVE:
                fputs("epm: WARNING - Package contains post-removal commands which are "
                      "not\n"
                      "     supported by the BSD packager.\n",
                      stderr);
                break;
            }

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++)
        if (tolower(file->type) == 'd' && file->subpackage == subpackage) {
            /*
             * @dir alone carries no ownership; set it explicitly as a
             * postinstall command, matching the legacy pkg_create path.
             */

            qprintf(fp, "@exec mkdir -p %s\n", file->dst);
            qprintf(fp, "@exec chown %s:%s %s\n", file->user, file->group, file->dst);
            qprintf(fp, "@exec chmod %04o %s\n", (int)file->mode, file->dst);
        }

    for (i = dist->num_files, file = dist->files, old_mode = 0, old_user = "",
        old_group = "";
         i > 0; i--, file++) {
        if (tolower(file->type) == 'd' || file->subpackage != subpackage)
            continue;

        if (file->mode != old_mode)
            fprintf(fp, "@mode %04o\n", old_mode = file->mode);
        if (strcmp(file->user, old_user))
            fprintf(fp, "@owner %s\n", old_user = file->user);
        if (strcmp(file->group, old_group))
            fprintf(fp, "@group %s\n", old_group = file->group);

        switch (tolower(file->type)) {
        case 'i':
            qprintf(fp, "usr/local/etc/rc.d/%s\n", file->dst);
            break;
        case 'c':
        case 'f':
        case 'l':
            qprintf(fp, "%s\n", file->dst + 1);
            break;
        }
    }

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++)
        if (tolower(file->type) == 'd' && file->subpackage == subpackage)
            qprintf(fp, "@dir %s\n", file->dst + 1);

    fclose(fp);

    if (copy_bsd_files(directory, prodfull, dist, subpackage))
        return (1);

    /*
     * Build the distribution...
     */

    if (Verbosity)
        printf("Building %s pkg(8) binary distribution...\n", prodfull);

    snprintf(rootdir, sizeof(rootdir), "%s/%s.buildroot", directory, prodfull);

    {
        char qmetadir[1024], qplistname[1024], qrootdir[1024], qdirectory[1024];

        if (run_quote(qmetadir, sizeof(qmetadir), metadir) ||
            run_quote(qplistname, sizeof(qplistname), plistname) ||
            run_quote(qrootdir, sizeof(qrootdir), rootdir) ||
            run_quote(qdirectory, sizeof(qdirectory), directory)) {
            fputs("epm: A build path is too long to quote safely.\n", stderr);
            return (1);
        }

        if (run_command(NULL, "/usr/sbin/pkg create -m %s -p %s -r %s -o %s -f txz",
                        qmetadir, qplistname, qrootdir, qdirectory))
            return (1);
    }

    /*
     * Remove temporary files...
     */

    if (!KeepFiles) {
        if (Verbosity)
            puts("Removing temporary distribution files...");

        unlink_directory(rootdir);
        unlink(manifestname);
        unlink(plistname);
        unlink_directory(metadir);
    }

    return (0);
}
#endif /* __FreeBSD__ */

/*
 * 'make_subpackage()' - Create a subpackage...
 */

static int                              /* O - 0 = success, 1 = fail */
make_subpackage(const char *prodname,   /* I - Product short name */
                const char *directory,  /* I - Directory for distribution files */
                const char *platname,   /* I - Platform name */
                dist_t *dist,           /* I - Distribution information */
                const char *subpackage) /* I - Subpackage name */
{
    int i;                  /* Looping var */
    FILE *fp;               /* Spec file */
    char prodfull[1024];    /* Full subpackage name */
    char name[1024];        /* Full product name */
    char commentname[1024]; /* pkg comment filename */
    char descrname[1024];   /* pkg descr filename */
    char plistname[1024];   /* pkg plist filename */
    char filename[1024];    /* Destination filename */
    char *old_user,         /* Old owner UID */
        *old_group;         /* Old group ID */
    int old_mode;           /* Old permissions */
    file_t *file;           /* Current distribution file */
    command_t *c;           /* Current command */
    depend_t *d;            /* Current dependency */
    char current[1024];     /* Current directory */

    getcwd(current, sizeof(current));

    if (subpackage)
        snprintf(prodfull, sizeof(prodfull), "%s-%s", prodname, subpackage);
    else
        strlcpy(prodfull, prodname, sizeof(prodfull));

    if (Verbosity)
        printf("Creating %s *BSD pkg distribution...\n", prodfull);

    if (dist->release[0]) {
        if (platname[0])
            snprintf(name, sizeof(name), "%s-%s-%s-%s", prodfull, dist->version,
                     dist->release, platname);
        else
            snprintf(name, sizeof(name), "%s-%s-%s", prodfull, dist->version,
                     dist->release);
    } else if (platname[0])
        snprintf(name, sizeof(name), "%s-%s-%s", prodfull, dist->version, platname);
    else
        snprintf(name, sizeof(name), "%s-%s", prodfull, dist->version);

#ifdef __FreeBSD__
    /*
     * pkg_create(8) was removed from FreeBSD years ago in favor of pkg(8);
     * detect which is actually present and use the UCL-manifest packager
     * when the legacy tool is gone (issue #14)...
     */

    if (!access("/usr/sbin/pkg", X_OK))
        return (make_freebsd_modern_pkg(prodname, directory, platname, dist, subpackage,
                                        prodfull));
#endif /* __FreeBSD__ */

    /*
     * Write the descr file for pkg...
     */

    if (Verbosity)
        printf("Creating %s.descr file...\n", prodfull);

    snprintf(descrname, sizeof(descrname), "%s/%s.descr", directory, prodfull);

    if ((fp = fopen(descrname, "w")) == NULL) {
        fprintf(stderr, "epm: Unable to create descr file \"%s\": %s\n", descrname,
                strerror(errno));
        return (1);
    }

    fprintf(fp, "%s\n", dist->product);

    fclose(fp);

    /*
     * Write the comment file for pkg...
     */

    if (Verbosity)
        printf("Creating %s.comment file...\n", prodfull);

    snprintf(commentname, sizeof(commentname), "%s/%s.comment", directory, prodfull);

    if ((fp = fopen(commentname, "w")) == NULL) {
        fprintf(stderr, "epm: Unable to create comment file \"%s\": %s\n", commentname,
                strerror(errno));
        return (1);
    }

    fprintf(fp, "Summary: %s\n", dist->product);
    fprintf(fp, "Name: %s\n", prodfull);
    fprintf(fp, "Version: %s\n", dist->version);
    fprintf(fp, "Release: %s\n", dist->release);
    fprintf(fp, "Copyright: %s\n", dist->copyright);
    fprintf(fp, "Packager: %s\n", dist->packager);
    fprintf(fp, "Vendor: %s\n", dist->vendor);
    fprintf(fp, "BuildRoot: %s/%s/%s.buildroot\n", current, directory, prodfull);
    fputs("Group: Applications\n", fp);

    fputs("Description:\n\n", fp);
    for (i = 0; i < dist->num_descriptions; i++)
        if (dist->descriptions[i].subpackage == subpackage)
            fprintf(fp, "%s\n", dist->descriptions[i].description);

    fclose(fp);

    /*
     * Write the plist file for pkg...
     */

    if (Verbosity)
        printf("Creating %s.plist file...\n", prodfull);

    snprintf(plistname, sizeof(plistname), "%s/%s.plist", directory, prodfull);

    if ((fp = fopen(plistname, "w")) == NULL) {
        fprintf(stderr, "epm: Unable to create plist file \"%s\": %s\n", plistname,
                strerror(errno));
        return (1);
    }

    /*
     * FreeBSD and NetBSD support both "source directory" and "preserve files"
     * options, OpenBSD does not...
     */

#ifdef __FreeBSD__
    fprintf(fp, "@srcdir %s/%s/%s.buildroot\n", current, directory, prodfull);
    fputs("@option preserve\n", fp);
#elif defined(__NetBSD__)
    fprintf(fp, "@src %s/%s/%s.buildroot\n", current, directory, prodfull);
    fputs("@option preserve\n", fp);
#endif /* __FreeBSD__ */

    for (i = dist->num_depends, d = dist->depends; i > 0; i--, d++) {
        if (d->subpackage != subpackage)
            continue;

        if (d->type == DEPEND_REQUIRES) {
#ifdef __FreeBSD__
            if (AooMode) {
                if (dist->vernumber) {
                    fprintf(fp, "@pkgdep %s-%s-%d-%s", d->product, dist->version,
                            dist->vernumber, platname);
                } else {
                    fprintf(fp, "@pkgdep %s-%s-%s", d->product, dist->version, platname);
                }
             } else
                 fprintf(fp, "@pkgdep %s", d->product);
#elif defined(__OpenBSD__)
                fprintf(fp, "@depend %s", d->product);
#else
                fprintf(fp, "@pkgdep %s", d->product);
#endif /* __FreeBSD__ */
        } else {
#ifdef __FreeBSD__
                /*
                 * FreeBSD uses @conflicts...
                 */
                fprintf(fp, "@conflicts %s", d->product);
#elif defined(__OpenBSD__)
                fprintf(fp, "@conflict %s", d->product);
#else
                fprintf(fp, "@pkgcfl %s", d->product);
#endif /* __FreeBSD__ */
        }
        if (d->vernumber[0] > 0)
            fprintf(fp, "-%s\n", d->version[0]);
        else
            putc('\n', fp);
    }

    for (i = dist->num_commands, c = dist->commands; i > 0; i--, c++)
        if (c->subpackage == subpackage)
            switch (c->type) {
            case COMMAND_PRE_INSTALL:
                fputs("WARNING: Package contains pre-install commands which are not "
                      "supported\n"
                      "         by the BSD packager.\n",
                      stderr);
                break;
            case COMMAND_POST_INSTALL:
                if (AooMode)
                    cr2semicolon(c->command);
                fprintf(fp, "@exec %s\n", c->command);
                break;
            case COMMAND_PRE_REMOVE:
                if (AooMode)
                    cr2semicolon(c->command);
                fprintf(fp, "@unexec %s\n", c->command);
                break;
            case COMMAND_POST_REMOVE:
                fputs("WARNING: Package contains post-removal commands which are not "
                      "supported\n"
                      "         by the BSD packager.\n",
                      stderr);
                break;
            }

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++)
        if (tolower(file->type) == 'd' && file->subpackage == subpackage) {
            /*
             * We create and update directories as postinstall commands to
             * avoid a bug in the FreeBSD pkg_delete command.
             */

            qprintf(fp, "@exec mkdir -p %s\n", file->dst);
            qprintf(fp, "@exec chown %s:%s %s\n", file->user, file->group, file->dst);
            qprintf(fp, "@exec chmod %04o %s\n", (int)file->mode, file->dst);
        }

    for (i = dist->num_files, file = dist->files, old_mode = 0, old_user = "",
        old_group = "";
         i > 0; i--, file++) {
        /*
         * The FreeBSD pkg_delete command (at least) doesn't like creating
         * and deleting directories.  I don't know if other BSD's have the
         * same problem, but for now just put the directory stuff in a
         * postinstall script...
         */

        if (tolower(file->type) == 'd' || file->subpackage != subpackage)
            continue;

        if (file->mode != old_mode)
            fprintf(fp, "@mode %04o\n", old_mode = file->mode);
        if (strcmp(file->user, old_user))
            fprintf(fp, "@owner %s\n", old_user = file->user);
        if (strcmp(file->group, old_group))
            fprintf(fp, "@group %s\n", old_group = file->group);

        switch (tolower(file->type)) {
        case 'i':
            qprintf(fp, "usr/local/etc/rc.d/%s\n", file->dst);
            break;
        case 'c':
        case 'f':
        case 'l':
#ifdef __OpenBSD__
            qprintf(fp, "@file %s\n", file->dst + 1);
            if (tolower(file->type) == 'c')
                qprintf(fp, "@sample %s\n", file->dst + 1);
#else
            qprintf(fp, "%s\n", file->dst + 1);
#endif /* __OpenBSD__ */
            break;
        }
    }

    /*
     * Need to list directories to remove in reverse order after
     * everything else...
     */

    for (i = dist->num_files, file = dist->files + i - 1; i > 0; i--, file--)
        if (tolower(file->type) == 'd' && file->subpackage == subpackage)
            qprintf(fp, "@dirrm %s\n", file->dst + 1);

    fclose(fp);

    if (copy_bsd_files(directory, prodfull, dist, subpackage))
        return (1);

    /*
     * Build the distribution...
     */

    if (Verbosity)
        printf("Building %s *BSD pkg binary distribution...\n", prodfull);

#ifdef __OpenBSD__
    if (run_command(NULL,
                    "pkg_create -p / -B %s/%s.buildroot "
                    "-c %s "
                    "-d %s "
                    "-f %s "
                    "%s.tgz",
                    directory, prodfull, commentname, descrname, plistname, name))
        return (1);

    if (run_command(NULL, "mv %s.tgz %s", name, directory))
        return (1);
#elif defined(__FreeBSD__)
    if (run_command(NULL,
                    "/usr/sbin/pkg_create -p / "
                    "-c %s "
                    "-d %s "
                    "-f %s "
                    "%s/%s.tbz",
                    commentname, descrname, plistname, directory, name))
        return (1);
#else
    if (run_command(NULL,
                    "pkg_create -p / "
                    "-c %s "
                    "-d %s "
                    "-f %s "
                    "%s/%s.tgz",
                    commentname, descrname, plistname, directory, name))
        return (1);
#endif /* __OpenBSD__ */

    /*
     * Remove temporary files...
     */

    if (!KeepFiles) {
        if (Verbosity)
            puts("Removing temporary distribution files...");

        snprintf(filename, sizeof(filename), "%s/%s.buildroot", directory, prodfull);
        unlink_directory(filename);

        unlink(plistname);
        unlink(commentname);
        unlink(descrname);
    }

    return (0);
}
