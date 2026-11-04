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

#include "generaliowrappers.h"
#include "SaveFileList.h"
#include "CasePath.h"

#include "logging.h"
#include "hook.h"
#include "GlobalState.h"
#include "global.h"

namespace libtas {

DEFINE_ORIG_POINTER(rename)
DEFINE_ORIG_POINTER(__fxstatat)
DEFINE_ORIG_POINTER(__fxstatat64)

int rename (const char *oldf, const char *newf) __THROW
{
    RETURN_IF_NATIVE(rename, (oldf, newf), nullptr);
    
    LOGTRACE(LCF_FILEIO, "%s call with old %s and new %s", __func__, oldf?oldf:"<NULL>", newf?newf:"<NULL>");

    if (Global::shared_config.debug_state & SharedConfig::DEBUG_NATIVE_FILEIO)
        RETURN_NATIVE(rename, (oldf, newf), nullptr);

    /* Check if file is a savefile */
    int ret = SaveFileList::renameSaveFile(oldf, newf);
    if (ret != 1) {
        return ret;
    }

    /* Both paths may differ by case from existing files */
    LINK_NAMESPACE_GLOBAL(rename);
    return CasePath::call(oldf, [&](const char* o) {
        return CasePath::call(newf, [&](const char* n) {return orig::rename(o, n);});
    });
}

/* Remove file FILENAME.  */
int remove (const char *filename) __THROW
{
    RETURN_IF_NATIVE(remove, (filename), nullptr);

    LOGTRACE(LCF_FILEIO, "%s call with file %s", __func__, filename?filename:"<NULL>");

    if (Global::shared_config.debug_state & SharedConfig::DEBUG_NATIVE_FILEIO)
        RETURN_NATIVE(remove, (filename), nullptr);

    /* Check if file is a savefile */
    int ret = SaveFileList::removeSaveFile(filename);
    if (ret != 1) {
        return ret;
    }

    RETURN_NATIVE_CASE(remove, filename, (filename));
}

/* Remove the link NAME.  */
int unlink (const char *name) __THROW
{
    RETURN_IF_NATIVE(unlink, (name), nullptr);

    LOGTRACE(LCF_FILEIO, "%s call with file %s", __func__, name?name:"<NULL>");

    if (Global::shared_config.debug_state & SharedConfig::DEBUG_NATIVE_FILEIO)
        RETURN_NATIVE(unlink, (name), nullptr);

    /* Check if file is a savefile */
    int ret = SaveFileList::removeSaveFile(name);
    if (ret != 1) {
        return ret;
    }

    RETURN_NATIVE_CASE(unlink, name, (name));
}


/* The following functions are only hooked for case-insensitive paths */

int scandir (const char *__restrict dir, struct dirent ***__restrict namelist,
    int (*selector) (const struct dirent *),
    int (*cmp) (const struct dirent **, const struct dirent **))
{
    RETURN_IF_NATIVE(scandir, (dir, namelist, selector, cmp), nullptr);
    RETURN_NATIVE_CASE(scandir, dir, (dir, namelist, selector, cmp));
}

int scandir64 (const char *__restrict dir, struct dirent64 ***__restrict namelist,
    int (*selector) (const struct dirent64 *),
    int (*cmp) (const struct dirent64 **, const struct dirent64 **))
{
    RETURN_IF_NATIVE(scandir64, (dir, namelist, selector, cmp), nullptr);
    RETURN_NATIVE_CASE(scandir64, dir, (dir, namelist, selector, cmp));
}

int __fxstatat(int ver, int dirfd, const char *path, struct stat *buf, int flag) __THROW
{
    LINK_NAMESPACE_VERSION(__fxstatat, "c", "GLIBC_2.4");
    if (GlobalState::isNative())
        return orig::__fxstatat(ver, dirfd, path, buf, flag);
    return CasePath::callAt(dirfd, path, [&](const char* f) {return orig::__fxstatat(ver, dirfd, f, buf, flag);});
}

int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *buf, int flag) __THROW
{
    LINK_NAMESPACE_VERSION(__fxstatat64, "c", "GLIBC_2.4");
    if (GlobalState::isNative())
        return orig::__fxstatat64(ver, dirfd, path, buf, flag);
    return CasePath::callAt(dirfd, path, [&](const char* f) {return orig::__fxstatat64(ver, dirfd, f, buf, flag);});
}

int rmdir (const char *path) __THROW
{
    RETURN_IF_NATIVE(rmdir, (path), nullptr);
    RETURN_NATIVE_CASE(rmdir, path, (path));
}

int mkdir (const char *path, mode_t mode) __THROW
{
    RETURN_IF_NATIVE(mkdir, (path, mode), nullptr);
    RETURN_NATIVE_CASE(mkdir, path, (path, mode));
}

int chmod (const char *path, mode_t mode) __THROW
{
    RETURN_IF_NATIVE(chmod, (path, mode), nullptr);
    RETURN_NATIVE_CASE(chmod, path, (path, mode));
}

int utime (const char *path, const struct utimbuf *times) __THROW
{
    RETURN_IF_NATIVE(utime, (path, times), nullptr);
    RETURN_NATIVE_CASE(utime, path, (path, times));
}

int utimes (const char *path, const struct timeval times[2]) __THROW
{
    RETURN_IF_NATIVE(utimes, (path, times), nullptr);
    RETURN_NATIVE_CASE(utimes, path, (path, times));
}

}
