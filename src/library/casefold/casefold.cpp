/*
    Copyright 2015-2026 Clément Gallet <clement.gallet@ens-lyon.org>

    This file is part of libTAS.

    libTAS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libTAS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libTAS.  If not, see <http://www.gnu.org/licenses/>.
 */

/* libtas-casefold.so: the case-insensitive paths of libTAS (see
 * fileio/CasePath.h) without the rest of libTAS, to run games made for
 * case-insensitive file systems without libTAS:
 *
 *     LD_PRELOAD=/path/to/libtas-casefold.so ./game
 */

#include "../fileio/CasePath.h"

#include <cstdarg>
#include <cstdio>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>
#include <unistd.h>

#define EXPORT extern "C" __attribute__((visibility("default")))

using namespace libtas;

__attribute__((constructor)) static void init()
{
    CasePath::enable();
}

/* Get the next definition of a function. The legacy stat functions of old
 * binaries only exist as versioned symbols in recent glibc versions. */
template<typename F>
static F next(F& function, const char* name, const char* version = nullptr)
{
    if (!function && version)
        function = reinterpret_cast<F>(dlvsym(RTLD_NEXT, name, version));
    if (!function)
        function = reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
    return function;
}

static mode_t openMode(int oflag, va_list args)
{
    if ((oflag & O_CREAT) || ((oflag & O_TMPFILE) == O_TMPFILE))
        return va_arg(args, mode_t);
    return 0;
}

#define OPEN_WRAPPER(FUNC) \
EXPORT int FUNC(const char* file, int oflag, ...) \
{ \
    va_list args; \
    va_start(args, oflag); \
    mode_t mode = openMode(oflag, args); \
    va_end(args); \
    static decltype(&FUNC) orig; \
    return CasePath::call(file, [&](const char* f) {return next(orig, #FUNC)(f, oflag, mode);}); \
}

#define OPENAT_WRAPPER(FUNC) \
EXPORT int FUNC(int dirfd, const char* file, int oflag, ...) \
{ \
    va_list args; \
    va_start(args, oflag); \
    mode_t mode = openMode(oflag, args); \
    va_end(args); \
    static decltype(&FUNC) orig; \
    return CasePath::callAt(dirfd, file, [&](const char* f) {return next(orig, #FUNC)(dirfd, f, oflag, mode);}); \
}

#define PATH_WRAPPER(RET, FUNC, VERSION, PARAMS, ARGS) \
EXPORT RET FUNC PARAMS \
{ \
    static RET (*orig) PARAMS; \
    return CasePath::call(path, [&](const char* path) {return next(orig, #FUNC, VERSION) ARGS;}); \
}

OPEN_WRAPPER(open)
OPEN_WRAPPER(open64)
OPENAT_WRAPPER(openat)
OPENAT_WRAPPER(openat64)

PATH_WRAPPER(FILE*, fopen, nullptr, (const char* path, const char* modes), (path, modes))
PATH_WRAPPER(FILE*, fopen64, nullptr, (const char* path, const char* modes), (path, modes))
PATH_WRAPPER(int, access, nullptr, (const char* path, int type), (path, type))
PATH_WRAPPER(DIR*, opendir, nullptr, (const char* path), (path))
PATH_WRAPPER(char*, realpath, nullptr, (const char* path, char* resolved), (path, resolved))
PATH_WRAPPER(int, stat, nullptr, (const char* path, struct stat* buf), (path, buf))
PATH_WRAPPER(int, stat64, nullptr, (const char* path, struct stat64* buf), (path, buf))
PATH_WRAPPER(int, lstat, nullptr, (const char* path, struct stat* buf), (path, buf))
PATH_WRAPPER(int, lstat64, nullptr, (const char* path, struct stat64* buf), (path, buf))
PATH_WRAPPER(int, __xstat, "GLIBC_2.2.5", (int ver, const char* path, struct stat* buf), (ver, path, buf))
PATH_WRAPPER(int, __xstat64, "GLIBC_2.2.5", (int ver, const char* path, struct stat64* buf), (ver, path, buf))
PATH_WRAPPER(int, __lxstat, "GLIBC_2.2.5", (int ver, const char* path, struct stat* buf), (ver, path, buf))
PATH_WRAPPER(int, __lxstat64, "GLIBC_2.2.5", (int ver, const char* path, struct stat64* buf), (ver, path, buf))
PATH_WRAPPER(int, scandir, nullptr, (const char* path, struct dirent*** namelist, int (*selector) (const struct dirent*), int (*cmp) (const struct dirent**, const struct dirent**)), (path, namelist, selector, cmp))
PATH_WRAPPER(int, scandir64, nullptr, (const char* path, struct dirent64*** namelist, int (*selector) (const struct dirent64*), int (*cmp) (const struct dirent64**, const struct dirent64**)), (path, namelist, selector, cmp))
PATH_WRAPPER(int, remove, nullptr, (const char* path), (path))
PATH_WRAPPER(int, unlink, nullptr, (const char* path), (path))
PATH_WRAPPER(int, rmdir, nullptr, (const char* path), (path))
PATH_WRAPPER(int, mkdir, nullptr, (const char* path, mode_t mode), (path, mode))
PATH_WRAPPER(int, chmod, nullptr, (const char* path, mode_t mode), (path, mode))
PATH_WRAPPER(int, utime, nullptr, (const char* path, const struct utimbuf* times), (path, times))
PATH_WRAPPER(int, utimes, nullptr, (const char* path, const struct timeval times[2]), (path, times))

#define FSTATAT_WRAPPER(FUNC, STAT) \
EXPORT int FUNC(int ver, int dirfd, const char* path, struct STAT* buf, int flag) \
{ \
    static int (*orig) (int, int, const char*, struct STAT*, int); \
    return CasePath::callAt(dirfd, path, [&](const char* f) {return next(orig, #FUNC, "GLIBC_2.4")(ver, dirfd, f, buf, flag);}); \
}

FSTATAT_WRAPPER(__fxstatat, stat)
FSTATAT_WRAPPER(__fxstatat64, stat64)

/* Both paths may differ by case from existing files */
EXPORT int rename(const char* oldf, const char* newf)
{
    static decltype(&rename) orig;
    return CasePath::call(oldf, [&](const char* o) {
        return CasePath::call(newf, [&](const char* n) {return next(orig, "rename")(o, n);});
    });
}
