#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Player/FortPlayerControllerZone.h"

class UFortWorldItem;
class UFortWorldItemDefinition;

struct FFortDepositedResources
{
    FString TemplateId;
    int32_t Quantity = 0;
};

static_assert(sizeof(FFortDepositedResources) == 0x18);

class AFortPlayerControllerOutpost : public AFortPlayerControllerZone
{
    DefineUnrealClass(AFortPlayerControllerOutpost, FortniteGame)
    DefineUFunction(ServerLeaveOutpostAsParty)
    DefineUFunction(ServeMoveItemToStorageVault)
    DefineUFunction(ServeMoveItemFromStorageVault)
    DefineUFunction(ServerDisintegrateItem)
    DefineUFunction(ServerFabricateItem)
    DefineUFunction(ServerDepositPOSTResource)
    DefineUFunction(ServerUpgradePOST)
    DefineUFunction(IsOutpostOwner)
    DefineUFunction(UpgradeBuildingByDefinition)

public:
    DefineUProperty(UFortWorldItemDefinition*, ItemPendingToFabricate)
    DefineUProperty(UFortWorldItem*, FabricatedItemPendingForNotification)
    DefineUProperty(int64_t, DisintegrationStartTime)
    DefineUProperty(int32_t, QuantumGooCompleted)
    DefineUProperty(int32_t, QuantumGooIncoming)
    DefineUProperty(TArray<FFortDepositedResources>, PostDepositedResources)

    void ServerLeaveOutpostAsParty_Implementation();
    void ServeMoveItemToStorageVault_Implementation(const FFortItemEntry& NewlyStoredItem);
    void ServeMoveItemFromStorageVault_Implementation(const FFortItemEntry& NewlyCollectedItem);
    void ServerDisintegrateItem_Implementation(const FGuid& ItemGuid, int32_t Count);
    void ServerFabricateItem_Implementation(const UFortWorldItemDefinition* ItemDef);
    void ServerDepositPOSTResource_Implementation(const FGuid& ItemGuid, int32_t Count);
    void ServerUpgradePOST_Implementation();
    bool IsOutpostOwner() const;
    void UpgradeBuildingByDefinition(UObject* OutpostBuilding);
    void UpdateFabricatorProgress();

    static void HookVTables();

private:
    static void DisintegrateItems_Completed(UObject* Object, const void* Result);
    static void FabricateItem_Completed(UObject* Object, const void* Result);
    static void DepositPOSTResource_Completed(UObject* Object, const void* Result);

    static void ServerLeaveOutpostAsParty_ImplementationHK(AFortPlayerControllerOutpost* This);
    static void ServeMoveItemToStorageVault_ImplementationHK(AFortPlayerControllerOutpost* This, const FFortItemEntry& NewlyStoredItem);
    static void ServeMoveItemFromStorageVault_ImplementationHK(AFortPlayerControllerOutpost* This, const FFortItemEntry& NewlyCollectedItem);
    static void ServerDisintegrateItem_ImplementationHK(AFortPlayerControllerOutpost* This, const FGuid& ItemGuid, int32_t Count);
    static void ServerFabricateItem_ImplementationHK(AFortPlayerControllerOutpost* This, const UFortWorldItemDefinition* ItemDef);
    static void ServerDepositPOSTResource_ImplementationHK(AFortPlayerControllerOutpost* This, const FGuid& ItemGuid, int32_t Count);
    static void ServerUpgradePOST_ImplementationHK(AFortPlayerControllerOutpost* This);
};
