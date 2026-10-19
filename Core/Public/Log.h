#pragma once

#include <format>
#include <string_view>
#include <utility>

enum class ELogLevel : uint8_t
{
    Info,
    Warning,
    Error,
    Fatal,
};

namespace Log
{
    void Initialize(const wchar_t* Name);
    void Write(ELogLevel Level, std::wstring_view Message);

    template <typename... TArgs>
    void Info(std::wformat_string<TArgs...> Format, TArgs&&... Args)
    {
        Write(ELogLevel::Info, std::format(Format, std::forward<TArgs>(Args)...));
    }

    template <typename... TArgs>
    void Warning(std::wformat_string<TArgs...> Format, TArgs&&... Args)
    {
        Write(ELogLevel::Warning, std::format(Format, std::forward<TArgs>(Args)...));
    }

    template <typename... TArgs>
    void Error(std::wformat_string<TArgs...> Format, TArgs&&... Args)
    {
        Write(ELogLevel::Error, std::format(Format, std::forward<TArgs>(Args)...));
    }

    [[noreturn]] void FatalError(std::wstring_view Message);

    template <typename... TArgs>
    [[noreturn]] void Fatal(std::wformat_string<TArgs...> Format, TArgs&&... Args)
    {
        FatalError(std::format(Format, std::forward<TArgs>(Args)...));
    }
}
