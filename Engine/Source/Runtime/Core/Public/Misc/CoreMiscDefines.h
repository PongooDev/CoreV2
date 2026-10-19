#pragma once

#include "Misc/AssertionMacros.h"

enum
{
    INDEX_NONE = -1
};

#define PURE_VIRTUAL(func, extra) { LowLevelFatalError(TEXT("Pure virtual not implemented (%s)"), TEXT(#func)); extra }
