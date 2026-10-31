#include "pch.h"
#include "Pawns/FortPlayerPawn.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameMode.h"
#include "FortGameState.h"
#include "GameFramework/Controller.h"
#include "Hooks.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Missions/FortMissionLibrary.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerControllerZone.h"
#include "ReflectionParams.h"
#include "StatManager.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

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
}

void AFortPlayerPawn::ResetGameplayAttributes()
{
    if (Finder::ResolveResetGameplayAttributesLive())
    {
        CallVirtual<void>(this, Natives::AFortPlayerPawn_ResetGameplayAttributesIndex);
    }
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

ptrdiff_t AFortPlayerPawn::LastEquippedWeaponGUIDOffset()
{
    return Member_LastEquippedWeaponGUID.GetOffsetChecked();
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
            Natives::AFortPlayerController_RefocusQuickBars(PlayerController);
        }
        // Save the World's death penalty takes durability off the weapons, except at the end of the zone.
        AFortGameMode* GameMode = Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode()));
        if (GameState && GameState->GameplayState != EFortGameplayState::GetEndOfZone() && GameMode && Natives::AFortPlayerControllerZone_ApplyResurrectDurabilityPenaltyIndex >= 0)
        {
            const TAssetPtr<UFortGameDeathPenalty>& DeathPenaltyData = GameMode->DeathPenaltyData;
            if (UFortGameDeathPenalty* DeathPenalty = DeathPenaltyData.LoadSynchronous())
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

    UE_LOG(LogFort, Log, TEXT("ReviveFromDBNO: %s revived %s to %.0f health"), *GetNameSafe(EventInstigator), *GetNameSafe(this), ReviveHealth);
#endif
}

void AFortPlayerPawn::ServerReviveFromDBNO_Implementation(AController* EventInstigator)
{
    UE_LOG(LogFort, Log, TEXT("ServerReviveFromDBNO: %s revives %s"), *GetNameSafe(EventInstigator), *GetNameSafe(this));
    ReviveFromDBNO(EventInstigator);

    // AFortPlayerPawnAthena's override (1.8 0x44B4F0) then ends the pawn's "being revived" state once it's up: no
    // revivers, no Gameplay.Status.DBNOBeingRevived tag, no GameplayCue.Athena.Player.BeingRevivedFromDBNO.
    // ServerDBNOReviveInterrupted_Implementation does exactly that when its last reviver stops, without using the
    // controller it runs on.
    AFortPlayerPawnAthena* AthenaPawn = Cast<AFortPlayerPawnAthena>(this);
    if (!AthenaPawn || bIsDBNO || Natives::AFortPlayerControllerAthena_ServerDBNOReviveInterruptedIndex < 0)
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
}

void AFortPlayerPawn::ServerReviveFromDBNO_ImplementationHK(AFortPlayerPawn* This, AController* EventInstigator)
{
    This->ServerReviveFromDBNO_Implementation(EventInstigator);
}
