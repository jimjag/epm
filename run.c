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
 * 'run_command()' - Run an external program.
 */

int                                /* O - Exit status */
run_command(const char *directory, /* I - Directory for command or NULL */
            const char *command,   /* I - Command string */
            ...)                   /* I - Additional arguments as needed */
{
    va_list ap;         /* Argument pointer */
    int pid,            /* Child process ID */
        status,         /* Status of child */
        argc;           /* Number of arguments */
    char argbuf[10240], /* Argument buffer */
        *argptr,        /* Argument string pointer */
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

    argv[0] = argbuf;

    for (argptr = argbuf, argc = 1; *argptr != '\0' && argc < 99; argptr++)
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

    if (argc >= 99 && *argptr != '\0')
        fprintf(stderr,
                "epm: Warning - command has more than 99 arguments, remainder left "
                "unsplit:\n     \"%s\"\n",
                argptr);

    argv[argc] = NULL;

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
