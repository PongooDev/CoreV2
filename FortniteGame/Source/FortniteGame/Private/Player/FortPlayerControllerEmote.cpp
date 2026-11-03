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
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPlayerPawn.h"
#include "Quests/FortQuestManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

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

    // It asks CanPerformAction(Gameplay.Action.Player.InteractUse): a pawn that isn't in stasis and whose ability system
    // doesn't block the tag (Athena's override adds nothing for it).
    const FGameplayTag InteractUse{ FName(L"Gameplay.Action.Player.InteractUse") };
    if (!CallVirtual<bool>(this, Natives::AFortPlayerController_CanPerformActionIndex, &InteractUse))
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
