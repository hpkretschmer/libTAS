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

#include "signalwrappers.h"

#include "logging.h"
#include "GlobalState.h"
#include "hook.h"
#include "checkpoint/ThreadSync.h"
#include "checkpoint/SaveStateManager.h" // checkpoint signals

#include <cstring>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <sys/uio.h>

namespace libtas {

DEFINE_ORIG_POINTER(signal)
DEFINE_ORIG_POINTER(sigblock)
DEFINE_ORIG_POINTER(sigsetmask)
DEFINE_ORIG_POINTER(siggetmask)
DEFINE_ORIG_POINTER(sigprocmask)
DEFINE_ORIG_POINTER(sigsuspend)
DEFINE_ORIG_POINTER(sigaction)
DEFINE_ORIG_POINTER(sigpending)
DEFINE_ORIG_POINTER(pthread_sigmask)
DEFINE_ORIG_POINTER(pthread_kill)
DEFINE_ORIG_POINTER(sigaltstack)

static sigset_t origUsrSetProcess;
static thread_local sigset_t origUsrSetThread;
static struct sigaction savedSuspendAction = {};
static struct sigaction savedCheckpointAction = {};
static bool savedSuspendActionValid = false;
static bool savedCheckpointActionValid = false;


/* CRASHDBG (temporary): with env LIBTAS_CRASH_LOG=<file>, log savestate events and,
 * on a crash, the faulting address, registers, /proc/self/maps and a stack scan,
 * before calling the game's own crash handler. */
extern uint64_t framecount;
static int crashlog_state = 0; /* 0 unknown, 1 on, -1 off */
static char crashlog_path[512];
static struct sigaction crashGameAction[65];
static bool crashGameActionValid[65];

static bool crashlog_on()
{
    if (crashlog_state == 0) {
        const char* p = getenv("LIBTAS_CRASH_LOG");
        if (p && p[0]) { strncpy(crashlog_path, p, sizeof(crashlog_path)-1); crashlog_state = 1; }
        else crashlog_state = -1;
    }
    return crashlog_state == 1;
}

static void crashlog_write(const char* buf, size_t len)
{
    int fd = syscall(SYS_openat, AT_FDCWD, crashlog_path, O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC, 0644);
    if (fd < 0) return;
    while (len > 0) {
        ssize_t r = syscall(SYS_write, fd, buf, len);
        if (r <= 0) break;
        buf += r; len -= r;
    }
    syscall(SYS_close, fd);
}

void crashlog_event(const char* fmt, ...)
{
    if (!crashlog_on()) return;
    char buf[1024];
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_REALTIME, &ts);
    int n = snprintf(buf, sizeof(buf), "%ld.%03ld f:%llu tid:%ld ", (long)ts.tv_sec, ts.tv_nsec/1000000,
        (unsigned long long)framecount, (long)syscall(SYS_gettid));
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(buf+n, sizeof(buf)-n-1, fmt, args);
    va_end(args);
    if (n > (int)sizeof(buf)-2) n = sizeof(buf)-2;
    buf[n++] = '\n';
    crashlog_write(buf, n);
}

