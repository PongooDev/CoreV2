#include "pch.h"
#include "Pawns/FortPawn.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Finder.h"
#include "GameFramework/PlayerState.h"
#include "GameThread.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPlayerPawn.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

bool AFortPawn::IsDead() const
{
    Reflection::FParams Params(StaticFunction_IsDead());
    return Params.Call(const_cast<AFortPawn*>(this)) && Params.Take<bool>(L"ReturnValue");
}

AFortWeapon* AFortPawn::EquipWeaponDefinition(const UFortWeaponItemDefinition* WeaponData, const FGuid& ItemEntryGuid)
{
    Reflection::FParams Params(StaticFunction_EquipWeaponDefinition());
    Params.Set(L"WeaponData", const_cast<UFortWeaponItemDefinition*>(WeaponData));
    Params.Set(L"ItemEntryGuid", ItemEntryGuid);
    return Params.Call(this) ? Params.Take<AFortWeapon*>(L"ReturnValue") : nullptr;
}

void AFortPawn::TriggerFeedbackEvent(FName EventName, AFortPawn* InstigatorPawn, AFortPawn* Recipient, float OverriddenDelay)
{
    Reflection::FParams Params(StaticFunction_TriggerFeedbackEvent());
    Params.Set(L"EventName", EventName);
    Params.Set(L"InstigatorPawn", InstigatorPawn);
    Params.Set(L"Recipient", Recipient);
    Params.Set(L"OverriddenDelay", OverriddenDelay);
    Params.Call(this);
}

namespace
{
    float CallFloatGetter(const AFortPawn* Pawn, UFunction* Function)
    {
        Reflection::FParams Params(Function);
        return Params.Call(const_cast<AFortPawn*>(Pawn)) ? Params.Take<float>(L"ReturnValue") : 0.0f;
    }
}

float AFortPawn::GetHealth() const
{
    return CallFloatGetter(this, StaticFunction_GetHealth());
}

float AFortPawn::GetMaxHealth() const
{
    return CallFloatGetter(this, StaticFunction_GetMaxHealth());
}

float AFortPawn::GetShield() const
{
    return CallFloatGetter(this, StaticFunction_GetShield());
}

float AFortPawn::GetMaxShield() const
{
    return CallFloatGetter(this, StaticFunction_GetMaxShield());
}

namespace
{
    bool CallFloatSetter(AFortPawn* Pawn, UFunction* Function, float Value)
    {
        Reflection::FParams Params(Function);
        return Function && Params.Set(L"NewHealthVal", Value) && Params.Call(Pawn);
    }
}

bool AFortPawn::SetHealth(float NewHealth)
{
    return CallFloatSetter(this, FindStaticFunction_SetHealth(), NewHealth);
}

bool AFortPawn::SetMaxHealth(float NewMaxHealth)
{
    return CallFloatSetter(this, FindStaticFunction_SetMaxHealth(), NewMaxHealth);
}

bool AFortPawn::ForceMaxHealth(float NewMaxHealth)
{
    // The aggregator works the current value out again only when MaxHealth's effects or base change; the property
    // (current value) replicates with the health set, and the client's OnRep_MaxHealth updates the HUD.
    UFortHealthSet* Set = HealthSet;
    if (!Set)
    {
        return false;
    }
    FFortGameplayAttributeData& MaxHealthData = Set->MaxHealth;
    // Health is clamped to its own Maximum whenever it changes (1.9 0x476AF0), which only MaxHealth's aggregator moves
    // (0x44DD70): a maximum set here would leave it at the old one, so SetHealth (and SetHealthPercent) couldn't heal
    // past it and the HUD's health bar showed 100 of 1000 (#155). It follows the new maximum, and a health above it
    // comes down to it, as the game's own change of maximum does.
    FFortGameplayAttributeData& HealthData = Set->Health;
    bool bChanged = false;
    if (FFortGameplayAttributeData::HasMaximum() && std::fabs(HealthData.Maximum - NewMaxHealth) > 0.5f)
    {
        HealthData.Maximum = NewMaxHealth;
        bChanged = true;
    }
    if (HealthData.CurrentValue > NewMaxHealth || HealthData.BaseValue > NewMaxHealth)
    {
        HealthData.CurrentValue = std::min(static_cast<float>(HealthData.CurrentValue), NewMaxHealth);
        HealthData.BaseValue = std::min(static_cast<float>(HealthData.BaseValue), NewMaxHealth);
        bChanged = true;
    }
    if (std::fabs(MaxHealthData.CurrentValue - NewMaxHealth) > 0.5f)
    {
        MaxHealthData.CurrentValue = NewMaxHealth;
        bChanged = true;
    }
    if (bChanged)
    {
        ForceNetUpdate();
        if (APlayerState* State = PlayerState)
        {
            State->ForceNetUpdate();
        }
    }
    return true;
}

