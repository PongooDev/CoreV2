#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Configuration.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "HAL/IConsoleManager.h"
#include "Hooks.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPlayerPawn.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <vector>

// 1.9 (CL-3757339) strips the play half of AFortPlayerController::ServerPlayEmoteItem_Implementation, the RPC the
// emote wheel sends (#141). 1.8.2 (CL-3741772) loads the emote's montage, then calls its play body (0x84F000 ->
// 0x82CD40), which gives the pawn's ability system UFortGameData::EmoteGameplayAbility with the emote as the spec's
// source object and activates it once; the ability plays the montage. 1.9 (0x85B400, slot 0x1AF) and 1.9.1 (CL-3775276,
// 0x85D9F0, slot 0x1B0) only load the montage: the play body is gone, and so is its call from ServerEmote's load
// callback (1.9 0x839090). The client still sends the RPC and nothing played. 1.10 (CL-3790078, slot 0x1AD) strips the
// whole body: the slot holds the shared empty function (#161).
//
// This follows 1.8.2's body. It leaves out spending a consumable emote (UAthenaConsumableEmoteItemDefinition) from the
// player's Athena profile through MCP, which 1.8.2 does only when the controller has one (0x41D780): consumable emotes
// play without being spent. 1.1 to 1.8.2 keep their own body and are left alone.

void AFortPlayerController::ServerPlayEmoteItem_Implementation(UFortMontageItemDefinitionBase* EmoteAsset)
{
#if WITH_SERVER_CODE
    // The part 1.9 and 1.9.1 keep: EmoteAsset->Animation.LoadSynchronous(). 1.10's slot is the shared empty function,
    // so the load is done here too (a loaded montage's is a lookup).
    ServerPlayEmoteItem_ImplementationOG(this, EmoteAsset);
    if (!EmoteAsset)
    {
        return;
    }
    EmoteAsset->Animation.LoadSynchronous();

    // 1.8.2 plays through the pawn's IAbilitySystemInterface, which returns its AbilitySystemComponent.
    AFortPawn* Pawn = MyFortPawn;
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    if (!AbilitySystem && Pawn)
    {
        AbilitySystem = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Pawn);
    }
    if (!AbilitySystem)
    {
        return;
    }

    // CanPerformAction: a pawn that isn't in stasis and whose ability system doesn't block the tag (Athena's override adds
    // nothing for it). 1.8.2's body asks Gameplay.Action.Player.InteractUse (its tag struct's +0x2D0); 4.1's client asks
    // Gameplay.Action.Player.Emote, the emote ability's own tag, before it sends the RPC (AFortPlayerController::
    // PlayEmoteItem, 4.1 0xF85F20, with the emote cooldown), and this asks the same. InteractUse is the wrong question
    // from then on: Thanos (4.1's Infinity Gauntlet) can't interact, so InteractUse is blocked for him, and his emotes
    // were refused here while his client had let them through (#297).
    const FGameplayTag EmoteTag{ FName(L"Gameplay.Action.Player.Emote") };
    if (!CallVirtual<bool>(this, Natives::AFortPlayerController_CanPerformActionIndex, &EmoteTag))
    {
        return;
    }

    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UClass* AbilityClass = GameData ? GameData->EmoteGameplayAbility.LoadSynchronous() : nullptr;
    UGameplayAbility* Ability = AbilityClass && AbilityClass->IsChildOf(UGameplayAbility::StaticClass()) ? static_cast<UGameplayAbility*>(AbilityClass->GetDefaultObject()) : nullptr;
    if (!Ability)
    {
        Log::Warning(L"Emote: {} asked to play {}, but UFortGameData::EmoteGameplayAbility isn't a gameplay ability", GetName().ToStdString(), EmoteAsset->GetName().ToStdString());
        return;
    }

    // FGameplayAbilitySpec(Ability's default object, level 1, no input, EmoteAsset), with a new handle.
    UScriptStruct* SpecStruct = FGameplayAbilitySpec::StaticStruct();
    std::vector<uint8_t> SpecStorage(SpecStruct->GetStructureSize(), 0);
    SpecStruct->InitializeStruct(SpecStorage.data());
    FGameplayAbilitySpec& Spec = *reinterpret_cast<FGameplayAbilitySpec*>(SpecStorage.data());
    FGameplayAbilitySpecHandle NewHandle;
    NewHandle.GenerateNewHandle();
    Spec.Handle = NewHandle;
    Spec.Ability = Ability;
    Spec.Level = 1;
    Spec.InputID = INDEX_NONE;
    Spec.SourceObject = EmoteAsset;
    const FGameplayAbilitySpecHandle Handle = AbilitySystem->GiveAbilityAndActivateOnce(Spec);
    SpecStruct->DestroyStruct(SpecStorage.data());

    if (Handle.IsValid())
    {
        Log::Info(L"Emote: {} plays {}", GetName().ToStdString(), EmoteAsset->GetName().ToStdString());
        SendEmoteStatEvent(EmoteAsset);
    }
    else
    {
        Log::Warning(L"Emote: {} couldn't play {} ({} wasn't given)", GetName().ToStdString(), EmoteAsset->GetName().ToStdString(), AbilityClass->GetName().ToStdString());
    }
