#include "pch.h"
#include "Pawns/FortPlayerPawn.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Configuration.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Athena/Mutators/FortAthenaMutator.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameMode.h"
#include "FortGameState.h"
#include "GameFramework/Controller.h"
#include "Hooks.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Missions/FortMissionLibrary.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerControllerZone.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "StatManager.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace
{
    // The revive's mission event parameters (UFortPlayerDBNORevivedParams, "PlayerDBNORevived").
    class UFortPlayerDBNORevivedParams : public UObject
    {
        DefineUnrealClass(UFortPlayerDBNORevivedParams, FortniteGame)
        DefineUFunction(SetParams)

    public:
        void SetParams(AFortPlayerPawn* RevivedPlayer, AController* RevivedBy)
        {
            Reflection::FParams Params(StaticFunction_SetParams());
            Params.Set(L"_RevivedPlayer", RevivedPlayer);
            Params.Set(L"_RevivedBy", RevivedBy);
            Params.Call(this);
        }
    };

    // The members of FGameplayEventData the revive's event fills in, read in place in a SendGameplayEventToActor
    // parameter (the struct holds refcounted handles, so Core leaves its copies to the engine).
    struct FReviveEventData
    {
        DefineUnrealStruct(FGameplayEventData, GameplayAbilities)
        DefineUProperty(AActor*, Instigator)
        DefineUProperty(AActor*, Target)
    };

    // Not FindFunction: inside an AFortPlayerPawn member that name is UObject::FindFunction(FName), which takes the
    // path as a function name of the pawn's class and finds nothing, without a word (#126).
    UFunction* FindFunctionByPath(const wchar_t* Path)
    {
        UFunction* Function = FindObject<UFunction>(nullptr, Path);
        if (!Function)
        {
            Log::Error(L"DBNO revive: {} not found", Path);
        }
        return Function;
    }

    UObject* DefaultOf(const wchar_t* ClassPath)
    {
        UClass* Class = FindObject<UClass>(nullptr, ClassPath);
        return Class ? Class->GetDefaultObject() : nullptr;
    }

    FGameplayTag TagNamed(const wchar_t* Name)
    {
        return FGameplayTag{ FName(Name, FNAME_Find) };
    }

    // FScalableFloat::GetValueAtLevel: Value, scaled by its curve table row at Level when it has one.
    float GetValueAtLevel(const FScalableFloat& ScalableFloat, float Level)
    {
        const float Value = ScalableFloat.Value;
        const FCurveTableRowHandle& Curve = ScalableFloat.Curve;
        UObject* CurveTable = Curve.CurveTable;
        if (!CurveTable)
        {
            return Value;
        }
        Reflection::FParams Params(FindFunctionByPath(L"/Script/Engine.DataTableFunctionLibrary:EvaluateCurveTableRow"), true);
        Params.Set(L"CurveTable", CurveTable);
        Params.Set(L"RowName", static_cast<FName>(Curve.RowName));
        Params.Set(L"InXY", Level);
        // EEvaluateCurveTableResult::RowFound is 0.
        UObject* Library = DefaultOf(L"/Script/Engine.DataTableFunctionLibrary");
        if (!Library || !Params.Call(Library) || Params.Take<uint8_t>(L"OutResult") != 0)
        {
            return Value;
        }
        return Value * Params.Take<float>(L"OutXY");
    }

    // 3.0 (CL-3901517) has EFortQuestObjectiveStatEvent::RevivePlayer, which Season 3's "Revive teammates" challenge
    // counts (revive_athena_player: Athena.Playlist context, no target or source tags; Core-Backend #214). No sender of
    // it is left in 3.0: ServerReviveFromDBNO_Implementation is stripped (Core's ReviveFromDBNO stands in) and the 11
    // native stat event senders send no revive. The teammate who revived sends it: their current sub game's quest
    // manager, the revived pawn as the target, their own source and context tags, once. A self revive is none.
    void SendRevivePlayerStatEvent(AFortPlayerControllerZone* Reviver, AFortPlayerPawn* Revived)
    {
#if WITH_SERVER_CODE
        // Looked up by name: builds before it have no RevivePlayer event.
        const EFortQuestObjectiveStatEvent RevivePlayer{ EFortQuestObjectiveStatEvent::StaticEnumInfo().FindValue(L"RevivePlayer") };
        UFortRegisteredPlayerInfo* PlayerInfo = Reviver ? static_cast<UFortRegisteredPlayerInfo*>(Reviver->MyPlayerInfo) : nullptr;
        if (static_cast<uint8_t>(RevivePlayer) == Reflection::InvalidEnumValue || !PlayerInfo)
        {
            return;
        }
        UFortQuestManager* QuestManager = Configuration::Get().IsBattleRoyale() ? PlayerInfo->GetAthenaQuestManager() : PlayerInfo->GetCampaignQuestManager();
        if (!QuestManager)
        {
            return;
        }
        FGameplayTagContainer SourceTags;
        FGameplayTagContainer ContextTags;
        QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
        const FGameplayTagContainer NoTargetTags;
        QuestManager->SendStatEvent(RevivePlayer, Revived, NoTargetTags, SourceTags, ContextTags, 1, false);
#endif
    }
}

void AFortPlayerPawn::ResetGameplayAttributes()
{
    if (Finder::ResolveResetGameplayAttributesLive())
    {
        CallVirtual<void>(this, Natives::AFortPlayerPawn_ResetGameplayAttributesIndex);
    }
}

bool AFortPlayerPawn::IsUsingPersonalVehicle() const
{
    UFunction* Function = FindStaticFunction_IsUsingPersonalVehicle();
    if (!Function)
    {
        return false;
    }
    Reflection::FParams Params(Function);
    return Params.Call(const_cast<AFortPlayerPawn*>(this)) && Params.Take<bool>(L"ReturnValue");
}

namespace
{
    // FFortGameplayTags' Gameplay.Action.Player.InteractSearch (5.30 +0x430, 5.40 +0x440), as a container of that tag.
    // Empty where the build has no such tag.
    const FGameplayTagContainer& InteractSearchTags()
    {
        static const FGameplayTagContainer Tags = []
        {
            FGameplayTagContainer Container;
            const FName TagName(L"Gameplay.Action.Player.InteractSearch", FNAME_Find);
            if (!TagName.IsNone())
            {
                Container.AddTag(FGameplayTag{ TagName });
            }
            return Container;
        }();
        return Tags;
    }
}

bool AFortPlayerPawn::StartItemInteractionAbility()
{
    UAbilitySystemComponent* const AbilitySystem = AbilitySystemComponent;
    const FGameplayTagContainer& Tags = InteractSearchTags();
    if (!AbilitySystem || Tags.Num() == 0)
    {
        return false;
    }
    return AbilitySystem->TryActivateAbilitiesByTag(Tags, true);
}