bool AFortPawn::EquipDefaultWeapon()
{
    if (!Finder::ResolveAbilityWeaponLive(CurrentWeaponListOffset(), AFortPlayerPawn::LastEquippedWeaponGUIDOffset()))
    {
        return false;
    }
    return CallVirtual<bool>(this, Natives::AFortPawn_EquipDefaultWeaponIndex);
}

void AFortPawn::RemoveAbilityWeapon(AFortWeapon* Weapon)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Weapon)
    {
        return;
    }

    TArray<AFortWeapon*>& Weapons = CurrentWeaponList;
    for (int32_t Index = Weapons.Num() - 1; Index >= 0; --Index)
    {
        if (Weapons[Index] == Weapon)
        {
            Weapons.RemoveAt(Index);
        }
    }

    if (static_cast<AFortWeapon*>(CurrentWeapon) == Weapon)
    {
        AFortPlayerPawn* PlayerPawn = Cast<AFortPlayerPawn>(this);
        if (!PlayerPawn || !PlayerPawn->EquipLastWeapon())
        {
            EquipDefaultWeapon();
        }
    }
    Weapon->K2_DestroyActor();
#endif
}

ptrdiff_t AFortPawn::CurrentWeaponListOffset()
{
    return Member_CurrentWeaponList.GetOffsetChecked();
}

ptrdiff_t AFortPawn::IsDBNOOffset()
{
    return Member_bIsDBNO.GetOffsetChecked();
}

void FFortPawnStats::ScaleSpeeds(float Scale)
{
    SpeedWalk = SpeedWalk * Scale;
    SpeedRun = SpeedRun * Scale;
    SpeedSprint = SpeedSprint * Scale;
    if (Member_SpeedCrouchedRun.IsAvailable() && Member_SpeedCrouchedSprint.IsAvailable())
    {
        SpeedCrouchedRun = SpeedCrouchedRun * Scale;
        SpeedCrouchedSprint = SpeedCrouchedSprint * Scale;
    }
}

std::vector<FFortGameplayAttributeData*> UFortMovementSet::SpeedsBesidesRun()
{
    std::vector<FFortGameplayAttributeData*> Speeds{ &this->WalkSpeed, &this->SprintSpeed };
    if (Member_CrouchedRunSpeed.IsAvailable() && Member_CrouchedSprintSpeed.IsAvailable())
    {
        Speeds.push_back(&this->CrouchedRunSpeed);
        Speeds.push_back(&this->CrouchedSprintSpeed);
    }
    return Speeds;
}

namespace
{
    // What AFortPawn::SetCheatMoveSpeed changed on a pawn, to give it back.
    struct FCheatMoveSpeed
    {
        TWeakObjectPtr<AFortPawn> Pawn;
        // Battle Royale: the pawn's own PawnStatHandle, and the table (the pawn's outer) with the row it reads meanwhile.
        UDataTable* OwnTable = nullptr;
        FName OwnRowName;
        TWeakObjectPtr<UDataTable> SpeedTable;
        // Save the World: its movement set (its player state's, which outlives the pawn) and the walk, run and sprint
        // speeds the set had, base and current.
        TWeakObjectPtr<UFortMovementSet> MovementSet;
        float OwnSpeeds[3][2] = {};
    };

