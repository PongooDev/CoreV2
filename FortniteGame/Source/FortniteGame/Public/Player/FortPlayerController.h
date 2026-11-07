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
class AFortSprayDecalInstance;
class UFortHero;
class UFortItemDefinition;
class UFortMcpProfileCampaign;
class UFortMontageItemDefinitionBase;
class UFortPersonalVehicleItem;
class UFortQuestManager;
class UFortRegisteredPlayerInfo;
class UFortSchematicItem;
class UAthenaSprayItemDefinition;
class UFortWorldItem;
class UStatManager;
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
    // The player's gameplay stats (SelfRevives, RevivesFromTeammate, TeammateRevives, the damage, shots, distance and
    // materials the end-of-match screen shows, ...).
    DefineUProperty(UStatManager*, StatManager)
    DefineBitfieldUProperty(bCraftFree)
    // BuildFree's flag, replicated: placing, editing and repairing pieces cost nothing (the controller's own cost checks
    // read it).
    DefineBitfieldUProperty(bBuildFree)
    static bool HasBuildFreeFlag() { return Member_bBuildFree.IsAvailable(); }
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
    // The player's profile0 (the server queries it for each player as the dedicated server).
    DefineUProperty(UFortMcpProfileCampaign*, MainMcpProfile)
    DefineUProperty(int32_t, OverriddenBackpackSize)
    DefineUStructProperty(FFortRewardReport, LatestRewardReport)
    DefineUProperty(TAssetPtr<UObject>, StartRepairSound)
    // The building material (EFortResourceType) and level (EFortResourceLevel, 0-based) this machine's build tool places,
    // and the class it places. Transient and not replicated: the client picks the class it sends in
    // ServerCreateBuildingActor, so the game's BuildWith only changes the pieces of the machine it runs on.
    DefineUProperty(uint8_t, CurrentResourceType)
    DefineUProperty(uint8_t, CurrentResourceLevel)
    DefineUProperty(UClass*, CurrentBuildableClass)
    // 4.0: the player's sprays still on the map, oldest first (fort.NumSpraysAlivePerPlayer of them at most).
    DefineUProperty(TArray<AFortSprayDecalInstance*>, ActiveSprayInstances)
    static bool HasBuildResourceSelection() { return Member_CurrentResourceType.IsAvailable() && Member_CurrentResourceLevel.IsAvailable() && Member_CurrentBuildableClass.IsAvailable(); }
    // Whether the server has finished loading for this player (replicated, OnRep_bHasServerFinishedLoading): the
    // client's CheckFinishedLoading waits for it before its SetHasFinishedLoading and OnReadyToStartMatch.
    DefineUProperty(bool, bHasServerFinishedLoading)
    DefineUFunction(ServerExecuteInventoryItem)
    DefineUFunction(ServerSpawnInventoryDrop)
    // 3.2 renames it, with the same parameters.
    DefineUFunction(ServerAttemptInventoryDrop)
    DefineUFunction(ServerRemoveInventoryItem)
    DefineUFunction(ServerDropAllItems)
    DefineUFunction(ServerRemoveInventoryStateValue)
    DefineUFunction(ServerSpotActor)
    DefineUFunction(ServerDisassembleInventoryItem)
    DefineUFunction(ServerDisassembleInventoryItems)
    DefineUFunction(ServerModDurability)
    DefineUFunction(ActivateSlot)
    DefineUFunction(GivePlayerAmmo)
    DefineUFunction(SwapQuickBarFocus)
    DefineUFunction(ClientReportDamagedResourceBuilding)
    DefineUFunction(ClientExecuteInventoryItem)
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
    DefineUFunction(ServerEmote)
    DefineUFunction(ServerPlaySprayItem)
    DefineUFunction(ServerModifyStat)
    DefineUFunction(TogglePersonalVehicle)
    DefineUFunction(IsPersonalVehicleAvailable)

    bool InitializeMcpProfile(const void* OnComplete);
    void ActivateSlot(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot);
    void GivePlayerAmmo(int32_t AmmoCount);
    void SwapQuickBarFocus(EFortQuickBars InQuickBar);
    void ClientReportDamagedResourceBuilding(ABuildingSMActor* BuildingSMActor, uint8_t PotentialResourceType, int32_t PotentialResourceCount, bool bDestroyed, bool bJustHitWeakspot);
    // bActivateSlotAfterSettingFocused (3.5): the client sends ServerExecuteInventoryItem for the item it focused and
    // equipped. A server slot activation sets it (ServerActivateSlotInternal_Implementation 3.5.2 0xD08860 -> 0xD09920
    // -> AFortPlayerController::ExecuteInventoryItem 0xECE9F0).
    void ClientExecuteInventoryItem(const FGuid& ItemGuid, float Delay, bool bForceExecute, bool bActivateSlotAfterSettingFocused = false);
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
    // A rejoining player's world (or storm shield) inventory, kept by their world profile since they left, becomes this
    // controller's again (UFortMcpProfileWorld::BindToPlayerController hands it over).
    void ReconnectWorldInventory(AFortInventory* InWorldInventory);
    void ReconnectOutpostInventory(AFortInventory* InOutpostInventory);
    void GiveStartingItems(const TStructArray<FItemAndCount>& Items);
    void TryAddToQuickBar(UFortWorldItem* Item);
    void TrySetHeroImplementation(UFortHero* NewHero, bool bSendToServer, bool bUnused);
    void ApplyHeroAbilityKitItems(UFortHero* Hero);
    void AddHeroAbilityKitGadgetsToQuickBars();
    void ApplyHarvestingTool();
    void ApplyHomebaseGadgets();
    void ServerExecuteInventoryItem_Implementation(const FGuid& ItemGuid);
    void ServerSpawnInventoryDrop_Implementation(const FGuid& ItemGuid, int32_t Count);
    // A gadget with bDropAllOnEquip (4.1's Infinity Gauntlet) makes its taker drop everything else (#297): the pickup's
    // LocalOnAttemptInteract sends ServerDropAllItems(the gadget) before ServerHandlePickup, and the gadget's
    // ApplyGadgetData calls DropAllItems on the server. 4.1 strips both bodies.
    void DropAllItems(const UFortItemDefinition* IgnoreItemDef);
    void ServerDropAllItems_Implementation(const UFortItemDefinition* IgnoreItemDef);
    void ServerSpotActor_Implementation(AActor* NewlySpottedActor);
    void ServerDisassembleInventoryItem_Implementation(const FGuid& ItemGuid);
    void ServerDisassembleInventoryItems_Implementation(const TArray<FGuid>& ItemGuids, const TArray<int32_t>& ItemCounts);
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
    // 3.0: the dance's EFortQuestObjectiveStatEvent::Emote, which the stripped play body sent.
    void SendEmoteStatEvent(UFortMontageItemDefinitionBase* EmoteAsset);
    // Save the World's emotes (/dance, the emote wheel): 1.9 strips ServerEmote's load callback; Core's plays the emote.
    void ServerEmote_Implementation(const FName& AssetName);
    // Sprays (4.0): the emote wheel's sprays send ServerPlaySprayItem, whose body 4.0 strips; Core's plays the spray's
    // ability, fades the player's oldest spray out past fort.NumSpraysAlivePerPlayer and sends the Spray stat event.
    void ServerPlaySprayItem_Implementation(UAthenaSprayItemDefinition* SprayAsset);
    void SendSprayStatEvent(UAthenaSprayItemDefinition* SprayAsset);
    // Personal vehicles (FortPlayerControllerPersonalVehicle.cpp): 3.0 strips the hoverboard's server RPC; Core's grants
    // and activates the vehicle's ability.
    void TogglePersonalVehicle_Implementation(bool bOn);
    UFortPersonalVehicleItem* GetCurrentlySelectedPersonalVehicle() const;
    // bForceRemoval (3.2 added it to IFortInventoryOwnerInterface::RemoveInventoryItem) takes an item whose definition
    // persists it at 0 (UFortWorldItemDefinition::ShouldPersistInInventoryWhenFinalStackEmpty) out of the inventory too.
    bool RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars, bool bForceRemoval = false);
    AFortPickup* SpawnDropPickup(const FFortItemEntry& ItemEntry, const FVector* SpawnLocation, const FRotator* SpawnRotation);
    bool ModDurability(const FGuid& ItemGuid, float DurabilityMod, bool bForceSet);
    bool HandleItemZeroDurability(const FGuid& ItemGuid);
    bool DisassembleInventoryItem(const FGuid& ItemGuid, int32_t Count = 1);
    void EndWeaponAbilities(AFortWeapon* Weapon);
    int32_t GetItemQuantity(const UFortItemDefinition* ItemDefinition);
    UFortQuestManager* GetQuestManager() const;
    UFortSchematicItem* GetSchematicForItem(const UFortItemDefinition* ItemDefinition);
    // Match stats (FortPlayerControllerMatchStats.cpp): the player's end-of-match screen gets their stats once their
    // place is final (21.40's AFortPlayerController::SendMatchStatsForPlayer; 1.10's is stripped).
    void SendMatchStatsForPlayer();
    // Then their rewards, which end the screen's wait: the match's XP as the backend gave it (battleroyale/recordresults'
    // answer for the player, RewardResultJson), or none when it gave nothing.
    void SendEndBattleRoyaleMatchForPlayer(const std::string& RewardResultJson);
    // The build's own AFortPlayerController::ServerModifyStat_Implementation (1.11 0x8AF900, 2.1 0x8B4C00): the
    // StatManager's UStatManager::ModifyStat(StatName, Amount, ModType), then a save of the stats if bForceStatSave.
    void ServerModifyStat_Implementation(FName StatName, int32_t Amount, uint8_t ModType, bool bForceStatSave);
    // Server, before each frame's replication (UNetDriver::AddPreReplicationCallback): a joining player's
    // bHasServerFinishedLoading goes out only once their client has the game state, so that their match report exists
    // when their client is ready to start the match (#296).
    static void ReplicateServerFinishedLoadingAfterGameState();

    static AFortPlayerController* FromInventoryOwner(IFortInventoryOwnerInterface* InventoryOwner);
    // The game's own removals from a player's inventory (IFortInventoryOwnerInterface::RemoveInventoryItem) reach Core.
    static bool HasInventoryRemovalHook() { return RemoveInventoryItemOG != nullptr; }
    static ptrdiff_t CriticalMatchBonusOffset();
    static ptrdiff_t QuickBarsOffset();
    static ptrdiff_t MyFortPawnOffset() { return Member_MyFortPawn.GetOffsetChecked(); }
    static bool ResolveDurability();

    static void Hook();
    static void HookVTables();
    // After UFortMcpProfileWorld::HookVTables resolves the bind's reconnect slots.
    static void HookReconnectVTables();
    static void HookBuildingVTables();
    static void HookEmoteVTables();
    static void HookSprayVTables();
    static void HookPersonalVehicleVTables();
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
    static void ServerDropAllItems_ImplementationHK(AFortPlayerController* This, const UFortItemDefinition* IgnoreItemDef);
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
    static void ServerEmote_ImplementationHK(AFortPlayerController* This, const FName& AssetName);
    static void ServerPlaySprayItem_ImplementationHK(AFortPlayerController* This, UAthenaSprayItemDefinition* SprayAsset);
    // The sprays this player has placed that are neither gone nor fading, oldest first, in ActiveSprayInstances.
    void UpdateActiveSprayInstances();
    static void TogglePersonalVehicle_ImplementationHK(AFortPlayerController* This, bool bOn);
    static void ServerSpotActor_ImplementationHK(AFortPlayerController* This, AActor* NewlySpottedActor);
    static void ServerCheat_ImplementationHK(AFortPlayerController* This, const FString& Msg);
    static void ServerRemoveInventoryStateValue_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType);
    static inline void (*ServerRemoveInventoryStateValue_ImplementationOG)(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType) = nullptr;
    static bool RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars, bool bForceRemoval);
    static inline bool (*RemoveInventoryItemOG)(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars, bool bForceRemoval) = nullptr;
    static bool ModDurabilityHK(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet);
    static void ServerDisassembleInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid);
    static void ServerDisassembleInventoryItems_ImplementationHK(AFortPlayerController* This, const TArray<FGuid>& ItemGuids, const TArray<int32_t>& ItemCounts);
    static inline bool (*ModDurabilityOG)(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet) = nullptr;
    static void ReconnectWorldInventoryHK(AFortPlayerController* This, AFortInventory* InWorldInventory);
    static void ReconnectOutpostInventoryHK(AFortPlayerController* This, AFortInventory* InOutpostInventory);
};