void AFortPlayerPawn::StopItemInteractionAbility()
{
    UAbilitySystemComponent* const AbilitySystem = AbilitySystemComponent;
    const FGameplayTagContainer& Tags = InteractSearchTags();
    if (!AbilitySystem || Tags.Num() == 0)
    {
        return;
    }
    AbilitySystem->CancelAbilities(&Tags, nullptr, nullptr);
}

bool AFortPlayerPawn::EquipLastWeapon()
{
    if (!Finder::ResolveAbilityWeaponLive(CurrentWeaponListOffset(), LastEquippedWeaponGUIDOffset()))
    {
        return false;
    }
    // 1.7.2 added a bool the game's own callers pass as false; 1.1 and 1.2 ignore it.
    return CallVirtual<bool>(this, Natives::AFortPlayerPawn_EquipLastWeaponIndex, false);
}

// 7.10 renamed LastEquippedWeaponGUID to LastEquippedWeaponOrGadgetGUID (EquipLastWeapon still reads it).
ptrdiff_t AFortPlayerPawn::LastEquippedWeaponGUIDOffset()
{
    if (Member_LastEquippedWeaponGUID.IsAvailable())
    {
        return Member_LastEquippedWeaponGUID.GetOffsetChecked();
    }
    // 8.40 (CL-6005771) no longer reflects it. Its EquipLastWeapon (0x1618A70) still reads it at +0x1510, 0x20 bytes
    // before VehicleInputComponent (+0x1530), as 7.10 to 8.30 lay it out (8.30: +0x14F8 and +0x1518).
    if (!Member_LastEquippedWeaponOrGadgetGUID.IsAvailable() && Member_VehicleInputComponent.IsAvailable())
    {
        return Member_VehicleInputComponent.GetOffsetChecked() - 0x20;
    }
    return Member_LastEquippedWeaponOrGadgetGUID.GetOffsetChecked();
}

