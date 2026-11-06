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

#include "PageScript.h"

#include "logging.h"
#include "GlobalState.h"
#include "Utils.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

namespace libtas {

/* The script, generated from web/libtas_web.js at build time */
static const char script[] =
#include "PageScriptSource.inc"
;

static bool enabled()
{
    const char* env = std::getenv("LIBTAS_PAGE_FRAMES");
    return env && (env[0] != '\0') && (strcmp(env, "0") != 0);
}

static bool readFile(const char* path, std::string& content)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return !stream.bad();
}

/* Directory of the NW.js app, which is the first argument of the command
 * line that is not an option, or empty if there is none */
static const std::string& appDirectory()
{
    static std::string dir;
    static bool done = false;
    if (done)
        return dir;
    done = true;

    std::string cmdline;
    if (!readFile("/proc/self/cmdline", cmdline))
        return dir;

    /* Arguments are separated by null characters, the first one is the
     * executable */
    size_t pos = cmdline.find('\0');
    while ((pos != std::string::npos) && (pos + 1 < cmdline.size())) {
        size_t end = cmdline.find('\0', pos + 1);
        std::string arg = cmdline.substr(pos + 1, end - pos - 1);
        if (!arg.empty() && (arg[0] != '-')) {
            char real[PATH_MAX];
            if (realpath(arg.c_str(), real))
                dir = real;
            break;
        }
        pos = end;
    }
    return dir;
}

/* Returns if `file` is the manifest package.json of the NW.js app. Node.js
 * modules of the app have their own package.json, which must be unchanged. */
static bool isAppManifest(const char* file)
{
    static const char suffix[] = "/package.json";
    size_t len = strlen(file);
    size_t suffix_len = sizeof(suffix) - 1;
    if ((len < suffix_len) || (strcmp(file + len - suffix_len, suffix) != 0))
        return false;

    const std::string& app = appDirectory();
    if (app.empty())
        return false;

    std::string dir(file, len - suffix_len);
    char real[PATH_MAX];
    return realpath(dir.empty() ? "/" : dir.c_str(), real) && (app == real);
}

/* Read-only copy of `content` in memory */
static int memoryFile(const char* name, const std::string& content)
{
    int fd = syscall(SYS_memfd_create, name, 0);
    if (fd < 0)
        return -1;
    if ((Utils::writeAll(fd, content.data(), content.size()) != static_cast<ssize_t>(content.size())) ||
        (lseek(fd, 0, SEEK_SET) < 0)) {
        close(fd);
        return -1;
    }
    return fd;
}

int PageScript::open(const char* file, int oflag)
{
    /* Only for reading, and when page frames are enabled */
    if (!file || ((oflag & O_ACCMODE) != O_RDONLY) || !enabled())
        return NOT_HANDLED;

    /* The checks below read files and must not go through our hooks */
    GlobalNative gn;

    if (strcmp(file, LIBTAS_PAGE_SCRIPT_PATH) == 0) {
        LOG(LL_DEBUG, LCF_FILEIO, "Giving the page script");
        return memoryFile("libtas_web.js", script);
    }

    if (!isAppManifest(file))
        return NOT_HANDLED;

    std::string manifest;
    if (!readFile(file, manifest))
        return NOT_HANDLED;

    if (manifest.find("\"inject_js_start\"") != std::string::npos) {
        LOG(LL_WARN, LCF_FILEIO, "The NW.js app already injects a script with inject_js_start, the page script of libTAS must be loaded by the page");
        return NOT_HANDLED;
    }

    /* Add the script as the first member of the manifest object */
    size_t brace = manifest.find('{');
    if (brace == std::string::npos) {
        LOG(LL_WARN, LCF_FILEIO, "Could not read the NW.js manifest %s, the page script of libTAS must be loaded by the page", file);
        return NOT_HANDLED;
    }
    size_t next = manifest.find_first_not_of(" \t\r\n", brace + 1);
    bool empty = (next != std::string::npos) && (manifest[next] == '}');
    std::string member = "\"inject_js_start\": \"" LIBTAS_PAGE_SCRIPT_PATH "\"";
    manifest.insert(brace + 1, empty ? member : (member + ","));

    LOG(LL_INFO, LCF_FILEIO, "Injecting the page script into the NW.js app with its manifest %s", file);
    return memoryFile("package.json", manifest);
}

}