#endif
}

// 3.0 (CL-3901517) adds EFortQuestObjectiveStatEvent::Emote, which Season 3's "Dance on top of" challenges count
// (dance_location_forbidden_01: an Emote event whose source tags have Athena.Location.Quest.Dance.Forbidden.01, the tag
// AFortPoiVolume::CheckPawnOverlap, 0x9D9A70, gives the pawn inside that volume). No sender of it is left in 3.0: its
// native stat event senders (the calls to the quest manager's stripped typed SendStatEvent, through
// AFortPlayerController::GetQuestManager's slot +0x1710) send Interact, Visit, Land, Damage, kills and WaveComplete only,
// and GAB_Emote_Generic sends nothing. It went with the stripped play body above, so the dance is sent here, as the
// other senders send theirs: the current sub game's quest manager (GetQuestManager(ESubGame::Invalid), Battle Royale's
// in Battle Royale), the emote as the target with its tags, the player's source and context tags, once.
void AFortPlayerController::SendEmoteStatEvent(UFortMontageItemDefinitionBase* EmoteAsset)
{
#if WITH_SERVER_CODE
    // Looked up by name: builds before 3.0 have no Emote event.
    const EFortQuestObjectiveStatEvent Emote{ EFortQuestObjectiveStatEvent::StaticEnumInfo().FindValue(L"Emote") };
    UFortRegisteredPlayerInfo* PlayerInfo = MyPlayerInfo;
    if (static_cast<uint8_t>(Emote) == Reflection::InvalidEnumValue || !PlayerInfo)
    {
        return;
    }
    UFortQuestManager* QuestManager = Configuration::Get().IsBattleRoyale() ? PlayerInfo->GetAthenaQuestManager() : PlayerInfo->GetCampaignQuestManager();
    if (QuestManager)
    {
        QuestManager->SendItemStatEvent(Emote, EmoteAsset, 1);
    }
#endif
}