void AFortPlayerPawn::ReviveFromDBNO(AController* EventInstigator)
{
#if WITH_SERVER_CODE
    // 1.8's AFortPlayerPawn::ServerReviveFromDBNO_Implementation (0x7FF160), step by step.
    // The native takes the pawn's IAbilitySystemInterface (+0x7C0), whose GetAbilitySystemComponent returns the
    // pawn's AbilitySystemComponent (1.8.1 0x62D5E0), and keeps it only as a UFortAbilitySystemComponent.
    static UClass* FortAbilitySystemClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortAbilitySystemComponent");
    UAbilitySystemComponent* AbilitySystem = AbilitySystemComponent;
    if (!AbilitySystem)
    {
        AbilitySystem = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(this);
    }
    if (AbilitySystem && (!FortAbilitySystemClass || !AbilitySystem->IsA(FortAbilitySystemClass)))
    {
        AbilitySystem = nullptr;
    }
    // The reviver must be a teammate who isn't down themselves; a self revive passes the pawn's own controller. The
    // interaction asked the same before it got here (the pawn's interact interface, 1.8.1 0x7E9840).
    const bool bCanRevive = CallVirtual<bool>(this, Natives::AFortPlayerPawn_CanReviveFromDBNOIndex, EventInstigator);
    if (!bCanRevive || !AbilitySystem)
    {
        UE_LOG(LogFort, Warning, TEXT("ReviveFromDBNO: %s can't revive %s (CanReviveFromDBNO %s, DBNO %s, ability system %s)"),
            *GetNameSafe(EventInstigator), *GetNameSafe(this), bCanRevive ? TEXT("yes") : TEXT("no"), bIsDBNO ? TEXT("yes") : TEXT("no"), *GetNameSafe(AbilitySystem));
        return;
    }

    const bool bSelfRevive = EventInstigator == static_cast<AController*>(Controller);
    UClass* ReviveEffect = bSelfRevive ? static_cast<UClass*>(SelfReviveGameplayEffect) : static_cast<UClass*>(TeammateReviveGameplayEffect);
    if (!ReviveEffect)
    {
        UE_LOG(LogFort, Warning, TEXT("ReviveFromDBNO: %s has no %s revive effect"), *GetNameSafe(this), bSelfRevive ? TEXT("self") : TEXT("teammate"));
        return;
    }
    const auto Failed = [this, EventInstigator](const wchar_t* Step)
    {
        UE_LOG(LogFort, Warning, TEXT("ReviveFromDBNO: %s can't revive %s, %s failed"), *GetNameSafe(EventInstigator), *GetNameSafe(this), Step);
    };
    Reflection::FParams Context(FindFunctionByPath(L"/Script/GameplayAbilities.AbilitySystemComponent:MakeEffectContext"), true);
    if (!Context.Call(AbilitySystem))
    {
        Failed(TEXT("MakeEffectContext"));
        return;
    }
    Reflection::FParams Outgoing(FindFunctionByPath(L"/Script/GameplayAbilities.AbilitySystemComponent:MakeOutgoingSpec"), true);
    if (!Outgoing.Set(L"GameplayEffectClass", ReviveEffect) || !Outgoing.Set(L"Level", 0.0f))
    {
        Failed(TEXT("MakeOutgoingSpec's GameplayEffectClass or Level"));
        return;
    }
    if (!Outgoing.CopyStruct(L"Context", Context.GetAddress(L"ReturnValue")))
    {
        Failed(TEXT("MakeOutgoingSpec's Context"));
        return;
    }
    if (!Outgoing.Call(AbilitySystem))
    {
        Failed(TEXT("MakeOutgoingSpec"));
        return;
    }
    // The handle's first member is its spec, null when the effect class is.
    const void* SpecHandle = Outgoing.GetAddress(L"ReturnValue");
    if (!SpecHandle || !*static_cast<void* const*>(SpecHandle))
    {
        UE_LOG(LogFort, Warning, TEXT("ReviveFromDBNO: %s made no spec of %s for %s"), *GetNameSafe(AbilitySystem), *GetNameSafe(ReviveEffect), *GetNameSafe(this));
        return;
    }

    bIsDBNO = false;
    bPlayedDying = false;

    // The revive effect (GE_Athena_Revive) overrides Healing with "ReviveHeal": what takes the pawn to the revive health.
    const float ReviveHealth = GetValueAtLevel(SetByCallerReviveHealth, 0.0f);
    UObject* AbilityLibrary = DefaultOf(L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary");
    if (ReviveHealth > 0.0f && AbilityLibrary)
    {
        UFortHealthSet* Health = HealthSet;
        const float CurrentHealth = Health ? Health->Health.GetCurrentValue() : 0.0f;
        Reflection::FParams SetByCaller(FindFunctionByPath(L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:AssignSetByCallerMagnitude"), true);
        if (!SetByCaller.CopyStruct(L"SpecHandle", SpecHandle) || !SetByCaller.Set(L"DataName", FName(L"ReviveHeal")) || !SetByCaller.Set(L"Magnitude", ReviveHealth - CurrentHealth)
            || !SetByCaller.Call(AbilityLibrary))
        {
            Failed(TEXT("AssignSetByCallerMagnitude (ReviveHeal)"));
        }
    }

    // No DBNO, so the box teammates revive through goes.
    Natives::AFortPlayerPawn_UpdateDBNOInteractCollision(this);

    Reflection::FParams Apply(FindFunctionByPath(L"/Script/GameplayAbilities.AbilitySystemComponent:BP_ApplyGameplayEffectSpecToSelf"), true);
    if (!Apply.CopyStruct(L"SpecHandle", SpecHandle) || !Apply.Call(AbilitySystem))
    {
        Failed(TEXT("BP_ApplyGameplayEffectSpecToSelf"));
    }

    // The DBNO abilities end (GAB_AthenaDBNO: Gameplay.Action.Player.DBNOAthena), and the AI forget the downed pawn.
    const FGameplayTag DBNOTag = TagNamed(L"Gameplay.Action.Player.DBNO");
    const FGameplayTag DBNOAthenaTag = TagNamed(L"Gameplay.Action.Player.DBNOAthena");
    FGameplayTagContainer DBNOTags;
    for (const FGameplayTag* Tag : { &DBNOTag, &DBNOAthenaTag })
    {
        if (Tag->IsValid())
        {
            CallVirtual<void>(this, Natives::AFortPawn_StopNativeActionIndex, Tag);
            DBNOTags.GameplayTags.Add(*Tag);
        }
    }
    if (Natives::AFortPawn_ForgetPawnForAllAI)
    {
        Natives::AFortPawn_ForgetPawnForAllAI(this);
    }
    Natives::UAbilitySystemComponent_CancelAbilities(AbilitySystem, &DBNOTags, nullptr, nullptr);

    // The timer that would have killed the pawn when it ran out.
    uint64_t& DBNOTimer = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(this) + Natives::AFortPlayerPawn_DBNOTimerHandle);
    if (DBNOTimer)
    {
        Reflection::FParams ClearTimer(FindFunctionByPath(L"/Script/Engine.KismetSystemLibrary:K2_ClearAndInvalidateTimerHandle"), true);
        UObject* SystemLibrary = DefaultOf(L"/Script/Engine.KismetSystemLibrary");
        if (!SystemLibrary || !ClearTimer.Set(L"WorldContextObject", static_cast<UObject*>(this)) || !ClearTimer.Set(L"Handle", DBNOTimer) || !ClearTimer.Call(SystemLibrary))
        {
            Failed(TEXT("K2_ClearAndInvalidateTimerHandle (the DBNO timer)"));
        }
        DBNOTimer = 0;
    }

    // Getting up (GAB_AthenaDBNORevive triggers on Event.RevivedAthena).
    Reflection::FParams Event(FindFunctionByPath(L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:SendGameplayEventToActor"), true);
    FReviveEventData* Payload = static_cast<FReviveEventData*>(Event.GetAddress(L"Payload"));
    if (AbilityLibrary && Payload && Event.Set(L"Actor", static_cast<AActor*>(this)) && Event.Set(L"EventTag", static_cast<FGameplayTag>(EventReviveTag)))
    {
        Payload->Instigator = EventInstigator;
        Payload->Target = this;
        Event.Call(AbilityLibrary);
    }
    else
    {
        Failed(TEXT("SendGameplayEventToActor (EventReviveTag)"));
    }

    APawn* InstigatorPawn = EventInstigator ? static_cast<APawn*>(EventInstigator->Pawn) : nullptr;
    TriggerFeedbackEvent(FName(L"Resurrected"), Cast<AFortPawn>(InstigatorPawn), this, -1.0f);

    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (GameState && GameState->MissionManager)
    {
        UObject* TransientPackage = StaticFindObject(nullptr, nullptr, L"/Engine/Transient");
        if (UFortPlayerDBNORevivedParams* Params = NewObject<UFortPlayerDBNORevivedParams>(TransientPackage, UFortPlayerDBNORevivedParams::StaticClass()))
        {
            Params->SetParams(this, EventInstigator);
            UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"PlayerDBNORevived"), Params);
        }
    }

    AFortPlayerControllerZone* PlayerController = Cast<AFortPlayerControllerZone>(static_cast<AController*>(Controller));
    if (World && PlayerController)
    {
        // The client's revive (ClientOnPawnRevived), SelfRevives or RevivesFromTeammate, and the quickbar focus back.
        CallVirtual<void>(PlayerController, Natives::AFortPlayerControllerZone_OnPawnRevivedIndex, EventInstigator);
        if (Natives::AFortPlayerController_RefocusQuickBars)
        {
            Natives::AFortPlayerController_RefocusQuickBars(PlayerController, false);
        }
        // Save the World's death penalty takes durability off the weapons, except at the end of the zone.
        AFortGameMode* GameMode = Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode()));
        if (GameState && GameState->GameplayState != EFortGameplayState::GetEndOfZone() && GameMode && Natives::AFortPlayerControllerZone_ApplyResurrectDurabilityPenaltyIndex >= 0)
        {
            if (UFortGameDeathPenalty* DeathPenalty = GameMode->GetDeathPenaltyData())
            {
                const float Penalty = bSelfRevive ? DeathPenalty->SelfResurrectDurabilityPenalty : DeathPenalty->TeammateResurrectDurabilityPenalty;
                CallVirtual<void>(PlayerController, Natives::AFortPlayerControllerZone_ApplyResurrectDurabilityPenaltyIndex, Penalty);
            }
        }
    }

    AFortPlayerControllerZone* Reviver = bSelfRevive ? nullptr : Cast<AFortPlayerControllerZone>(EventInstigator);
    UObject* ReviverStats = Reviver ? static_cast<UObject*>(Reviver->StatManager) : nullptr;
    if (ReviverStats && Natives::UStatManager_FindStat && Natives::FStat_Increment)
    {
        // The end-of-match screen's Revives (AFortPlayerController::SendMatchStatsForPlayer). 1.10's change takes the
        // amount as a double.
        if (void* Stat = Natives::UStatManager_FindStat(ReviverStats, FName(L"TeammateRevives"), 3))
        {
            if (Natives::FStat_bDoubleAmount)
            {
                reinterpret_cast<void (*)(void*, double, uint8_t)>(Natives::FStat_Increment)(Stat, 1.0, 0);
            }
            else
            {
                Natives::FStat_Increment(Stat, 1, 0);
            }
        }
    }

    SendRevivePlayerStatEvent(Reviver, this);

    UE_LOG(LogFort, Log, TEXT("ReviveFromDBNO: %s revived %s to %.0f health"), *GetNameSafe(EventInstigator), *GetNameSafe(this), ReviveHealth);
#endif
}

