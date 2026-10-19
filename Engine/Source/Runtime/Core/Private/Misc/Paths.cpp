#include "pch.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "UObject/NameTypes.h"

FString FPaths::GameDir()
{
    return FString(FPlatformMisc::GameDir());
}

FString FPaths::GameContentDir()
{
    return FPaths::GameDir() + TEXT("Content/");
}

FString FPaths::GameDevelopersDir()
{
    return FPaths::GameContentDir() + TEXT("Developers/");
}

FString FPaths::GameUserDeveloperDir()
{
    static FString UserFolder;

    if (UserFolder.Len() == 0)
    {
        const FString InvalidChars = INVALID_LONGPACKAGE_CHARACTERS;
        UserFolder = FPlatformProcess::UserName();
        for (int32_t CharIdx = 0; CharIdx < InvalidChars.Len(); ++CharIdx)
        {
            for (int32_t Pos = 0; Pos < UserFolder.Len(); ++Pos)
            {
                if (UserFolder[Pos] == InvalidChars[CharIdx])
                {
                    UserFolder[Pos] = L'_';
                }
            }
        }
    }

    return FPaths::GameDevelopersDir() + UserFolder + TEXT("/");
}
