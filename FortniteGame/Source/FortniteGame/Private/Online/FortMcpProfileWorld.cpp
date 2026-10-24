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
#include "UObject/WeakObjectPtr.h"
#include "GameFramework/PlayerState.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Logging/LogMacros.h"
#include "McpProfile.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortWorld, Log, All);

namespace
{
    // World items made for MCP items (load, full profile updates); their definitions are only filled in by the populate after.
    std::vector<TWeakObjectPtr<UFortWorldItem>> ProfileItems;
    std::vector<std::wstring> NeverPersistedNames;

    UFortWorldItemDefinition* DefinitionOf(UFortWorldItem* Item)
    {
        FFortItemEntry& Entry = Item->ItemEntry;
        return Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry.ItemDefinition));
    }
}

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
    *reinterpret_cast<FString*>(Bytes + Natives::FBaseUrlContext_UrlConfigName) = FString(L"McpDedicatedServerCommandUrl");
    *reinterpret_cast<int32_t*>(Bytes + Natives::FBaseUrlContext_ContextCredentials) = 1;
    if (!Object || !Completed)
    {
        return;
    }

    uint8_t* Instance = static_cast<uint8_t*>(Natives::FDelegateBase_Allocate(Bytes + Natives::FBaseUrlContext_Delegate, 0x30));
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
        FDedicatedServerUrlContext* Context = Params.Get<FDedicatedServerUrlContext>(L"Context");
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

UObject* UFortMcpProfileWorld::CreateItemInstance(UClass* ItemClass, void* InstanceId)
{
    UObject* const Item = CreateItemInstanceOG(this, ItemClass, InstanceId);
#if WITH_SERVER_CODE
    if (UFortWorldItem* WorldItem = Cast<UFortWorldItem>(Item))
    {
        ProfileItems.emplace_back(WorldItem);
    }
#endif
    return Item;
}

void UFortMcpProfileWorld::RequestBatchUpdate(TArray<UFortWorldItem*>* TheaterItems, TArray<UFortWorldItem*>* OutpostItems)
{
#if WITH_SERVER_CODE
    // A profile only holds what was saved, so an item loaded from one stays saved (else this save would remove it).
    std::erase_if(ProfileItems, [](const TWeakObjectPtr<UFortWorldItem>& Pointer)
    {
        UFortWorldItem* Item = Pointer.Get();
        UFortWorldItemDefinition* Definition = Item ? DefinitionOf(Item) : nullptr;
        if (Definition)
        {
            Definition->KeepPersisted();
        }
        return !Item || Definition;
    });

    NeverPersistedNames.clear();
    for (TArray<UFortWorldItem*>* Items : { TheaterItems, OutpostItems })
    {
        for (int32_t Index = 0; Items && Index < Items->Num(); ++Index)
        {
            UFortWorldItemDefinition* Definition = (*Items)[Index] ? DefinitionOf((*Items)[Index]) : nullptr;
            const std::wstring Name = Definition && Definition->bNeverPersisted ? Definition->GetName().ToStdString() : std::wstring();
            if (!Name.empty() && std::find(NeverPersistedNames.begin(), NeverPersistedNames.end(), Name) == NeverPersistedNames.end())
            {
                NeverPersistedNames.push_back(Name);
            }
        }
    }
#endif
    RequestBatchUpdateOG(this, TheaterItems, OutpostItems);
    NeverPersistedNames.clear();
}

