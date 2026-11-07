#include "pch.h"
#include "Online/FortMcpProfileWorld.h"
#include "Delegates/DelegateBase.h"
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
#include "UObject/WeakObjectPtr.h"

#include <cstring>

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

void FDedicatedServerUrlContext::Bind(UObject* Object, void (*Completed)(UObject* Object, const void* Result))
{
    uint8_t* const Bytes = reinterpret_cast<uint8_t*>(this);
    *reinterpret_cast<FString*>(Bytes + Natives::FBaseUrlContext_UrlConfigName) = FString(L"McpDedicatedServerCommandUrl");
    *reinterpret_cast<int32_t*>(Bytes + Natives::FBaseUrlContext_ContextCredentials) = 1;
    if (!Object || !Completed)
    {
        return;
    }

    // FDelegateBase::Allocate(0x30): up to 4.1 the build's own. 4.2 inlines it into each CreateCopy (0x29824B0): its
    // FHeapAllocator delegate (Data, DelegateSize) is resized to 3 FAlignedInlineDelegateType when it isn't already.
    // The command's parameters are freshly initialized, so the delegate holds no instance to destroy first.
    uint8_t* Instance = nullptr;
    if (Natives::FBaseUrlContext_bHeapDelegate)
    {
        constexpr int32_t NewDelegateSize = 0x30 / sizeof(FAlignedInlineDelegateType);
        void*& Data = *reinterpret_cast<void**>(Bytes + Natives::FBaseUrlContext_Delegate);
        int32_t& DelegateSize = *reinterpret_cast<int32_t*>(Bytes + Natives::FBaseUrlContext_Delegate + 8);
        if (DelegateSize != NewDelegateSize)
        {
            Data = FMemory::Realloc(Data, NewDelegateSize * sizeof(FAlignedInlineDelegateType));
            DelegateSize = NewDelegateSize;
        }
        Instance = static_cast<uint8_t*>(Data);
    }
    else
    {
        Instance = static_cast<uint8_t*>(Natives::FDelegateBase_Allocate(Bytes + Natives::FBaseUrlContext_Delegate, 0x30));
    }
    if (!Instance)
    {
        return;
    }
    std::memset(Instance, 0, 0x30);
    *reinterpret_cast<void**>(Instance) = Natives::McpRequestDelegateInstanceVTable;
    *reinterpret_cast<FWeakObjectPtr*>(Instance + 0x08) = Object;
    *reinterpret_cast<void**>(Instance + 0x10) = reinterpret_cast<void*>(Completed);
    *reinterpret_cast<uint64_t*>(Instance + 0x28) = Natives::FDelegateHandle_GenerateNewID();
}

namespace
{
    bool SendCommand(UFortMcpProfileWorld* Profile, Reflection::FParams& Params, UObject* Object, void (*Completed)(UObject* Object, const void* Result))
    {
        FDedicatedServerUrlContext* Context = static_cast<FDedicatedServerUrlContext*>(Params.GetAddress(L"Context"));
        if (!Context || !Finder::ResolveOutpostMcpRequestLive())
        {
            return false;
        }
        Context->Bind(Object, Completed);
        return Params.Call(Profile);
    }
}

bool UFortMcpProfileWorld::DisintegrateItems(const TArray<FString>& ItemIds, const TArray<int32_t>& ItemCounts, UObject* Object, void (*Completed)(UObject* Object, const void* Result))
{
    Reflection::FParams Params(StaticFunction_DisintegrateItems(), true);
    Params.Set(L"itemsToDisintegrateIds", ItemIds);
    Params.Set(L"itemCountsToDisintegrate", ItemCounts);
    return SendCommand(this, Params, Object, Completed);
}

bool UFortMcpProfileWorld::DepositPostResources(const TArray<FString>& ItemIds, const TArray<int32_t>& ItemCounts, UObject* Object, void (*Completed)(UObject* Object, const void* Result))
{
    Reflection::FParams Params(StaticFunction_DepositPostResources(), true);
    Params.Set(L"itemsToDepositIds", ItemIds);
    Params.Set(L"itemCountsToDeposit", ItemCounts);
    return SendCommand(this, Params, Object, Completed);
}

bool UFortMcpProfileWorld::FabricateItem(const FString& TemplateId, UObject* Object, void (*Completed)(UObject* Object, const void* Result))
{
    Reflection::FParams Params(StaticFunction_FabricateItem(), true);
    Params.Set(L"TemplateId", TemplateId);
    return SendCommand(this, Params, Object, Completed);
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
