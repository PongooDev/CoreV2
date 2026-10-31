#pragma once

#include "FortRewardReport.h"
#include "GameFramework/PlayerController.h"
#include "Items/FortQuickBars.h"
#include "UObject/Stack.h"
#include "Misc/Guid.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"

struct FUniqueNetIdRepl;

class AFortInventory;
class AFortPawn;
class AFortPickup;
class AFortWeapon;
class APawn;
class IFortInventoryOwnerInterface;
class AFortQuickBars;
class UFortHero;
class UFortItemDefinition;
class UFortMontageItemDefinitionBase;
class UFortQuestManager;
class UFortRegisteredPlayerInfo;
class UFortSchematicItem;
class UFortWorldItem;
struct FFortItemEntry;
struct FItemDefinitionAndCount;
struct FItemAndCount;
struct FRotator;
struct FVector;

class ABuildingActor;
class ABuildingSMActor;

// The class to build and the levels it is built at (ServerCreateBuildingActor's first parameter).
struct FBuildingClassData
{
    UClass* BuildingClass = nullptr;
    int32_t PreviousBuildingLevel = -1;
    int32_t UpgradeLevel = 0;
};

static_assert(sizeof(FBuildingClassData) == 0x10);

class AFortPlayerController : public APlayerController
{
    DefineUnrealClass(AFortPlayerController, FortniteGame)

public:
    DefineUProperty(AFortQuickBars*, QuickBars)
    DefineUProperty(AFortInventory*, WorldInventory)
    DefineUProperty(AFortInventory*, OutpostInventory)
    DefineUProperty(AFortPawn*, MyFortPawn)
    // The player's match stats (UStatManager: SelfRevives, RevivesFromTeammate, TeammateRevives, ...).
    DefineUProperty(UObject*, StatManager)
    DefineBitfieldUProperty(bCraftFree)
    DefineBitfieldUProperty(bInfiniteDurability)
    static bool HasInfiniteDurabilityFlag() { return Member_bInfiniteDurability.IsAvailable(); }
    // ToggleInfiniteAmmo's flag, replicated to the player's client: GetRemainingAmmo is then unlimited (the reserve never
    // runs out; 1.9 0x6B5150, 1.10 0x6F2E60). "cheat infiniteammo" sets it on its own and "cheat infinitemag" with the
    // magazine kept full; Core's client keeps its own count of the magazine full only while the server's is (#155).
    DefineBitfieldUProperty(bInfiniteAmmo)
    static bool HasInfiniteAmmoFlag() { return Member_bInfiniteAmmo.IsAvailable(); }
    DefineBitfieldUProperty(bCheatGhost)
    DefineBitfieldUProperty(bCheatFly)
    // ToggleNoCoolDown's flag, replicated; nothing in these shipping builds reads it (CheckCooldown's test of it is
    // compiled out). "cheat nocooldown" sets it along with clearing the cooldowns itself.
    DefineBitfieldUProperty(bNoCoolDown)
    static bool HasNoCoolDownFlag() { return Member_bNoCoolDown.IsAvailable(); }
    DefineUProperty(bool, bShouldReceiveCriticalMatchBonus)
    DefineUProperty(UFortRegisteredPlayerInfo*, MyPlayerInfo)
    DefineUProperty(int32_t, OverriddenBackpackSize)
    DefineUStructProperty(FFortRewardReport, LatestRewardReport)
    DefineUProperty(TAssetPtr<UObject>, StartRepairSound)
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
    DefineUFunction(SetRotatePawnToCamera)
    DefineUFunction(GetRotatePawnToCamera)
    DefineUFunction(Cheat)
    DefineUFunction(ServerCheat)
    DefineUFunction(ServerReturnToMainMenu)
    DefineUFunction(ServerSetPartyOwner)
    DefineUFunction(ClientFinishedInteractionInZone)
    DefineUFunction(OnRep_CheatMovement)
    DefineUFunction(ServerCreateBuildingActor)
    DefineUFunction(ServerEditBuildingActor)
    DefineUFunction(ServerBeginEditingBuildingActor)
    DefineUFunction(ServerEndEditingBuildingActor)
    DefineUFunction(ServerRepairBuildingActor)
    DefineUFunction(ServerUpgradeBuildingActor)
    DefineUFunction(FortClientPlaySoundAtLocation)
    DefineUFunction(ServerPlayEmoteItem)

