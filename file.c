/*
 * File functions for the ESP Package Manager (EPM).
 *
 * Copyright © 2020 by Jim Jagielski
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
 * 'get_uid()' - Resolve a list-file owner to a numeric UID.
 *
 * Tries a user name lookup first; if that fails and the name is purely
 * numeric, it is used as a literal UID so file entries can specify an
 * owner that has no passwd entry on the build system.  Falls back to 0
 * (root) if neither resolves, matching prior behavior.
 */

uid_t                    /* O - Resolved user ID */
get_uid(const char *name) /* I - User name or numeric UID string */
{
    struct passwd *pwd; /* Pointer to user record */
    char *end;          /* End of numeric conversion */
    long uid;           /* Converted UID */

    if (name && *name) {
        if ((pwd = getpwnam(name)) != NULL) {
            endpwent();
            return (pwd->pw_uid);
        }

        endpwent();

        uid = strtol(name, &end, 10);
        if (end != name && *end == '\0' && uid >= 0)
            return ((uid_t)uid);
    }

    return (0);
}

/*
 * 'get_gid()' - Resolve a list-file group to a numeric GID.
 *
 * Same fallback behavior as get_uid() for group names/numeric GIDs.
 */

gid_t                     /* O - Resolved group ID */
get_gid(const char *name) /* I - Group name or numeric GID string */
{
    struct group *grp; /* Pointer to group record */
    char *end;         /* End of numeric conversion */
    long gid;          /* Converted GID */

    if (name && *name) {
        if ((grp = getgrnam(name)) != NULL) {
            endgrent();
            return (grp->gr_gid);
        }

        endgrent();

        gid = strtol(name, &end, 10);
        if (end != name && *end == '\0' && gid >= 0)
            return ((gid_t)gid);
    }

    return (0);
}

/*
 * 'copy_file()' - Copy a file.
 */

int                        /* O - 0 on success, -1 on failure */
copy_file(const char *dst, /* I - Destination file */
          const char *src, /* I - Source file */
          mode_t mode,     /* I - Permissions */
          uid_t owner,     /* I - Owner ID */
          gid_t group)     /* I - Group ID */
{
    FILE *dstfile,     /* Destination file */
        *srcfile;      /* Source file */
    char buffer[8192]; /* Copy buffer */
    char *slash;       /* Pointer to trailing slash */
    size_t bytes;      /* Number of bytes read/written */

    /*
     * Check that the destination directory exists...
     */

    strlcpy(buffer, dst, sizeof(buffer));
    if ((slash = strrchr(buffer, '/')) != NULL)
        *slash = '\0';

    if (access(buffer, F_OK))
        make_directory(buffer, 0755, owner, group);

    /*
     * Open files...
     */

    if ((dstfile = fopen(dst, "wb")) == NULL) {
        fprintf(stderr, "epm: Unable to create \"%s\" -\n     %s\n", dst,
                strerror(errno));
        return (-1);
    }

    if ((srcfile = fopen(src, "rb")) == NULL) {
        fclose(dstfile);
        unlink(dst);

        fprintf(stderr, "epm: Unable to open \"%s\" -\n     %s\n", src, strerror(errno));
        return (-1);
    }

    /*
     * Copy from src to dst...
     */

    while ((bytes = fread(buffer, 1, sizeof(buffer), srcfile)) > 0)
        if (fwrite(buffer, 1, bytes, dstfile) < bytes) {
            fprintf(stderr, "epm: Unable to write to \"%s\" -\n     %s\n", dst,
                    strerror(errno));

            fclose(srcfile);
            fclose(dstfile);
            unlink(dst);

            return (-1);
        }

    /*
     * Close files, change permissions, and return...
     */

    fclose(srcfile);
    fclose(dstfile);

    if (mode)
        chmod(dst, mode);
    if (!AooMode)
        if (owner != (uid_t)-1 && group != (gid_t)-1)
            chown(dst, owner, group);

    return (0);
}

