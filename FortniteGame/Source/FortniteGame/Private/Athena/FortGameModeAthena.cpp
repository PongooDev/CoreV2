#include "pch.h"
#include "Athena/FortGameModeAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortZoneTheme.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

FString AFortGameModeAthena::DefaultMatchURL()
{
    // BP_ZT_Athena: the zone theme of Battle Royale's map (ZoneToUse /Game/Athena/Maps/Athena_Terrain, ZoneGameMode
    // Athena_GameMode_C, PlaylistId 2 solo; Core-Docs Athena-Boot-1.7.2.md §1 D5). The match's own playlist comes with
    // its matchmaking assignment. Builds whose Battle Royale map is named elsewhere (Chapter 2's) need their own source.
    // The engine asks for its default map many times while it starts.
    static FString Cached;
    if (Cached.Len() > 0)
    {
        return Cached;
    }

    constexpr const wchar_t* ThemeClassPath = L"/Game/World/ZoneThemes/PvP/BP_ZT_Athena.BP_ZT_Athena_C";
    UClass* ThemeClass = LoadObject<UClass>(nullptr, ThemeClassPath);
    UFortZoneTheme* Theme = ThemeClass ? Cast<UFortZoneTheme>(ThemeClass->GetDefaultObject()) : nullptr;
    const TAssetPtr<UWorld>* Zone = Theme ? &static_cast<TAssetPtr<UWorld>&>(Theme->ZoneToUse) : nullptr;
    const FString Map = Zone ? Zone->ToStringReference().GetLongPackageName() : FString();
    if (Map.Len() == 0)
    {
        Log::Error(L"Battle Royale: no map from {}, the server starts on its default map", ThemeClassPath);
        return FString();
    }

    const std::wstring URL = std::format(L"{}?playlistId={}", Map.ToStdString(), static_cast<int32_t>(Theme->PlaylistId));
    Log::Info(L"Battle Royale: the server starts on {}", URL);
    Cached = FString(URL.c_str());
    return Cached;
}

AFortGameModeAthena* AFortGameModeAthena::GetCurrent(const UObject* WorldContextObject)
{
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    return World ? Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
}

FName AFortGameModeAthena::RedirectLootTierGroup(FName TierGroup)
{
    // The override (1.7.2 0x3FA190) is intact, but its caller went with the stripped container and building code, so
    // Battle Royale's chests and ammo boxes rolled Save the World's Loot_Treasure and Loot_Ammo.
    // FName* RedirectLootTierGroup(FName* OutResult, FName InGroup), the name passed by value in r8.
    if (!Finder::ResolveLootTierRedirectLive())
    {
        return TierGroup;
    }
    FName Redirected;
    CallVirtual<FName*>(this, Natives::AFortGameMode_RedirectLootTierGroupIndex, &Redirected, TierGroup);
    return Redirected;
}

void AFortGameModeAthena::HookVTables()
{
    UClass* AthenaClass = StaticClass();
    UObject* ZoneDefault = AthenaClass ? AFortGameModeZone::StaticClass()->GetDefaultObject(false) : nullptr;
    void** ZoneVTable = ZoneDefault ? *reinterpret_cast<void***>(ZoneDefault) : nullptr;
    if (!ZoneVTable || !Natives::AFortGameModeZone_SpawnDefaultPawnFor)
    {
        return;
    }

    // AFortGameModeAthena::SpawnDefaultPawnFor_Implementation (1.7.2 0x3FEE60) keeps its four "SpawnDefaultPawnFor:
    // Failed due to no ..." checks and returns null where it spawned the pawn, so nothing spawns: not on the warmup island,
    // not on a warmup respawn, not when jumping from the aircraft or at its forced drop (all RestartPlayer). Its slot is the
    // Zone mode's; the Zone mode's restoration (the checks, then AFortGameMode's spawn at the start spot) is the same.
    // The first slot holding it: vtables sit back to back (no RTTI), so the next class's repeats it, which
    // Finder::VTableIndex counts as ambiguous.
    int32_t SpawnIndex = -1;
    for (int32_t Index = 0; Index < 0x200 && SpawnIndex < 0; ++Index)
    {
        SpawnIndex = ZoneVTable[Index] == reinterpret_cast<void*>(Natives::AFortGameModeZone_SpawnDefaultPawnFor) ? Index : -1;
    }
    if (SpawnIndex < 0)
    {
        Log::Error(L"Finder: AFortGameModeZone::SpawnDefaultPawnFor_Implementation's slot not found, Battle Royale players won't spawn");
        return;
    }
    for (TObjectIterator<UClass> It; SpawnIndex >= 0 && It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[SpawnIndex] != reinterpret_cast<void*>(&SpawnDefaultPawnFor_ImplementationHK))
        {
            Hooks::SwapVTable(L"AFortGameModeAthena::SpawnDefaultPawnFor_Implementation", VTable, SpawnIndex, reinterpret_cast<void*>(&SpawnDefaultPawnFor_ImplementationHK));
        }
    }
}

APawn* AFortGameModeAthena::SpawnDefaultPawnFor_ImplementationHK(AFortGameModeAthena* This, AController* NewPlayer, AActor* StartSpot)
{
    return This->SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}
