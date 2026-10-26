#pragma once

#include "FortRewardReport.h"
#include "GameFramework/PlayerController.h"
#include "Items/FortQuickBars.h"
#include "UObject/Stack.h"
#include "Misc/Guid.h"
#include "StructArray.h"

class AFortInventory;
class AFortPawn;
class AFortPickup;
class AFortWeapon;
class APawn;
class IFortInventoryOwnerInterface;
class AFortQuickBars;
class UFortHero;
class UFortItemDefinition;
class UFortQuestManager;
class UFortRegisteredPlayerInfo;
class UFortSchematicItem;
class UFortWorldItem;
struct FFortItemEntry;
struct FItemDefinitionAndCount;
struct FRotator;
struct FVector;

class ABuildingSMActor;

class AFortPlayerController : public APlayerController
{
    DefineUnrealClass(AFortPlayerController, FortniteGame)

public:
    DefineUProperty(AFortQuickBars*, QuickBars)
    DefineUProperty(AFortInventory*, WorldInventory)
    DefineUProperty(AFortInventory*, OutpostInventory)
    DefineUProperty(AFortPawn*, MyFortPawn)
    DefineBitfieldUProperty(bCraftFree)
    DefineBitfieldUProperty(bInfiniteDurability)
    DefineUProperty(bool, bShouldReceiveCriticalMatchBonus)
    DefineUProperty(UFortRegisteredPlayerInfo*, MyPlayerInfo)
    DefineUStructProperty(FFortRewardReport, LatestRewardReport)
    DefineUFunction(ServerExecuteInventoryItem)
    DefineUFunction(ServerSpawnInventoryDrop)
    DefineUFunction(ServerRemoveInventoryItem)
    DefineUFunction(ServerRemoveInventoryStateValue)
    DefineUFunction(ServerSpotActor)
    DefineUFunction(ServerDisassembleInventoryItem)
    DefineUFunction(ServerModDurability)
    DefineUFunction(ActivateSlot)
    DefineUFunction(GivePlayerAmmo)
    DefineUFunction(SwapQuickBarFocus)
    DefineUFunction(ClientReportDamagedResourceBuilding)

    bool InitializeMcpProfile(const void* OnComplete);
    void ActivateSlot(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot);
    void GivePlayerAmmo(int32_t AmmoCount);
    void SwapQuickBarFocus(EFortQuickBars InQuickBar);
    void ClientReportDamagedResourceBuilding(ABuildingSMActor* BuildingSMActor, uint8_t PotentialResourceType, int32_t PotentialResourceCount, bool bDestroyed, bool bJustHitWeakspot);
    void Possess(APawn* InPawn);
    void SpawnQuickBars();
    void SetupQuickBars();
    void InitializeInventory(const TStructArray<FItemDefinitionAndCount>& Items);
    void TryAddToQuickBar(UFortWorldItem* Item);
    void TrySetHeroImplementation(UFortHero* NewHero, bool bSendToServer, bool bUnused);
    void ApplyHeroAbilityKitItems(UFortHero* Hero);
    void AddHeroAbilityKitGadgetsToQuickBars();
    void ApplyHarvestingTool();
    void ApplyHomebaseGadgets();
    void ServerExecuteInventoryItem_Implementation(const FGuid& ItemGuid);
    void ServerSpawnInventoryDrop_Implementation(const FGuid& ItemGuid, int32_t Count);
    void ServerSpotActor_Implementation(AActor* NewlySpottedActor);
    void ServerDisassembleInventoryItem_Implementation(const FGuid& ItemGuid);
    void ServerRemoveInventoryStateValue_Implementation(const FGuid& ItemGuid, EFortItemEntryState StateType);
    bool RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);
    AFortPickup* SpawnDropPickup(const FFortItemEntry& ItemEntry, const FVector* SpawnLocation, const FRotator* SpawnRotation);
    bool ModDurability(const FGuid& ItemGuid, float DurabilityMod, bool bForceSet);
    bool HandleItemZeroDurability(const FGuid& ItemGuid);
    bool DisassembleInventoryItem(const FGuid& ItemGuid);
    void EndWeaponAbilities(AFortWeapon* Weapon);
    int32_t GetItemQuantity(const UFortItemDefinition* ItemDefinition);
    UFortQuestManager* GetQuestManager() const;
    UFortSchematicItem* GetSchematicForItem(const UFortItemDefinition* ItemDefinition);

    static AFortPlayerController* FromInventoryOwner(IFortInventoryOwnerInterface* InventoryOwner);
    static ptrdiff_t CriticalMatchBonusOffset();
    static bool ResolveDurability();

    static void Hook();
    static void HookVTables();

    DECLARE_FUNCTION(execActivateSlot);
    DECLARE_FUNCTION(execGivePlayerAmmo);
    DECLARE_FUNCTION(execSwapQuickBarFocus);

private:
    static inline bool (*InitializeMcpProfileOG)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    static bool InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete);
    static inline void (*PossessOG)(AFortPlayerController* This, APawn* InPawn) = nullptr;
    static void PossessHK(AFortPlayerController* This, APawn* InPawn);
    static inline void (*TrySetHeroImplementationOG)(AFortPlayerController* This, UFortHero* NewHero, bool bSendToServer, bool bUnused) = nullptr;
    static void TrySetHeroImplementationHK(AFortPlayerController* This, UFortHero* NewHero, bool bSendToServer, bool bUnused);
    static void ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid);
    static void ServerSpawnInventoryDrop_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, int32_t Count);
    static void ServerSpotActor_ImplementationHK(AFortPlayerController* This, AActor* NewlySpottedActor);
    static void ServerRemoveInventoryStateValue_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType);
    static inline void (*ServerRemoveInventoryStateValue_ImplementationOG)(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType) = nullptr;
    static bool RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);
    static inline bool (*RemoveInventoryItemOG)(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars) = nullptr;
    static bool ModDurabilityHK(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet);
    static void ServerDisassembleInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid);
    static inline bool (*ModDurabilityOG)(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet) = nullptr;
};