void AFortPlayerPawn::ServerReviveFromDBNO_Implementation(AController* EventInstigator)
{
    UE_LOG(LogFort, Log, TEXT("ServerReviveFromDBNO: %s revives %s"), *GetNameSafe(EventInstigator), *GetNameSafe(this));
    ReviveFromDBNO(EventInstigator);

    AFortPlayerPawnAthena* AthenaPawn = Cast<AFortPlayerPawnAthena>(this);
    if (!AthenaPawn || bIsDBNO)
    {
        return;
    }

    // From 6.00 the squad sees the player down for as long as their player state's death info holds the knock
    // (HasDeathInfoSquadStates: the team member widget reads its bInitialized && bDBNO, not the pawn), which the pawn's
    // StartDBNO stored. The revive is what ends it, and this override is stripped with it (6.20: AFortPlayerPawnAthena
    // slot 0x198 is the shared empty function, as AFortPlayerPawn's): in 6.00 and 6.20 only RespawnPlayerAfterDeath and
    // the player state's Reset clear the death info, and neither runs for a revive. Left as it was, a revived player
    // stood up while their squad's HUD kept showing them down (CoreV2 #425) until they died or came back. A death info
    // without bDBNO is a death's or none, and stays.
    AFortPlayerStateAthena* AthenaState = AFortPlayerStateAthena::HasDeathInfoSquadStates() ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState))) : nullptr;
    if (AthenaState && AthenaState->DeathInfo.bDBNO)
    {
        AthenaState->ClearDeathInfo();
        UE_LOG(LogFort, Log, TEXT("ServerReviveFromDBNO: %s's death info cleared (bDBNO %s, bInitialized %s), the squad no longer sees them down"), *GetNameSafe(this),
            AthenaState->DeathInfo.bDBNO ? TEXT("true") : TEXT("false"), AthenaState->DeathInfo.bInitialized ? TEXT("true") : TEXT("false"));
    }

    // AFortPlayerPawnAthena's override (1.8 0x44B4F0) then ends the pawn's "being revived" state once it's up: no
    // revivers, no Gameplay.Status.DBNOBeingRevived tag, no GameplayCue.Athena.Player.BeingRevivedFromDBNO.
    // ServerDBNOReviveInterrupted_Implementation does exactly that when its last reviver stops, without using the
    // controller it runs on.
    if (Natives::AFortPlayerControllerAthena_ServerDBNOReviveInterruptedIndex < 0)
    {
        return;
    }
    AFortPlayerControllerAthena* AthenaController = Cast<AFortPlayerControllerAthena>(EventInstigator);
    if (!AthenaController)
    {
        AthenaController = Cast<AFortPlayerControllerAthena>(static_cast<AController*>(Controller));
    }
    if (!AthenaController)
    {
        AthenaController = static_cast<AFortPlayerControllerAthena*>(AFortPlayerControllerAthena::StaticClass()->GetDefaultObject());
    }
    AthenaPawn->DBNORevivalStacking = 1;
    CallVirtual<void>(AthenaController, Natives::AFortPlayerControllerAthena_ServerDBNOReviveInterruptedIndex, AthenaPawn);
}

void AFortPlayerPawn::SetLastEquippedWeaponOrGadget(const UFortItem* Item, const UFortWorldItemDefinition* ItemDefinition)
{
#if WITH_SERVER_CODE
    // 9.01's body (CL-6428087 0x1771560: on the server, a valid GUID replaces LastEquippedWeaponOrGadgetGUID), given the
    // item instead of its GUID as 9.10 passes it. 9.10's EquipLastWeapon (0x17C44F0) first tries the new TempWeaponsStack,
    // which no code left in the build fills, then this GUID.
    (void)ItemDefinition;
    const UFortWorldItem* WorldItem = Cast<UFortWorldItem>(const_cast<UFortItem*>(Item));
    if (Role != ROLE_Authority || !WorldItem)
    {
        return;
    }
    const FFortItemEntry& Entry = WorldItem->ItemEntry;
    const FGuid Guid = Entry.ItemGuid;
    if (Guid.IsValid())
    {
        *reinterpret_cast<FGuid*>(reinterpret_cast<uint8_t*>(this) + LastEquippedWeaponGUIDOffset()) = Guid;
    }
#endif
}

void AFortPlayerPawn::SetLastEquippedWeaponOrGadgetHK(AFortPlayerPawn* This, const UFortItem* Item, const UFortWorldItemDefinition* ItemDefinition)
{
    This->SetLastEquippedWeaponOrGadget(Item, ItemDefinition);
}

void AFortPlayerPawn::HookLastEquippedWeaponVTables()
{
    // 9.10 (CL-6639283): slot 0x1E2. Builds that keep it (9.01 and earlier take the GUID) are left alone.
    const int32_t Index = Finder::SetLastEquippedWeaponOrGadgetSlot();
    if (Index < 0)
    {
        return;
    }
    UObject* PawnDefault = StaticClass()->GetDefaultObject(false);
    void* const Stub = PawnDefault ? (*reinterpret_cast<void***>(PawnDefault))[Index] : nullptr;
    for (TObjectIterator<UClass> It; It && Stub; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Stub)
        {
            Hooks::SwapVTable(L"AFortPlayerPawn::SetLastEquippedWeaponOrGadget", VTable, Index, reinterpret_cast<void*>(&SetLastEquippedWeaponOrGadgetHK));
        }
    }
}

void AFortPlayerPawn::HookVTables()
{
    // 1.8.1 (CL-3729133) strips ServerReviveFromDBNO_Implementation, the server half of every DBNO revive: a teammate's
    // interaction, the BlueprintAuthorityOnly ReviveFromDBNO and the RPC all reach the shared empty function, so the
    // downed player never gets up. Builds that keep it (1.7.2, 1.8) are left alone.
    UFunction* ServerReviveFromDBNO = FindStaticFunction_ServerReviveFromDBNO();
    const int32_t Index = ServerReviveFromDBNO ? Finder::ImplementationIndex(ServerReviveFromDBNO) : -1;
    UObject* PawnDefault = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** PawnVTable = PawnDefault ? *reinterpret_cast<void***>(PawnDefault) : nullptr;
    if (Index < 0 || !PawnVTable)
    {
        Log::Error(L"Finder: AFortPlayerPawn::ServerReviveFromDBNO_Implementation slot not found");
        return;
    }
    if (!Finder::IsStubFunction(reinterpret_cast<uintptr_t>(PawnVTable[Index])))
    {
        Log::Info(L"Finder: AFortPlayerPawn::ServerReviveFromDBNO_Implementation (slot 0x{:X}) is native in this build", Index);
        return;
    }
    if (!Finder::ResolveDBNOReviveLive(IsDBNOOffset(), AFortPlayerController::QuickBarsOffset()))
    {
        Log::Error(L"DBNO revive: ServerReviveFromDBNO_Implementation is stripped and can't be restored, downed players won't get up");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            Hooks::SwapVTable(L"AFortPlayerPawn::ServerReviveFromDBNO_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerReviveFromDBNO_ImplementationHK));
        }
    }
    Log::Info(L"DBNO revive: {}", AFortPlayerStateAthena::HasDeathInfoSquadStates()
        ? L"the squad sees a player down from their player state's death info (6.00+), which the revive clears"
        : L"the squad sees a player down from their pawn's bIsDBNO, the revive leaves the death info as it is");
}

