/*
 * String functions for the ESP Package Manager (EPM).
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

#include "epmstring.h"

/*
 * 'epm_strdup()' - Duplicate a string.
 */

#ifndef HAVE_STRDUP
char *                    /* O - New string pointer */
epm_strdup(const char *s) /* I - String to duplicate */
{
    char *t; /* New string pointer */

    if (s == NULL)
        return (NULL);

    if ((t = malloc(strlen(s) + 1)) == NULL)
        return (NULL);

    return (strcpy(t, s));
}
#endif /* !HAVE_STRDUP */

/*
 * 'epm_strcasecmp()' - Do a case-insensitive comparison.
 */

#ifndef HAVE_STRCASECMP
int                           /* O - Result of comparison (-1, 0, or 1) */
epm_strcasecmp(const char *s, /* I - First string */
               const char *t) /* I - Second string */
{
    while (*s != '\0' && *t != '\0') {
        if (tolower(*s) < tolower(*t))
            return (-1);
        else if (tolower(*s) > tolower(*t))
            return (1);

        s++;
        t++;
    }

    if (*s == '\0' && *t == '\0')
        return (0);
    else if (*s != '\0')
        return (1);
    else
        return (-1);
}
#endif /* !HAVE_STRCASECMP */

#ifndef HAVE_STRLCAT
/*
 * 'epm_strlcat()' - Safely concatenate two strings.
 */

size_t                       /* O - Length of string */
epm_strlcat(char *dst,       /* O - Destination string */
            const char *src, /* I - Source string */
            size_t size)     /* I - Size of destination string buffer */
{
    size_t srclen; /* Length of source string */
    size_t dstlen; /* Length of destination string */
    size_t room;   /* Room left in destination buffer */

    /*
     * Figure out how much room is left...  If the destination string
     * already fills (or overflows) the buffer, there is no room and we
     * must not touch dst[] - just report the length the concatenation
     * would have had, as strlcat() does...
     */

    if (size == 0)
        return (strlen(src));

    dstlen = strlen(dst);
    srclen = strlen(src);

    if (dstlen >= (size - 1))
        return (dstlen + srclen);

    room = size - dstlen - 1;

    /*
     * Copy the appropriate amount...
     */

    if (srclen > room)
        srclen = room;

    memcpy(dst + dstlen, src, srclen);
    dst[dstlen + srclen] = '\0';

    return (dstlen + srclen);
}
#endif /* !HAVE_STRLCAT */

#ifndef HAVE_STRLCPY
/*
 * 'epm_strlcpy()' - Safely copy two strings.
 */

size_t                       /* O - Length of string */
epm_strlcpy(char *dst,       /* O - Destination string */
            const char *src, /* I - Source string */
            size_t size)     /* I - Size of destination string buffer */
{
    size_t srclen; /* Length of source string */

    /*
     * Figure out how much room is needed...
     */

    size--;

    srclen = strlen(src);

    /*
     * Copy the appropriate amount...
     */

    if (srclen > size)
        srclen = size;

    memcpy(dst, src, srclen);
    dst[srclen] = '\0';

    return (srclen);
}
#endif /* !HAVE_STRLCPY */

/*
 * 'epm_strncasecmp()' - Do a case-insensitive comparison on up to N chars.
 */

#ifndef HAVE_STRNCASECMP
int                            /* O - Result of comparison (-1, 0, or 1) */
epm_strncasecmp(const char *s, /* I - First string */
                const char *t, /* I - Second string */
                size_t n)      /* I - Maximum number of characters to compare */
{
    while (*s != '\0' && *t != '\0' && n > 0) {
        if (tolower(*s) < tolower(*t))
            return (-1);
        else if (tolower(*s) > tolower(*t))
            return (1);

        s++;
        t++;
        n--;
    }

    if (n == 0)
        return (0);
    else if (*s == '\0' && *t == '\0')
        return (0);
    else if (*s != '\0')
        return (1);
    else
        return (-1);
}
#endif /* !HAVE_STRNCASECMP */
