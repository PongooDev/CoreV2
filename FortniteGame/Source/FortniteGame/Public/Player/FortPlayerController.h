#pragma once

#include "GameFramework/PlayerController.h"
#include "Misc/Guid.h"
#include "StructArray.h"

class AFortInventory;
class AFortPawn;
class IFortInventoryOwnerInterface;
class AFortQuickBars;
class UFortWorldItem;
struct FItemDefinitionAndCount;

class AFortPlayerController : public APlayerController
{
    DefineUnrealClass(AFortPlayerController, FortniteGame)

public:
    DefineUProperty(AFortQuickBars*, QuickBars)
    DefineUProperty(AFortInventory*, WorldInventory)
    DefineUProperty(AFortPawn*, MyFortPawn)
    DefineUFunction(ServerExecuteInventoryItem)
    DefineUFunction(ServerSpawnInventoryDrop)
    DefineUFunction(ServerRemoveInventoryItem)

    bool InitializeMcpProfile(const void* OnComplete);
    void SpawnQuickBars();
    void SetupQuickBars();
    void InitializeInventory(const TStructArray<FItemDefinitionAndCount>& Items);
    void TryAddToQuickBar(UFortWorldItem* Item);
    void ServerExecuteInventoryItem_Implementation(const FGuid& ItemGuid);
    void ServerSpawnInventoryDrop_Implementation(const FGuid& ItemGuid, int32_t Count);
    bool RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);

    static AFortPlayerController* FromInventoryOwner(IFortInventoryOwnerInterface* InventoryOwner);

    static void Hook();
    static void HookVTables();

private:
    static inline bool (*InitializeMcpProfileOG)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    static bool InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete);
    static void ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid);
    static void ServerSpawnInventoryDrop_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, int32_t Count);
    static bool RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);
    static inline bool (*RemoveInventoryItemOG)(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars) = nullptr;
};
