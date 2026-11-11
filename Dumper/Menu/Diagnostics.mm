//
//  Diagnostics.mm
//  Dumper
//

#include "Diagnostics.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <exception>
#include <typeinfo>

#import <Foundation/Foundation.h>

namespace
{
    int g_Fd = -1;
    char g_Path[1024] = { 0 };

    volatile int g_Installed = 0;
    volatile int g_HasFatal = 0;

    uintptr_t g_DumperBase = 0;
    uintptr_t g_MainBase = 0;
    int g_Pid = 0;

    char g_Stage[256] = "startup";
    char g_Fatal[512] = { 0 };

    pthread_mutex_t g_Lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t g_InstallLock = PTHREAD_MUTEX_INITIALIZER;

    uint8_t g_AltStack[64 * 1024] = { 0 };

    /* --------------------------------------------------------------------- */
    /* Async-signal-safe writers. Nothing here allocates or uses stdio.       */
    /* --------------------------------------------------------------------- */

    void RawWrite(const char* Data, size_t Length)
    {
        if (g_Fd < 0 || !Data)
            return;

        while (Length > 0)
        {
            const ssize_t Written = write(g_Fd, Data, Length);
            if (Written <= 0)
                return;

            Data += Written;
            Length -= static_cast<size_t>(Written);
        }
    }

    void RawStr(const char* Str)
    {
        if (Str && *Str)
            RawWrite(Str, strlen(Str));
    }

    void RawHex(uintptr_t Value)
    {
        char Buffer[2 + sizeof(uintptr_t) * 2 + 1];
        size_t Position = 0;

        Buffer[Position++] = '0';
        Buffer[Position++] = 'x';

        bool bStarted = false;
        for (int Shift = static_cast<int>(sizeof(uintptr_t) * 8) - 4; Shift >= 0; Shift -= 4)
        {
            const unsigned Nibble = static_cast<unsigned>((Value >> Shift) & 0xF);
            if (Nibble == 0 && !bStarted && Shift != 0)
                continue;

            bStarted = true;
            Buffer[Position++] = static_cast<char>(Nibble < 10 ? ('0' + Nibble) : ('a' + (Nibble - 10)));
        }

        RawWrite(Buffer, Position);
    }

    void RawDec(long long Value)
    {
        char Buffer[24];
        size_t Position = sizeof(Buffer);
        Buffer[--Position] = '\0';

        const bool bNegative = Value < 0;
        unsigned long long Absolute = bNegative ? static_cast<unsigned long long>(-Value) : static_cast<unsigned long long>(Value);

        if (Absolute == 0)
            Buffer[--Position] = '0';

        while (Absolute > 0)
        {
            Buffer[--Position] = static_cast<char>('0' + (Absolute % 10));
            Absolute /= 10;
        }

        if (bNegative)
            Buffer[--Position] = '-';

        RawWrite(Buffer + Position, sizeof(Buffer) - Position - 1);
    }

    /* --------------------------------------------------------------------- */
    /* Log file                                                              */
    /* --------------------------------------------------------------------- */

    bool OpenLogAt(const char* Path, bool bTruncate)
    {
        if (!Path || !*Path)
            return false;

        int Flags = O_WRONLY | O_CREAT | O_APPEND;
        if (bTruncate)
            Flags = O_WRONLY | O_CREAT | O_TRUNC;

        const int Fd = open(Path, Flags, 0644);
        if (Fd < 0)
            return false;

        g_Fd = Fd;
        strncpy(g_Path, Path, sizeof(g_Path) - 1);
        g_Path[sizeof(g_Path) - 1] = '\0';
        return true;
    }

    void PickLogPath()
    {
        @autoreleasepool
        {
            NSArray<NSString*>* Directories = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
            for (NSString* Directory in Directories)
            {
                NSString* Candidate = [Directory stringByAppendingPathComponent:@"dumper_diag.log"];
                if (OpenLogAt([Candidate fileSystemRepresentation], true))
                    return;
            }
        }

        char Buffer[1024];
        const char* Home = getenv("HOME");
        if (Home && *Home)
        {
            static const char* const HomeSuffixes[] = { "/Documents/dumper_diag.log", "/dumper_diag.log" };
            for (const char* Suffix : HomeSuffixes)
            {
                snprintf(Buffer, sizeof(Buffer), "%s%s", Home, Suffix);
                if (OpenLogAt(Buffer, true))
                    return;
            }
        }

        static const char* const Fallbacks[] = {
            "/var/mobile/Documents/dumper_diag.log",
            "/var/mobile/Media/dumper_diag.log",
            "/tmp/dumper_diag.log",
            "/var/tmp/dumper_diag.log",
        };
        for (const char* Fallback : Fallbacks)
        {
            if (OpenLogAt(Fallback, true))
                return;
        }
    }

