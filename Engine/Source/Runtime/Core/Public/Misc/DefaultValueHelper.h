#pragma once

#include "Containers/UnrealString.h"

class FDefaultValueHelper
{
public:
    static bool IsWhitespace(wchar_t Char);
    static bool HasWhitespaces(const FString& Source);
};