static void crashlog_handler(int sig, siginfo_t* info, void* uc)
{
    char name[17] = {};
    prctl(PR_GET_NAME, name);
    ucontext_t* ctx = static_cast<ucontext_t*>(uc);
    greg_t* r = ctx->uc_mcontext.gregs;
    crashlog_event("CRASH signal %d code %d addr %p thread '%s' rip %llx rsp %llx rbp %llx rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx trapno %llx err %llx",
        sig, info->si_code, info->si_addr, name,
        (unsigned long long)r[REG_RIP], (unsigned long long)r[REG_RSP], (unsigned long long)r[REG_RBP],
        (unsigned long long)r[REG_RAX], (unsigned long long)r[REG_RBX], (unsigned long long)r[REG_RCX],
        (unsigned long long)r[REG_RDX], (unsigned long long)r[REG_RSI], (unsigned long long)r[REG_RDI],
        (unsigned long long)r[REG_R8], (unsigned long long)r[REG_R9], (unsigned long long)r[REG_R10],
        (unsigned long long)r[REG_R11], (unsigned long long)r[REG_R12], (unsigned long long)r[REG_R13],
        (unsigned long long)r[REG_R14], (unsigned long long)r[REG_R15],
        (unsigned long long)r[REG_TRAPNO], (unsigned long long)r[REG_ERR]);

    /* Memory map */
    {
        crashlog_write("MAPS\n", 5);
        int fd = syscall(SYS_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY|O_CLOEXEC);
        if (fd >= 0) {
            char buf[4096];
            ssize_t len;
            while ((len = syscall(SYS_read, fd, buf, sizeof(buf))) > 0)
                crashlog_write(buf, len);
            syscall(SYS_close, fd);
        }
        crashlog_write("END\n", 4);
    }

    /* Stack scan: up to 2048 words from rsp, read with process_vm_readv so
     * that the end of the stack mapping does not fault */
    {
        char buf[4096];
        int n = snprintf(buf, sizeof(buf), "STACK\n");
        unsigned long long words[64];
        char* sp = reinterpret_cast<char*>(r[REG_RSP]);
        for (int chunk = 0; chunk < 32; chunk++) {
            struct iovec local = {words, sizeof(words)};
            struct iovec remote = {sp + chunk*sizeof(words), sizeof(words)};
            ssize_t got = syscall(SYS_process_vm_readv, syscall(SYS_getpid), &local, 1, &remote, 1, 0);
            if (got <= 0) break;
            for (int i = 0; i < got/8; i++) {
                n += snprintf(buf+n, sizeof(buf)-n, "%llx\n", words[i]);
                if (n > (int)sizeof(buf) - 40) { crashlog_write(buf, n); n = 0; }
            }
            if (got < (ssize_t)sizeof(words)) break;
        }
        crashlog_write(buf, n);
        crashlog_write("STACKEND\n", 9);
    }

    /* Call the game's handler */
    if (sig > 0 && sig < 65 && crashGameActionValid[sig]) {
        struct sigaction& ga = crashGameAction[sig];
        if (ga.sa_flags & SA_SIGINFO) { ga.sa_sigaction(sig, info, uc); return; }
        if (ga.sa_handler != SIG_DFL && ga.sa_handler != SIG_IGN) { ga.sa_handler(sig); return; }
    }
    /* Default action */
    struct sigaction dfl = {};
    dfl.sa_handler = SIG_DFL;
    orig::sigaction(sig, &dfl, nullptr);
    raise(sig);
}

static bool crashlog_is_crash_signal(int sig)
{
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGABRT || sig == SIGTRAP;
}

static int signalMaskBit(int sig)
{
    if (sig <= 0 || sig > static_cast<int>(sizeof(unsigned int) * 8))
        return 0;

    return static_cast<int>(1u << (sig - 1));
}

/* Override */ sighandler_t signal (int sig, sighandler_t handler) __THROW
{
    LOGTRACE_SIMPLE(LCF_SIGNAL);
    LINK_NAMESPACE_GLOBAL(signal);

    /* Our checkpoint code uses signals, so we must prevent the game from
     * signaling threads at the same time.
     */
    WrapperLock wrapperLock;

    LOG(LL_DEBUG, LCF_SIGNAL, "    Setting handler %p for signal %s", reinterpret_cast<void*>(handler), strsignal(sig));

    if ((sig == SaveStateManager::sigSuspend()) || (sig == SaveStateManager::sigCheckpoint())) {
        sighandler_t previousHandler = SIG_DFL;
        if (sig == SaveStateManager::sigSuspend()) {
            if (savedSuspendActionValid) {
                previousHandler = savedSuspendAction.sa_handler;
            }
            savedSuspendAction.sa_handler = handler;
            savedSuspendAction.sa_flags = 0;
            sigemptyset(&savedSuspendAction.sa_mask);
            savedSuspendActionValid = true;
        }
        else {
            if (savedCheckpointActionValid) {
                previousHandler = savedCheckpointAction.sa_handler;
            }
            savedCheckpointAction.sa_handler = handler;
            savedCheckpointAction.sa_flags = 0;
            sigemptyset(&savedCheckpointAction.sa_mask);
            savedCheckpointActionValid = true;
        }
        return previousHandler;
    }

    sighandler_t ret = orig::signal(sig, handler);

    return ret;
}
    