    void WriteTimePrefix()
    {
        struct timeval Now;
        if (gettimeofday(&Now, nullptr) != 0)
            return;

        struct tm Local;
        const time_t Seconds = Now.tv_sec;
        localtime_r(&Seconds, &Local);

        char Buffer[40];
        const int Length = snprintf(Buffer, sizeof(Buffer), "[%02d:%02d:%02d.%03d] ",
                                    Local.tm_hour, Local.tm_min, Local.tm_sec,
                                    static_cast<int>(Now.tv_usec / 1000));
        if (Length > 0)
            RawWrite(Buffer, static_cast<size_t>(Length));
    }

    /* --------------------------------------------------------------------- */
    /* Crash reporting                                                       */
    /* --------------------------------------------------------------------- */

    const char* SignalName(int Signal)
    {
        switch (Signal)
        {
            case SIGSEGV: return "SIGSEGV";
            case SIGBUS:  return "SIGBUS";
            case SIGILL:  return "SIGILL";
            case SIGFPE:  return "SIGFPE";
            case SIGABRT: return "SIGABRT";
            case SIGTRAP: return "SIGTRAP";
            case SIGSYS:  return "SIGSYS";
            default:      return "SIGNAL";
        }
    }

    /* Nearest loaded image at or below Pc, used to print "image+offset". */
    void RawImageOffset(uintptr_t Pc)
    {
        uint32_t Count = _dyld_image_count();
        if (Count > 4096)
            Count = 4096;

        uintptr_t BestBase = 0;
        const char* BestName = nullptr;

        for (uint32_t i = 0; i < Count; i++)
        {
            const uintptr_t Base = reinterpret_cast<uintptr_t>(_dyld_get_image_header(i));
            if (Base == 0 || Base > Pc)
                continue;

            if (Base >= BestBase)
            {
                BestBase = Base;
                BestName = _dyld_get_image_name(i);
            }
        }

        if (BestBase == 0 || Pc < BestBase)
            return;

        const char* Slash = BestName ? strrchr(BestName, '/') : nullptr;
        RawStr(" ");
        RawStr(Slash ? Slash + 1 : (BestName ? BestName : "?"));
        RawStr("+");
        RawHex(Pc - BestBase);
    }

    void RawBacktrace()
    {
        void* Frames[64];
        const int FrameCount = backtrace(Frames, 64);

        for (int i = 0; i < FrameCount; i++)
        {
            const uintptr_t Pc = reinterpret_cast<uintptr_t>(Frames[i]);
            RawStr("  #");
            RawDec(i);
            RawStr(" ");
            RawHex(Pc);
            RawImageOffset(Pc);
            RawStr("\n");
        }

        if (FrameCount <= 0)
            RawStr("  <backtrace unavailable>\n");
    }

    void DiagSignalHandler(int Signal, siginfo_t* Info, void* /*Context*/)
    {
        /* No locks, no allocation: a thread may have died holding g_Lock. */
        RawStr("\n===== FATAL SIGNAL =====\n");
        RawStr("signal   : ");
        RawDec(Signal);
        RawStr(" (");
        RawStr(SignalName(Signal));
        RawStr(")\n");

        if (Info)
        {
            RawStr("code     : ");
            RawDec(Info->si_code);
            RawStr("\nfault addr: ");
            RawHex(reinterpret_cast<uintptr_t>(Info->si_addr));
            RawStr("\n");
        }

        RawStr("pid      : ");
        RawDec(g_Pid);
        RawStr("\nthread   : ");
        /* pthread_mach_thread_np() returns mach_port_t, an unsigned 32-bit
         * integer - reinterpret_cast between integer types is ill-formed. */
        RawHex(static_cast<uintptr_t>(pthread_mach_thread_np(pthread_self())));
        RawStr("\nstage    : ");
        RawStr(g_Stage);
        RawStr("\nfatal    : ");
        RawStr(g_HasFatal ? g_Fatal : "no");
        RawStr("\nbacktrace:\n");
        RawBacktrace();
        RawStr("dumper   : ");
        RawHex(g_DumperBase);
        RawStr("\nmain     : ");
        RawHex(g_MainBase);
        RawStr("\n========================\n");

        /* Let the default action run so the OS still writes its own crash log. */
        signal(Signal, SIG_DFL);
        raise(Signal);
        _exit(128 + Signal);
    }