// Save the World plays its emotes (UFortEmoteItemDefinition, the "Emote" primary asset type: /dance and the emote wheel)
// through Emote(AssetName), which sends ServerEmote. Its body loads the emote by FPrimaryAssetId(Emote, AssetName)
// through the asset manager and plays it once loaded. 1.9 and later keep the load but bind the load's callback to the
// shared empty function (2.1 0x8B4100, 2.2 0x8982D0; AFortPlayerController's own slot 0x1AF is empty), so nothing ever
// played (#195); Battle Royale's emote wheel sends ServerPlayEmoteItem instead. This loads the emote and plays it with
// the same play body.
void AFortPlayerController::ServerEmote_Implementation(const FName& AssetName)
{
#if WITH_SERVER_CODE
    UAssetManager* Manager = GEngine ? static_cast<UAssetManager*>(GEngine->AssetManager) : nullptr;
    TArray<FAssetData> AssetDataList;
    if (AssetName.IsNone() || !Manager || !Manager->GetPrimaryAssetDataList(FName(L"Emote"), AssetDataList))
    {
        return;
    }

    UFortMontageItemDefinitionBase* EmoteAsset = nullptr;
    for (int32_t Index = 0; Index < AssetDataList.Num() && !EmoteAsset; ++Index)
    {
        const FAssetData& AssetData = AssetDataList[Index];
        if (AssetData.AssetName == AssetName)
        {
            EmoteAsset = LoadObject<UFortMontageItemDefinitionBase>(nullptr, *AssetData.ObjectPath.ToString(), nullptr, static_cast<uint32_t>(LOAD_NoWarn | LOAD_Quiet));
        }
    }
    if (!EmoteAsset)
    {
        Log::Warning(L"Emote: {} asked for emote {}, which isn't an Emote primary asset", GetName().ToStdString(), AssetName.ToString().ToStdString());
        return;
    }
    ServerPlayEmoteItem_Implementation(EmoteAsset);
#endif
}

// Sprays (4.0, CL-4039451; #283). The emote wheel's sprays (UAthenaSprayItemDefinition) go through the controller's own
// PlaySprayItem (slot 0x30E, 0xF6F430), which is kept: it asks CanPerformAction(Gameplay.Action.Player.Spray), waits out
// SprayOffCooldownTime (MinSprayCooldown or the spray's own cooldown, whichever is longer) and sends
// ServerPlaySprayItem(SprayAsset). 4.0 strips that RPC's body: AFortPlayerController's slot 0x1B1 (and every controller
// class's) is the shared empty function, so nothing was ever sprayed.
//
// What is left says what the body did. UFortGameData::SprayGameplayAbility is GAB_Spray_Generic, a GAB_Emote_Generic
// that takes its spray from GetCurrentSourceObject (as the emote ability takes its emote) and, on the server once its
// montage starts, traces from the pawn and spawns BP_SprayDecal (an AFortSprayDecalInstance) with the spray and
// SpawningPlayerController set; the decal's BeginPlay calls the native SetSprayParameters (0xBA37A0, kept), which fills
// the replicated SprayInfo (the spray, the player's banner and stat value). The game's cvar fort.NumSpraysAlivePerPlayer
// (3: "if a spray is triggered when this many exist already, then the oldest one will quickly fade out") is registered
// but nothing reads it any more, and nothing but the controller's constructor and destructor touches its
// ActiveSprayInstances: both went with the body. AFortSprayDecalInstance::StartSprayFadeOutDueToNewPlacement is the
// decal's own one-second fade (BP_SprayDecal: DecalFadeoutTime 1, SetLifeSpan).
//
// So this gives the pawn SprayGameplayAbility with the spray as the spec's source object and activates it once, as the
// emote body does with EmoteGameplayAbility, fading the player's oldest spray when fort.NumSpraysAlivePerPlayer are
// already on the map. The cooldown is the client's own (PlaySprayItem above); the 1.8.2 emote body the spray body
// mirrors checks none either.
// An ABuildingSMActor in the game; only the fade is called.
class AFortSprayDecalInstance : public AActor
{
    DefineUnrealClass(AFortSprayDecalInstance, FortniteGame)

public:
    DefineUFunction(StartSprayFadeOutDueToNewPlacement)
};

namespace
{
    TAutoConsoleVariable<int32_t> CVarNumSpraysAlivePerPlayer(
        TEXT("fort.NumSpraysAlivePerPlayer"),
        3,
        TEXT("The number of sprays allowed to be alive per player (if a spray is triggered when this many exist already, then the oldest one will quickly fade out)"));

