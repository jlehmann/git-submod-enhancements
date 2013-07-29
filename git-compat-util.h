#ifndef GIT_COMPAT_UTIL_H
#define GIT_COMPAT_UTIL_H

#define _FILE_OFFSET_BITS 64

#ifndef FLEX_ARRAY
/*
 * See if our compiler is known to support flexible array members.
 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) && (!defined(__SUNPRO_C) || (__SUNPRO_C > 0x580))
# define FLEX_ARRAY /* empty */
#elif defined(__GNUC__)
# if (__GNUC__ >= 3)
#  define FLEX_ARRAY /* empty */
# else
#  define FLEX_ARRAY 0 /* older GNU extension */
# endif
#endif

/*
 * Otherwise, default to safer but a bit wasteful traditional style
 */
#ifndef FLEX_ARRAY
# define FLEX_ARRAY 1
#endif
#endif

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#define bitsizeof(x)  (CHAR_BIT * sizeof(x))

#define maximum_signed_value_of_type(a) \
    (INTMAX_MAX >> (bitsizeof(intmax_t) - bitsizeof(a)))

#define maximum_unsigned_value_of_type(a) \
    (UINTMAX_MAX >> (bitsizeof(uintmax_t) - bitsizeof(a)))

/*
 * Signed integer overflow is undefined in C, so here's a helper macro
 * to detect if the sum of two integers will overflow.
 *
 * Requires: a >= 0, typeof(a) equals typeof(b)
 */
#define signed_add_overflows(a, b) \
    ((b) > maximum_signed_value_of_type(a) - (a))

#define unsigned_add_overflows(a, b) \
    ((b) > maximum_unsigned_value_of_type(a) - (a))

#ifdef __GNUC__
#define TYPEOF(x) (__typeof__(x))
#else
#define TYPEOF(x)
#endif

#define MSB(x, bits) ((x) & TYPEOF(x)(~0ULL << (bitsizeof(x) - (bits))))
#define HAS_MULTI_BITS(i)  ((i) & ((i) - 1))  /* checks if an integer has more than 1 bit set */

#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))

/* Approximation of the length of the decimal representation of this type. */
#define decimal_length(x)	((int)(sizeof(x) * 2.56 + 0.5) + 1)

#if defined(__sun__)
 /*
  * On Solaris, when _XOPEN_EXTENDED is set, its header file
  * forces the programs to be XPG4v2, defeating any _XOPEN_SOURCE
  * setting to say we are XPG5 or XPG6.  Also on Solaris,
  * XPG6 programs must be compiled with a c99 compiler, while
  * non XPG6 programs must be compiled with a pre-c99 compiler.
  */
# if __STDC_VERSION__ - 0 >= 199901L
# define _XOPEN_SOURCE 600
# else
# define _XOPEN_SOURCE 500
# endif
#elif !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__USLC__) && \
      !defined(_M_UNIX) && !defined(__sgi) && !defined(__DragonFly__) && \
      !defined(__TANDEM) && !defined(__QNX__)
#define _XOPEN_SOURCE 600 /* glibc2 and AIX 5.3L need 500, OpenBSD needs 600 for S_ISLNK() */
#define _XOPEN_SOURCE_EXTENDED 1 /* AIX 5.3L needs this */
#endif
#define _ALL_SOURCE 1
#define _GNU_SOURCE 1
#define _BSD_SOURCE 1
#define _NETBSD_SOURCE 1
#define _SGI_SOURCE 1

#if defined(WIN32) && !defined(__CYGWIN__) /* Both MinGW and MSVC */
# if defined (_MSC_VER)
#  define _WIN32_WINNT 0x0502
# endif
#define WIN32_LEAN_AND_MEAN  /* stops windows.h including winsock.h */
#include <winsock2.h>
#include <windows.h>
#define GIT_WINDOWS_NATIVE
#endif

#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#include <strings.h> /* for strcasecmp() */
#endif
#include <errno.h>
#include <limits.h>
#ifdef NEEDS_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/types.h>
#include <dirent.h>
#include <sys/time.h>
#include <time.h>
#include <signal.h>
#ifndef USE_WILDMATCH
#include <fnmatch.h>
#endif
#include <assert.h>
#include <regex.h>
#include <utime.h>
#include <syslog.h>
#ifndef NO_SYS_POLL_H
#include <sys/poll.h>
#else
#include <poll.h>
#endif

