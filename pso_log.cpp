#include "pso_log.hpp"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// File I/O goes through raw Win32 (CreateFileA/WriteFile/FlushFileBuffers/
// CloseHandle) rather than the CRT (fopen/fprintf/fflush/fclose). The
// earlier MinGW build used the CRT and that was fine for GCC's stdio,
// but switching to MSVC made the very first CRT file call inside
// DllMain-ATTACH silently kill the process — most likely because
// MSVC's file-descriptor table is not yet ready at the point the
// loader calls into us. Raw Win32 file I/O has no such dependency and
// works in every DllMain phase. Formatting still goes through
// vsnprintf, which is CRT but buffer-only (no file state).

namespace pso_log {

static HANDLE            s_log          = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION  s_lock;
static bool              s_lock_init    = false;
static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter = nullptr;
static bool              s_filter_installed = false;

static void WriteRaw(const char *buf, size_t len)
{
    if (s_log == INVALID_HANDLE_VALUE || len == 0) return;
    DWORD written = 0;
    WriteFile(s_log, buf, static_cast<DWORD>(len), &written, nullptr);
}

void Init(const char *addon_dir)
{
    if (s_log != INVALID_HANDLE_VALUE) return;

    if (!s_lock_init)
    {
        InitializeCriticalSection(&s_lock);
        s_lock_init = true;
    }

    char path[MAX_PATH];
    if (addon_dir != nullptr && addon_dir[0] != '\0')
        std::snprintf(path, sizeof(path), "%spixelated_mods.log", addon_dir);
    else
        std::snprintf(path, sizeof(path), "pixelated_mods.log");

    // FILE_APPEND_DATA + OPEN_ALWAYS => create-or-open in append mode.
    // FILE_SHARE_READ lets external tools (tail, editors) view the log
    // while the process holds it open.
    s_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s_log == INVALID_HANDLE_VALUE) return;
    SetFilePointer(s_log, 0, nullptr, FILE_END);

    // Session banner so crash reports are easy to scope to one run.
    SYSTEMTIME t;
    GetLocalTime(&t);
    char banner[128];
    const int banner_len = std::snprintf(banner, sizeof(banner),
        "\r\n========== pixelated_mods session %04d-%02d-%02d %02d:%02d:%02d ==========\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (banner_len > 0)
        WriteRaw(banner, static_cast<size_t>(banner_len));
    FlushFileBuffers(s_log);
}

void Shutdown()
{
    if (s_log != INVALID_HANDLE_VALUE)
    {
        Write("pso_log::Shutdown");
        FlushFileBuffers(s_log);
        CloseHandle(s_log);
        s_log = INVALID_HANDLE_VALUE;
    }
    if (s_lock_init)
    {
        DeleteCriticalSection(&s_lock);
        s_lock_init = false;
    }
}

void Write(const char *fmt, ...)
{
    if (s_log == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME t;
    GetLocalTime(&t);

    // Single fixed buffer — PSO_LOG messages are always short. 1 KB
    // accommodates a 40-char timestamp prefix plus a large message.
    char line[1024];
    int n = std::snprintf(line, sizeof(line),
                          "[%02d:%02d:%02d.%03d] ",
                          t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (n < 0 || n >= static_cast<int>(sizeof(line))) n = 0;

    va_list ap;
    va_start(ap, fmt);
    const int m = std::vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    if (m > 0)
    {
        int total = n + m;
        if (total >= static_cast<int>(sizeof(line)) - 2)
            total = static_cast<int>(sizeof(line)) - 2;
        line[total++] = '\r';
        line[total++] = '\n';

        EnterCriticalSection(&s_lock);
        WriteRaw(line, static_cast<size_t>(total));
        FlushFileBuffers(s_log);
        LeaveCriticalSection(&s_lock);
    }
}

void Flush()
{
    if (s_log == INVALID_HANDLE_VALUE) return;
    if (s_lock_init) EnterCriticalSection(&s_lock);
    FlushFileBuffers(s_log);
    if (s_lock_init) LeaveCriticalSection(&s_lock);
}

// Try to identify which loaded module (EXE or DLL) contains the given
// address. Writes the module's base filename into out_name, or "<unknown>"
// if the address isn't inside any mapped module.
static void DescribeAddress(void *addr, char *out_name, size_t out_cap,
                            uintptr_t *out_rva)
{
    HMODULE hmod = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(addr), &hmod) && hmod != nullptr)
    {
        char full[MAX_PATH] = {};
        GetModuleFileNameA(hmod, full, MAX_PATH);
        const char *base = std::strrchr(full, '\\');
        base = base ? base + 1 : full;
        std::snprintf(out_name, out_cap, "%s", base);
        *out_rva = reinterpret_cast<uintptr_t>(addr) -
                   reinterpret_cast<uintptr_t>(hmod);
    }
    else
    {
        std::snprintf(out_name, out_cap, "<unknown>");
        *out_rva = 0;
    }
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    if (ep == nullptr || ep->ExceptionRecord == nullptr)
    {
        Write("*** CRASH (null EXCEPTION_POINTERS) ***");
        Flush();
        return s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    char mod_name[MAX_PATH];
    uintptr_t rva = 0;
    DescribeAddress(er->ExceptionAddress, mod_name, sizeof(mod_name), &rva);

    Write("*** UNHANDLED EXCEPTION ***");
    Write("  code     = 0x%08lX", er->ExceptionCode);
    Write("  address  = 0x%p  (%s + 0x%IX)",
          er->ExceptionAddress, mod_name, rva);
    Write("  flags    = 0x%08lX", er->ExceptionFlags);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2)
    {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        const ULONG_PTR va   = er->ExceptionInformation[1];
        const char *op =
            kind == 0 ? "read" :
            kind == 1 ? "write" :
            kind == 8 ? "DEP execute" : "unknown";
        Write("  access violation: %s at 0x%p", op, (void*)va);
    }

    for (DWORD i = 0; i < er->NumberParameters && i < 8; ++i)
    {
        Write("  param[%lu] = 0x%p",
              i, (void*)er->ExceptionInformation[i]);
    }

#if defined(_M_IX86) || defined(__i386__)
    if (ep->ContextRecord != nullptr)
    {
        const CONTEXT *ctx = ep->ContextRecord;
        Write("  eip=0x%08lX  esp=0x%08lX  ebp=0x%08lX",
              ctx->Eip, ctx->Esp, ctx->Ebp);
        Write("  eax=0x%08lX  ebx=0x%08lX  ecx=0x%08lX  edx=0x%08lX",
              ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
        Write("  esi=0x%08lX  edi=0x%08lX",
              ctx->Esi, ctx->Edi);
    }
#endif

    Flush();
    return s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler()
{
    if (s_filter_installed) return;
    s_prev_filter = SetUnhandledExceptionFilter(CrashFilter);
    s_filter_installed = true;
}

} // namespace pso_log