/*
 * 'make_directory()' - Make a directory.
 */

int                                   /* O - 0 = success, -1 = error */
make_directory(const char *directory, /* I - Directory */
               mode_t mode,           /* I - Permissions */
               uid_t owner,           /* I - Owner ID */
               gid_t group)           /* I - Group ID */
{
    char buffer[8192], /* Filename buffer */
        *bufptr;       /* Pointer into buffer */
    const char *start; /* Start of directory string, for error messages */
    int status;        /* Return status */

    start = directory;
    status = 0;

    for (bufptr = buffer; *directory;) {
        if (*directory == '/' && bufptr > buffer) {
            *bufptr = '\0';

            if (access(buffer, F_OK)) {
                if (mkdir(buffer, 0755) && errno != EEXIST) {
                    fprintf(stderr, "epm: Unable to create directory \"%s\": %s\n",
                            buffer, strerror(errno));
                    status = -1;
                } else {
                    if (mode)
                        chmod(buffer, mode | 0700);
                    if (!AooMode)
                        if (owner != (uid_t)-1 && group != (gid_t)-1)
                            chown(buffer, owner, group);
                }
            }
        }

        if (bufptr >= (buffer + sizeof(buffer) - 1)) {
            fprintf(stderr, "epm: Directory path \"%s\" is too long.\n", start);
            return (-1);
        }

        *bufptr++ = *directory++;
    }

    *bufptr = '\0';

    if (access(buffer, F_OK)) {
        if (mkdir(buffer, 0755) && errno != EEXIST) {
            fprintf(stderr, "epm: Unable to create directory \"%s\": %s\n", buffer,
                    strerror(errno));
            status = -1;
        } else {
            if (mode)
                chmod(buffer, mode | 0700);
            if (!AooMode)
                if (owner != (uid_t)-1 && group != (gid_t)-1)
                    chown(buffer, owner, group);
        }
    }

    return (status);
}

/*
 * 'make_link()' - Make a symbolic link.
 */

int                        /* O - 0 = success, -1 = error */
make_link(const char *dst, /* I - Destination file */
          const char *src) /* I - Link */
{
    char buffer[8192], /* Copy buffer */
        *slash;        /* Pointer to trailing slash */

    /*
     * Check that the destination directory exists...
     */

    strlcpy(buffer, dst, sizeof(buffer));
    if ((slash = strrchr(buffer, '/')) != NULL)
        *slash = '\0';

    if (access(buffer, F_OK))
        make_directory(buffer, 0755, 0, 0);

    /*
     * Make the symlink...
     */

    return (symlink(src, dst));
}

/*
 * 'strip_execs()' - Strip symbols from executable files in the distribution.
 */

void strip_execs(dist_t *dist) /* I - Distribution to strip... */
{
    int i;          /* Looping var */
    file_t *file;   /* Software file */
    FILE *fp;       /* File pointer */
    char header[4]; /* File header... */

    /*
     * Loop through the distribution files and strip any executable
     * files.
     */

    for (i = dist->num_files, file = dist->files; i > 0; i--, file++)
        if (tolower(file->type) == 'f' && (file->mode & 0111) &&
            strstr(file->options, "nostrip()") == NULL) {
            /*
             * OK, this file has executable permissions; see if it is a
             * script...
             */

            if ((fp = fopen(file->src, "rb")) != NULL) {
                /*
                 * Read the first 3 bytes of the file...
                 */

                if (fread(header, 1, sizeof(header) - 1, fp) == 0) {
                    fclose(fp);
                    continue;
                }

                header[sizeof(header) - 1] = '\0';

                fclose(fp);

                /*
                 * Check for "#!/" or "#" + whitespace at the beginning of the file...
                 */

                if (!strncmp(header, "#!/", 3) ||
                    (header[0] == '#' && isspace(header[1] & 255)))
                    continue;
            } else {
                /*
                 * File could not be opened; error out...
                 */

                fprintf(stderr,
                        "epm: Unable to open file \"%s\" for destination "
                        "\"%s\" -\n     %s\n",
                        file->src, file->dst, strerror(errno));

                exit(1);
            }

            /*
             * Strip executables...
             */

            run_command(NULL, EPM_STRIP " %s", file->src);
        }
}