#if defined(__MINGW32__)
/* pull in Windows compatibility stuff */
#include "compat/mingw.h"
#elif defined(_MSC_VER)
#include "compat/msvc.h"
#else
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <termios.h>
#ifndef NO_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/un.h>
#ifndef NO_INTTYPES_H
#include <inttypes.h>
#else
#include <stdint.h>
#endif
#ifdef NO_INTPTR_T
/*
 * On I16LP32, ILP32 and LP64 "long" is the save bet, however
 * on LLP86, IL33LLP64 and P64 it needs to be "long long",
 * while on IP16 and IP16L32 it is "int" (resp. "short")
 * Size needs to match (or exceed) 'sizeof(void *)'.
 * We can't take "long long" here as not everybody has it.
 */
typedef long intptr_t;
typedef unsigned long uintptr_t;
#endif
#if defined(__CYGWIN__)
#undef _XOPEN_SOURCE
#include <grp.h>
#define _XOPEN_SOURCE 600
#else
#undef _ALL_SOURCE /* AIX 5.3L defines a struct list with _ALL_SOURCE. */
#include <grp.h>
#define _ALL_SOURCE 1
#endif
#endif

/* used on Mac OS X */
#ifdef PRECOMPOSE_UNICODE
#include "compat/precompose_utf8.h"
#else
#define precompose_str(in,i_nfd2nfc)
#define precompose_argv(c,v)
#define probe_utf8_pathname_composition(a,b)
#endif

#ifdef NEEDS_CLIPPED_WRITE
ssize_t clipped_write(int fildes, const void *buf, size_t nbyte);
#define write(x,y,z) clipped_write((x),(y),(z))
#endif

#ifdef MKDIR_WO_TRAILING_SLASH
#define mkdir(a,b) compat_mkdir_wo_trailing_slash((a),(b))
extern int compat_mkdir_wo_trailing_slash(const char*, mode_t);
#endif

#ifdef NO_STRUCT_ITIMERVAL
struct itimerval {
	struct timeval it_interval;
	struct timeval it_value;
}
#endif

#ifdef NO_SETITIMER
#define setitimer(which,value,ovalue)
#endif

#ifndef NO_LIBGEN_H
#include <libgen.h>
#else
#define basename gitbasename
extern char *gitbasename(char *);
#endif

#ifndef NO_ICONV
#include <iconv.h>
#endif

#ifndef NO_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

/* On most systems <netdb.h> would have given us this, but
 * not on some systems (e.g. z/OS).
 */
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

#ifndef NI_MAXSERV
#define NI_MAXSERV 32
#endif

/* On most systems <limits.h> would have given us this, but
 * not on some systems (e.g. GNU/Hurd).
 */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef PRIuMAX
#define PRIuMAX "llu"
#endif

#ifndef PRIu32
#define PRIu32 "u"
#endif

#ifndef PRIx32
#define PRIx32 "x"
#endif

#ifndef PRIo32
#define PRIo32 "o"
#endif

#ifndef PATH_SEP
#define PATH_SEP ':'
#endif

#ifdef HAVE_PATHS_H
#include <paths.h>
#endif
#ifndef _PATH_DEFPATH
#define _PATH_DEFPATH "/usr/local/bin:/usr/bin:/bin"
#endif

#ifndef STRIP_EXTENSION
#define STRIP_EXTENSION ""
#endif

#ifndef has_dos_drive_prefix
#define has_dos_drive_prefix(path) 0
#endif

#ifndef is_dir_sep
#define is_dir_sep(c) ((c) == '/')
#endif

#ifndef find_last_dir_sep
#define find_last_dir_sep(path) strrchr(path, '/')
#endif

#if defined(__HP_cc) && (__HP_cc >= 61000)
#define NORETURN __attribute__((noreturn))
#define NORETURN_PTR
#elif defined(__GNUC__) && !defined(NO_NORETURN)
#define NORETURN __attribute__((__noreturn__))
#define NORETURN_PTR __attribute__((__noreturn__))
#elif defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#define NORETURN_PTR
#else
#define NORETURN
#define NORETURN_PTR
#ifndef __attribute__
#define __attribute__(x)
#endif
#endif

/* The sentinel attribute is valid from gcc version 4.0 */
#if defined(__GNUC__) && (__GNUC__ >= 4)
#define LAST_ARG_MUST_BE_NULL __attribute__((sentinel))
#else
#define LAST_ARG_MUST_BE_NULL
#endif

#include "compat/bswap.h"