void AFortPlayerPawn::ServerReviveFromDBNO_ImplementationHK(AFortPlayerPawn* This, AController* EventInstigator)
{
    This->ServerReviveFromDBNO_Implementation(EventInstigator);
}

void AFortPlayerPawn::ServerSendZiplineState_Implementation(const FZiplinePawnState& InZiplineState)
{
#if WITH_SERVER_CODE
    // 7.10's (0x129AAB0): the client's state, when newer than the server's (its AuthoritativeValue, which the client raises
    // with each change), replaces the server's; a jump off the line launches the pawn; then OnRep_ZiplineState, which
    // puts the pawn in (or out of) the zipline movement on the server too.
    FZiplinePawnState& State = ZiplineState;
    UScriptStruct* const StateStruct = FZiplinePawnState::StaticStruct();
    if (StateStruct && InZiplineState.AuthoritativeValue > State.AuthoritativeValue)
    {
        // 7.20's two times are RepSkip, so the RPC doesn't carry them: they are kept, and set as the client's own begin
        // (0x136F89D: the world's time) and end (0x1373B98: the time for a jump, else -1) set them.
        const bool bWasZiplining = State.bIsZiplining;
        const bool bHasRideTimes = FZiplinePawnState::HasRideTimes();
        const float Began = bHasRideTimes ? static_cast<float>(State.TimeZipliningBegan) : 0.0f;
        const float EndedFromJump = bHasRideTimes ? static_cast<float>(State.TimeZipliningEndedFromJump) : 0.0f;
        memcpy(&State, &InZiplineState, StateStruct->GetStructureSize());
        UWorld* World = GetWorld();
        const float Now = World ? World->GetTimeSeconds() : 0.0f;
        if (bHasRideTimes)
        {
            State.TimeZipliningBegan = State.bIsZiplining && !bWasZiplining ? Now : Began;
            State.TimeZipliningEndedFromJump = bWasZiplining && !State.bIsZiplining ? (State.bJumped ? Now : -1.0f) : EndedFromJump;
        }
        if (!State.bIsZiplining && State.bJumped && HasZiplineJump())
        {
            // The jump off (7.10 0x1277A90; 7.20's AFortPlayerPawn::ForceLaunchPlayerZiplining, 0x1373840): the pawn's
            // velocity along the ground times ZiplineJumpDampening, each axis within 750, and ZiplineJumpStrength up; the
            // Z override only when falling; no fall damage skipped, the feedback event played. Both are curve table rows
            // the game evaluates with FCurveTableRowHandle::Eval, each 0 when its row isn't found (CoreV2 #419: read as
            // FScalableFloat, the member lookup failed and the server stopped at the first jump off a zipline).
            const FVector Velocity = GetVelocity();
            const wchar_t* const Context = L"AFortPlayerPawn::ForceLaunchPlayerZiplining";
            float Dampening = 0.0f;
            ZiplineJumpDampening.Eval(0.0f, &Dampening, Context);
            float Strength = 0.0f;
            ZiplineJumpStrength.Eval(0.0f, &Strength, Context);
            const FVector Launch(std::clamp(Dampening * Velocity.X, -750.0f, 750.0f), std::clamp(Dampening * Velocity.Y, -750.0f, 750.0f), Strength);
            LaunchCharacterJump(Launch, false, Velocity.Z < 0.0f, false, true);
        }
    }
    // OnRep_ZiplineState is a native function on 7.20, not a UFunction.
    if (UFunction* OnRep = FindStaticFunction_OnRep_ZiplineState())
    {
        Reflection::FParams Params(OnRep, true);
        Params.Call(this);
    }
    else if (Natives::AFortPlayerPawn_OnRep_ZiplineState)
    {
        Natives::AFortPlayerPawn_OnRep_ZiplineState(this);
    }
#endif
}

void AFortPlayerPawn::ServerSendZiplineState_ImplementationHK(AFortPlayerPawn* This, const FZiplinePawnState& InZiplineState)
{
    This->ServerSendZiplineState_Implementation(InZiplineState);
}

void AFortPlayerPawn::HookZiplineVTables()
{
#if WITH_SERVER_CODE
    // 7.20 (CL-4727874) strips ServerSendZiplineState_Implementation (slot 0x1BA, the shared empty function in
    // AFortPlayerPawn, AFortPlayerPawnAthena and AFortRemoteControlledPawnAthena), the server half of every zipline ride:
    // the server never took the rider's state, so it kept them in walking movement (corrections, other players not seeing
    // the ride, no launch off the line). 7.00 and 7.10 keep it (7.10 0x129AAB0) and are left alone.
    UFunction* SendZiplineState = FindStaticFunction_ServerSendZiplineState();
    const int32_t Index = SendZiplineState ? Finder::ImplementationIndex(SendZiplineState) : -1;
    UObject* PawnDefault = Index >= 0 && StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** PawnVTable = PawnDefault ? *reinterpret_cast<void***>(PawnDefault) : nullptr;
    if (!PawnVTable)
    {
        if (SendZiplineState)
        {
            Log::Error(L"Finder: AFortPlayerPawn::ServerSendZiplineState_Implementation slot not found");
        }
        return;
    }
    if (!Finder::IsStubFunction(reinterpret_cast<uintptr_t>(PawnVTable[Index])))
    {
        Log::Info(L"Finder: AFortPlayerPawn::ServerSendZiplineState_Implementation (slot 0x{:X}) is native in this build", Index);
        return;
    }
    if (!FZiplinePawnState::StaticStruct() || (!FindStaticFunction_OnRep_ZiplineState() && !Finder::ResolveZiplineStateLive()))
    {
        Log::Error(L"Zipline: ServerSendZiplineState_Implementation is stripped and FZiplinePawnState or OnRep_ZiplineState wasn't found, so the server never takes a zipline ride");
        return;
    }
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            Hooks::SwapVTable(L"AFortPlayerPawn::ServerSendZiplineState_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerSendZiplineState_ImplementationHK));
        }
    }
    Log::Info(L"Zipline: ServerSendZiplineState_Implementation (slot 0x{:X}) is stripped, restored{}", Index,
        FindStaticFunction_LaunchCharacterJump() ? L"" : L" without the jump's launch (no LaunchCharacterJump)");
#endif
}