/* Override */ int sigblock (int mask) __THROW
{
    LOGTRACE_SIMPLE(LCF_SIGNAL);
    LINK_NAMESPACE_GLOBAL(sigblock);

    static const int bannedMask = signalMaskBit(SaveStateManager::sigSuspend()) | signalMaskBit(SaveStateManager::sigCheckpoint());

    /* Remove our signals from the list of blocked signals */
    int oldmask = orig::sigblock(mask & ~bannedMask);

    /* Add which of our signals were blocked */
    if (sigismember(&origUsrSetProcess, SaveStateManager::sigSuspend()) == 1)
        oldmask |= signalMaskBit(SaveStateManager::sigSuspend());
    if (sigismember(&origUsrSetProcess, SaveStateManager::sigCheckpoint()) == 1)
        oldmask |= signalMaskBit(SaveStateManager::sigCheckpoint());

    /* Update which of our signals are blocked */
    if (mask & signalMaskBit(SaveStateManager::sigSuspend()))
        sigaddset(&origUsrSetProcess, SaveStateManager::sigSuspend());
    if (mask & signalMaskBit(SaveStateManager::sigCheckpoint()))
        sigaddset(&origUsrSetProcess, SaveStateManager::sigCheckpoint());

    return oldmask;
}

/* Override */ int sigsetmask (int mask) __THROW
{
    LOGTRACE_SIMPLE(LCF_SIGNAL);
    LINK_NAMESPACE_GLOBAL(sigsetmask);

    static const int bannedMask = signalMaskBit(SaveStateManager::sigSuspend()) | signalMaskBit(SaveStateManager::sigCheckpoint());

    /* Remove our signals from the list of blocked signals */
    int oldmask = orig::sigsetmask(mask & ~bannedMask);

    /* Update which of our signals were blocked */
    if (sigismember(&origUsrSetProcess, SaveStateManager::sigSuspend()) == 1)
        oldmask |= signalMaskBit(SaveStateManager::sigSuspend());
    if (sigismember(&origUsrSetProcess, SaveStateManager::sigCheckpoint()) == 1)
        oldmask |= signalMaskBit(SaveStateManager::sigCheckpoint());

    /* Update which of our signals are blocked */
    sigemptyset(&origUsrSetProcess);
    if (mask & signalMaskBit(SaveStateManager::sigSuspend()))
        sigaddset(&origUsrSetProcess, SaveStateManager::sigSuspend());
    if (mask & signalMaskBit(SaveStateManager::sigCheckpoint()))
        sigaddset(&origUsrSetProcess, SaveStateManager::sigCheckpoint());

    return oldmask;
}

/* Override */ int siggetmask (void) __THROW
{
    LOGTRACE_SIMPLE(LCF_SIGNAL);
    LINK_NAMESPACE_GLOBAL(siggetmask);

    int oldmask = orig::siggetmask();

    /* Update which of our signals were blocked */
    if (sigismember(&origUsrSetProcess, SaveStateManager::sigSuspend()) == 1)
        oldmask |= signalMaskBit(SaveStateManager::sigSuspend());
    if (sigismember(&origUsrSetProcess, SaveStateManager::sigCheckpoint()) == 1)
        oldmask |= signalMaskBit(SaveStateManager::sigCheckpoint());

    return oldmask;
}

/* Override */ int sigprocmask (int how, const sigset_t *set, sigset_t *oset) __THROW
{
    LOGTRACE_SIMPLE(LCF_SIGNAL);
    LINK_NAMESPACE_GLOBAL(sigprocmask);

    if (GlobalState::isNative())
        return orig::sigprocmask(how, set, oset);

    sigset_t newset;
    if (set) {
        newset = *set;
        sigdelset(&newset, SaveStateManager::sigSuspend());
        sigdelset(&newset, SaveStateManager::sigCheckpoint());
    }

    int ret = orig::sigprocmask(how, set?&newset:set, oset);

    if (ret != -1) {
        if (oset) {
            if (sigismember(&origUsrSetProcess, SaveStateManager::sigSuspend()) == 1)
                sigaddset(oset, SaveStateManager::sigSuspend());
            if (sigismember(&origUsrSetProcess, SaveStateManager::sigCheckpoint()) == 1)
                sigaddset(oset, SaveStateManager::sigCheckpoint());
        }

        if (set) {
            if (how == SIG_BLOCK) {
                if (sigismember(set, SaveStateManager::sigSuspend()) == 1)
                    sigaddset(&origUsrSetProcess, SaveStateManager::sigSuspend());
                if (sigismember(set, SaveStateManager::sigCheckpoint()) == 1)
                    sigaddset(&origUsrSetProcess, SaveStateManager::sigCheckpoint());
            }
            if (how == SIG_UNBLOCK) {
                if (sigismember(set, SaveStateManager::sigSuspend()) == 1)
                    sigdelset(&origUsrSetProcess, SaveStateManager::sigSuspend());
                if (sigismember(set, SaveStateManager::sigCheckpoint()) == 1)
                    sigdelset(&origUsrSetProcess, SaveStateManager::sigCheckpoint());
            }
            if (how == SIG_SETMASK) {
                sigemptyset(&origUsrSetProcess);
                if (sigismember(set, SaveStateManager::sigSuspend()) == 1)
                    sigaddset(&origUsrSetProcess, SaveStateManager::sigSuspend());
                if (sigismember(set, SaveStateManager::sigCheckpoint()) == 1)
                    sigaddset(&origUsrSetProcess, SaveStateManager::sigCheckpoint());
            }
        }
    }
    return ret;
}