#ifdef USE_WILDMATCH
#include "wildmatch.h"
#define FNM_PATHNAME WM_PATHNAME
#define FNM_CASEFOLD WM_CASEFOLD
#define FNM_NOMATCH  WM_NOMATCH
static inline int fnmatch(const char *pattern, const char *string, int flags)
{
	return wildmatch(pattern, string, flags, NULL);
}
#endif

/* General helper functions */
extern void vreportf(const char *prefix, const char *err, va_list params);
extern void vwritef(int fd, const char *prefix, const char *err, va_list params);
extern NORETURN void usage(const char *err);
extern NORETURN void usagef(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern NORETURN void die(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern NORETURN void die_errno(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern int error(const char *err, ...) __attribute__((format (printf, 1, 2)));
extern void warning(const char *err, ...) __attribute__((format (printf, 1, 2)));

/*
 * Let callers be aware of the constant return value; this can help
 * gcc with -Wuninitialized analysis. We restrict this trick to gcc, though,
 * because some compilers may not support variadic macros. Since we're only
 * trying to help gcc, anyway, it's OK; other compilers will fall back to
 * using the function as usual.
 */
#if defined(__GNUC__) && ! defined(__clang__)
#define error(...) (error(__VA_ARGS__), -1)
#endif

extern void set_die_routine(NORETURN_PTR void (*routine)(const char *err, va_list params));
extern void set_error_routine(void (*routine)(const char *err, va_list params));
extern void set_die_is_recursing_routine(int (*routine)(void));

extern int prefixcmp(const char *str, const char *prefix);
extern int suffixcmp(const char *str, const char *suffix);

static inline const char *skip_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) ? NULL : str + len;
}

#if defined(NO_MMAP) || defined(USE_WIN32_MMAP)

#ifndef PROT_READ
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 1
#endif

#define mmap git_mmap
#define munmap git_munmap
extern void *git_mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset);
extern int git_munmap(void *start, size_t length);

#else /* NO_MMAP || USE_WIN32_MMAP */

#include <sys/mman.h>

#endif /* NO_MMAP || USE_WIN32_MMAP */

#ifdef NO_MMAP

/* This value must be multiple of (pagesize * 2) */
#define DEFAULT_PACKED_GIT_WINDOW_SIZE (1 * 1024 * 1024)

#else /* NO_MMAP */

/* This value must be multiple of (pagesize * 2) */
#define DEFAULT_PACKED_GIT_WINDOW_SIZE \
	(sizeof(void*) >= 8 \
		?  1 * 1024 * 1024 * 1024 \
		: 32 * 1024 * 1024)

#endif /* NO_MMAP */

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#ifdef NO_ST_BLOCKS_IN_STRUCT_STAT
#define on_disk_bytes(st) ((st).st_size)
#else
#define on_disk_bytes(st) ((st).st_blocks * 512)
#endif

#define DEFAULT_PACKED_GIT_LIMIT \
	((1024L * 1024L) * (sizeof(void*) >= 8 ? 8192 : 256))

#ifdef NO_PREAD
#define pread git_pread
extern ssize_t git_pread(int fd, void *buf, size_t count, off_t offset);
#endif
/*
 * Forward decl that will remind us if its twin in cache.h changes.
 * This function is used in compat/pread.c.  But we can't include
 * cache.h there.
 */
extern ssize_t read_in_full(int fd, void *buf, size_t count);

#ifdef NO_SETENV
#define setenv gitsetenv
extern int gitsetenv(const char *, const char *, int);
#endif

#ifdef NO_MKDTEMP
#define mkdtemp gitmkdtemp
extern char *gitmkdtemp(char *);
#endif

#ifdef NO_MKSTEMPS
#define mkstemps gitmkstemps
extern int gitmkstemps(char *, int);
#endif

#ifdef NO_UNSETENV
#define unsetenv gitunsetenv
extern void gitunsetenv(const char *);
#endif

#ifdef NO_STRCASESTR
#define strcasestr gitstrcasestr
extern char *gitstrcasestr(const char *haystack, const char *needle);
#endif

#ifdef NO_STRLCPY
#define strlcpy gitstrlcpy
extern size_t gitstrlcpy(char *, const char *, size_t);
#endif

#ifdef NO_STRTOUMAX
#define strtoumax gitstrtoumax
extern uintmax_t gitstrtoumax(const char *, char **, int);
#define strtoimax gitstrtoimax
extern intmax_t gitstrtoimax(const char *, char **, int);
#endif

