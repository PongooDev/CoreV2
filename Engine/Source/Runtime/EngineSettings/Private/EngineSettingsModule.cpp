#include "pch.h"
#include "GameMapsSettings.h"
#include "Configuration.h"
#include "CoreGlobals.h"
#include "Hooks.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"

const FString UGameMapsSettings::GetGameDefaultMap()
{
    // StartGameInstance parses the default map with its options (FURL), so -CoreMap's URL carries them.
    if (IsRunningDedicatedServer() && !Configuration::Get().Map.empty())
    {
        return FString(Configuration::Get().Map.c_str());
    }
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

void UGameMapsSettings::Hook()
{
    Hooks::Detour(L"UGameMapsSettings::GetGameDefaultMap", reinterpret_cast<void*>(Natives::UGameMapsSettings_GetGameDefaultMap), reinterpret_cast<void*>(&GetGameDefaultMap), nullptr);
    Hooks::Detour(L"UGameMapsSettings::GetGlobalDefaultGameMode", reinterpret_cast<void*>(Natives::UGameMapsSettings_GetGlobalDefaultGameMode), reinterpret_cast<void*>(&GetGlobalDefaultGameMode), nullptr);
}
