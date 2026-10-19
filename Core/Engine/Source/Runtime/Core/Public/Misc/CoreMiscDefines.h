#pragma once

#include "Log.h"

#define PURE_VIRTUAL(func, extra) { Log::Fatal(L"Pure virtual not implemented ({})", TEXT(#func)); extra }