namespace
{
    // The pawn's override stacks: AFortPlayerPawn keeps them in members the build doesn't reflect (6.21 +0x1780, cleared
    // by its constructor 0x121C5E0 next to AnimBPOverride +0x1798 and OriginalAnimBP +0x17A0), so Core keeps them per
    // pawn, checked against a weak pointer so a new pawn at a freed one's address starts empty.
    struct FOverrideStack
    {
        TWeakObjectPtr<UObject> Pawn;
        std::vector<std::pair<int32_t, UObject*>> Entries;
    };
    std::unordered_map<const UObject*, FOverrideStack> AnimBPOverrideStacks;
    std::unordered_map<const UObject*, FOverrideStack> FootstepBankOverrideStacks;

    FOverrideStack& StackOf(std::unordered_map<const UObject*, FOverrideStack>& Stacks, UObject* Pawn)
    {
        FOverrideStack& Stack = Stacks[Pawn];
        if (Stack.Pawn.Get() != Pawn)
        {
            Stack.Pawn = TWeakObjectPtr<UObject>(Pawn);
            Stack.Entries.clear();
        }
        return Stack;
    }

    // The top: the highest priority, the latest pushed among equals. Null when empty.
    UObject* TopOf(const FOverrideStack& Stack)
    {
        const std::pair<int32_t, UObject*>* Top = nullptr;
        for (const auto& Entry : Stack.Entries)
        {
            if (!Top || Entry.first >= Top->first)
            {
                Top = &Entry;
            }
        }
        return Top ? Top->second : nullptr;
    }

    void RemoveEntry(FOverrideStack& Stack, int32_t Priority, UObject* Value)
    {
        const auto Found = std::find(Stack.Entries.begin(), Stack.Entries.end(), std::pair<int32_t, UObject*>(Priority, Value));
        if (Found != Stack.Entries.end())
        {
            Stack.Entries.erase(Found);
        }
    }
}

void AFortPlayerPawn::PushAnimBPOverride(int32_t Priority, UClass* InAnimBPOverride)
{
#if WITH_SERVER_CODE
    // 6.00's (CL-4395664 on; 21.40 has neither, its gadgets use anim layers): the client builds keep the UFunctions
    // and strip the bodies (6.21: Push's and Pop's exec 0x15EFA60 steps the two parameters and calls the shared empty
    // function, and so do UFortGadgetItemDefinition::ApplyGadgetData's and RemoveGadgetData's calls). What is left shows
    // what they did: AnimBPOverride is replicated, and its OnRep (6.21 0x12400B0) runs on the server too (it logs
    // "[SERVER] ... OnRep_AnimBPOverride - Saving Original Anim BP"): it keeps the mesh's anim class in OriginalAnimBP and
    // plays AnimBPOverride, or the original again once it is null. 🟡 So the body is inferred: the pawn's override is the
    // top of a priority stack, and a change of the top sets it and runs the OnRep here. Without it 6.21's Butterfly ghost
    // (AGID_Butterfly's Fortnite_Butterfly_Ghost_AnimBlueprint) hung in the In-Between in the default pose (CoreV2 #439).
    if (Role != ROLE_Authority || !InAnimBPOverride || !Member_AnimBPOverride.IsAvailable())
    {
        return;
    }
    FOverrideStack& Stack = StackOf(AnimBPOverrideStacks, this);
    Stack.Entries.emplace_back(Priority, InAnimBPOverride);
    UClass* const Top = static_cast<UClass*>(TopOf(Stack));
    if (static_cast<UClass*>(AnimBPOverride) != Top)
    {
        FlushNetDormancy();
        AnimBPOverride = Top;
        Reflection::FParams(StaticFunction_OnRep_AnimBPOverride()).Call(this);
    }
    UE_LOG(LogFort, Log, TEXT("PushAnimBPOverride: %s plays %s (priority %d pushed)"), *GetName(), *GetNameSafe(Top), Priority);
#endif
}

void AFortPlayerPawn::PopAnimBPOverride(int32_t Priority, UClass* InAnimBPOverride)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Member_AnimBPOverride.IsAvailable())
    {
        return;
    }
    FOverrideStack& Stack = StackOf(AnimBPOverrideStacks, this);
    RemoveEntry(Stack, Priority, InAnimBPOverride);
    UClass* const Top = static_cast<UClass*>(TopOf(Stack));
    if (static_cast<UClass*>(AnimBPOverride) != Top)
    {
        FlushNetDormancy();
        AnimBPOverride = Top;
        Reflection::FParams(StaticFunction_OnRep_AnimBPOverride()).Call(this);
    }
    UE_LOG(LogFort, Log, TEXT("PopAnimBPOverride: %s plays %s (priority %d popped)"), *GetName(), Top ? *GetNameSafe(Top) : TEXT("its own anim Blueprint"), Priority);
#endif
}

void AFortPlayerPawn::PushFootstepBankOverride(int32_t Priority, UObject* InFootstepBankOverride)
{
#if WITH_SERVER_CODE
    // The footsteps' twin of PushAnimBPOverride (🟡 inferred the same way): FootstepBankOverride is replicated with its
    // own OnRep.
    if (Role != ROLE_Authority || !InFootstepBankOverride || !Member_FootstepBankOverride.IsAvailable())
    {
        return;
    }
    FOverrideStack& Stack = StackOf(FootstepBankOverrideStacks, this);
    Stack.Entries.emplace_back(Priority, InFootstepBankOverride);
    UObject* const Top = TopOf(Stack);
    if (static_cast<UObject*>(FootstepBankOverride) != Top)
    {
        FlushNetDormancy();
        FootstepBankOverride = Top;
        Reflection::FParams(StaticFunction_OnRep_FootstepBankOverride()).Call(this);
    }
#endif
}

void AFortPlayerPawn::PopFootstepBankOverride(int32_t Priority, UObject* InFootstepBankOverride)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Member_FootstepBankOverride.IsAvailable())
    {
        return;
    }
    FOverrideStack& Stack = StackOf(FootstepBankOverrideStacks, this);
    RemoveEntry(Stack, Priority, InFootstepBankOverride);
    UObject* const Top = TopOf(Stack);
    if (static_cast<UObject*>(FootstepBankOverride) != Top)
    {
        FlushNetDormancy();
        FootstepBankOverride = Top;
        Reflection::FParams(StaticFunction_OnRep_FootstepBankOverride()).Call(this);
    }
#endif
}

DEFINE_FUNCTION(AFortPlayerPawn::execPushAnimBPOverride)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Priority);
    P_GET_OBJECT(UClass, Z_Param_InAnimBPOverride);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawn*>(Context)->PushAnimBPOverride(Z_Param_Priority, Z_Param_InAnimBPOverride);
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortPlayerPawn::execPopAnimBPOverride)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Priority);
    P_GET_OBJECT(UClass, Z_Param_InAnimBPOverride);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawn*>(Context)->PopAnimBPOverride(Z_Param_Priority, Z_Param_InAnimBPOverride);
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortPlayerPawn::execPushFootstepBankOverride)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Priority);
    P_GET_OBJECT(UObject, Z_Param_InFootstepBankOverride);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawn*>(Context)->PushFootstepBankOverride(Z_Param_Priority, Z_Param_InFootstepBankOverride);
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortPlayerPawn::execPopFootstepBankOverride)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_Priority);
    P_GET_OBJECT(UObject, Z_Param_InFootstepBankOverride);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawn*>(Context)->PopFootstepBankOverride(Z_Param_Priority, Z_Param_InFootstepBankOverride);
    P_NATIVE_END;
}