    // A decal that is gone or fading (StartSprayFadeOutDueToNewPlacement gives it a lifespan) no longer counts.
    bool IsSprayAlive(const AFortSprayDecalInstance* Decal)
    {
        return Decal && !Decal->IsPendingKill() && Decal->InitialLifeSpan <= 0.0f;
    }

    // BP_SprayDecal's SpawningPlayerController, which GAB_Spray_Generic sets before the decal finishes spawning.
    AFortPlayerController* GetSpawningPlayerController(AFortSprayDecalInstance* Decal)
    {
        const UProperty* Property = Decal->GetClass()->FindPropertyByName(FName(L"SpawningPlayerController", FNAME_Find));
        return Property ? *reinterpret_cast<AFortPlayerController**>(reinterpret_cast<uint8_t*>(Decal) + Property->GetOffset_ForInternal()) : nullptr;
    }
}

void AFortPlayerController::UpdateActiveSprayInstances()
{
#if WITH_SERVER_CODE
    TArray<AFortSprayDecalInstance*>& Active = ActiveSprayInstances;
    for (int32_t Index = Active.Num() - 1; Index >= 0; --Index)
    {
        if (!IsSprayAlive(Active[Index]))
        {
            Active.RemoveAt(Index);
        }
    }

    // The decal the last spray placed (the ability spawns it once its montage starts, after the RPC has returned).
    for (TObjectIterator<AFortSprayDecalInstance> It; It; ++It)
    {
        AFortSprayDecalInstance* Decal = *It;
        if (IsSprayAlive(Decal) && GetSpawningPlayerController(Decal) == this && !Active.Contains(Decal))
        {
            Active.Add(Decal);
        }
    }
#endif
}

void AFortPlayerController::ServerPlaySprayItem_Implementation(UAthenaSprayItemDefinition* SprayAsset)
{
#if WITH_SERVER_CODE
    if (!SprayAsset || !SprayAsset->IsA(UAthenaSprayItemDefinition::StaticClass()))
    {
        return;
    }

    AFortPawn* Pawn = MyFortPawn;
    UAbilitySystemComponent* AbilitySystem = Pawn ? static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent) : nullptr;
    if (!AbilitySystem && Pawn)
    {
        AbilitySystem = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Pawn);
    }
    if (!AbilitySystem)
    {
        return;
    }

    // The tag the client's PlaySprayItem asks for, asked again here.
    const FGameplayTag SprayTag{ FName(L"Gameplay.Action.Player.Spray") };
    if (!CallVirtual<bool>(this, Natives::AFortPlayerController_CanPerformActionIndex, &SprayTag))
    {
        return;
    }

    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UClass* AbilityClass = GameData ? GameData->SprayGameplayAbility.LoadSynchronous() : nullptr;
    UGameplayAbility* Ability = AbilityClass && AbilityClass->IsChildOf(UGameplayAbility::StaticClass()) ? static_cast<UGameplayAbility*>(AbilityClass->GetDefaultObject()) : nullptr;
    if (!Ability)
    {
        Log::Warning(L"Spray: {} asked to spray {}, but UFortGameData::SprayGameplayAbility isn't a gameplay ability", GetName().ToStdString(), SprayAsset->GetName().ToStdString());
        return;
    }
    SprayAsset->Animation.LoadSynchronous();

    // FGameplayAbilitySpec(Ability's default object, level 1, no input, SprayAsset), with a new handle.
    UScriptStruct* SpecStruct = FGameplayAbilitySpec::StaticStruct();
    std::vector<uint8_t> SpecStorage(SpecStruct->GetStructureSize(), 0);
    SpecStruct->InitializeStruct(SpecStorage.data());
    FGameplayAbilitySpec& Spec = *reinterpret_cast<FGameplayAbilitySpec*>(SpecStorage.data());
    FGameplayAbilitySpecHandle NewHandle;
    NewHandle.GenerateNewHandle();
    Spec.Handle = NewHandle;
    Spec.Ability = Ability;
    Spec.Level = 1;
    Spec.InputID = INDEX_NONE;
    Spec.SourceObject = SprayAsset;
    const FGameplayAbilitySpecHandle Handle = AbilitySystem->GiveAbilityAndActivateOnce(Spec);
    SpecStruct->DestroyStruct(SpecStorage.data());

    if (!Handle.IsValid())
    {
        Log::Warning(L"Spray: {} couldn't spray {} ({} wasn't given)", GetName().ToStdString(), SprayAsset->GetName().ToStdString(), AbilityClass->GetName().ToStdString());
        return;
    }

    // A spray triggered with fort.NumSpraysAlivePerPlayer already on the map fades the oldest out.
    UpdateActiveSprayInstances();
    TArray<AFortSprayDecalInstance*>& Active = ActiveSprayInstances;
    const int32_t MaxSprays = std::max(CVarNumSpraysAlivePerPlayer.GetValueOnGameThread(), 1);
    while (Active.Num() >= MaxSprays)
    {
        AFortSprayDecalInstance* Oldest = Active[0];
        Active.RemoveAt(0);
        Reflection::FParams Params(Oldest->FindFunction(AFortSprayDecalInstance::StaticFunction_StartSprayFadeOutDueToNewPlacement()->GetFName()));
        Params.Call(Oldest);
    }

    Log::Info(L"Spray: {} sprays {}", GetName().ToStdString(), SprayAsset->GetName().ToStdString());
    SendSprayStatEvent(SprayAsset);
