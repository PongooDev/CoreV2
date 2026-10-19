#pragma once

#include "Containers/UnrealString.h"

class FTextLocalizationManager
{
public:
    static FTextLocalizationManager& Get();

    bool FindDisplayString(const FString& Namespace, const FString& Key, FString& OutDisplayString, const FString* SourceString = nullptr) const;
};