    std::vector<FCheatMoveSpeed> CheatMoveSpeeds;
    bool bWatchingCheatMoveSpeeds = false;

    std::array<FFortGameplayAttributeData*, 3> MovementSpeeds(UFortMovementSet* Set)
    {
        return { &Set->WalkSpeed, &Set->RunSpeed, &Set->SprintSpeed };
    }

    void RestoreMovementSet(const FCheatMoveSpeed& Entry)
    {
        if (UFortMovementSet* Set = Entry.MovementSet.Get())
        {
            const std::array<FFortGameplayAttributeData*, 3> Speeds = MovementSpeeds(Set);
            for (size_t Index = 0; Index < Speeds.size(); ++Index)
            {
                Speeds[Index]->BaseValue = Entry.OwnSpeeds[Index][0];
                Speeds[Index]->CurrentValue = Entry.OwnSpeeds[Index][1];
            }
        }
    }

    // Gives the pawn its own speeds back and forgets it.
    void Forget(AFortPawn* Pawn, FCheatMoveSpeed* State)
    {
        RestoreMovementSet(*State);
        UDataTable* const SpeedTable = State->SpeedTable.Get();
        if (SpeedTable && static_cast<UDataTable*>(Pawn->PawnStatHandle.DataTable) == SpeedTable)
        {
            Pawn->PawnStatHandle.DataTable = State->OwnTable;
            Pawn->PawnStatHandle.RowName = State->OwnRowName;
        }
        CheatMoveSpeeds.erase(CheatMoveSpeeds.begin() + (State - CheatMoveSpeeds.data()));
    }

    // A dead pawn's speed table goes with it, but its player state keeps the movement set: that gets its speeds back.
    void ForgetDeadPawns()
    {
        std::erase_if(CheatMoveSpeeds, [](const FCheatMoveSpeed& Entry)
        {
            if (Entry.Pawn.IsValid())
            {
                return false;
            }
            RestoreMovementSet(Entry);
            return true;
        });
    }

    void WatchCheatMoveSpeeds()
    {
        ForgetDeadPawns();
        const bool bAnyMovementSet = std::any_of(CheatMoveSpeeds.begin(), CheatMoveSpeeds.end(), [](const FCheatMoveSpeed& Entry) { return Entry.MovementSet.IsValid(); });
        bWatchingCheatMoveSpeeds = bAnyMovementSet && GameThread::Enqueue(&WatchCheatMoveSpeeds);
    }

    float SetMovementSetSpeed(AFortPawn* Pawn, UFortMovementSet* Set, FCheatMoveSpeed* State, float Speed)
    {
        const float OwnSpeed = State ? State->OwnSpeeds[1][1] : Set->RunSpeed.CurrentValue;
        if (Speed <= 0.0f || OwnSpeed <= 0.0f)
        {
            if (State)
            {
                Forget(Pawn, State);
            }
            return OwnSpeed;
        }

        const std::array<FFortGameplayAttributeData*, 3> Speeds = MovementSpeeds(Set);
        if (!State)
        {
            State = &CheatMoveSpeeds.emplace_back();
            State->Pawn = TWeakObjectPtr<AFortPawn>(Pawn);
            State->MovementSet = TWeakObjectPtr<UFortMovementSet>(Set);
            for (size_t Index = 0; Index < Speeds.size(); ++Index)
            {
                State->OwnSpeeds[Index][0] = Speeds[Index]->BaseValue;
                State->OwnSpeeds[Index][1] = Speeds[Index]->CurrentValue;
            }
        }
        // The base too: a gameplay effect on a speed works its current value out from the base again.
        const float Scale = Speed / OwnSpeed;
        for (size_t Index = 0; Index < Speeds.size(); ++Index)
        {
            Speeds[Index]->BaseValue = State->OwnSpeeds[Index][0] * Scale;
            Speeds[Index]->CurrentValue = State->OwnSpeeds[Index][1] * Scale;
        }
        if (!bWatchingCheatMoveSpeeds)
        {
            bWatchingCheatMoveSpeeds = GameThread::Enqueue(&WatchCheatMoveSpeeds);
        }
        return OwnSpeed;
    }