#endif
}

// 4.0's EFortQuestObjectiveStatEvent::Spray, which went with the stripped body as the Emote event went with the emote's:
// sent as the emote's is (SendEmoteStatEvent), the spray as the target, the player's source and context tags (their
// location among them), once.
void AFortPlayerController::SendSprayStatEvent(UAthenaSprayItemDefinition* SprayAsset)
{
#if WITH_SERVER_CODE
    const EFortQuestObjectiveStatEvent Spray{ EFortQuestObjectiveStatEvent::StaticEnumInfo().FindValue(L"Spray") };
    UFortRegisteredPlayerInfo* PlayerInfo = MyPlayerInfo;
    if (static_cast<uint8_t>(Spray) == Reflection::InvalidEnumValue || !PlayerInfo)
    {
        return;
    }
    UFortQuestManager* QuestManager = Configuration::Get().IsBattleRoyale() ? PlayerInfo->GetAthenaQuestManager() : PlayerInfo->GetCampaignQuestManager();
    if (QuestManager)
    {
        QuestManager->SendItemStatEvent(Spray, SprayAsset, 1);
    }
#endif
}

void AFortPlayerController::HookSprayVTables()
{
    // Builds before 4.0 have no sprays.
    UFunction* ServerPlaySprayItem = FindStaticFunction_ServerPlaySprayItem();
    if (!ServerPlaySprayItem || !UAthenaSprayItemDefinition::StaticClass())
    {
        return;
    }

    const int32_t Index = Finder::ImplementationIndex(ServerPlaySprayItem);
    UObject* PlayerControllerDefault = StaticClass()->GetDefaultObject(false);
    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;
    if (Index < 0 || !PlayerControllerVTable)
    {
        Log::Error(L"Finder: AFortPlayerController::ServerPlaySprayItem_Implementation slot not found");
        return;
    }
    if (PlayerControllerVTable[Index] != Natives::SharedEmptyFunction)
    {
        Log::Info(L"Finder: AFortPlayerController::ServerPlaySprayItem_Implementation (slot 0x{:X}) is native in this build", Index);
        return;
    }
    if (!Member_ActiveSprayInstances.IsAvailable() || !AFortSprayDecalInstance::FindStaticFunction_StartSprayFadeOutDueToNewPlacement()
        || !Finder::ResolveEmoteLive(MyFortPawnOffset(), AFortPlayerPawn::StasisModeOffset()))
    {
        // ResolveEmoteLive finds the spec handle counter, GiveAbilityAndActivateOnce and CanPerformAction.
        Log::Error(L"Spray: this build strips ServerPlaySprayItem_Implementation and its helpers were not found, so nobody can spray");
        return;
    }

    int32_t Swapped = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPlayerController::ServerPlaySprayItem_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerPlaySprayItem_ImplementationHK));
            ++Swapped;
        }
    }
    Log::Info(L"Finder: AFortPlayerController::ServerPlaySprayItem_Implementation slot 0x{:X}, restored in {} controller classes", Index, Swapped);
}