    void DiagTerminateHandler()
    {
        pthread_mutex_lock(&g_Lock);
        RawStr("\n===== STD::TERMINATE =====\n");
        RawStr("stage    : ");
        RawStr(g_Stage);
        RawStr("\n");

        if (std::exception_ptr Exception = std::current_exception())
        {
            try
            {
                std::rethrow_exception(Exception);
            }
            catch (const std::exception& Ex)
            {
                RawStr("exception: ");
                RawStr(typeid(Ex).name());
                RawStr(": ");
                RawStr(Ex.what());
                RawStr("\n");
            }
            catch (...)
            {
                RawStr("exception: <non-std>\n");
            }
        }
        else
        {
            RawStr("exception: <none recorded>\n");
        }

        RawStr("backtrace:\n");
        RawBacktrace();
        RawStr("dumper   : ");
        RawHex(g_DumperBase);
        RawStr("\n==========================\n");
        pthread_mutex_unlock(&g_Lock);

        signal(SIGABRT, SIG_DFL);
        abort();
    }
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void DiagInstall()
{
    pthread_mutex_lock(&g_InstallLock);

    if (g_Installed)
    {
        pthread_mutex_unlock(&g_InstallLock);
        return;
    }

    g_Pid = static_cast<int>(getpid());

    Dl_info ImageInfo = { };
    if (dladdr(reinterpret_cast<void*>(&DiagInstall), &ImageInfo) && ImageInfo.dli_fbase)
        g_DumperBase = reinterpret_cast<uintptr_t>(ImageInfo.dli_fbase);

    if (_dyld_image_count() > 0)
        g_MainBase = reinterpret_cast<uintptr_t>(_dyld_get_image_header(0));

    PickLogPath();

    RawStr("============================================================\n");
    RawStr("Dumper-7 diagnostics\n");
    RawStr("pid        : ");
    RawDec(g_Pid);
    RawStr("\n");
    RawStr("dumper base: ");
    RawHex(g_DumperBase);
    RawStr("\n");
    RawStr("main base  : ");
    RawHex(g_MainBase);
    RawStr("\n");
    RawStr("log file   : ");
    RawStr(g_Path[0] ? g_Path : "<not writable, logs only in console>");
    RawStr("\n");
    RawStr("============================================================\n");

    /* Dedicated stack so a SIGSEGV caused by a stack overflow can still log. */
    stack_t AltStack;
    memset(&AltStack, 0, sizeof(AltStack));
    AltStack.ss_sp = g_AltStack;
    AltStack.ss_size = sizeof(g_AltStack);
    AltStack.ss_flags = 0;
    sigaltstack(&AltStack, nullptr);

    struct sigaction Action;
    memset(&Action, 0, sizeof(Action));
    Action.sa_sigaction = &DiagSignalHandler;
    Action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&Action.sa_mask);

    static const int Signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS };
    for (int Signal : Signals)
        sigaction(Signal, &Action, nullptr);

    std::set_terminate(&DiagTerminateHandler);

    g_Installed = 1;
    pthread_mutex_unlock(&g_InstallLock);
}

void DiagWriteLine(const char* Text)
{
    if (!Text)
        return;

    pthread_mutex_lock(&g_Lock);
    if (g_Fd >= 0)
    {
        WriteTimePrefix();
        RawStr(Text);

        const size_t Length = strlen(Text);
        if (Length == 0 || Text[Length - 1] != '\n')
            RawStr("\n");
    }
    pthread_mutex_unlock(&g_Lock);
}

void DiagLogV(const char* fmt, va_list Args)
{
    if (!fmt)
        return;

    char Buffer[2048];
    vsnprintf(Buffer, sizeof(Buffer), fmt, Args);
    DiagWriteLine(Buffer);
}

void DiagLog(const char* fmt, ...)
{
    va_list Args;
    va_start(Args, fmt);
    DiagLogV(fmt, Args);
    va_end(Args);
}

void DiagSetStage(const char* fmt, ...)
{
    if (!fmt)
        return;

    char Buffer[256];
    va_list Args;
    va_start(Args, fmt);
    vsnprintf(Buffer, sizeof(Buffer), fmt, Args);
    va_end(Args);

    pthread_mutex_lock(&g_Lock);
    strncpy(g_Stage, Buffer, sizeof(g_Stage) - 1);
    g_Stage[sizeof(g_Stage) - 1] = '\0';
    pthread_mutex_unlock(&g_Lock);

    DiagLog("[STAGE] %s\n", Buffer);
}

void DiagMarkFatal(const char* fmt, ...)
{
    if (!fmt)
        return;

    char Buffer[512];
    va_list Args;
    va_start(Args, fmt);
    vsnprintf(Buffer, sizeof(Buffer), fmt, Args);
    va_end(Args);

    pthread_mutex_lock(&g_Lock);
    g_HasFatal = 1;
    strncpy(g_Fatal, Buffer, sizeof(g_Fatal) - 1);
    g_Fatal[sizeof(g_Fatal) - 1] = '\0';
    pthread_mutex_unlock(&g_Lock);

    DiagLog("[FATAL] %s\n", Buffer);
}

bool DiagHasFatal()
{
    return g_HasFatal != 0;
}

const char* DiagGetLogPath()
{
    return g_Path;
}

/* Installed as soon as the dylib is loaded, so crashes that happen before the
 * user opens the menu are captured too. */
__attribute__((constructor)) static void DiagLoadHook()
{
    DiagInstall();
}
