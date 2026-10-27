#include "pch.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortAthenaAircraft.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Math/Rotator.h"
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

#include <cmath>

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

APawn* AFortGameModeAthena::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
#if WITH_SERVER_CODE
    // The aircraft is the start spot of everyone in it (ChoosePlayerStart, 0x3CA260), and at the end of its path everyone
    // still inside leaves it in the same frame (OnAircraftExitedDropZone, 0x3F35B0: RestartPlayer for each). AFortGameMode's
    // spawn (0x592920) won't put a pawn within 6 units of another player's: it asks ChoosePlayerStart for another spot, the
    // aircraft again, five times, then fails. So only the first of them left the bus and the rest stayed inside (#80). The
    // aircraft spawns the pawn where it is, whatever is there, as the engine's default spawn does (the spot's location and
    // yaw); the warmup island's spots keep AFortGameMode's spawn.
    AFortAthenaAircraft* Aircraft = Cast<AFortAthenaAircraft>(StartSpot);
    UClass* PawnClass = Aircraft && NewPlayer ? GetDefaultPawnClassForController(NewPlayer) : nullptr;
    UWorld* World = PawnClass ? GetWorld() : nullptr;
    if (World)
    {
        const FTransform SpotTransform = Aircraft->GetTransform();
        const float HalfYaw = SpotTransform.Rotation.Rotator().Yaw * (3.1415926535897932f / 360.0f);
        FTransform SpawnTransform;
        SpawnTransform.Rotation.Z = std::sin(HalfYaw);
        SpawnTransform.Rotation.W = std::cos(HalfYaw);
        SpawnTransform.SetLocation(SpotTransform.GetLocation());

        FActorSpawnParameters SpawnInfo;
        SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
        SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        APawn* Pawn = Cast<APawn>(World->SpawnActor(PawnClass, &SpawnTransform, SpawnInfo));
        if (Pawn)
        {
            Log::Info(L"Battle Royale: {} leaves the aircraft as {}", NewPlayer->GetName().ToStdString(), Pawn->GetName().ToStdString());
        }
        else
        {
            Log::Error(L"Battle Royale: couldn't spawn {}'s {} at the aircraft", NewPlayer->GetName().ToStdString(), PawnClass->GetName().ToStdString());
        }
        return Pawn;
    }
#endif
    return AFortGameModeZone::SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
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
    // Zone mode's; the Zone mode's restoration (the checks, then AFortGameMode's spawn at the start spot) is the same, apart
    // from the aircraft (SpawnDefaultPawnFor_Implementation).
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
