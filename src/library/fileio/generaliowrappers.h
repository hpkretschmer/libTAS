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

#ifndef LIBTAS_GENERALIO_H_INCLUDED
#define LIBTAS_GENERALIO_H_INCLUDED

#include "hook.h"

#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <utime.h>

namespace libtas {

/* Rename file OLD to NEW.  */
OVERRIDE int rename (const char *oldf, const char *newf) __THROW;

/* Remove file FILENAME.  */
OVERRIDE int remove (const char *filename) __THROW;

/* Remove the link NAME.  */
OVERRIDE int unlink (const char *name) __THROW;

/* Scan the directory DIR */
OVERRIDE int scandir (const char *__restrict dir, struct dirent ***__restrict namelist,
    int (*selector) (const struct dirent *),
    int (*cmp) (const struct dirent **, const struct dirent **));
OVERRIDE int scandir64 (const char *__restrict dir, struct dirent64 ***__restrict namelist,
    int (*selector) (const struct dirent64 *),
    int (*cmp) (const struct dirent64 **, const struct dirent64 **));

/* Legacy stat functions relative to a directory, used by old binaries */
OVERRIDE int __fxstatat(int ver, int dirfd, const char *path, struct stat *buf, int flag) __THROW;
OVERRIDE int __fxstatat64(int ver, int dirfd, const char *path, struct stat64 *buf, int flag) __THROW;

/* Remove, create, and change directories and files */
OVERRIDE int rmdir (const char *path) __THROW;
OVERRIDE int mkdir (const char *path, mode_t mode) __THROW;
OVERRIDE int chmod (const char *path, mode_t mode) __THROW;
OVERRIDE int utime (const char *path, const struct utimbuf *times) __THROW;
OVERRIDE int utimes (const char *path, const struct timeval times[2]) __THROW;

}

#endif