void AFortPlayerPawn::PushAnimBPOverrideHK(AFortPlayerPawn* This, int32_t Priority, UClass* InAnimBPOverride)
{
    This->PushAnimBPOverride(Priority, InAnimBPOverride);
}

void AFortPlayerPawn::PopAnimBPOverrideHK(AFortPlayerPawn* This, int32_t Priority, UClass* InAnimBPOverride)
{
    This->PopAnimBPOverride(Priority, InAnimBPOverride);
}

void AFortPlayerPawn::PushFootstepBankOverrideHK(AFortPlayerPawn* This, int32_t Priority, UObject* InFootstepBankOverride)
{
    This->PushFootstepBankOverride(Priority, InFootstepBankOverride);
}

void AFortPlayerPawn::PopFootstepBankOverrideHK(AFortPlayerPawn* This, int32_t Priority, UObject* InFootstepBankOverride)
{
    This->PopFootstepBankOverride(Priority, InFootstepBankOverride);
}

void AFortPlayerPawn::ManualReviveFromDBNO()
{
#if WITH_SERVER_CODE
    // 🟡 The body is inferred, as AFortPlayerPawnAthena::ForceReviveFromDBNO's: a revive nobody performs is the build's
    // own revive with the pawn's own controller as the reviver (a self revive), through the ReviveFromDBNO UFunction
    // (ProcessEvent of ServerReviveFromDBNO, which on the server runs its _Implementation in place: Core's restored one
    // where the build strips it, 9.00 among them).
    if (Role != ROLE_Authority || !bIsDBNO)
    {
        return;
    }
    AController* const PawnController = static_cast<AController*>(Controller);
    static UFunction* const Revive = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerPawn:ReviveFromDBNO");
    Reflection::FParams Params(Revive, true);
    if (!PawnController || !Revive || !Params.Set(L"EventInstigator", PawnController) || !Params.Call(this))
    {
        UE_LOG(LogFort, Warning, TEXT("ManualReviveFromDBNO: %s can't be revived (controller %s, ReviveFromDBNO %s)"), *GetName(), *GetNameSafe(PawnController),
            Revive ? TEXT("found") : TEXT("not found"));
        return;
    }
    UE_LOG(LogFort, Log, TEXT("ManualReviveFromDBNO: %s %s"), *GetName(), bIsDBNO ? TEXT("is still down (its controller's revive was refused)") : TEXT("is up"));
#endif
}

DEFINE_FUNCTION(AFortPlayerPawn::execManualReviveFromDBNO)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawn*>(Context)->ManualReviveFromDBNO();
    P_NATIVE_END;
}

void AFortPlayerPawn::HookManualReviveNative()
{
    // 8.30 on. 9.00 (CL-6337466): the exec (0xA1E060, folded with UNiagaraScript::execOnCompilationComplete) steps the
    // frame and calls an empty function (0x32B4B0), so GenMission_TieredWaves' DoTeamRespawn got no downed player up at a
    // wave's end (a Wargame's simulation to its amplifier). Builds without it, or that keep it, are left alone.
    UFunction* Function = FindStaticFunction_ManualReviveFromDBNO();
    if (!Function)
    {
        return;
    }
    const uintptr_t Exec = reinterpret_cast<uintptr_t>(Function->GetNativeFunc());
    uintptr_t ExecNative = 0;
    for (uintptr_t Site = Exec; Exec >= Finder::TextBegin() && Site + 5 <= Exec + 0x20 && Site + 5 <= Finder::TextEnd() && !ExecNative; ++Site)
    {
        const uint8_t Opcode = *reinterpret_cast<const uint8_t*>(Site);
        if (Opcode == 0xE8 || Opcode == 0xE9)
        {
            const uintptr_t Target = Finder::RelativeTarget(Site, 1, 5);
            ExecNative = Target >= Finder::TextBegin() && Target < Finder::TextEnd() ? Target : 0;
        }
    }
    if (!ExecNative || !Finder::IsStubFunction(ExecNative))
    {
        Log::Info(L"Finder: AFortPlayerPawn::ManualReviveFromDBNO is native in this build (exec +0x{:X})", Exec ? Exec - Finder::ImageBase() : 0);
        return;
    }
    Function->SetNativeFunc(&execManualReviveFromDBNO);
    Log::Info(L"Hook AFortPlayerPawn::ManualReviveFromDBNO: native (the build's exec +0x{:X} calls the stub +0x{:X})", Exec - Finder::ImageBase(), ExecNative - Finder::ImageBase());
}

void AFortPlayerPawn::UpdateGameStateModifiedGravity(float GravityMultiplier)
{
#if WITH_SERVER_CODE
    // 🟡 The body is inferred from its name and its caller: the gravity the game state gives a pawn (9.00 CL-6337466
    // 0x1013770 asks the game state's mutators, 0x1048C80, then calls SetGravityJumpMultipliers), 1.0 without a gravity
    // mutator, times GravityMultiplier, set as the leap's SetGravityMultiplier sets it. Core's game state answer reads
    // the playlist's gravity mutator only, not AFortAthenaMutator_TeamSettings' per-team GravityData.
    if (Role != ROLE_Authority)
    {
        return;
    }
    float GravityZScale = 1.0f;
    UWorld* const World = static_cast<const UObject*>(this)->GetWorld();
    const AFortGameStateAthena* const GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    FGravityMovementData Gravity;
    if (GameState && GameState->GetMutatorGravityData(Gravity))
    {
        GravityZScale = Gravity.GravityZScale;
    }
    SetGravityMultiplier(GravityZScale * GravityMultiplier);
#endif
}

DEFINE_FUNCTION(AFortPlayerPawn::execUpdateGameStateModifiedGravity)
{
    P_GET_PROPERTY(UFloatProperty, Z_Param_GravityMultiplier);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerPawn*>(Context)->UpdateGameStateModifiedGravity(Z_Param_GravityMultiplier);
    P_NATIVE_END;
}