    float SetStatsRowSpeed(AFortPawn* Pawn, FCheatMoveSpeed* State, float Speed)
    {
        UDataTable* const OwnTable = State ? State->OwnTable : static_cast<UDataTable*>(Pawn->PawnStatHandle.DataTable);
        const FName OwnRowName = State ? State->OwnRowName : static_cast<FName>(Pawn->PawnStatHandle.RowName);
        const FFortPawnStats* const OwnRow = OwnTable ? OwnTable->FindRow<FFortPawnStats>(OwnRowName, L"AFortPawn::SetCheatMoveSpeed", false) : nullptr;
        if (Speed <= 0.0f)
        {
            if (State)
            {
                Forget(Pawn, State);
            }
            return OwnRow ? static_cast<float>(OwnRow->SpeedRun) : 0.0f;
        }

        // Without a row the pawn reads FFortPawnStats' defaults (1.7.2 0x40A720), which a new row starts with too.
        UScriptStruct* const RowStruct = OwnRow ? static_cast<UScriptStruct*>(OwnTable->RowStruct) : FFortPawnStats::StaticStruct();
        const FName RowName = OwnRowName.IsNone() ? FName(L"CoreCheatMoveSpeed") : OwnRowName;
        UDataTable* SpeedTable = State ? State->SpeedTable.Get() : nullptr;
        if (!SpeedTable && RowStruct)
        {
            SpeedTable = static_cast<UDataTable*>(StaticConstructObject_Internal(UDataTable::StaticClass(), Pawn, FName(), RF_Transient));
            if (SpeedTable)
            {
                SpeedTable->RowStruct = RowStruct;
                SpeedTable->AddRow(RowName);
            }
            if (State)
            {
                State->SpeedTable = TWeakObjectPtr<UDataTable>(SpeedTable);
            }
        }
        uint8_t* const Row = SpeedTable ? SpeedTable->FindRowUnchecked(RowName) : nullptr;
        if (!Row)
        {
            Log::Error(L"AFortPawn::SetCheatMoveSpeed: no stats row for {}", Pawn->GetName());
            return 0.0f;
        }

        // From the pawn's own row every time, so a new speed doesn't scale the last one.
        if (OwnRow)
        {
            RowStruct->CopyScriptStruct(Row, OwnRow);
        }
        else
        {
            RowStruct->InitializeStruct(Row);
        }
        FFortPawnStats* const Stats = reinterpret_cast<FFortPawnStats*>(Row);
        const float OwnSpeed = Stats->SpeedRun;
        if (OwnSpeed <= 0.0f)
        {
            if (State)
            {
                Forget(Pawn, State);
            }
            return 0.0f;
        }
        Stats->ScaleSpeeds(Speed / OwnSpeed);

        if (!State)
        {
            State = &CheatMoveSpeeds.emplace_back();
            State->Pawn = TWeakObjectPtr<AFortPawn>(Pawn);
            State->OwnTable = OwnTable;
            State->OwnRowName = OwnRowName;
            State->SpeedTable = TWeakObjectPtr<UDataTable>(SpeedTable);
        }
        Pawn->PawnStatHandle.DataTable = SpeedTable;
        Pawn->PawnStatHandle.RowName = RowName;
        return OwnSpeed;
    }
}

namespace
{
    // "cheat speed" (#155): a pawn's speed multiplier, and the flying speed it has meanwhile (0: none).
    struct FCheatSpeedMultiplier
    {
        TWeakObjectPtr<AFortPawn> Pawn;
        float Multiplier = 1.0f;
        float FlySpeed = 0.0f;
    };
    std::vector<FCheatSpeedMultiplier> CheatSpeedMultipliers;

    FCheatSpeedMultiplier* FindSpeedMultiplier(const AFortPawn* Pawn)
    {
        std::erase_if(CheatSpeedMultipliers, [](const FCheatSpeedMultiplier& Entry) { return !Entry.Pawn.IsValid(); });
        auto Found = std::find_if(CheatSpeedMultipliers.begin(), CheatSpeedMultipliers.end(), [Pawn](const FCheatSpeedMultiplier& Entry) { return Entry.Pawn.Get() == Pawn; });
        return Found != CheatSpeedMultipliers.end() ? &*Found : nullptr;
    }

