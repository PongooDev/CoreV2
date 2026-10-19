#pragma once

#define CA_IN
#define CA_OUT
#define CA_READ_ONLY
#define CA_WRITE_ONLY
#define CA_VALID_POINTER
#define CA_CHECK_RETVAL
#define CA_NO_RETURN
#define CA_SUPPRESS(WarningNumber)
#define CA_ASSUME(Expr)
#define CA_CONSTANT_IF(Condition) if (Condition)

#include "Misc/AssertionMacros.h"

enum
{
    INDEX_NONE = -1
};

#define PURE_VIRTUAL(func, extra) { LowLevelFatalError(TEXT("Pure virtual not implemented (%s)"), TEXT(#func)); extra }
