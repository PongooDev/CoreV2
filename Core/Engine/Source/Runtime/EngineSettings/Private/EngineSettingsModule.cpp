#include "pch.h"
#include "GameMapsSettings.h"
#include "CoreGlobals.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"

const FString UGameMapsSettings::GetGameDefaultMap()
{
    return IsRunningDedicatedServer()
        ? GetDefault<UGameMapsSettings>()->ServerDefaultMap.GetLongPackageName()
        : GetDefault<UGameMapsSettings>()->GameDefaultMap.GetLongPackageName();
}

const FString& UGameMapsSettings::GetGlobalDefaultGameMode()
{
    UGameMapsSettings* GameMapsSettings = Cast<UGameMapsSettings>(UGameMapsSettings::StaticClass()->GetDefaultObject());

    return IsRunningDedicatedServer() && GameMapsSettings->GlobalDefaultServerGameMode.IsValid()
        ? GameMapsSettings->GlobalDefaultServerGameMode.ToString()
        : GameMapsSettings->GlobalDefaultGameMode.ToString();
}