    // The pawn's speeds at Speed (its running speed; 0: its own), from its own every time.
    float ApplyCheatMoveSpeed(AFortPawn* Pawn, float Speed)
    {
        ForgetDeadPawns();
        auto Found = std::find_if(CheatMoveSpeeds.begin(), CheatMoveSpeeds.end(), [Pawn](const FCheatMoveSpeed& Entry) { return Entry.Pawn.Get() == Pawn; });
        FCheatMoveSpeed* const State = Found != CheatMoveSpeeds.end() ? &*Found : nullptr;
        UFortMovementSet* const Set = Pawn->MovesWithMovementSet() ? static_cast<UFortMovementSet*>(Pawn->MovementSet) : nullptr;
        return Set ? SetMovementSetSpeed(Pawn, Set, State, Speed) : SetStatsRowSpeed(Pawn, State, Speed);
    }

    // Its own speeds, then those times its multiplier.
    float ApplyOwnSpeedTimes(AFortPawn* Pawn, float Multiplier)
    {
        const float OwnSpeed = ApplyCheatMoveSpeed(Pawn, 0.0f);
        if (Multiplier != 1.0f && OwnSpeed > 0.0f)
        {
            ApplyCheatMoveSpeed(Pawn, OwnSpeed * Multiplier);
        }
        return OwnSpeed;
    }

    // "cheat speed" on a Battle Royale pawn with a movement set (#155): 1.9's AFortPlayerPawnAthena::GetMovementSpeed
    // (0x4219D0) reads the set's WalkSpeed, RunSpeed, SprintSpeed and crouched speeds whenever the pawn has one, and
    // its stats row only without. The move speed effect multiplies RunSpeed alone, so walking was faster and
    // sprinting wasn't. The server scales the set's other speeds by the multiplier (base and current: an effect on one
    // works its value out from the base again), and they replicate to the owning client with the set. Builds whose pawn
    // reads the row never read these, which the row scaling covers.
    struct FScaledSetSpeeds
    {
        TWeakObjectPtr<AFortPawn> Pawn;
        TWeakObjectPtr<UFortMovementSet> Set;
        // Base and current value of each of the set's SpeedsBesidesRun before they were scaled.
        std::vector<std::pair<float, float>> Own;
    };
    std::vector<FScaledSetSpeeds> ScaledSetSpeeds;

    void RestoreSetSpeeds(const FScaledSetSpeeds& Entry)
    {
        UFortMovementSet* Set = Entry.Set.Get();
        const std::vector<FFortGameplayAttributeData*> Speeds = Set ? Set->SpeedsBesidesRun() : std::vector<FFortGameplayAttributeData*>();
        for (size_t Index = 0; Index < Speeds.size() && Index < Entry.Own.size(); ++Index)
        {
            Speeds[Index]->BaseValue = Entry.Own[Index].first;
            Speeds[Index]->CurrentValue = Entry.Own[Index].second;
        }
    }

    void ScaleSetSpeedsBesidesRun(AFortPawn* Pawn, float Multiplier)
    {
        // A pawn that is gone gives its set's speeds back, and so does the one scaled before on the same set (a dying
        // pawn whose player state's set a new pawn takes), so the new scaling starts from the set's own speeds.
        UFortMovementSet* const Set = Pawn->MovementSet;
        std::erase_if(ScaledSetSpeeds, [Pawn, Set](const FScaledSetSpeeds& Entry)
        {
            if (Entry.Pawn.IsValid() && Entry.Pawn.Get() != Pawn && (!Set || Entry.Set.Get() != Set))
            {
                return false;
            }
            RestoreSetSpeeds(Entry);
            return true;
        });
        if (!Set || Multiplier == 1.0f)
        {
            return;
        }
        FScaledSetSpeeds& Entry = ScaledSetSpeeds.emplace_back();
        Entry.Pawn = TWeakObjectPtr<AFortPawn>(Pawn);
        Entry.Set = TWeakObjectPtr<UFortMovementSet>(Set);
        for (FFortGameplayAttributeData* Speed : Set->SpeedsBesidesRun())
        {
            Entry.Own.emplace_back(Speed->BaseValue, Speed->CurrentValue);
            Speed->BaseValue = Speed->BaseValue * Multiplier;
            Speed->CurrentValue = Speed->CurrentValue * Multiplier;
        }
    }
}

