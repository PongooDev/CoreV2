#include "pch.h"
#include "Logging/LogCategory.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"

#include <cstdarg>
#include <cstdio>

const FLogCategoryBase* FLogCategoryBinding::Get() const
{
    if (Category)
    {
        return Category;
    }

    if (!bSearchedGame)
    {
        bSearchedGame = true;
        Category = reinterpret_cast<FLogCategoryBase*>(Finder::LogCategory(CategoryName));
        if (Category)
        {
            return Category;
        }
    }

    if (!Natives::FLogCategoryBase_FLogCategoryBase || !Natives::GIsGameThreadIdInitialized || !*Natives::GIsGameThreadIdInitialized)
    {
        return nullptr;
    }

    FLogCategoryBase* Constructed = reinterpret_cast<FLogCategoryBase*>(Storage);
    Natives::FLogCategoryBase_FLogCategoryBase(Constructed, CategoryName, DefaultVerbosity, CompileTimeVerbosity);
    Category = Constructed;
    return Category;
}

void FLogCategoryBinding::WriteToCoreLog(ELogVerbosity::Type Verbosity, const wchar_t* Format, ...) const
{
    if ((Verbosity & ELogVerbosity::VerbosityMask) > DefaultVerbosity)
    {
        return;
    }

    wchar_t Message[2048] = {};
    va_list Args;
    va_start(Args, Format);
    _vsnwprintf_s(Message, _TRUNCATE, Format, Args);
    va_end(Args);

    const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
    const ELogLevel CoreLevel = Level <= ELogVerbosity::Error ? ELogLevel::Error : Level == ELogVerbosity::Warning ? ELogLevel::Warning : ELogLevel::Info;
    Log::Write(CoreLevel, std::format(L"{}: {}", CategoryName, Message));
}
