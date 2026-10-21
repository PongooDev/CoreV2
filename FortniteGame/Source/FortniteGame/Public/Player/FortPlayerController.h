#pragma once

#include "FortRewardReport.h"
#include "GameFramework/PlayerController.h"
#include "Items/FortQuickBars.h"
#include "UObject/Stack.h"
#include "Misc/Guid.h"
#include "StructArray.h"

class AFortInventory;
class AFortPawn;
class APawn;
class IFortInventoryOwnerInterface;
class AFortQuickBars;
class UFortItemDefinition;
class UFortQuestManager;
class UFortRegisteredPlayerInfo;
class UFortSchematicItem;
class UFortWorldItem;
struct FItemDefinitionAndCount;

class AFortPlayerController : public APlayerController
{
    DefineUnrealClass(AFortPlayerController, FortniteGame)

public:
    DefineUProperty(AFortQuickBars*, QuickBars)
    DefineUProperty(AFortInventory*, WorldInventory)
    DefineUProperty(AFortPawn*, MyFortPawn)
    DefineBitfieldUProperty(bCraftFree)
    DefineBitfieldUProperty(bShouldReceiveCriticalMatchBonus)
    DefineUProperty(UFortRegisteredPlayerInfo*, MyPlayerInfo)
    DefineUStructProperty(FFortRewardReport, LatestRewardReport)
    DefineUFunction(ServerExecuteInventoryItem)
    DefineUFunction(ServerSpawnInventoryDrop)
    DefineUFunction(ServerRemoveInventoryItem)
    DefineUFunction(ActivateSlot)

    bool InitializeMcpProfile(const void* OnComplete);
    void ActivateSlot(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot);
    void Possess(APawn* InPawn);
    void SpawnQuickBars();
    void SetupQuickBars();
    void InitializeInventory(const TStructArray<FItemDefinitionAndCount>& Items);
    void TryAddToQuickBar(UFortWorldItem* Item);
    void ServerExecuteInventoryItem_Implementation(const FGuid& ItemGuid);
    void ServerSpawnInventoryDrop_Implementation(const FGuid& ItemGuid, int32_t Count);
    bool RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);
    int32_t GetItemQuantity(const UFortItemDefinition* ItemDefinition);
    UFortQuestManager* GetQuestManager() const;
    UFortSchematicItem* GetSchematicForItem(const UFortItemDefinition* ItemDefinition);

    static AFortPlayerController* FromInventoryOwner(IFortInventoryOwnerInterface* InventoryOwner);

    static void Hook();
    static void HookVTables();

    DECLARE_FUNCTION(execActivateSlot);

private:
    static inline bool (*InitializeMcpProfileOG)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    static bool InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete);
    static inline void (*PossessOG)(AFortPlayerController* This, APawn* InPawn) = nullptr;
    static void PossessHK(AFortPlayerController* This, APawn* InPawn);
    static void ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid);
    static void ServerSpawnInventoryDrop_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, int32_t Count);
    static bool RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);
    static inline bool (*RemoveInventoryItemOG)(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars) = nullptr;
};
