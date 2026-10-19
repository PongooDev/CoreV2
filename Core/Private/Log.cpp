#include "pch.h"
#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
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
    _wfopen_s(&LogFile, std::format(L"{}{}_log_{}.txt", ExecutableDirectory(), Name, GetCurrentProcessId()).c_str(), L"w, ccs=UTF-8");
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