/* Override */ int sigsuspend (const sigset_t *set)
{
    LOGTRACE_SIMPLE(LCF_SIGNAL | LCF_TODO);
    LINK_NAMESPACE_GLOBAL(sigsuspend);

    sigset_t tmp;
    if (set) {
        tmp = *set;
        sigdelset(&tmp, SaveStateManager::sigSuspend());
        sigdelset(&tmp, SaveStateManager::sigCheckpoint());
        set = &tmp;
    }

    return orig::sigsuspend(set);
}

/* Override */ int sigaction (int sig, const struct sigaction *act,
    struct sigaction *oact) __THROW
{
    LINK_NAMESPACE_GLOBAL(sigaction);

    if (GlobalState::isNative()) {
        return orig::sigaction(sig, act, oact);
    }

    LOGTRACE_SIMPLE(LCF_SIGNAL);

    /* Our checkpoint code uses signals, so we must prevent the game from
     * signaling threads at the same time.
     */
    WrapperLock wrapperLock;

    /* Save the original handlers for signals that we will skip */
    if (sig == SaveStateManager::sigSuspend()) {
        LOG(LL_DEBUG, LCF_SIGNAL, "    Skipping because libTAS uses that signal for suspend");
        
        if (oact != nullptr) {
            if (savedSuspendActionValid)
                *oact = savedSuspendAction;
            else {
                oact->sa_handler = SIG_DFL;
                oact->sa_flags = 0;
                sigemptyset(&oact->sa_mask);
            }
        }

        if (act != nullptr) {
            savedSuspendAction = *act;
            savedSuspendActionValid = true;
        }

        return 0;
    }

    if (sig == SaveStateManager::sigCheckpoint()) {
        LOG(LL_DEBUG, LCF_SIGNAL, "    Skipping because libTAS uses that signal for checkpoint");
        
        if (oact != nullptr) {
            if (savedCheckpointActionValid)
                *oact = savedCheckpointAction;
            else {
                oact->sa_handler = SIG_DFL;
                oact->sa_flags = 0;
                sigemptyset(&oact->sa_mask);
            }
        }

        if (act != nullptr) {
            savedCheckpointAction = *act;
            savedCheckpointActionValid = true;
        }

        return 0;
    }
    
    if (act != nullptr) {
        LOG(LL_DEBUG, LCF_SIGNAL, "    Setting handler %p for signal %d (%s)", (act->sa_flags & SA_SIGINFO)?
                reinterpret_cast<void*>(act->sa_sigaction):
                reinterpret_cast<void*>(act->sa_handler), sig, strsignal(sig));
    }

    /* CRASHDBG: put our crash logger in front of the game's crash handlers */
    if (crashlog_is_crash_signal(sig) && crashlog_on()) {
        struct sigaction prev = crashGameAction[sig];
        bool prevValid = crashGameActionValid[sig];
        int ret = 0;
        if (act != nullptr) {
            crashGameAction[sig] = *act;
            crashGameActionValid[sig] = true;
            struct sigaction ours = *act;
            if (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
                ours.sa_sigaction = crashlog_handler;
                ours.sa_flags |= SA_SIGINFO;
            }
            ret = orig::sigaction(sig, &ours, nullptr);
            crashlog_event("sigaction %d handler %p flags %x", sig, (act->sa_flags & SA_SIGINFO)?
                reinterpret_cast<void*>(act->sa_sigaction): reinterpret_cast<void*>(act->sa_handler), act->sa_flags);
        }
        if (oact != nullptr) {
            if (prevValid) *oact = prev;
            else orig::sigaction(sig, nullptr, oact);
        }
        return ret;
    }

    int ret = orig::sigaction(sig, act, oact);

