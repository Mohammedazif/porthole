#ifndef PORTHOLE_CORE_UPATH_H
#define PORTHOLE_CORE_UPATH_H

/* File calls on UTF-8 paths. Windows' narrow C calls read paths in the ANSI code
 * page, so a file the iPhone names "Résumé.pdf" would not open there. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <wchar.h>

static inline wchar_t *ph_wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w;

    if (n <= 0)
        return NULL;
    w = malloc((size_t)n * sizeof *w);
    if (w && !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

/* Wide to UTF-8 into out. False when it does not fit. */
static inline bool ph_narrow(const wchar_t *wide, char *out, size_t cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL);
    return n > 0;
}

static inline FILE *ph_fopen(const char *path, const char *mode)
{
    wchar_t *wp = ph_wide(path), *wm = ph_wide(mode);
    FILE *f = wp && wm ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
}

static inline int ph_remove(const char *path)
{
    wchar_t *wp = ph_wide(path);
    int r = wp ? _wremove(wp) : -1;
    free(wp);
    return r;
}

/* Replaces an existing to, as rename does on Linux. */
static inline int ph_rename(const char *from, const char *to)
{
    wchar_t *wf = ph_wide(from), *wt = ph_wide(to);
    int r = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 0 : -1;
    free(wf);
    free(wt);
    return r;
}

static inline int ph_mkdir(const char *path)
{
    wchar_t *wp = ph_wide(path);
    int r = wp ? _wmkdir(wp) : -1;
    free(wp);
    return r;
}

static inline int ph_rmdir(const char *path)
{
    wchar_t *wp = ph_wide(path);
    int r = wp ? _wrmdir(wp) : -1;
    free(wp);
    return r;
}

/* Size of a regular file, or -1. */
/* Seeks to a 64-bit offset: plain fseek takes a 32-bit long on Windows,
 * so resuming a file past 2 GB failed. */
static inline int ph_fseek64(FILE *file, unsigned long long offset)
{
    return _fseeki64(file, (__int64)offset, SEEK_SET);
}

static inline long long ph_file_size(const char *path)
{
    wchar_t *wp = ph_wide(path);
    struct _stat64 st;
    long long size = -1;

    if (wp && _wstat64(wp, &st) == 0 && (st.st_mode & _S_IFMT) == _S_IFREG)
        size = st.st_size;
    free(wp);
    return size;
}

static inline bool ph_path_exists(const char *path)
{
    wchar_t *wp = ph_wide(path);
    bool ok = wp && GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES;
    free(wp);
    return ok;
}

static inline bool ph_path_absolute(const char *path)
{
    return path && ((((path[0] | 32) >= 'a' && (path[0] | 32) <= 'z') && path[1] == ':'
                     && (path[2] == '\\' || path[2] == '/'))
                    || (path[0] == '\\' && path[1] == '\\'));
}

#else

static inline FILE *ph_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

static inline int ph_remove(const char *path)
{
    return remove(path);
}

static inline int ph_rename(const char *from, const char *to)
{
    return rename(from, to);
}

static inline int ph_mkdir(const char *path)
{
    return mkdir(path, 0700);
}

#include <unistd.h>
static inline int ph_rmdir(const char *path)
{
    return rmdir(path);
}

static inline int ph_fseek64(FILE *file, unsigned long long offset)
{
    return fseeko(file, (off_t)offset, SEEK_SET);
}

static inline long long ph_file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return (long long)st.st_size;
}

static inline bool ph_path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static inline bool ph_path_absolute(const char *path)
{
    return path && path[0] == '/';
}

#endif

/* The last part of a path, after '/' or (on Windows) '\'. */
static inline const char *ph_path_base(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *back = strrchr(path, '\\');
    if (back && (!slash || back > slash))
        slash = back;
#endif
    return slash ? slash + 1 : path;
}

/* Atomically publish a completed file alongside its temporary file. */
static inline bool ph_publish_new(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t *source = ph_wide(from), *target = ph_wide(to);
    bool ok = source && target && MoveFileW(source, target);
    free(source);
    free(target);
    return ok;
#else
#if defined(SYS_renameat2) && (defined(_GNU_SOURCE) || defined(__USE_MISC))
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 1) == 0)
        return true;
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP)
        return false;
#endif
    if (link(from, to) != 0)
        return false;
    ph_remove(from);
    return true;
#endif
}

/* Flush buffered bytes before publishing a file or acknowledging its receipt. */
static inline time_t ph_file_mtime(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = ph_wide(path);
    struct _stat64 st;
    time_t stamp = wide && _wstat64(wide, &st) == 0 ? (time_t)st.st_mtime : 0;
    free(wide);
    return stamp;
#else
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
#endif
}

static inline bool ph_flush_file(FILE *file)
{
    if (fflush(file) != 0)
        return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

#endif
