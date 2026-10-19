#include "pch.h"
#include "Misc/PackageName.h"
#include "Natives.h"

bool FPackageName::FindPackagesInDirectory(TArray<FString>& OutPackages, const FString& RootDir)
{
    return Natives::FPackageName_FindPackagesInDirectory(OutPackages, RootDir);
}

FString FPackageName::GetMapPackageExtension()
{
    return *Natives::FPackageName_MapPackageExtension;
}
