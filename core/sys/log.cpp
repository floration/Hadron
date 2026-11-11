#include "core/sys/log.h"
#include "core/sys/sys.h"
#include <cstdio>
#include <dbghelp.h>
#include <shlobj.h>

namespace hx::sys {

static FILE* g_log = nullptr;
static CRITICAL_SECTION g_lock;
static bool g_lockInit = false;

static std::wstring DataDir(const wchar_t* sub) {
    wchar_t local[MAX_PATH];
    std::wstring dir;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, local))) {
        dir = std::wstring(local) + L"\\Hadron\\" + sub;
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    }
    return dir;
}

void OpenLog() {
    if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }
    std::wstring dir = DataDir(L"logs");
    if (dir.empty()) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, 64, L"\\%04d%02d%02d-%02d%02d%02d.log",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_log = _wfopen((dir + name).c_str(), L"wb");
}

void Write(hx::LogLevel level, const std::wstring& msg) {
    if (!g_log) return;
    static const char* kLvl[] = {"DBG", "INF", "WRN", "ERR"};
    EnterCriticalSection(&g_lock);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d %s ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, kLvl[(int)level]);
    int bn = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), (int)msg.size(), nullptr, 0, nullptr, nullptr);
    std::string u8(bn, '\0');
    WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), (int)msg.size(), u8.data(), bn, nullptr, nullptr);
    fwrite(u8.data(), 1, u8.size(), g_log);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}

void CloseLog() {
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    std::wstring dir = DataDir(L"crash");
    if (!dir.empty()) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t name[64];
        swprintf(name, 64, L"\\%04d%02d%02d-%02d%02d%02d.dmp",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        HANDLE f = CreateFileW((dir + name).c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(f);
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashHandler() { SetUnhandledExceptionFilter(OnCrash); }

}  // namespace hx::sys