bool AFortPawn::MovesWithMovementSet() const
{
    return !IsA(AFortPlayerPawnAthena::StaticClass()) && static_cast<UFortMovementSet*>(MovementSet);
}

float AFortPawn::SetCheatSpeedMultiplier(float Multiplier)
{
    FCheatSpeedMultiplier* Entry = FindSpeedMultiplier(this);
    if (!Entry && Multiplier != 1.0f)
    {
        Entry = &CheatSpeedMultipliers.emplace_back();
        Entry->Pawn = TWeakObjectPtr<AFortPawn>(this);
    }
    const float FlySpeed = Entry ? Entry->FlySpeed : 0.0f;
    if (Entry)
    {
        Entry->Multiplier = Multiplier;
    }
    if (Entry && Multiplier == 1.0f && FlySpeed <= 0.0f)
    {
        CheatSpeedMultipliers.erase(CheatSpeedMultipliers.begin() + (Entry - CheatSpeedMultipliers.data()));
    }
    // A Battle Royale pawn's movement set: on the server, which replicates it.
    if (Role == ROLE_Authority && !MovesWithMovementSet())
    {
        ScaleSetSpeedsBesidesRun(this, Multiplier);
    }
    // Flying at a speed of its own: the multiplier waits for the walk.
    if (FlySpeed > 0.0f)
    {
        return ApplyCheatMoveSpeed(this, FlySpeed);
    }
    return ApplyOwnSpeedTimes(this, Multiplier);
}

float AFortPawn::GetCheatSpeedMultiplier() const
{
    const FCheatSpeedMultiplier* Entry = FindSpeedMultiplier(this);
    return Entry ? Entry->Multiplier : 1.0f;
}

float AFortPawn::SetCheatMoveSpeed(float Speed)
{
    // UFortMovementComp_Character::GetMaxSpeed (1.7.2 0x6A4250, 1.8 0x6D8FE0, 1.8.1 0x6D1290) never reads MaxFlySpeed
    // for a Fort pawn: flying or not, it returns the pawn's GetMovementSpeed for its movement style (run, walk or
    // sprint). AFortPawn's reads the movement set when the pawn has one (a Save the World player's, its player
    // state's); Battle Royale's pawn reads its PawnStatHandle row instead. Nothing in that row replicates, so the
    // server and the owning client each change their own copy: the movement set's speeds, or the row the pawn reads (a
    // copy of its own with the speeds scaled, in a table of the pawn's).
    //
    // "cheat speed" (#155): without a flying speed the pawn moves at its own speeds times its speed multiplier. A flying
    // speed is kept too, so a multiplier set meanwhile (a Battle Royale client learns it late) doesn't replace it.
    FCheatSpeedMultiplier* Entry = FindSpeedMultiplier(this);
    if (!Entry && Speed > 0.0f)
    {
        Entry = &CheatSpeedMultipliers.emplace_back();
        Entry->Pawn = TWeakObjectPtr<AFortPawn>(this);
    }
    const float Multiplier = Entry ? Entry->Multiplier : 1.0f;
    if (Entry)
    {
        Entry->FlySpeed = Speed > 0.0f ? Speed : 0.0f;
    }
    if (Entry && Speed <= 0.0f && Multiplier == 1.0f)
    {
        CheatSpeedMultipliers.erase(CheatSpeedMultipliers.begin() + (Entry - CheatSpeedMultipliers.data()));
    }
    if (Speed > 0.0f || Multiplier == 1.0f)
    {
        return ApplyCheatMoveSpeed(this, Speed);
    }
    return ApplyOwnSpeedTimes(this, Multiplier);
}
