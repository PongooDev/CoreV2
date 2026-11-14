#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Configuration.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortRuntimeOptions.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerState.h"
#include "GameplayTagContainer.h"
#include "HAL/IConsoleManager.h"
#include "Hooks.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Math/Rotator.h"
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

// The spray decal (4.0, sprays below). An ABuildingSMActor in the game; only the fade is called.
class AFortSprayDecalInstance : public AActor
{
    DefineUnrealClass(AFortSprayDecalInstance, FortniteGame)

public:
    DefineUFunction(StartSprayFadeOutDueToNewPlacement)
};

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

    // 4.5 (CL-4166199) drops ServerPlaySprayItem and PlaySprayItem: the emote wheel's sprays go through PlayEmoteItem
    // (4.5 0xFA8E79), which asks UAthenaSprayItemDefinition apart for its cooldown (SprayOffCooldownTime) and its
    // CanPerformAction tag (Gameplay.Action.Player.Spray), then sends the spray as this RPC's EmoteAsset (#338). A spray
    // given EmoteGameplayAbility placed no decal, so it plays the spray body. 4.0 to 4.4 send theirs to
    // ServerPlaySprayItem; builds without the decal class (before 4.0) have no sprays.
    UClass* SprayClass = UAthenaSprayItemDefinition::StaticClass();
    if (SprayClass && AFortSprayDecalInstance::StaticClass() && Member_ActiveSprayInstances.IsAvailable() && EmoteAsset->IsA(SprayClass))
    {
        ServerPlaySprayItem_Implementation(static_cast<UAthenaSprayItemDefinition*>(EmoteAsset));
        return;
    }

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

    // 5.0's toys (UAthenaToyItemDefinition, Season 5) play their own ToySpawnAbility (GAB_Toy_BasketballToss,
    // GAB_Toy_GolfSwing, GAB_Toy_BeachBall: GAB_ToyThrow_Base children of GAB_Emote_Generic), which takes the toy from
    // GetCurrentSourceObject as the emote ability takes its emote, loads its ToyActorClass and, from the montage's
    // UFortAnimNotifyState_ToySpawn, asks the controller's SpawnToyInstance (below) for the toy it throws. Given
    // EmoteGameplayAbility, a toy only played its throw montage and no toy came out (#344). Every other emote plays
    // UFortGameData::EmoteGameplayAbility.
    UClass* ToyItemClass = UAthenaToyItemDefinition::StaticClass();
    UAthenaToyItemDefinition* Toy = ToyItemClass && UAthenaToyItemDefinition::HasToySpawnAbility() && EmoteAsset->IsA(ToyItemClass) ? static_cast<UAthenaToyItemDefinition*>(EmoteAsset) : nullptr;
    UFortGameData* GameData = Toy ? nullptr : UFortAssetManager::Get().GetGameData();
    UClass* AbilityClass = Toy ? Toy->ToySpawnAbility.LoadSynchronous() : GameData ? GameData->EmoteGameplayAbility.LoadSynchronous() : nullptr;
    UGameplayAbility* Ability = AbilityClass && AbilityClass->IsChildOf(UGameplayAbility::StaticClass()) ? static_cast<UGameplayAbility*>(AbilityClass->GetDefaultObject()) : nullptr;
    if (!Ability)
    {
        Log::Warning(L"Emote: {} asked to play {}, but its {} isn't a gameplay ability", GetName().ToStdString(), EmoteAsset->GetName().ToStdString(), Toy ? L"ToySpawnAbility" : L"UFortGameData::EmoteGameplayAbility");
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

// Toys (5.0, CL-4204761 and CL-4214610; #344). A toy's ability (GAB_ToyThrow_Base, its ToySpawnAbility above) loads the
// toy's ToyActorClass and, when its throw montage reaches UFortAnimNotifyState_ToySpawn (kept, 0xB5DED0: off a client
// it asks the montage's ability, through IFortToyAbilityInterface::NotifyAbilityToSpawnToy, for the toy at the hand's
// socket), calls the controller's SpawnToyInstance(ToyToSpawn, DesiredLocation), makes the toy dormant and, on
// NotifyAbilityToReleaseToy, throws it (SetBallVelocityAndStartTicking). 5.0 strips SpawnToyInstance's body: its exec
// (0x1425F80) steps both parameters and calls the shared return-zero stub, so the ability got no toy and threw nothing.
//
// What is left says what the body did. The controller keeps ActiveToyInstances and ToySummonCounts (TMap<UClass*,
// int32>), which nothing else touches; the game's cvar fort.NumToysAlivePerPlayer (1: "if a toy is triggered when this
// many exist already, then the oldest one will quickly fade out") is registered but nothing reads it; and the toy's
// IFortToyInterface events (InitializeToyInstance(OwningPC, NumTimesSummoned), NotifyToyInstanceOfReuse,
// StartToyFadeOutDueToNewPlacement) have no caller left: their Execute_ thunks' FNames are never read. B_PlayerSpawnedBall
// implements them: InitializeToyInstance stops the ball where it is and keeps that as its spawn position
// (B_ToyBasketball_Base also keeps NumTimesSummoned as its BallSerialNumber), NotifyToyInstanceOfReuse plays its
// CueToPlayWhenReused where it is (the teleport-away cue), and StartToyFadeOutDueToNewPlacement plays
// CueToPlayWhenExpired and dies.
//
// So this counts the summon, takes back the player's toy of the same class when one is still out, fades the oldest toys
// out until fort.NumToysAlivePerPlayer leave room, and spawns a new one owned by the player's state, initialized with
// the summon count and handed back.
//
// The toy's owner is the thrower's player state, not the controller (#441). Every toy blueprint reads it that way:
// B_Toy_Tomato_Base's OnPawnTouchedMe only counts a pawn whose Controller->PlayerState isn't GetOwner(), then casts
// GetOwner() to AFortPlayerState for CreateMessage(BallOwner, OtherPlayer) (gated on UFortRuntimeOptions
// bToyMessagingEnabled, +0x43E on 6.21, which the constructor 0x141E760 turns on) and for its quest
// (GetFortPlayerControllerFromActor(GetOwner())); B_Toy_BeachBall_Base's RecordPlayerPawnHit compares Controller->PlayerState == GetOwner() and
// its CreateMessage takes Cast<AFortPlayerState>(GetOwner()). The message replicates (OnRep_MessageContents) and each
// client prints it through UFortToyContext::ConditionalPrintToMessageFeed: "{ThrowerPlayerName} splatted
// {ImpactPlayerName} with a {ToyDisplayName} ({Distance}m)". Owned by the controller, the cast failed and no toy put a
// line in the feed. Nothing else sets the owner (GAB_ToyThrow_Base only spawns, makes dormant and launches), and the
// player state's own owner is the controller, so the toy's net owner is the same connection as before.
//
// A toy taken back plays its reuse cue where it is and then dies silently, as StartToyFadeOutDueToNewPlacement's own
// cue-then-SilentDie does; it is not moved to the new spot (#344). The ability attaches whatever it gets to the hand
// socket (UFortAnimNotifyState_ToySpawn's NotifyBegin, 0xB5DED0, AttachToComponent at SocketName plus LocationOffset)
// and makes it dormant there in the same frame, so a ball still in flight never sends its clients a stop: their copy
// kept its projectile velocity and the Ball mesh's interpolation offset, and drifted away from the hand while attached,
// and the throw then left from that point. A new actor reaches clients already attached and at rest.

namespace
{
    TAutoConsoleVariable<int32_t> CVarNumToysAlivePerPlayer(
        TEXT("fort.NumToysAlivePerPlayer"),
        1,
        TEXT("The number of toys allowed to be alive per player (if a toy is triggered when this many exist already, then the oldest one will quickly fade out)"));

    // One of the toy's IFortToyInterface events (BlueprintNativeEvents), called as the interface's Execute_ thunks call
    // it: through the toy's own function by that name, which its blueprint implements.
    void CallToyEvent(AActor* Toy, const wchar_t* EventName, AFortPlayerController* OwningPC = nullptr, int32_t NumTimesSummoned = 0)
    {
        UFunction* Event = Toy ? Toy->FindFunction(FName(EventName, FNAME_Find)) : nullptr;
        if (!Event)
        {
            return;
        }
        Reflection::FParams Params(Event);
        if (OwningPC)
        {
            Params.Set(L"OwningPC", OwningPC);
            Params.Set(L"NumTimesSummoned", NumTimesSummoned);
        }
        Params.Call(Toy);
    }

    // A rotation a UFunction returns (ReturnValue), or none when the object's class has no such function.
    bool CallRotation(UObject* Object, const wchar_t* FunctionName, FRotator& Out)
    {
        UFunction* Function = Object ? Object->GetClass()->FindFunctionByName(FName(FunctionName, FNAME_Find)) : nullptr;
        if (!Function)
        {
            return false;
        }
        Reflection::FParams Params(Function);
        Params.Call(Object);
        const FRotator* Value = Params.Get<FRotator>(L"ReturnValue");
        if (Value)
        {
            Out = *Value;
        }
        return Value != nullptr;
    }

    // #444: a toy sometimes left in a direction the player wasn't looking, for a stretch of throws. GAB_ToyThrow_Base's
    // LaunchBall takes the direction from the pawn's GetBaseAimRotation (AFortPlayerPawn's, 6.22 0x122D0E0), which on the
    // server is the controller's GetPlayerViewPoint (0x288C710): the camera manager's cached view, which only the client
    // fills (ServerUpdateCamera, 0x2898280, sent with its moves), or, before the first one, the view target's actor
    // rotation. None of it is stripped or Core's. This logs the four rotations at each summon (the release is the same
    // montage's notify 0.158 s later, Toy_TomatoLarge_Throw_M), so a log from a bad stretch shows which one is off.
    void LogToyAim(AFortPlayerController* Controller)
    {
        APawn* Pawn = Controller->GetPawn();
        FRotator Control, Camera, Aim, Body;
        const bool bControl = CallRotation(Controller, L"GetControlRotation", Control);
        const bool bCamera = CallRotation(reinterpret_cast<UObject*>(static_cast<APlayerCameraManager*>(Controller->PlayerCameraManager)), L"GetCameraRotation", Camera);
        const bool bAim = CallRotation(Pawn, L"GetBaseAimRotation", Aim);
        const bool bBody = CallRotation(Pawn, L"K2_GetActorRotation", Body);
        auto Text = [](bool bHas, const FRotator& R) {
            return bHas ? std::format(L"pitch {:.1f} yaw {:.1f}", R.Pitch, R.Yaw) : std::wstring(L"unknown");
        };
        Log::Info(L"Toy: {}'s aim: base aim {} (what the throw uses), camera {}, control {}, pawn {}", Controller->GetName().ToStdString(),
            Text(bAim, Aim), Text(bCamera, Camera), Text(bControl, Control), Text(bBody, Body));
    }
}

AActor* AFortPlayerController::SpawnToyInstance(UClass* ToyClass, const FTransform& SpawnPosition)
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    if (!ToyClass || !World || Role != ROLE_Authority || !ToyClass->IsChildOf(AActor::StaticClass()))
    {
        return nullptr;
    }

    int32_t& NumTimesSummoned = GetToySummonCounts().FindOrAdd(ToyClass);
    ++NumTimesSummoned;

    TArray<AActor*>& Active = ActiveToyInstances;
    for (int32_t Index = Active.Num() - 1; Index >= 0; --Index)
    {
        if (!Active[Index] || Active[Index]->IsPendingKill())
        {
            Active.RemoveAt(Index);
        }
    }

    // The player's toy of this class, if one is still out, teleports away (its reuse cue where it is) and dies; the new
    // one starts at the hand.
    for (int32_t Index = 0; Index < Active.Num(); ++Index)
    {
        AActor* Reused = Active[Index];
        if (Reused->GetClass() != ToyClass)
        {
            continue;
        }
        Active.RemoveAt(Index);
        CallToyEvent(Reused, L"NotifyToyInstanceOfReuse");
        if (Reused->FindFunction(FName(L"SilentDie", FNAME_Find)))
        {
            CallToyEvent(Reused, L"SilentDie");
        }
        else
        {
            Reused->K2_DestroyActor();
        }
        break;
    }

    const int32_t MaxToys = std::max(CVarNumToysAlivePerPlayer.GetValueOnGameThread(), 1);
    while (Active.Num() >= MaxToys)
    {
        AActor* Oldest = Active[0];
        Active.RemoveAt(0);
        CallToyEvent(Oldest, L"StartToyFadeOutDueToNewPlacement");
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Owner = PlayerState ? static_cast<AActor*>(PlayerState) : static_cast<AActor*>(this);
    SpawnParameters.Instigator = MyFortPawn;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* NewToy = World->SpawnActor(ToyClass, &SpawnPosition, SpawnParameters);
    if (!NewToy)
    {
        Log::Warning(L"Toy: {} couldn't spawn {}", GetName().ToStdString(), ToyClass->GetName().ToStdString());
        return nullptr;
    }

    Active.Add(NewToy);
    CallToyEvent(NewToy, L"InitializeToyInstance", this, NumTimesSummoned);
    Log::Info(L"Toy: {} summons {} (summon {}), owned by {}", GetName().ToStdString(), NewToy->GetName().ToStdString(), NumTimesSummoned,
        SpawnParameters.Owner->GetName().ToStdString());
    LogToyAim(this);
    static bool bLoggedToyMessaging = false;
    if (!bLoggedToyMessaging)
    {
        bLoggedToyMessaging = true;
        const UFortRuntimeOptions* Options = UFortRuntimeOptions::Get();
        Log::Info(L"Toy: the runtime options' bToyMessagingEnabled is {} (toy hits and keep-ups go to the message feed when on)",
            Options && UFortRuntimeOptions::HasToyMessaging() ? (Options->bToyMessagingEnabled ? L"on" : L"off") : L"unknown");
    }
    return NewToy;
#else
    return nullptr;
#endif
}

DEFINE_FUNCTION(AFortPlayerController::execSpawnToyInstance)
{
    P_GET_OBJECT(UClass, Z_Param_ToyClass);
    P_GET_STRUCT(FTransform, Z_Param_SpawnPosition);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AActor**>(RESULT_PARAM) = static_cast<AFortPlayerController*>(Context)->SpawnToyInstance(Z_Param_ToyClass, Z_Param_SpawnPosition);
    P_NATIVE_END;
}

void AFortPlayerController::HookToyNatives()
{
    // Builds before 5.0 have no SpawnToyInstance (4.5 already has UAthenaToyItemDefinition and ActiveToyInstances, but
    // no toy items in its paks), so it is looked up without failing there.
    UFunction* Function = FindStaticFunction_SpawnToyInstance();
    if (!Function || !Member_ActiveToyInstances.IsAvailable() || !Member_ToySummonCounts.IsAvailable() || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
    {
        Log::Info(L"Finder: AFortPlayerController::SpawnToyInstance is native in this build");
        return;
    }
    Function->SetNativeFunc(&execSpawnToyInstance);
    Log::Info(L"Hook AFortPlayerController::SpawnToyInstance: native");
}

void AFortPlayerController::HookSprayVTables()
{
    // Builds before 4.0 have no sprays. 3.5 (CL-4000805) to 3.6 (CL-4019403) already declare ServerPlaySprayItem,
    // PlaySprayItem and UAthenaSprayItemDefinition, but nothing a spray needs: no AFortSprayDecalInstance, no
    // ActiveSprayInstances, and their paks hold no spray items, GAB_Spray_Generic or BP_SprayDecal, so no client can
    // send one. The decal class arrives with 4.0's sprays.
    UFunction* ServerPlaySprayItem = FindStaticFunction_ServerPlaySprayItem();
    if (!UAthenaSprayItemDefinition::StaticClass() || !AFortSprayDecalInstance::StaticClass())
    {
        return;
    }
    if (!ServerPlaySprayItem)
    {
        // 4.5 and later: sprays come through ServerPlayEmoteItem (its body above).
        Log::Info(L"Finder: no AFortPlayerController::ServerPlaySprayItem in this build, sprays play through ServerPlayEmoteItem");
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
