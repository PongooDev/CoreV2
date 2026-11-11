#include "pch.h"
#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <io.h>
#include <mutex>
#include <share.h>
#include <string>

namespace
{
    std::mutex LogMutex;
    FILE* LogFile = nullptr;

    std::wstring ExecutableDirectory()
    {
        wchar_t Path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, Path, MAX_PATH);
        const std::wstring FullPath = Path;
        return FullPath.substr(0, FullPath.find_last_of(L"\\/") + 1);
    }

    const wchar_t* LevelName(ELogLevel Level)
    {
        switch (Level)
        {
        case ELogLevel::Warning:
            return L"Warning";
        case ELogLevel::Error:
            return L"Error";
        case ELogLevel::Fatal:
            return L"Fatal";
        default:
            return L"Info";
        }
    }
}

void Log::Initialize(const wchar_t* Name)
{
    std::scoped_lock Lock(LogMutex);
    LogFile = _wfsopen(std::format(L"{}{}_log_{}.txt", ExecutableDirectory(), Name, GetCurrentProcessId()).c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
}

void Log::KeepFromChildProcesses()
{
    // The CRT's file handles are inheritable unless opened with 'N', and the engine's crash handler starts
    // CrashReportClient.exe with bInheritHandles (FWindowsPlatformProcess::CreateProc, 1.8.1 0x13651F0, from the crash
    // reporter launch at 0x135BD00), so the crash reporter could hold the log open after the server had died: the
    // launcher couldn't move a crashed 1.8.1 server's logs ("being used by another process", CoreV2 #352).
    std::scoped_lock Lock(LogMutex);
    if (LogFile)
    {
        SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(LogFile))), HANDLE_FLAG_INHERIT, 0);
    }
}

void Log::Write(ELogLevel Level, std::wstring_view Message)
{
    SYSTEMTIME Time = {};
    GetLocalTime(&Time);

    const std::wstring Line = std::format(L"[{:02}:{:02}:{:02}.{:03}] [{}] {}\n", Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds, LevelName(Level), Message);

    std::scoped_lock Lock(LogMutex);

    if (LogFile)
    {
        fputws(Line.c_str(), LogFile);
        fflush(LogFile);
    }

    OutputDebugStringW(Line.c_str());
}

void Log::FatalError(std::wstring_view Message)
{
    Write(ELogLevel::Fatal, Message);
    std::abort();
}
