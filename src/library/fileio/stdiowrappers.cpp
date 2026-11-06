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

#include "stdiowrappers.h"
#include "CasePath.h"
#include "SaveFileList.h"
#include "SaveFile.h"
#ifdef __linux__
#include "URandom.h"
#endif

#include "logging.h"
#include "hook.h"
#include "GlobalState.h"
#include "PageScript.h"
#include "global.h"

#include <cerrno>
#include <fcntl.h>


namespace libtas {

DEFINE_ORIG_POINTER(fopen)
DEFINE_ORIG_POINTER(fopen64)
DEFINE_ORIG_POINTER(fclose)
DEFINE_ORIG_POINTER(fileno)
DEFINE_ORIG_POINTER(setbuf)
DEFINE_ORIG_POINTER(setvbuf)
DEFINE_ORIG_POINTER(setbuffer)
DEFINE_ORIG_POINTER(setlinebuf)

FILE *fopen (const char *filename, const char *modes)
{
    LINK_NAMESPACE_GLOBAL(fopen);

    if (GlobalState::isNative())
        return orig::fopen(filename, modes);

    LOGTRACE(LCF_FILEIO, "%s call with filename %s and mode %s", __func__, filename?filename:"<NULL>", modes?modes:"<NULL>");
    if (!filename) {
        return orig::fopen(filename, modes);
    }

    if (Global::shared_config.debug_state & SharedConfig::DEBUG_NATIVE_FILEIO)
        return orig::fopen(filename, modes);

    FILE* f = nullptr;

    /* Manifest and injected script of browser-based games */
    if (modes && (modes[0] == 'r') && !strchr(modes, '+')) {
        int fd = PageScript::open(filename, O_RDONLY);
        if (fd == -1)
            return nullptr;
        if (fd != PageScript::NOT_HANDLED)
            return fdopen(fd, "r");
    }

#ifdef __linux__
    if ((strcmp(filename, "/dev/urandom") == 0) || (strcmp(filename, "/dev/random") == 0)) {
        return urandom_create_file();
    } else
#endif

    if (strcmp(filename, "/proc/uptime") == 0) {
        if (SaveFileList::getSaveFileFd(filename) == 0) {
            /* Create a file with memory storage (reusing the savefile code),
             * and fill it with values from the initial time, so that, for
             * games that use it as PRNG seed, tweaking the initial time will
             * change the seed value.
             */
            f = SaveFileList::openSaveFile(filename, "w");

            std::ostringstream datestr;
            datestr << Global::shared_config.initial_time_sec << ".";
            datestr << std::setfill ('0') << std::setw (2);
            datestr << Global::shared_config.initial_time_nsec / 10000000;

            std::string s = datestr.str();

            LOG(LL_DEBUG, LCF_FILEIO, "Creating fake %s with %s", filename, s.c_str());
            if ((fwrite(s.c_str(), sizeof(char), s.size(), f) != s.size()) ||
                (fwrite(" ", sizeof(char), 1, f) != 1) ||
                (fwrite(s.c_str(), sizeof(char), s.size(), f) != s.size())) {
                SaveFileList::closeSaveFile(f);
                errno = EIO;
                return nullptr;
            }
            if (fseek(f, 0, SEEK_SET) != 0) {
                SaveFileList::closeSaveFile(f);
                return nullptr;
            }
        }
        else {
            f = SaveFileList::openSaveFile(filename, modes);
        }
    }

    else if (!GlobalState::isOwnCode() && SaveFileList::isSaveFile(filename, modes)) {
        LOG(LL_DEBUG, LCF_FILEIO, "  savefile detected");
        f = SaveFileList::openSaveFile(filename, modes);
    }

    else {
        f = CasePath::call(filename, [&](const char* fn) {return orig::fopen(fn, modes);});
    }

    return f;
}

FILE *fopen64 (const char *filename, const char *modes)
{
    LINK_NAMESPACE_GLOBAL(fopen64);

    if (GlobalState::isNative())
        return orig::fopen64(filename, modes);

    LOGTRACE(LCF_FILEIO, "%s call with filename %s and mode %s", __func__, filename?filename:"<NULL>", modes?modes:"<NULL>");

    if (!filename) {
        return orig::fopen64(filename, modes);
    }

    if (Global::shared_config.debug_state & SharedConfig::DEBUG_NATIVE_FILEIO)
        return orig::fopen64(filename, modes);

    FILE* f = nullptr;

    /* Manifest and injected script of browser-based games */
    if (modes && (modes[0] == 'r') && !strchr(modes, '+')) {
        int fd = PageScript::open(filename, O_RDONLY);
        if (fd == -1)
            return nullptr;
        if (fd != PageScript::NOT_HANDLED)
            return fdopen(fd, "r");
    }

#ifdef __linux__
    if ((strcmp(filename, "/dev/urandom") == 0) || (strcmp(filename, "/dev/random") == 0)) {
        return urandom_create_file();
    } else
#endif

    if (strcmp(filename, "/proc/uptime") == 0) {
        if (SaveFileList::getSaveFileFd(filename) == 0) {
            /* Create a file with memory storage (reusing the savefile code),
             * and fill it with values from the initial time, so that, for
             * games that use it as PRNG seed, tweaking the initial time will
             * change the seed value.
             */
            f = SaveFileList::openSaveFile(filename, "w");

            std::ostringstream datestr;
            datestr << Global::shared_config.initial_time_sec << ".";
            datestr << std::setfill ('0') << std::setw (2);
            datestr << Global::shared_config.initial_time_nsec / 10000000;

            std::string s = datestr.str();

            LOG(LL_DEBUG, LCF_FILEIO, "Creating fake %s with %s", filename, s.c_str());
            if ((fwrite(s.c_str(), sizeof(char), s.size(), f) != s.size()) ||
                (fwrite(" ", sizeof(char), 1, f) != 1) ||
                (fwrite(s.c_str(), sizeof(char), s.size(), f) != s.size())) {
                SaveFileList::closeSaveFile(f);
                errno = EIO;
                return nullptr;
            }
            if (fseek(f, 0, SEEK_SET) != 0) {
                SaveFileList::closeSaveFile(f);
                return nullptr;
            }
        }
        else {
            f = SaveFileList::openSaveFile(filename, modes);
        }
    }

    else if (!GlobalState::isOwnCode() && SaveFileList::isSaveFile(filename, modes)) {
        LOG(LL_DEBUG, LCF_FILEIO, "  savefile detected");
        f = SaveFileList::openSaveFile(filename, modes);
    }

    else {
        f = CasePath::call(filename, [&](const char* fn) {return orig::fopen64(fn, modes);});
    }

    return f;
}

int fclose (FILE *stream)
{
    LINK_NAMESPACE_GLOBAL(fclose);

    if (GlobalState::isNative())
        return orig::fclose(stream);

    LOGTRACE_SIMPLE(LCF_FILEIO);

    if (Global::shared_config.debug_state & SharedConfig::DEBUG_NATIVE_FILEIO)
        return orig::fclose(stream);

#ifdef __linux__
    /* Check for urandom */
    if (urandom_get_file() == stream) {
        return 0;
    }
#endif

    int ret = SaveFileList::closeSaveFile(stream);
    if (ret != 1)
        return ret;

    return orig::fclose(stream);
}

int fileno (FILE *stream) __THROW
{
    LINK_NAMESPACE_GLOBAL(fileno);

    if (GlobalState::isNative())
        return orig::fileno(stream);

    LOGTRACE_SIMPLE(LCF_FILEIO);

    /* If the stream is a savefile, we need to look into the savefile to get the
     * fd, because our custom SaveFileStream does not have a native associated fd */
    const SaveFile* sf = SaveFileList::getSaveFile(stream);
    if (sf) {
        LOG(LL_DEBUG, LCF_FILEIO, "Our custom savefile stream for %s does not have a valid fd. Subsequent operations may fail.", sf->filename.c_str());
        return sf->fd;
    }

    return orig::fileno(stream);
}

void setbuf (FILE *stream, char *buf) __THROW
{
    LINK_NAMESPACE_GLOBAL(setbuf);

    if (GlobalState::isNative())
        return orig::setbuf(stream, buf);

    LOGTRACE_SIMPLE(LCF_FILEIO);

    /* If the stream is a savefile, keep the stream unbuffered. */
    const SaveFile* sf = SaveFileList::getSaveFile(stream);
    if (sf)
        return;

    return orig::setbuf(stream, buf);
}

int setvbuf (FILE *stream, char *buf, int modes, size_t n) __THROW
{
    LINK_NAMESPACE_GLOBAL(setvbuf);

    if (GlobalState::isNative())
        return orig::setvbuf(stream, buf, modes, n);

    LOGTRACE(LCF_FILEIO, "%s call with stream %p and mode %d", __func__, stream, modes);

    /* If the stream is a savefile, keep the stream unbuffered. */
    const SaveFile* sf = SaveFileList::getSaveFile(stream);
    if (sf)
        return 0;

    return orig::setvbuf(stream, buf, modes, n);
}

void setbuffer (FILE *stream, char *buf, size_t size) __THROW
{
    LINK_NAMESPACE_GLOBAL(setbuffer);

    if (GlobalState::isNative())
        return orig::setbuffer(stream, buf, size);

    LOGTRACE_SIMPLE(LCF_FILEIO);

    /* If the stream is a savefile, keep the stream unbuffered. */
    const SaveFile* sf = SaveFileList::getSaveFile(stream);
    if (sf)
        return;

    return orig::setbuffer(stream, buf, size);
}

void setlinebuf (FILE *stream) __THROW
{
    LINK_NAMESPACE_GLOBAL(setlinebuf);

    if (GlobalState::isNative())
        return orig::setlinebuf(stream);

    LOGTRACE_SIMPLE(LCF_FILEIO);

    /* If the stream is a savefile, keep the stream unbuffered. */
    const SaveFile* sf = SaveFileList::getSaveFile(stream);
    if (sf)
        return;

    return orig::setlinebuf(stream);
}

}
