#pragma once

#include "Logging/LogVerbosity.h"
#include "UObject/NameTypes.h"

struct FLogCategoryBase
{
    bool IsSuppressed(ELogVerbosity::Type VerbosityLevel) const
    {
        return !((VerbosityLevel & ELogVerbosity::VerbosityMask) <= Verbosity);
    }

    FName GetCategoryName() const
    {
        return CategoryFName;
    }

    ELogVerbosity::Type GetVerbosity() const
    {
        return Verbosity;
    }

protected:
    ELogVerbosity::Type Verbosity;
    bool DebugBreakOnLog;
    uint8_t DefaultVerbosity;
    ELogVerbosity::Type CompileTimeVerbosity;
    FName CategoryFName;
};

static_assert(sizeof(FLogCategoryBase) == 0x10);

class FLogCategoryBinding
{
public:
    constexpr FLogCategoryBinding(const wchar_t* InCategoryName, ELogVerbosity::Type InDefaultVerbosity, ELogVerbosity::Type InCompileTimeVerbosity)
        : CategoryName(InCategoryName), DefaultVerbosity(InDefaultVerbosity), CompileTimeVerbosity(InCompileTimeVerbosity)
    {
    }

    const FLogCategoryBase* Get() const;
    void WriteToCoreLog(ELogVerbosity::Type Verbosity, const wchar_t* Format, ...) const;

private:
    const wchar_t* CategoryName;
    ELogVerbosity::Type DefaultVerbosity;
    ELogVerbosity::Type CompileTimeVerbosity;
    mutable FLogCategoryBase* Category = nullptr;
    mutable bool bSearchedGame = false;
    alignas(FLogCategoryBase) mutable uint8_t Storage[sizeof(FLogCategoryBase)] = {};
};

template <uint8_t InDefaultVerbosity, uint8_t InCompileTimeVerbosity>
struct FLogCategory : public FLogCategoryBinding
{
    static_assert((InDefaultVerbosity & ELogVerbosity::VerbosityMask) < ELogVerbosity::NumVerbosity, "Bogus default verbosity.");
    static_assert(InCompileTimeVerbosity < ELogVerbosity::NumVerbosity, "Bogus compile time verbosity.");

    enum
    {
        CompileTimeVerbosity = InCompileTimeVerbosity,
        // The most verbose line WriteToCoreLog writes, known at compile time so UE_LOG only makes the arguments of a
        // line that is written.
        CoreLogVerbosity = InDefaultVerbosity
    };

    constexpr FLogCategory(const wchar_t* InCategoryName)
        : FLogCategoryBinding(InCategoryName, ELogVerbosity::Type(InDefaultVerbosity), ELogVerbosity::Type(InCompileTimeVerbosity))
    {
    }

    bool IsSuppressed(ELogVerbosity::Type VerbosityLevel) const
    {
        const FLogCategoryBase* Category = Get();
        return !Category || Category->IsSuppressed(VerbosityLevel);
    }

    FName GetCategoryName() const
    {
        return Get()->GetCategoryName();
    }

    ELogVerbosity::Type GetVerbosity() const
    {
        const FLogCategoryBase* Category = Get();
        return Category ? Category->GetVerbosity() : ELogVerbosity::NoLogging;
    }
};
