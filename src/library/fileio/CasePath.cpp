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

#include "CasePath.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <strings.h> // strcasecmp
#include <sys/syscall.h>
#include <unistd.h>

namespace libtas {

/* Files are accessed with raw system calls, so that this code does not go
 * through the hooked functions that call it */

static std::atomic<bool> forced(false);

bool CasePath::enabled()
{
    static const bool from_env = []{
        const char* env = std::getenv("LIBTAS_CASE_INSENSITIVE");
        return env && (env[0] != '\0') && (strcmp(env, "0") != 0);
    }();
    return from_env || forced;
}

void CasePath::enable()
{
    forced = true;
}

static bool exists(int dirfd, const std::string& path)
{
    return syscall(SYS_faccessat, dirfd, path.c_str(), F_OK) == 0;
}

/* Directory entry of getdents64 */
struct LinuxDirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

/* Look in directory `dir` for an entry named `name` ignoring case */
static bool findIgnoringCase(int dirfd, const std::string& dir, const std::string& name, std::string& found)
{
    int fd = syscall(SYS_openat, dirfd, dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;

    bool ret = false;
    alignas(LinuxDirent64) char buf[4096];
    long n;
    while (!ret && (n = syscall(SYS_getdents64, fd, buf, sizeof(buf))) > 0) {
        for (long off = 0; off < n;) {
            const LinuxDirent64* entry = reinterpret_cast<const LinuxDirent64*>(buf + off);
            if (strcasecmp(entry->d_name, name.c_str()) == 0) {
                found = entry->d_name;
                ret = true;
                break;
            }
            off += entry->d_reclen;
        }
    }

    syscall(SYS_close, fd);
    return ret;
}

bool CasePath::resolve(int dirfd, const char* path, std::string& resolved)
{
    if (!path || (path[0] == '\0'))
        return false;

    std::string result = (path[0] == '/') ? "/" : "";
    bool changed = false;

    const char* p = path;
    while (true) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;

        const char* end = strchr(p, '/');
        size_t len = end ? static_cast<size_t>(end - p) : strlen(p);
        std::string name(p, len);
        p += len;

        bool last = (p[strspn(p, "/")] == '\0');

        if ((name != ".") && (name != "..") && !exists(dirfd, result + name)) {
            std::string found;
            if (findIgnoringCase(dirfd, result.empty() ? "." : result, name, found)) {
                name = found;
                changed = true;
            }
            else if (!last) {
                /* A missing directory, the path does not exist */
                return false;
            }
        }

        result += name;
        if (*p == '/')
            result += '/';
    }

    if (!changed)
        return false;

    resolved = result;
    return true;
}

}
