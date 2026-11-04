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

#ifndef LIBTAS_CASEPATH_H_INCLUDED
#define LIBTAS_CASEPATH_H_INCLUDED

/* Games made for Windows may access their files with a different case than
 * the files have (e.g. `Leaf.png` for `leaf.png`), which only works on
 * case-insensitive file systems. When the environment variable
 * LIBTAS_CASE_INSENSITIVE=1 is set, a path that does not exist is retried
 * with each missing component replaced by an existing file whose name only
 * differs by case, like Wine does.
 *
 * This file does not depend on the rest of libTAS, so that it can also be
 * built into the standalone library libtas-casefold.so, for running games
 * without libTAS. */

#include <cerrno>
#include <fcntl.h> // AT_FDCWD
#include <string>

namespace libtas {

namespace CasePath
{
/* Returns if case-insensitive paths are enabled */
bool enabled();

/* Enable case-insensitive paths, regardless of the environment variable */
void enable();

/* Find the path that matches `path` (relative to `dirfd` if not absolute)
 * when ignoring case. Each missing component is replaced by an existing name
 * that only differs by case. The last component may not exist (e.g. a file
 * being created). Returns false if no path different from `path` was found. */
bool resolve(int dirfd, const char* path, std::string& resolved);

inline bool isFailure(int ret) {return ret < 0;}
template<typename T> inline bool isFailure(T* ret) {return ret == nullptr;}

/* Call `call` with `path`, and if it fails because the path does not exist,
 * call it again with the path matching ignoring case. */
template<typename F>
auto callAt(int dirfd, const char* path, F call) -> decltype(call(path))
{
    auto ret = call(path);
    if (!isFailure(ret) || !path || ((errno != ENOENT) && (errno != ENOTDIR)) || !enabled())
        return ret;

    int saved_errno = errno;
    std::string resolved;
    if (resolve(dirfd, path, resolved))
        return call(resolved.c_str());

    errno = saved_errno;
    return ret;
}

template<typename F>
auto call(const char* path, F call) -> decltype(call(path))
{
    return callAt(AT_FDCWD, path, call);
}
}

}

#endif