    if (oact != nullptr) {
        LOG(LL_DEBUG, LCF_SIGNAL, "    Getting handler %p for signal %d (%s)", (oact->sa_flags & SA_SIGINFO)?
            reinterpret_cast<void*>(oact->sa_sigaction):
            reinterpret_cast<void*>(oact->sa_handler), sig, strsignal(sig));
    }

    return ret;
}

/* Override */ int sigpending (sigset_t *set) __THROW
{
    LOGTRACE_SIMPLE(LCF_SIGNAL | LCF_TODO);
    RETURN_NATIVE(sigpending, (set), nullptr);
}

/* Override */ int sigwait (const sigset_t *set, int *sig)
{
    LOGTRACE_SIMPLE(LCF_SIGNAL | LCF_TODO);
    RETURN_NATIVE(sigwait, (set, sig), nullptr);
}

/* Override */ int sigwaitinfo (const sigset_t *set, siginfo_t *info)
{
    LOGTRACE_SIMPLE(LCF_SIGNAL | LCF_TODO);
    RETURN_NATIVE(sigwaitinfo, (set, info), nullptr);
}

/* Override */ int sigtimedwait (const sigset_t *set,
    siginfo_t *info, const struct timespec *timeout)
{
    LOGTRACE_SIMPLE(LCF_SIGNAL | LCF_TODO);
    RETURN_NATIVE(sigtimedwait, (set, info, timeout), nullptr);
}

/* Override */ int sigaltstack (const stack_t *ss, stack_t *oss) __THROW
{
    LINK_NAMESPACE_GLOBAL(sigaltstack);
    if (GlobalState::isNative())
        return orig::sigaltstack(ss, oss);

    LOGTRACE_SIMPLE(LCF_SIGNAL);

    if (ss) {
        LOG(LL_DEBUG, LCF_SIGNAL, "    Setting altstack with base address %p and size %d", ss->ss_sp, ss->ss_size);
    }
    
    int ret = orig::sigaltstack(ss, oss);

    if (oss) {
        LOG(LL_DEBUG, LCF_SIGNAL, "    Getting altstack with base address %p and size %d", oss->ss_sp, oss->ss_size);
    }

    return ret;
}

