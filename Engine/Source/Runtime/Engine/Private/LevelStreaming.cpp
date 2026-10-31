#include "pch.h"
#include "Engine/LevelStreaming.h"

FName ULevelStreaming::GetWorldAssetPackageFName() const
{
    const FString WorldAssetPath = WorldAsset.ToString();
    FString PackageName;
    if (!WorldAssetPath.Split(L".", &PackageName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromStart))
    {
        PackageName = WorldAssetPath;
    }
    return FName(*PackageName, FNAME_Find);
}
