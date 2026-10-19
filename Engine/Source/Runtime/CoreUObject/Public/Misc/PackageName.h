#pragma once

#include "Containers/UnrealString.h"

class FPackageName
{
public:
    static bool FindPackagesInDirectory(TArray<FString>& OutPackages, const FString& RootDir);
    static FString GetMapPackageExtension();
};
