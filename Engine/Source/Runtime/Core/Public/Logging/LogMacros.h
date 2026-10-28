#pragma once

#include "Misc/AssertionMacros.h"
#include "Logging/LogCategory.h"

struct FMsg
{
    template <typename... TArgs>
    static void Logf_Internal(const char* File, int32_t Line, const FName& Category, ELogVerbosity::Type Verbosity, const wchar_t* Format, TArgs... Args)
    {
        Natives::FMsg_Logf_Internal(File, Line, Category, Verbosity, Format, Args...);
    }
};

#define UE_LOG_EXPAND_IS_FATAL(Verbosity, ActiveBlock, InactiveBlock) UE_LOG_EXPAND_IS_FATAL_##Verbosity(ActiveBlock, InactiveBlock)
#define UE_LOG_EXPAND_IS_FATAL_Fatal(ActiveBlock, InactiveBlock) ActiveBlock
#define UE_LOG_EXPAND_IS_FATAL_Error(ActiveBlock, InactiveBlock) InactiveBlock
#define UE_LOG_EXPAND_IS_FATAL_Warning(ActiveBlock, InactiveBlock) InactiveBlock
#define UE_LOG_EXPAND_IS_FATAL_Display(ActiveBlock, InactiveBlock) InactiveBlock
#define UE_LOG_EXPAND_IS_FATAL_Log(ActiveBlock, InactiveBlock) InactiveBlock
#define UE_LOG_EXPAND_IS_FATAL_Verbose(ActiveBlock, InactiveBlock) InactiveBlock
#define UE_LOG_EXPAND_IS_FATAL_VeryVerbose(ActiveBlock, InactiveBlock) InactiveBlock
#define UE_LOG_EXPAND_IS_FATAL_All(ActiveBlock, InactiveBlock) InactiveBlock
#define UE_LOG_EXPAND_IS_FATAL_SetColor(ActiveBlock, InactiveBlock) InactiveBlock