/* Override */ int pthread_sigmask (int how, const sigset_t *newmask,
    sigset_t *oldmask) __THROW
{
    LOGTRACE_SIMPLE(LCF_SIGNAL | LCF_THREAD);
    LINK_NAMESPACE_GLOBAL(pthread_sigmask);

    /* This is a bit of a workaround. We still want native threads
     * (like pulseaudio thread) to be able to be suspended, but we also want
     * threads to unblock SIGUSR1 and SIGUSR2, so we only allow native threads
     * to unblock.
     */
    if (GlobalState::isNative() && (how == SIG_UNBLOCK))
        return orig::pthread_sigmask(how, newmask, oldmask);

    if (newmask) {
        if (how == SIG_BLOCK)
            LOG(LL_DEBUG, LCF_SIGNAL | LCF_THREAD, "    Blocking signals:");
        if (how == SIG_UNBLOCK)
            LOG(LL_DEBUG, LCF_SIGNAL | LCF_THREAD, "    Unblocking signals:");
        if (how == SIG_SETMASK)
            LOG(LL_DEBUG, LCF_SIGNAL | LCF_THREAD, "    Setting signals to block:");
        // for (int s=1; s<NSIG; s++) {
        //     if (sigismember(newmask, s) == 1)
                /* I encountered a deadlock here when using strsignal() to print
                 * the signal name with the following pattern:
                 * malloc() -> acquires lock -> signal handler called ->
                 * pthread_sigmask() -> strsignal() -> malloc() ->
                 * acquires lock -> deadlock
                 *
                 * So I don't use any function that is making memory allocation.
                 */
                // LOG(LL_DEBUG, LCF_SIGNAL | LCF_THREAD, "        %d", s);
        // }
    }
    else if (oldmask) {
        LOG(LL_DEBUG, LCF_SIGNAL | LCF_THREAD, "    Getting blocked signals");
    }

    sigset_t tmpmask;
    if (newmask) {
        tmpmask = *newmask;
        sigdelset(&tmpmask, SaveStateManager::sigSuspend());
        sigdelset(&tmpmask, SaveStateManager::sigCheckpoint());
    }

    int ret = orig::pthread_sigmask(how, (newmask==nullptr)?nullptr:&tmpmask, oldmask);

    if (ret != -1) {
        if (oldmask) {
#if defined(__APPLE__) && defined(__MACH__)
            if (!Global::is_inited) {
                if (sigismember(&origUsrSetProcess, SaveStateManager::sigSuspend()) == 1)
                    sigaddset(oldmask, SaveStateManager::sigSuspend());
                if (sigismember(&origUsrSetProcess, SaveStateManager::sigCheckpoint()) == 1)
                    sigaddset(oldmask, SaveStateManager::sigCheckpoint());
            } else {
#endif
            if (sigismember(&origUsrSetThread, SaveStateManager::sigSuspend()) == 1)
                sigaddset(oldmask, SaveStateManager::sigSuspend());
            if (sigismember(&origUsrSetThread, SaveStateManager::sigCheckpoint()) == 1)
                sigaddset(oldmask, SaveStateManager::sigCheckpoint());
#if defined(__APPLE__) && defined(__MACH__)
            }
#endif
        }

        if (newmask) {
#if defined(__APPLE__) && defined(__MACH__)
            if (!Global::is_inited) {
                if (how == SIG_BLOCK) {
                    if (sigismember(newmask, SaveStateManager::sigSuspend()) == 1)
                        sigaddset(&origUsrSetProcess, SaveStateManager::sigSuspend());
                    if (sigismember(newmask, SaveStateManager::sigCheckpoint()) == 1)
                        sigaddset(&origUsrSetProcess, SaveStateManager::sigCheckpoint());
                }
                if (how == SIG_UNBLOCK) {
                    if (sigismember(newmask, SaveStateManager::sigSuspend()) == 1)
                        sigdelset(&origUsrSetProcess, SaveStateManager::sigSuspend());
                    if (sigismember(newmask, SaveStateManager::sigCheckpoint()) == 1)
                        sigdelset(&origUsrSetProcess, SaveStateManager::sigCheckpoint());
                }
                if (how == SIG_SETMASK) {
                    sigemptyset(&origUsrSetProcess);
                    if (sigismember(newmask, SaveStateManager::sigSuspend()) == 1)
                        sigaddset(&origUsrSetProcess, SaveStateManager::sigSuspend());
                    if (sigismember(newmask, SaveStateManager::sigCheckpoint()) == 1)
                        sigaddset(&origUsrSetProcess, SaveStateManager::sigCheckpoint());
                }
            } else {
#endif
            if (how == SIG_BLOCK) {
                if (sigismember(newmask, SaveStateManager::sigSuspend()) == 1)
                    sigaddset(&origUsrSetThread, SaveStateManager::sigSuspend());
                if (sigismember(newmask, SaveStateManager::sigCheckpoint()) == 1)
                    sigaddset(&origUsrSetThread, SaveStateManager::sigCheckpoint());
            }
            if (how == SIG_UNBLOCK) {
                if (sigismember(newmask, SaveStateManager::sigSuspend()) == 1)
                    sigdelset(&origUsrSetThread, SaveStateManager::sigSuspend());
                if (sigismember(newmask, SaveStateManager::sigCheckpoint()) == 1)
                    sigdelset(&origUsrSetThread, SaveStateManager::sigCheckpoint());
            }
            if (how == SIG_SETMASK) {
                sigemptyset(&origUsrSetThread);
                if (sigismember(newmask, SaveStateManager::sigSuspend()) == 1)
                    sigaddset(&origUsrSetThread, SaveStateManager::sigSuspend());
                if (sigismember(newmask, SaveStateManager::sigCheckpoint()) == 1)
                    sigaddset(&origUsrSetThread, SaveStateManager::sigCheckpoint());
            }
#if defined(__APPLE__) && defined(__MACH__)
            }
#endif
        }
    }

    return ret;
}

/* Override */ int pthread_kill (pthread_t threadid, int signo) __THROW
{
    LINK_NAMESPACE_GLOBAL(pthread_kill);

    if (GlobalState::isNative())
        return orig::pthread_kill(threadid, signo);

    LOGTRACE(LCF_SIGNAL | LCF_THREAD, "%s called with thread %p and signo %d", __func__, threadid, signo);

    /* Our checkpoint code uses signals, so we must prevent the game from
     * signaling threads at the same time.
     */
    WrapperLock wrapperLock;

    int ret = orig::pthread_kill(threadid, signo);

    return ret;
}

/* Override */ int pthread_sigqueue (pthread_t threadid, int signo,
                 const union sigval value) __THROW
{
    LOGTRACE_SIMPLE(LCF_SIGNAL | LCF_THREAD);
    RETURN_NATIVE(pthread_sigqueue, (threadid, signo, value), nullptr);
}

}