bool UFortMcpProfileWorld::BuildChangeRequest(FMcpProfileChangeRequest* Request, void* Items, void* Stats)
{
    const bool bBuilt = BuildChangeRequestOG(this, Request, Items, Stats);
#if WITH_SERVER_CODE
    if (!bBuilt || !Request)
    {
        return bBuilt;
    }

    const std::wstring Id = static_cast<const FString&>(ProfileId).ToStdString();
    const bool bTheater = Id.rfind(L"theater", 0) == 0;
    TStructArray<FMcpAddItemRequest>& Adds = Request->AddRequests;
    const int32_t Removes = Request->RemoveRequests.Num();
    const int32_t Quantities = Request->ChangeQuantityRequests.Num();
    const int32_t Attributes = Request->ChangeAttributesRequests.Num();
    if (!bTheater && Adds.Num() + Removes + Quantities + Attributes == 0)
    {
        return bBuilt;
    }

    std::wstring Added;
    for (int32_t Index = 0; Index < Adds.Num() && Index < 12; ++Index)
    {
        FMcpAddItemRequest& Add = Adds[Index];
        Added += std::format(L"{}{} x{}", Added.empty() ? L"" : L", ", static_cast<const FString&>(Add.TemplateId).ToStdString(), static_cast<int32_t>(Add.Quantity));
    }
    std::wstring Text = std::format(L"{} save for {}: +{} [{}] -{} qty {} attrs {} stats {}", Id, OwnerName(), Adds.Num(), Added, Removes, Quantities, Attributes, Request->ChangeStatRequests.Num());
    if (bTheater && !NeverPersistedNames.empty())
    {
        std::wstring Names;
        for (size_t Index = 0; Index < NeverPersistedNames.size() && Index < 8; ++Index)
        {
            Names += (Index ? L", " : L"") + NeverPersistedNames[Index];
        }
        Text += std::format(L"; left out as bNeverPersisted: {} [{}]", NeverPersistedNames.size(), Names);
    }
    UE_LOG(LogFortWorld, Log, TEXT("%s"), Text.c_str());
#endif
    return bBuilt;
}

std::wstring UFortMcpProfileWorld::OwnerName()
{
    AFortInventory* Inventory = Natives::UFortMcpProfileWorld_Inventory > 0 ? *reinterpret_cast<AFortInventory**>(reinterpret_cast<uint8_t*>(this) + Natives::UFortMcpProfileWorld_Inventory) : nullptr;
    AFortPlayerController* PlayerController = Inventory ? Cast<AFortPlayerController>(static_cast<AActor*>(Inventory->Owner)) : nullptr;
    APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(PlayerController->PlayerState) : nullptr;
    return PlayerState ? static_cast<const FString&>(PlayerState->PlayerName).ToStdString() : GetName().ToStdString();
}

void UFortMcpProfileWorld::Hook()
{
    if (!Finder::ResolveMcpProfileWorldSave())
    {
        return;
    }
    Hooks::Detour(L"UFortMcpProfileWorld::RequestBatchUpdate", reinterpret_cast<void*>(Natives::UFortMcpProfileWorld_RequestBatchUpdate), &RequestBatchUpdateHK, &RequestBatchUpdateOG);
    Hooks::Detour(L"UFortMcpProfileWorld change request builder", reinterpret_cast<void*>(Natives::UFortMcpProfileWorld_BuildChangeRequest), &BuildChangeRequestHK, &BuildChangeRequestOG);
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

    const int32_t CreateIndex = Finder::McpProfileWorldCreateItemInstanceIndex(ProfileVTable);
    if (CreateIndex < 0)
    {
        return;
    }
    void* const Create = ProfileVTable[CreateIndex];
    CreateItemInstanceOG = reinterpret_cast<decltype(CreateItemInstanceOG)>(Create);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[CreateIndex] == Create)
        {
            Hooks::SwapVTable(L"UFortMcpProfileWorld::CreateItemInstance", VTable, CreateIndex, reinterpret_cast<void*>(&CreateItemInstanceHK));
        }
    }
}

void* UFortMcpProfileWorld::BindToPlayerControllerHK(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory)
{
    return This->BindToPlayerController(PlayerController, bUseOutpostInventory);
}

UObject* UFortMcpProfileWorld::CreateItemInstanceHK(UFortMcpProfileWorld* This, UClass* ItemClass, void* InstanceId)
{
    return This->CreateItemInstance(ItemClass, InstanceId);
}

void UFortMcpProfileWorld::RequestBatchUpdateHK(UFortMcpProfileWorld* This, TArray<UFortWorldItem*>* TheaterItems, TArray<UFortWorldItem*>* OutpostItems)
{
    This->RequestBatchUpdate(TheaterItems, OutpostItems);
}

bool UFortMcpProfileWorld::BuildChangeRequestHK(UFortMcpProfileWorld* This, FMcpProfileChangeRequest* Request, void* Items, void* Stats)
{
    return This->BuildChangeRequest(Request, Items, Stats);
}