#if NO_LOGGING
    struct FNoLoggingCategory
    {
    };

    #define UE_LOG(CategoryName, Verbosity, Format, ...) \
    { \
        static_assert(IS_TCHAR_ARRAY(Format), "Formatting string must be a TCHAR array."); \
        if (ELogVerbosity::Verbosity == ELogVerbosity::Fatal) \
        { \
            LowLevelFatalErrorHandler(__FILE__, __LINE__, Format, ##__VA_ARGS__); \
            _DebugBreakAndPromptForRemote(); \
            FDebug::AssertFailed("", __FILE__, __LINE__, Format, ##__VA_ARGS__); \
        } \
    }
    #define UE_CLOG(Condition, CategoryName, Verbosity, Format, ...) \
    { \
        static_assert(IS_TCHAR_ARRAY(Format), "Formatting string must be a TCHAR array."); \
        if (ELogVerbosity::Verbosity == ELogVerbosity::Fatal && (Condition)) \
        { \
            LowLevelFatalErrorHandler(__FILE__, __LINE__, Format, ##__VA_ARGS__); \
            _DebugBreakAndPromptForRemote(); \
            FDebug::AssertFailed("", __FILE__, __LINE__, Format, ##__VA_ARGS__); \
        } \
    }
    #define UE_LOG_ACTIVE(...) (0)
    #define UE_GET_LOG_VERBOSITY(...) (ELogVerbosity::NoLogging)
    #define DECLARE_LOG_CATEGORY_EXTERN(CategoryName, DefaultVerbosity, CompileTimeVerbosity) extern FNoLoggingCategory CategoryName
    #define DEFINE_LOG_CATEGORY(...)
    #define DEFINE_LOG_CATEGORY_STATIC(...)
    #define DECLARE_LOG_CATEGORY_CLASS(...)
    #define DEFINE_LOG_CATEGORY_CLASS(...)
#else
    #define UE_LOG_ACTIVE(CategoryName, Verbosity) \
        ((ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) <= ELogVerbosity::COMPILED_IN_MINIMUM_VERBOSITY && \
         (ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) <= FLogCategory##CategoryName::CompileTimeVerbosity && \
         !CategoryName.IsSuppressed(ELogVerbosity::Verbosity))

    #define UE_GET_LOG_VERBOSITY(CategoryName) CategoryName.GetVerbosity()

    #define UE_LOG(CategoryName, Verbosity, Format, ...) \
    { \
        static_assert(IS_TCHAR_ARRAY(Format), "Formatting string must be a TCHAR array."); \
        static_assert((ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) < ELogVerbosity::NumVerbosity && ELogVerbosity::Verbosity > 0, "Verbosity must be constant and in range."); \
        if constexpr ((ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) <= ELogVerbosity::COMPILED_IN_MINIMUM_VERBOSITY && (ELogVerbosity::Warning & ELogVerbosity::VerbosityMask) <= FLogCategory##CategoryName::CompileTimeVerbosity) \
        { \
            if constexpr ((ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) <= ELogVerbosity::Log && (ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) <= FLogCategory##CategoryName::CoreLogVerbosity) \
            { \
                CategoryName.WriteToCoreLog(ELogVerbosity::Verbosity, Format, ##__VA_ARGS__); \
            } \
            UE_LOG_EXPAND_IS_FATAL(Verbosity, , if (!CategoryName.IsSuppressed(ELogVerbosity::Verbosity))) \
            { \
                FMsg::Logf_Internal(__FILE__, __LINE__, CategoryName.GetCategoryName(), ELogVerbosity::Verbosity, Format, ##__VA_ARGS__); \
                UE_LOG_EXPAND_IS_FATAL(Verbosity, \
                    { \
                        _DebugBreakAndPromptForRemote(); \
                        FDebug::AssertFailed("", __FILE__, __LINE__, Format, ##__VA_ARGS__); \
                    }, \
                ) \
            } \
        } \
    }

    #define UE_CLOG(Condition, CategoryName, Verbosity, Format, ...) \
    { \
        static_assert(IS_TCHAR_ARRAY(Format), "Formatting string must be a TCHAR array."); \
        static_assert((ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) < ELogVerbosity::NumVerbosity && ELogVerbosity::Verbosity > 0, "Verbosity must be constant and in range."); \
        if constexpr ((ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) <= ELogVerbosity::COMPILED_IN_MINIMUM_VERBOSITY && (ELogVerbosity::Warning & ELogVerbosity::VerbosityMask) <= FLogCategory##CategoryName::CompileTimeVerbosity) \
        { \
            if constexpr ((ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) <= ELogVerbosity::Log && (ELogVerbosity::Verbosity & ELogVerbosity::VerbosityMask) <= FLogCategory##CategoryName::CoreLogVerbosity) \
            { \
                if (Condition) \
                { \
                    CategoryName.WriteToCoreLog(ELogVerbosity::Verbosity, Format, ##__VA_ARGS__); \
                } \
            } \
            UE_LOG_EXPAND_IS_FATAL(Verbosity, , if (!CategoryName.IsSuppressed(ELogVerbosity::Verbosity))) \
            { \
                if (Condition) \
                { \
                    FMsg::Logf_Internal(__FILE__, __LINE__, CategoryName.GetCategoryName(), ELogVerbosity::Verbosity, Format, ##__VA_ARGS__); \
                    UE_LOG_EXPAND_IS_FATAL(Verbosity, \
                        { \
                            _DebugBreakAndPromptForRemote(); \
                            FDebug::AssertFailed("", __FILE__, __LINE__, Format, ##__VA_ARGS__); \
                        }, \
                    ) \
                } \
            } \
        } \
    }

    #define DECLARE_LOG_CATEGORY_EXTERN(CategoryName, DefaultVerbosity, CompileTimeVerbosity) \
        extern struct FLogCategory##CategoryName : public FLogCategory<ELogVerbosity::DefaultVerbosity, ELogVerbosity::CompileTimeVerbosity> \
        { \
            constexpr FLogCategory##CategoryName() : FLogCategory(TEXT(#CategoryName)) {} \
        } CategoryName;

    #define DEFINE_LOG_CATEGORY(CategoryName) constinit FLogCategory##CategoryName CategoryName;

    #define DEFINE_LOG_CATEGORY_STATIC(CategoryName, DefaultVerbosity, CompileTimeVerbosity) \
        static constinit struct FLogCategory##CategoryName : public FLogCategory<ELogVerbosity::DefaultVerbosity, ELogVerbosity::CompileTimeVerbosity> \
        { \
            constexpr FLogCategory##CategoryName() : FLogCategory(TEXT(#CategoryName)) {} \
        } CategoryName;

    #define DECLARE_LOG_CATEGORY_CLASS(CategoryName, DefaultVerbosity, CompileTimeVerbosity) \
        DEFINE_LOG_CATEGORY_STATIC(CategoryName, DefaultVerbosity, CompileTimeVerbosity)

    #define DEFINE_LOG_CATEGORY_CLASS(Class, CategoryName) Class::FLogCategory##CategoryName Class::CategoryName;
#endif