#ifdef NO_HSTRERROR
#define hstrerror githstrerror
extern const char *githstrerror(int herror);
#endif

#ifdef NO_MEMMEM
#define memmem gitmemmem
void *gitmemmem(const void *haystack, size_t haystacklen,
                const void *needle, size_t needlelen);
#endif

#ifdef NO_GETPAGESIZE
#define getpagesize() sysconf(_SC_PAGESIZE)
#endif

#ifdef FREAD_READS_DIRECTORIES
#ifdef fopen
#undef fopen
#endif
#define fopen(a,b) git_fopen(a,b)
extern FILE *git_fopen(const char*, const char*);
#endif

#ifdef SNPRINTF_RETURNS_BOGUS
#define snprintf git_snprintf
extern int git_snprintf(char *str, size_t maxsize,
			const char *format, ...);
#define vsnprintf git_vsnprintf
extern int git_vsnprintf(char *str, size_t maxsize,
			 const char *format, va_list ap);
#endif

#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 1)
#define HAVE_STRCHRNUL
#define HAVE_MEMPCPY
#endif
#endif

#ifndef HAVE_STRCHRNUL
#define strchrnul gitstrchrnul
static inline char *gitstrchrnul(const char *s, int c)
{
	while (*s && *s != c)
		s++;
	return (char *)s;
}
#endif

#ifndef HAVE_MEMPCPY
#define mempcpy gitmempcpy
static inline void *gitmempcpy(void *dest, const void *src, size_t n)
{
	return (char *)memcpy(dest, src, n) + n;
}
#endif

#ifdef NO_INET_PTON
int inet_pton(int af, const char *src, void *dst);
#endif

#ifdef NO_INET_NTOP
const char *inet_ntop(int af, const void *src, char *dst, size_t size);
#endif

extern void release_pack_memory(size_t, int);

typedef void (*try_to_free_t)(size_t);
extern try_to_free_t set_try_to_free_routine(try_to_free_t);

extern char *xstrdup(const char *str);
extern void *xmalloc(size_t size);
extern void *xmallocz(size_t size);
extern void *xmemdupz(const void *data, size_t len);
extern char *xstrndup(const char *str, size_t len);
extern void *xrealloc(void *ptr, size_t size);
extern void *xcalloc(size_t nmemb, size_t size);
extern void *xmmap(void *start, size_t length, int prot, int flags, int fd, off_t offset);
extern ssize_t xread(int fd, void *buf, size_t len);
extern ssize_t xwrite(int fd, const void *buf, size_t len);
extern int xdup(int fd);
extern FILE *xfdopen(int fd, const char *mode);
extern int xmkstemp(char *template);
extern int xmkstemp_mode(char *template, int mode);
extern int odb_mkstemp(char *template, size_t limit, const char *pattern);
extern int odb_pack_keep(char *name, size_t namesz, unsigned char *sha1);

static inline size_t xsize_t(off_t len)
{
	if (len > (size_t) len)
		die("Cannot handle files this big");
	return (size_t)len;
}

static inline int has_extension(const char *filename, const char *ext)
{
	size_t len = strlen(filename);
	size_t extlen = strlen(ext);
	return len > extlen && !memcmp(filename + len - extlen, ext, extlen);
}

/* in ctype.c, for kwset users */
extern const char tolower_trans_tbl[256];

/* Sane ctype - no locale, and works with signed chars */
#undef isascii
#undef isspace
#undef isdigit
#undef isalpha
#undef isalnum
#undef isprint
#undef islower
#undef isupper
#undef tolower
#undef toupper
#undef iscntrl
#undef ispunct
#undef isxdigit

extern const unsigned char sane_ctype[256];
#define GIT_SPACE 0x01
#define GIT_DIGIT 0x02
#define GIT_ALPHA 0x04
#define GIT_GLOB_SPECIAL 0x08
#define GIT_REGEX_SPECIAL 0x10
#define GIT_PATHSPEC_MAGIC 0x20
#define GIT_CNTRL 0x40
#define GIT_PUNCT 0x80
#define sane_istest(x,mask) ((sane_ctype[(unsigned char)(x)] & (mask)) != 0)
#define isascii(x) (((x) & ~0x7f) == 0)
#define isspace(x) sane_istest(x,GIT_SPACE)
#define isdigit(x) sane_istest(x,GIT_DIGIT)
#define isalpha(x) sane_istest(x,GIT_ALPHA)
#define isalnum(x) sane_istest(x,GIT_ALPHA | GIT_DIGIT)
#define isprint(x) ((x) >= 0x20 && (x) <= 0x7e)
#define islower(x) sane_iscase(x, 1)
#define isupper(x) sane_iscase(x, 0)
#define is_glob_special(x) sane_istest(x,GIT_GLOB_SPECIAL)
#define is_regex_special(x) sane_istest(x,GIT_GLOB_SPECIAL | GIT_REGEX_SPECIAL)
#define iscntrl(x) (sane_istest(x,GIT_CNTRL))
#define ispunct(x) sane_istest(x, GIT_PUNCT | GIT_REGEX_SPECIAL | \
		GIT_GLOB_SPECIAL | GIT_PATHSPEC_MAGIC)
