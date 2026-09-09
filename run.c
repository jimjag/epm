/*
 * External program function for the ESP Package Manager (EPM).
 *
 * Copyright © 2020 by Jim Jagielski
 * Copyright 1999-2014 by Michael R Sweet
 * Copyright 1999-2005 by Easy Software Products.
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
#include <fcntl.h>
#include <stdarg.h>
#include <sys/wait.h>

/*
 * 'split_args()' - Split a command buffer into argv entries in place.
 *
 * Arguments are separated by whitespace and may be quoted by " or '; within
 * a quoted span, \X yields a literal X (including \' inside '...' and \"
 * inside "..."). This is the argument-splitting half of run_command(),
 * pulled out so it can be exercised directly by run_quote()'s round-trip
 * test without forking a real process.
 */

int                     /* O - Number of arguments (argc) */
split_args(char *argbuf, /* IO - Command buffer, modified in place */
           char **argv, /* O - Argument pointers */
           int maxargs) /* I - Size of the argv array */
{
    char *argptr; /* Argument string pointer */
    int argc;     /* Number of arguments */

    /*
     * One slot has to be left for the terminating NULL, so anything smaller
     * than two cannot hold even a single argument.
     */

    if (maxargs < 2)
        return (0);

    argv[0] = argbuf;

    for (argptr = argbuf, argc = 1; *argptr != '\0' && argc < maxargs - 1; argptr++)
        if (isspace(*argptr & 255)) {
            *argptr++ = '\0';

            while (isspace(*argptr & 255))
                argptr++;

            if (*argptr != '\0') {
                argv[argc] = argptr;
                argc++;
            }

            argptr--;
        } else if (*argptr == '\'') {
            if (argptr == argv[argc - 1])
                argv[argc - 1]++;

            for (argptr++; *argptr && *argptr != '\''; argptr++)
                if (*argptr == '\\' && argptr[1])
                    memmove(argptr, argptr + 1, strlen(argptr));

            if (*argptr == '\'')
                memmove(argptr, argptr + 1, strlen(argptr));

            argptr--;
        } else if (*argptr == '\"') {
            if (argptr == argv[argc - 1])
                argv[argc - 1]++;

            for (argptr++; *argptr && *argptr != '\"'; argptr++)
                if (*argptr == '\\' && argptr[1])
                    memmove(argptr, argptr + 1, strlen(argptr));

            if (*argptr == '\"')
                memmove(argptr, argptr + 1, strlen(argptr));

            argptr--;
        }

    if (argc >= maxargs - 1 && *argptr != '\0')
        fprintf(stderr,
                "epm: Warning - command has more than %d arguments, remainder left "
                "unsplit:\n     \"%s\"\n",
                maxargs - 1, argptr);

    argv[argc] = NULL;

    return (argc);
}

/*
 * 'run_quote()' - Quote a value for safe interpolation into a run_command()
 *                 format string via a plain %s (no surrounding '...' needed
 *                 in the format string - this supplies its own).
 *
 * split_args() only treats \X as an escape inside a quoted span, so a
 * literal ' or \ in src must be backslash-escaped or it corrupts argv
 * splitting (a bare embedded ' ends the span early; a bare embedded \
 * eats the next character). Truncation is reported via the return value
 * rather than silently dropping characters.
 */

int                        /* O - 0 = success, 1 = dst too small */
run_quote(char *dst,       /* O - Quoted output buffer */
          size_t dstsize,  /* I - Size of dst */
          const char *src) /* I - Value to quote */
{
    size_t len = 0; /* Bytes written to dst so far */

    if (dstsize < 3)
        return (1);

    dst[len++] = '\'';

    for (; *src; src++) {
        if (*src == '\'' || *src == '\\') {
            if (len >= dstsize - 3)
                return (1);
            dst[len++] = '\\';
        }

        if (len >= dstsize - 2)
            return (1);

        dst[len++] = *src;
    }

    dst[len++] = '\'';
    dst[len] = '\0';

    return (0);
}

/*
 * 'run_command()' - Run an external program.
 */

int                                /* O - Exit status */
run_command(const char *directory, /* I - Directory for command or NULL */
            const char *command,   /* I - Command string */
            ...)                   /* I - Additional arguments as needed */
{
    va_list ap;         /* Argument pointer */
    int pid,            /* Child process ID */
        status;         /* Status of child */
    char argbuf[10240], /* Argument buffer */
        *argv[100];     /* Argument strings */
    int fmtlen;         /* Length vsnprintf() would have produced */

    /*
     * Format the command string...
     */

    va_start(ap, command);
    fmtlen = vsnprintf(argbuf, sizeof(argbuf) - 1, command, ap);
    va_end(ap);
    argbuf[sizeof(argbuf) - 1] = '\0';

    if (fmtlen < 0 || (size_t)fmtlen >= sizeof(argbuf) - 1) {
        fprintf(stderr,
                "epm: Command too long for internal buffer (%d bytes) -\n"
                "     not running \"%.100s...\".\n",
                (int)sizeof(argbuf) - 1, argbuf);
        return (1);
    }

    if (Verbosity > 1)
        puts(argbuf);

    /*
     * Parse the argument string; arguments can be separated by whitespace
     * and quoted by " and '...
     */

    split_args(argbuf, argv, (int)(sizeof(argv) / sizeof(argv[0])));

    /*
     * Execute the command...
     */

    if ((pid = fork()) == 0) {
        /*
         * Child comes here...  Redirect stdin, stdout, and stderr to /dev/null
         * if !Verbosity...
         */

        if (Verbosity < 2) {
            close(0);
            close(1);
            close(2);

            open("/dev/null", O_RDWR);
            dup(0);
            dup(0);
        }

        /*
         * Change directories...  A failure here means the command would run
         * in the wrong directory, which is worse than not running it at all -
         * bail out rather than fall through to execvp()...
         */

        if (directory && chdir(directory)) {
            fprintf(stderr, "epm: Unable to change to directory \"%s\": %s\n", directory,
                    strerror(errno));
            _exit(errno ? errno : 1);
        }

        /*
         * Execute the program; if an error occurs, exit with the UNIX error...
         */

        execvp(argv[0], argv);
        fprintf(stderr, "epm: Unable to execute \"%s\" program: %s\n", argv[0],
                strerror(errno));
        _exit(errno ? errno : 1);
    } else if (pid < 0) {
        /*
         * Error - can't fork!
         */

        perror("epm: fork failed");
        return (1);
    }

    /*
     * Fork successful - wait for the child and return the error status...
     */

    if (wait(&status) != pid) {
        fputs("epm: Got exit status from wrong program!\n", stderr);
        return (1);
    } else if (WIFSIGNALED(status))
        return (-WTERMSIG(status));
    else
        return (WEXITSTATUS(status));
}