/*
 * 'unlink_directory()' - Delete a directory and all of its nodes.
 */

int                                     /* O - 0 on success, -1 on failure */
unlink_directory(const char *directory) /* I - Directory */
{
    DIR *dir;             /* Directory */
    DIRENT *dent;         /* Directory entry */
    char filename[1024];  /* Filename */
    struct stat fileinfo; /* Information on the source file */

    /*
     * Try opening the source directory...
     */

    if ((dir = opendir(directory)) == NULL) {
        fprintf(stderr, "epm: Unable to open directory \"%s\": %s\n", directory,
                strerror(errno));

        return (-1);
    }

    /*
     * Read from the directory...
     */

    while ((dent = readdir(dir)) != NULL) {
        /*
         * Skip "." and ".."...
         */

        if (!strcmp(dent->d_name, ".") || !strcmp(dent->d_name, ".."))
            continue;

        /*
         * Get file info...
         */

        snprintf(filename, sizeof(filename), "%s/%s", directory, dent->d_name);

        if (lstat(filename, &fileinfo)) {
            fprintf(stderr, "epm: Unable to stat \"%s\": %s\n", filename,
                    strerror(errno));
            continue;
        }

        /*
         * Process accordingly...
         */

        if (Verbosity)
            puts(filename);

        if (S_ISDIR(fileinfo.st_mode)) {
            /*
             * Directory...
             */

            if (unlink_directory(filename))
                goto fail;
        } else {
            /*
             * Regular file or symlink...
             */

            if (unlink(filename)) {
                fprintf(stderr, "epm: Unable to remove \"%s\": %s\n", filename,
                        strerror(errno));
                goto fail;
            }
        }
    }

    closedir(dir);

    if (rmdir(directory)) {
        fprintf(stderr, "epm: Unable to remove \"%s\": %s\n", directory, strerror(errno));
        return (-1);
    }

    return (0);

fail:

    closedir(dir);
    return (-1);
}

/*
 * 'unlink_package()' - Delete a package file.
 */

int                                    /* O - 0 on success, -1 on failure */
unlink_package(const char *ext,        /* I - Package filename extension */
               const char *prodname,   /* I - Product short name */
               const char *directory,  /* I - Directory for distribution files */
               const char *platname,   /* I - Platform name */
               dist_t *dist,           /* I - Distribution information */
               const char *subpackage) /* I - Subpackage */
{
    char prodfull[255], /* Full name of product */
        name[1024],     /* Full product name */
        filename[1024]; /* File to archive */

    /*
     * Figure out the full name of the distribution...
     */

    if (subpackage)
        snprintf(prodfull, sizeof(prodfull), "%s-%s", prodname, subpackage);
    else
        strlcpy(prodfull, prodname, sizeof(prodfull));

    /*
     * Then the subdirectory name...
     */

    if (dist->release[0])
        snprintf(name, sizeof(name), "%s-%s-%s", prodfull, dist->version, dist->release);
    else
        snprintf(name, sizeof(name), "%s-%s", prodfull, dist->version);

    if (platname[0]) {
        strlcat(name, "-", sizeof(name));
        strlcat(name, platname, sizeof(name));
    }

    strlcat(name, ".", sizeof(name));
    strlcat(name, ext, sizeof(name));

    /*
     * Remove the package file...
     */

    snprintf(filename, sizeof(filename), "%s/%s", directory, name);

    if (unlink(filename)) {
        fprintf(stderr, "epm: Unable to remove \"%s\": %s\n", filename, strerror(errno));
        return (-1);
    } else
        return (0);
}