#define isxdigit(x) (hexval_table[x] != -1)
#define tolower(x) sane_case((unsigned char)(x), 0x20)
#define toupper(x) sane_case((unsigned char)(x), 0)
#define is_pathspec_magic(x) sane_istest(x,GIT_PATHSPEC_MAGIC)

static inline int sane_case(int x, int high)
{
	if (sane_istest(x, GIT_ALPHA))
		x = (x & ~0x20) | high;
	return x;
}

static inline int sane_iscase(int x, int is_lower)
{
	if (!sane_istest(x, GIT_ALPHA))
		return 0;

	if (is_lower)
		return (x & 0x20) != 0;
	else
		return (x & 0x20) == 0;
}

static inline int strtoul_ui(char const *s, int base, unsigned int *result)
{
	unsigned long ul;
	char *p;

	errno = 0;
	ul = strtoul(s, &p, base);
	if (errno || *p || p == s || (unsigned int) ul != ul)
		return -1;
	*result = ul;
	return 0;
}

static inline int strtol_i(char const *s, int base, int *result)
{
	long ul;
	char *p;

	errno = 0;
	ul = strtol(s, &p, base);
	if (errno || *p || p == s || (int) ul != ul)
		return -1;
	*result = ul;
	return 0;
}

#ifdef INTERNAL_QSORT
void git_qsort(void *base, size_t nmemb, size_t size,
	       int(*compar)(const void *, const void *));
#define qsort git_qsort
#endif

#ifndef DIR_HAS_BSD_GROUP_SEMANTICS
# define FORCE_DIR_SET_GID S_ISGID
#else
# define FORCE_DIR_SET_GID 0
#endif

#ifdef NO_NSEC
#undef USE_NSEC
#define ST_CTIME_NSEC(st) 0
#define ST_MTIME_NSEC(st) 0
#else
#ifdef USE_ST_TIMESPEC
#define ST_CTIME_NSEC(st) ((unsigned int)((st).st_ctimespec.tv_nsec))
#define ST_MTIME_NSEC(st) ((unsigned int)((st).st_mtimespec.tv_nsec))
#else
#define ST_CTIME_NSEC(st) ((unsigned int)((st).st_ctim.tv_nsec))
#define ST_MTIME_NSEC(st) ((unsigned int)((st).st_mtim.tv_nsec))
#endif
#endif

#ifdef UNRELIABLE_FSTAT
#define fstat_is_reliable() 0
#else
#define fstat_is_reliable() 1
#endif

#ifndef va_copy
/*
 * Since an obvious implementation of va_list would be to make it a
 * pointer into the stack frame, a simple assignment will work on
 * many systems.  But let's try to be more portable.
 */
#ifdef __va_copy
#define va_copy(dst, src) __va_copy(dst, src)
#else
#define va_copy(dst, src) ((dst) = (src))
#endif
#endif

/*
 * Preserves errno, prints a message, but gives no warning for ENOENT.
 * Always returns the return value of unlink(2).
 */
int unlink_or_warn(const char *path);
/*
 * Likewise for rmdir(2).
 */
int rmdir_or_warn(const char *path);
/*
 * Calls the correct function out of {unlink,rmdir}_or_warn based on
 * the supplied file mode.
 */
int remove_or_warn(unsigned int mode, const char *path);

/*
 * Call access(2), but warn for any error except "missing file"
 * (ENOENT or ENOTDIR).
 */
#define ACCESS_EACCES_OK (1U << 0)
int access_or_warn(const char *path, int mode, unsigned flag);
int access_or_die(const char *path, int mode, unsigned flag);

/* Warn on an inaccessible file that ought to be accessible */
void warn_on_inaccessible(const char *path);

/* Get the passwd entry for the UID of the current process. */
struct passwd *xgetpwuid_self(void);

#endif
