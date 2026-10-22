#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Items/FortQuickBars.h"
#include "UObject/Object.h"

class AFortPlayerController;

struct FFortWorldPlayerLoadout
{
    DefineUnrealStruct(FFortWorldPlayerLoadout, FortniteGame)
    DefineBitfieldUProperty(bPlayerIsNew)
    DefineUStructProperty(FQuickBar, PrimaryQuickBarRecord)
    DefineUStructProperty(FQuickBar, SecondaryQuickBarRecord)
};

struct alignas(16) FDedicatedServerUrlContext
{
    uint8_t Bytes[0x70];

    void Bind(UObject* Object, void (*Completed)(UObject* Object, const void* Result));
};

static_assert(sizeof(FDedicatedServerUrlContext) == 0x70);

class UFortMcpProfileWorld : public UObject
{
    DefineUnrealClass(UFortMcpProfileWorld, FortniteGame)
    DefineUFunction(InitializeTheater)
    DefineUFunction(DisintegrateItems)
    DefineUFunction(DepositPostResources)
    DefineUFunction(FabricateItem)

public:
    DefineUProperty(FString, ProfileId)
    DefineUStructProperty(FFortWorldPlayerLoadout, CachedPlayerLoadout)

    void* BindToPlayerController(AFortPlayerController* PlayerController, bool bUseOutpostInventory);
    void InitializeTheater(const FString& TheaterGuid);
    bool DisintegrateItems(const TArray<FString>& ItemIds, const TArray<int32_t>& ItemCounts, UObject* Object, void (*Completed)(UObject* Object, const void* Result));
    bool DepositPostResources(const TArray<FString>& ItemIds, const TArray<int32_t>& ItemCounts, UObject* Object, void (*Completed)(UObject* Object, const void* Result));
    bool FabricateItem(const FString& TemplateId, UObject* Object, void (*Completed)(UObject* Object, const void* Result));

    static void HookVTables();

private:
    static inline void* (*BindToPlayerControllerOG)(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory) = nullptr;
    static void* BindToPlayerControllerHK(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory);
};