void AFortPlayerController::ServerPlaySprayItem_ImplementationHK(AFortPlayerController* This, UAthenaSprayItemDefinition* SprayAsset)
{
    This->ServerPlaySprayItem_Implementation(SprayAsset);
}

void AFortPlayerController::HookEmoteVTables()
{
    UFunction* ServerPlayEmoteItem = FindStaticFunction_ServerPlayEmoteItem();
    if (!ServerPlayEmoteItem)
    {
        return;
    }
    const int32_t Index = Finder::ImplementationIndex(ServerPlayEmoteItem);
    UObject* PlayerControllerDefault = StaticClass()->GetDefaultObject(false);
    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;
    if (Index < 0 || !PlayerControllerVTable)
    {
        Log::Error(L"Finder: AFortPlayerController::ServerPlayEmoteItem_Implementation slot not found");
        return;
    }

    // Only where the build's body is down to the montage load: 1.1 to 1.8.2 keep theirs.
    void* const Implementation = PlayerControllerVTable[Index];
    if (!Finder::OnlyLoadsEmoteAnimation(reinterpret_cast<uintptr_t>(Implementation), UFortMontageItemDefinitionBase::AnimationOffset()))
    {
        Log::Info(L"Finder: AFortPlayerController::ServerPlayEmoteItem_Implementation (slot 0x{:X}) is native in this build", Index);
        return;
    }
    if (!Finder::ResolveEmoteLive(MyFortPawnOffset(), AFortPlayerPawn::StasisModeOffset()))
    {
        Log::Error(L"Emote: this build strips ServerPlayEmoteItem_Implementation and its helpers were not found, so nobody can emote");
        return;
    }

    // ServerEmote's load callback goes with the play body (1.9's comment above), so its implementation is Core's too,
    // in every controller class: AFortPlayerController's own slot is the shared empty function on 2.1 and 2.2.
    UFunction* ServerEmote = FindStaticFunction_ServerEmote();
    const int32_t EmoteIndex = ServerEmote ? Finder::ImplementationIndex(ServerEmote) : -1;

    ServerPlayEmoteItem_ImplementationOG = reinterpret_cast<decltype(ServerPlayEmoteItem_ImplementationOG)>(Implementation);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Implementation)
        {
            Hooks::SwapVTable(L"AFortPlayerController::ServerPlayEmoteItem_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerPlayEmoteItem_ImplementationHK));
        }
        if (VTable && EmoteIndex >= 0 && VTable[EmoteIndex] != reinterpret_cast<void*>(&ServerEmote_ImplementationHK))
        {
            Hooks::SwapVTable(L"AFortPlayerController::ServerEmote_Implementation", VTable, EmoteIndex, reinterpret_cast<void*>(&ServerEmote_ImplementationHK));
        }
    }
    if (ServerEmote && EmoteIndex < 0)
    {
        Log::Error(L"Finder: AFortPlayerController::ServerEmote_Implementation slot not found");
    }
}

void AFortPlayerController::ServerEmote_ImplementationHK(AFortPlayerController* This, const FName& AssetName)
{
    This->ServerEmote_Implementation(AssetName);
}

void AFortPlayerController::ServerPlayEmoteItem_ImplementationHK(AFortPlayerController* This, UFortMontageItemDefinitionBase* EmoteAsset)
{
    This->ServerPlayEmoteItem_Implementation(EmoteAsset);
}
