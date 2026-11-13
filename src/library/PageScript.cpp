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
#include "global.h" // Global::shared_config
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

/* Adds `member` as the first member of the manifest object, returns false if
 * the manifest has no object */
static bool insertMember(std::string& manifest, const std::string& member)
{
    size_t brace = manifest.find('{');
    if (brace == std::string::npos)
        return false;
    size_t next = manifest.find_first_not_of(" \t\r\n", brace + 1);
    bool empty = (next != std::string::npos) && (manifest[next] == '}');
    manifest.insert(brace + 1, empty ? member : (member + ","));
    return true;
}

/* Seed of Math.random, from the initial time. V8 (`--random-seed`) needs a
 * nonzero int. */
static int randomSeed()
{
    const int64_t max = 2147483647;
    int64_t seed = ((Global::shared_config.initial_time_sec % max) + max) % max;
    return seed ? static_cast<int>(seed) : static_cast<int>(max);
}

/* Adds `flag` at the end of the string member `name` of the manifest (e.g.
 * "js-flags"), returns false if the manifest could not be changed */
static bool addFlag(std::string& manifest, const char* name, const std::string& flag)
{
    std::string key = std::string("\"") + name + "\"";
    size_t pos = manifest.find(key);
    if (pos == std::string::npos)
        return insertMember(manifest, key + ": \"" + flag + "\"");

    /* Add the flag at the end of the string value */
    size_t colon = manifest.find_first_not_of(" \t\r\n", pos + key.size());
    if ((colon == std::string::npos) || (manifest[colon] != ':'))
        return false;
    size_t quote = manifest.find_first_not_of(" \t\r\n", colon + 1);
    if ((quote == std::string::npos) || (manifest[quote] != '"'))
        return false;
    size_t end = quote + 1;
    while ((end < manifest.size()) && (manifest[end] != '"')) {
        if (manifest[end] == '\\')
            end++;
        end++;
    }
    if (end >= manifest.size())
        return false;
    bool empty = manifest.find_first_not_of(" ", quote + 1) == end;
    manifest.insert(end, empty ? flag : (" " + flag));
    return true;
}

int PageScript::open(const char* file, int oflag, bool giveManifest)
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

    if (!giveManifest || !isAppManifest(file))
        return NOT_HANDLED;

    std::string manifest;
    if (!readFile(file, manifest))
        return NOT_HANDLED;
    bool changed = false;

    /* Seed Math.random from the initial time. Otherwise V8 seeds it from
     * /dev/urandom, through a generator that it also uses for other things
     * (e.g. identity hashes), so the random numbers of the game would depend
     * on the timing of the threads that read /dev/urandom and of the work
     * that the page does before its first random number. */
    if (manifest.find("--random-seed") != std::string::npos) {
        LOG(LL_WARN, LCF_FILEIO, "The NW.js app already sets a V8 random seed, the random numbers do not depend on the initial time");
    }
    else {
        std::string flag = "--random-seed=" + std::to_string(randomSeed());
        if (addFlag(manifest, "js-flags", flag)) {
            LOG(LL_INFO, LCF_FILEIO, "Seeding the random numbers of the NW.js app with %s", flag.c_str());
            changed = true;
        }
        else {
            LOG(LL_WARN, LCF_FILEIO, "Could not add a V8 random seed to the NW.js manifest %s, the random numbers may not be deterministic", file);
        }
    }

    /* Chromium options from LIBTAS_CHROMIUM_ARGS, added to the manifest, so
     * that the command line of the game only has its own arguments, as with
     * Steam: some games read them, e.g. OMORI 1.0.6 takes its decryption key
     * from them. */
    const char* chromiumArgs = std::getenv("LIBTAS_CHROMIUM_ARGS");
    if (chromiumArgs && (chromiumArgs[0] != '\0')) {
        /* As a JSON string */
        std::string args;
        for (const char* c = chromiumArgs; *c; c++) {
            if ((*c == '"') || (*c == '\\'))
                args += '\\';
            args += *c;
        }
        if (addFlag(manifest, "chromium-args", args)) {
            LOG(LL_INFO, LCF_FILEIO, "Adding %s to the chromium-args of the NW.js manifest", chromiumArgs);
            changed = true;
        }
        else {
            LOG(LL_WARN, LCF_FILEIO, "Could not add the Chromium options to the NW.js manifest %s", file);
        }
    }

    /* Add the script as the first member of the manifest object */
    if (manifest.find("\"inject_js_start\"") != std::string::npos) {
        LOG(LL_WARN, LCF_FILEIO, "The NW.js app already injects a script with inject_js_start, the page script of libTAS must be loaded by the page");
    }
    else if (insertMember(manifest, "\"inject_js_start\": \"" LIBTAS_PAGE_SCRIPT_PATH "\"")) {
        LOG(LL_INFO, LCF_FILEIO, "Injecting the page script into the NW.js app with its manifest %s", file);
        changed = true;
    }
    else {
        LOG(LL_WARN, LCF_FILEIO, "Could not read the NW.js manifest %s, the page script of libTAS must be loaded by the page", file);
    }

    if (!changed)
        return NOT_HANDLED;
    return memoryFile("package.json", manifest);
}

}
