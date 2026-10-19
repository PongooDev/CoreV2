#pragma once

#include "Misc/AssertionMacros.h"

#define PURE_VIRTUAL(func, extra) { LowLevelFatalError(TEXT("Pure virtual not implemented (%s)"), TEXT(#func)); extra }