    bool InitializeMcpProfile(const void* OnComplete);
    void ActivateSlot(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot);
    void GivePlayerAmmo(int32_t AmmoCount);
    void SwapQuickBarFocus(EFortQuickBars InQuickBar);
    void ClientReportDamagedResourceBuilding(ABuildingSMActor* BuildingSMActor, uint8_t PotentialResourceType, int32_t PotentialResourceCount, bool bDestroyed, bool bJustHitWeakspot);
    void SetRotatePawnToCamera(bool bNewRotatePawnToCamera);
    bool GetRotatePawnToCamera();
    void ServerSetPartyOwner(const FUniqueNetIdRepl& PartyOwnerUniqueId);
    void Possess(APawn* InPawn);
    // Whether Possess and TrySetHeroImplementation can run here (the server found and hooked the natives).
    static bool CanPossess() { return PossessOG != nullptr; }
    static bool CanTrySetHero() { return TrySetHeroImplementationOG != nullptr; }
    void SpawnQuickBars();
    void SetupQuickBars();
    void InitializeInventory(const TStructArray<FItemDefinitionAndCount>& Items);
    void GiveStartingItems(const TStructArray<FItemAndCount>& Items);
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
    void Cheat(const FString& Msg);
    void ServerCheat(const FString& Msg);
    void ServerCheat_Implementation(const FString& Msg);
    // Runs a cheat this player may use (CoreV2 #138: ServerCheat_Implementation asks the backend first).
    void RunServerCheat(const FString& Msg);
    void OnRep_CheatMovement();
    // Building (FortPlayerControllerBuilding.cpp): 1.8.1 strips these server RPCs' bodies; Core's follow 1.8's.
    void ServerCreateBuildingActor_Implementation(const FBuildingClassData& BuildingClassData, const FVector& BuildLoc, const FRotator& BuildRot, bool bMirrored);
    void ServerEditBuildingActor_Implementation(ABuildingSMActor* BuildingActorToEdit, UClass* NewBuildingClass, int32_t RotationIterations, bool bMirrored);
    void ServerBeginEditingBuildingActor_Implementation(ABuildingSMActor* BuildingActorToEdit);
    void ServerEndEditingBuildingActor_Implementation(ABuildingSMActor* BuildingActorToStopEditing);
    void ServerRepairBuildingActor_Implementation(ABuildingSMActor* BuildingActorToRepair);
    void ServerUpgradeBuildingActor_Implementation(ABuildingActor* BuildingActorToUpgrade, int32_t NewUpgradeLevel);
    void FortClientPlaySoundAtLocation(UObject* Sound, const FVector& Location, float VolumeMultiplier, float PitchMultiplier);
    // Emotes (FortPlayerControllerEmote.cpp): 1.9 strips the emote RPC's play body; Core's follows 1.8.2's.
    void ServerPlayEmoteItem_Implementation(UFortMontageItemDefinitionBase* EmoteAsset);
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
    static ptrdiff_t QuickBarsOffset();
    static ptrdiff_t MyFortPawnOffset() { return Member_MyFortPawn.GetOffsetChecked(); }
    static bool ResolveDurability();

    static void Hook();
    static void HookVTables();
    static void HookBuildingVTables();
    static void HookEmoteVTables();
    // Client: Fortnite's push to talk key (PushToTalk, Y) talks through engine voice chat instead of Vivox.
    static void HookVoice();
    static void HookClientNatives();

    DECLARE_FUNCTION(execActivateSlot);
    DECLARE_FUNCTION(execGivePlayerAmmo);
    DECLARE_FUNCTION(execSwapQuickBarFocus);
    DECLARE_FUNCTION(execCheat);
    DECLARE_FUNCTION(execOnRep_CheatMovement);

private:
    static inline bool (*InitializeMcpProfileOG)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    static bool InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete);
    static inline void (*PossessOG)(AFortPlayerController* This, APawn* InPawn) = nullptr;
    static inline void (*AthenaPossessOG)(AFortPlayerController* This, APawn* InPawn) = nullptr;
    static void PossessHK(AFortPlayerController* This, APawn* InPawn);
    static void PushToTalkPressedHK(AFortPlayerController* This);
    static void PushToTalkReleasedHK(AFortPlayerController* This);
    // Client, every frame: Battle Royale's team list shows who is talking in the engine's voice chat.
    static void UpdateTeamTalking();
    static inline void (*TrySetHeroImplementationOG)(AFortPlayerController* This, UFortHero* NewHero, bool bSendToServer, bool bUnused) = nullptr;
    static void TrySetHeroImplementationHK(AFortPlayerController* This, UFortHero* NewHero, bool bSendToServer, bool bUnused);
    static void ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid);
    static void ServerSpawnInventoryDrop_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, int32_t Count);
    bool ValidateBuildingClassData(FBuildingClassData& BuildingClassData);
    bool HasRequiredBuildingLevel(const FBuildingClassData& BuildingClassData);
    void ExecuteEditTool(ABuildingSMActor* EditActor);
    static void ServerCreateBuildingActor_ImplementationHK(AFortPlayerController* This, const FBuildingClassData& BuildingClassData, const FVector& BuildLoc, const FRotator& BuildRot, bool bMirrored);
    static void ServerEditBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingSMActor* BuildingActorToEdit, UClass* NewBuildingClass, int32_t RotationIterations, bool bMirrored);
    static void ServerBeginEditingBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingSMActor* BuildingActorToEdit);
    static void ServerEndEditingBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingSMActor* BuildingActorToStopEditing);
    static void ServerRepairBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingSMActor* BuildingActorToRepair);
    static void ServerUpgradeBuildingActor_ImplementationHK(AFortPlayerController* This, ABuildingActor* BuildingActorToUpgrade, int32_t NewUpgradeLevel);
    static inline void (*ServerPlayEmoteItem_ImplementationOG)(AFortPlayerController* This, UFortMontageItemDefinitionBase* EmoteAsset) = nullptr;
    static void ServerPlayEmoteItem_ImplementationHK(AFortPlayerController* This, UFortMontageItemDefinitionBase* EmoteAsset);
    static void ServerSpotActor_ImplementationHK(AFortPlayerController* This, AActor* NewlySpottedActor);
    static void ServerCheat_ImplementationHK(AFortPlayerController* This, const FString& Msg);
    static void ServerRemoveInventoryStateValue_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType);
    static inline void (*ServerRemoveInventoryStateValue_ImplementationOG)(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType) = nullptr;
    static bool RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);
    static inline bool (*RemoveInventoryItemOG)(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars) = nullptr;
    static bool ModDurabilityHK(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet);
    static void ServerDisassembleInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid);
    static inline bool (*ModDurabilityOG)(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet) = nullptr;
};
