#pragma once

#include "Containers/UnrealString.h"

class FPaths
{
public:
    static FString GameDir();
    static FString GameContentDir();
    static FString GameDevelopersDir();
    static FString GameUserDeveloperDir();
};
