/*
 * Stub config.h used ONLY by tests/unit/test_strlcat.c.
 *
 * It is identical to a normal generated config.h except that HAVE_STRLCAT
 * and HAVE_STRLCPY are left undefined, which forces string.c to compile its
 * own epm_strlcat()/epm_strlcpy() fallbacks (the code path taken on systems
 * with a pre-2.38 glibc and no libbsd) instead of silently deferring to the
 * host's native strlcat()/strlcpy() - which is what every developer's Mac or
 * modern Linux box would otherwise do, hiding regressions in the fallback.
 */

#define EPM_VERSION "ESP Package Manager (EPM) test build"
#define EPM_SOFTWARE "/etc/software"
#define EPM_BINDIR "/usr/local/bin"
#define EPM_DATADIR "/usr/local/share/epm"
#define EPM_LIBDIR "/usr/local/lib/epm"
#define EPM_STRIP "/usr/bin/strip"
#define EPM_RPMBUILD "rpm"
#define EPM_RPMARCH "--target"

#define HAVE_STRINGS_H 1
#define HAVE_SYS_PARAM_H 1

#define HAVE_STRCASECMP 1
#define HAVE_STRDUP 1
/* HAVE_STRLCAT and HAVE_STRLCPY deliberately left undefined */
#define HAVE_STRNCASECMP 1

#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1

#define HAVE_DIRENT_H 1

#define EPM_GZIP "/usr/bin/gzip"
#define EPM_COMPRESS 1