void AFortPlayerPawn::HookGameStateModifiedGravityNative()
{
    // 8.20 on. 9.00 (CL-6337466): the exec (0x1A68420, folded with UFortCheatManager::execEncounterSetDifficultyLevel)
    // steps the float and calls the empty function; 9.10 (CL-6573057 0x1B00840) the same. AFortPawn::SetGravityMultiplier
    // is intact, so the leap's down slash left the wielder's GravityZScale raised until a respawn (CoreV2 #695). Builds
    // without it, or that keep it, are left alone.
    UFunction* const Function = FindStaticFunction_UpdateGameStateModifiedGravity();
    if (!Function || !FindStaticFunction_SetGravityMultiplier() || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }
    const uint8_t* const NativeCall = Finder::ImplementationCall(Function);
    const uintptr_t Native = NativeCall ? Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5) : 0;
    if (!Finder::IsStubFunction(Native))
    {
        Log::Info(L"Finder: AFortPlayerPawn::UpdateGameStateModifiedGravity is native in this build");
        return;
    }
    Function->SetNativeFunc(&execUpdateGameStateModifiedGravity);
    Log::Info(L"Hook AFortPlayerPawn::UpdateGameStateModifiedGravity: native (the build's exec calls the stub +0x{:X})", Native - Finder::ImageBase());
}

void AFortPlayerPawn::HookNatives()
{
    // The four overrides (6.00 on): each UFunction whose exec calls a stub gets Core's, and the gadget data's direct
    // calls (Finder::GadgetOverrideCalls) go to Core's bodies when they reach that same stub. Builds without them, or
    // that keep them, are left alone.
    UFunction* const Push = FindStaticFunction_PushAnimBPOverride();
    if (!Push || !FindStaticFunction_OnRep_AnimBPOverride() || !Member_AnimBPOverride.IsAvailable() || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }
    const auto StubOf = [](UFunction* Function)
    {
        uint8_t* NativeCall = Function ? Finder::ImplementationCall(Function) : nullptr;
        const uintptr_t Target = NativeCall ? Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5) : 0;
        return Finder::IsStubFunction(Target) ? Target : 0;
    };
    const uintptr_t Stub = StubOf(Push);
    if (!Stub)
    {
        Log::Info(L"Finder: AFortPlayerPawn::PushAnimBPOverride is native in this build");
        return;
    }
    for (const auto& [Function, Native, Name] : {
             std::tuple{ Push, &execPushAnimBPOverride, L"AFortPlayerPawn::PushAnimBPOverride" },
             std::tuple{ FindStaticFunction_PopAnimBPOverride(), &execPopAnimBPOverride, L"AFortPlayerPawn::PopAnimBPOverride" },
             std::tuple{ FindStaticFunction_PushFootstepBankOverride(), &execPushFootstepBankOverride, L"AFortPlayerPawn::PushFootstepBankOverride" },
             std::tuple{ FindStaticFunction_PopFootstepBankOverride(), &execPopFootstepBankOverride, L"AFortPlayerPawn::PopFootstepBankOverride" } })
    {
        if (Function && StubOf(Function))
        {
            Function->SetNativeFunc(Native);
            Log::Info(L"Hook {}: native", Name);
        }
    }
    if (!UFortGadgetItemDefinition::HasOverrides() || !FindStaticFunction_OnRep_FootstepBankOverride() || !Member_FootstepBankOverride.IsAvailable())
    {
        return;
    }
    const Finder::FGadgetOverrideCalls Calls = Finder::GadgetOverrideCalls(UFortGadgetItemDefinition::GadgetPriorityOffset(),
        UFortGadgetItemDefinition::AnimBPOverrideOffset(), UFortGadgetItemDefinition::FootstepBankOverrideOffset());
    for (const auto& [Site, Destination, Name] : {
             std::tuple{ Calls.PushAnim, reinterpret_cast<void*>(&PushAnimBPOverrideHK), L"UFortGadgetItemDefinition::ApplyGadgetData -> AFortPlayerPawn::PushAnimBPOverride" },
             std::tuple{ Calls.PushFootstep, reinterpret_cast<void*>(&PushFootstepBankOverrideHK), L"UFortGadgetItemDefinition::ApplyGadgetData -> AFortPlayerPawn::PushFootstepBankOverride" },
             std::tuple{ Calls.PopAnim, reinterpret_cast<void*>(&PopAnimBPOverrideHK), L"UFortGadgetItemDefinition::RemoveGadgetData -> AFortPlayerPawn::PopAnimBPOverride" },
             std::tuple{ Calls.PopFootstep, reinterpret_cast<void*>(&PopFootstepBankOverrideHK), L"UFortGadgetItemDefinition::RemoveGadgetData -> AFortPlayerPawn::PopFootstepBankOverride" } })
    {
        if (Site && Finder::RelativeTarget(reinterpret_cast<uintptr_t>(Site), 1, 5) == Stub)
        {
            Hooks::RedirectCall(Name, Site, Destination);
        }
    }
}

void AFortPlayerPawn::TryToAutoPickupWeapon(AFortPickup* Pickup)
{
    // The game's walk-over check (7.40 0x13DF580) hands each overlapped pickup here when the pawn IsLocallyControlled:
    // on a player's own game for their pawn, and on a dedicated server for a pawn an AIController drives, since that
    // controller is local to the server (AController::IsLocalController, 7.40 0x2809E20). The tick asks it too for
    // every pickup in the pawn's overlaps (AFortPlayerPawn::Tick, 0x13C6237). The game's body opens by looking up the
    // world's local player controller (0x10A9FA0) and reading its Player (0x13FD260, [PC+0x3B0]) without checking it. A
    // dedicated server has no local player controller, so a Core bot walking over a pickup crashed the server there
    // (CoreV2 #523, #524). With none, there's no local player whose settings or interact the pickup could go through,
    // so it returns as the game's own body does when it finds no settings: nothing is picked up. Bots take pickups
    // through their own controller (FCoreAthenaBotController::TakePickup).
    UWorld* World = static_cast<const UObject*>(this)->GetWorld();
    if (!World || !Natives::AFortPlayerPawn_AutoPickupLocalPlayerController(World, true, true))
    {
        return;
    }
    TryToAutoPickupWeaponOG(this, Pickup);
}

void AFortPlayerPawn::TryToAutoPickupWeaponHK(AFortPlayerPawn* This, AFortPickup* Pickup)
{
    This->TryToAutoPickupWeapon(Pickup);
}

void AFortPlayerPawn::Hook()
{
    // An exception the owner OK'd (2026-11-21) to "no hooks on intact engine code" and, for CoreClient.dll, to "no
    // client fixes": the game's own TryToAutoPickupWeapon is intact, but its client-side check runs for AI-controlled
    // bot pawns, and a dedicated server has no local player controller for it to read (CoreV2 #523, #524). Only where
    // the pawn's tick reaches it (7.40, 8.00; Finder::ResolveAutoPickupLive), so other builds are untouched.
    if (!Finder::ResolveAutoPickupLive())
    {
        return;
    }
    Hooks::Detour(L"AFortPlayerPawn::TryToAutoPickupWeapon", reinterpret_cast<void*>(Natives::AFortPlayerPawn_TryToAutoPickupWeapon), &TryToAutoPickupWeaponHK, &TryToAutoPickupWeaponOG);
}
