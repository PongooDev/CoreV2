#pragma once

#include <cstdint>

#ifndef COMPILED_IN_MINIMUM_VERBOSITY
    #define COMPILED_IN_MINIMUM_VERBOSITY VeryVerbose
#endif

namespace ELogVerbosity
{
    enum Type : uint8_t
    {
        NoLogging = 0,
        Fatal,
        Error,
        Warning,
        Display,
        Log,
        Verbose,
        VeryVerbose,
        All = VeryVerbose,
        NumVerbosity,
        VerbosityMask = 0xf,
        SetColor = 0x40,
        BreakOnLog = 0x80,
    };
}
