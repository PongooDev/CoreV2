#include "pch.h"
#include "Online/FortMcpProfileWorld.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "Hooks.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void* UFortMcpProfileWorld::BindToPlayerController(AFortPlayerController* PlayerController, bool bUseOutpostInventory)
{
    void* const Result = BindToPlayerControllerOG(this, PlayerController, bUseOutpostInventory);

#if WITH_SERVER_CODE
    uint8_t* const Profile = reinterpret_cast<uint8_t*>(this);
    if (*reinterpret_cast<void**>(Profile + Natives::UFortMcpProfileWorld_Inventory))
    {
        Profile[Natives::UFortMcpProfileWorld_bIsInventoryInitialized] = 1;
    }

    FFortWorldPlayerLoadout& Loadout = CachedPlayerLoadout;
    const FString& Id = ProfileId;
    if (Loadout.bPlayerIsNew && Id.ToStdString().rfind(L"theater", 0) == 0)
    {
        UWorld* World = PlayerController ? PlayerController->GetWorld() : nullptr;
        AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
        InitializeTheater(GameState ? static_cast<const FString&>(GameState->TheaterUniqueId) : FString());
    }
#endif
    return Result;
}

void UFortMcpProfileWorld::InitializeTheater(const FString& TheaterGuid)
{
    Reflection::FParams Params(StaticFunction_InitializeTheater(), true);
    Params.Set(L"TheaterGuid", TheaterGuid);
    Params.Call(this);
}

void UFortMcpProfileWorld::HookVTables()
{
    UObject* ProfileDefault = StaticClass()->GetDefaultObject(false);
    void** ProfileVTable = ProfileDefault ? *reinterpret_cast<void***>(ProfileDefault) : nullptr;
    if (!ProfileVTable || !Finder::ResolveMcpProfileWorldLive(ProfileVTable))
    {
        return;
    }

    const int32_t Index = Natives::UFortMcpProfileWorld_BindToPlayerControllerIndex;
    void* const Bind = ProfileVTable[Index];
    BindToPlayerControllerOG = reinterpret_cast<decltype(BindToPlayerControllerOG)>(Bind);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Bind)
        {
            Hooks::SwapVTable(L"UFortMcpProfileWorld::BindToPlayerController", VTable, Index, reinterpret_cast<void*>(&BindToPlayerControllerHK));
        }
    }
}

void* UFortMcpProfileWorld::BindToPlayerControllerHK(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory)
{
    return This->BindToPlayerController(PlayerController, bUseOutpostInventory);
}
