#include "pch.h"
#include "Athena/AI/FortAthenaAIBotController.h"
#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "Athena/AI/FortServerBotManagerAthena.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/DataTable.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "Heroes/FortHeroType.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortCheatManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace
{
    // EMovementMode: walking on the ground.
    constexpr uint8_t MoveWalking = 1;

    FVector LocationOf(const AActor* Actor)
    {
        return Actor ? Actor->GetTransform().GetLocation() : FVector();
    }

    float Dist2D(const FVector& A, const FVector& B)
    {
        return std::sqrt((A.X - B.X) * (A.X - B.X) + (A.Y - B.Y) * (A.Y - B.Y));
    }

    FVector Direction2D(const FVector& From, const FVector& To)
    {
        const float Length = Dist2D(From, To);
        return Length > 1.0f ? FVector((To.X - From.X) / Length, (To.Y - From.Y) / Length, 0.0f) : FVector();
    }

    float RandomRange(float Min, float Max)
    {
        return Min + (Max - Min) * FMath::FRand();
    }

    std::wstring NameOf(const UObject* Object)
    {
        return Object ? Object->GetName().ToStdString() : std::wstring(L"none");
    }

    // The outfits bots wear: Battle Royale's default characters (8.30 reads the bot's from its customization loadout).
    UAthenaCharacterItemDefinition* RandomCharacter()
    {
        static std::vector<UAthenaCharacterItemDefinition*> Characters;
        static bool bLoaded = false;
        if (!bLoaded)
        {
            bLoaded = true;
            for (const wchar_t* Path : { L"/Game/Athena/Items/Cosmetics/Characters/CID_001_Athena_Commando_F_Default.CID_001_Athena_Commando_F_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_002_Athena_Commando_F_Default.CID_002_Athena_Commando_F_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_003_Athena_Commando_F_Default.CID_003_Athena_Commando_F_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_004_Athena_Commando_F_Default.CID_004_Athena_Commando_F_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_005_Athena_Commando_M_Default.CID_005_Athena_Commando_M_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_006_Athena_Commando_M_Default.CID_006_Athena_Commando_M_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_007_Athena_Commando_M_Default.CID_007_Athena_Commando_M_Default",
                     L"/Game/Athena/Items/Cosmetics/Characters/CID_008_Athena_Commando_M_Default.CID_008_Athena_Commando_M_Default" })
            {
                if (UAthenaCharacterItemDefinition* Character = LoadObject<UAthenaCharacterItemDefinition>(nullptr, Path))
                {
                    Characters.push_back(Character);
                }
            }
            Log::Info(L"Battle Royale bots: {} default outfits for the bots", Characters.size());
        }
        return Characters.empty() ? nullptr : Characters[static_cast<size_t>(FMath::Rand()) % Characters.size()];
    }

    UAthenaGliderItemDefinition* DefaultGlider()
    {
        static UAthenaGliderItemDefinition* Glider = LoadObject<UAthenaGliderItemDefinition>(nullptr, L"/Game/Athena/Items/Cosmetics/Gliders/DefaultGlider.DefaultGlider");
        return Glider;
    }

    UAthenaPickaxeItemDefinition* DefaultPickaxe()
    {
        static UAthenaPickaxeItemDefinition* Pickaxe = LoadObject<UAthenaPickaxeItemDefinition>(nullptr, L"/Game/Athena/Items/Cosmetics/Pickaxes/DefaultPickaxe.DefaultPickaxe");
        return Pickaxe;
    }

    // The players' pawn class (PlayerPawn_Athena_C, 8.30's UFortServerBotManagerAthena::BotPlayerPawnClass). The game
    // mode gives an AI controller the engine's DefaultPawn (GetDefaultPawnClassForController only answers players).
    UClass* BotPawnClass(AFortGameModeAthena* GameMode, AAIController* AIController)
    {
        static UClass* Cached = nullptr;
        if (Cached)
        {
            return Cached;
        }
        UClass* AthenaPawn = AFortPlayerPawnAthena::StaticClass();
        for (TObjectIterator<UClass> It; It && AthenaPawn; ++It)
        {
            UClass* Class = *It;
            if (Class->IsChildOf(AthenaPawn) && Class->GetName().ToStdString() == L"PlayerPawn_Athena_C")
            {
                Cached = Class;
                break;
            }
        }
        if (!Cached)
        {
            UClass* Fallback = GameMode->GetDefaultPawnClassForController(AIController);
            Cached = Fallback && AthenaPawn && Fallback->IsChildOf(AthenaPawn) ? Fallback : nullptr;
        }
        Log::Info(L"Battle Royale bots: the bots' pawn class is {}", NameOf(Cached));
        return Cached;
    }

    // A number from the weapon's stats row (FFortBaseWeaponStats / FFortRangedWeaponStats), by its property's name.
    bool ReadStat(UFortWeaponItemDefinition* Definition, const wchar_t* Name, float& OutValue)
    {
        const FDataTableRowHandle& StatHandle = Definition->WeaponStatHandle;
        UDataTable* Table = StatHandle.DataTable;
        uint8_t* Row = Table ? Table->FindRowUnchecked(StatHandle.RowName) : nullptr;
        UScriptStruct* RowStruct = Row ? static_cast<UScriptStruct*>(Table->RowStruct) : nullptr;
        UProperty* Property = RowStruct ? RowStruct->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        if (!Property || Property->ElementSize != 4)
        {
            return false;
        }
        const uint8_t* Value = Row + Property->GetOffset_ForInternal();
        const bool bInt = Property->GetClass()->GetName().ToStdString() == L"IntProperty";
        OutValue = bInt ? static_cast<float>(*reinterpret_cast<const int32_t*>(Value)) : *reinterpret_cast<const float*>(Value);
        return true;
    }

    void ReadWeaponStats(FFortAthenaAIBotWeapon& Weapon)
    {
        float Value = 0.0f;
        if (ReadStat(Weapon.WeaponDefinition, L"DmgPB", Value) && Value > 0.0f)
        {
            Weapon.Damage = Value;
        }
        if (ReadStat(Weapon.WeaponDefinition, L"BulletsPerCartridge", Value) && Value >= 1.0f)
        {
            Weapon.Pellets = static_cast<int32_t>(Value);
        }
        if (ReadStat(Weapon.WeaponDefinition, L"FiringRate", Value) && Value > 0.0f)
        {
            Weapon.FiringRate = Value;
        }
    }

    int32_t BulletsPerClip(AFortWeapon* Weapon)
    {
        UFunction* Function = Weapon ? AFortWeapon::FindStaticFunction_GetBulletsPerClip() : nullptr;
        Reflection::FParams Params(Function, true);
        return Function && Params.Call(Weapon) ? Params.Take<int32_t>(L"ReturnValue") : 0;
    }

    // Who killed the pawn: the newest of its damagers, through the weapon to its owner, as the death report names the
    // killer (8.30's bots learn it from the pawn's death delegate, which Core can't bind).
    AFortPlayerStateAthena* KillerOf(AFortPawn* Pawn)
    {
        TStructArray<FDamagerInfo>& Damagers = Pawn->Damagers;
        for (int32_t Index = Damagers.Num() - 1; Index >= 0; --Index)
        {
            AActor* Causer = Damagers[Index].DamageCauser;
            AActor* Owner = Causer;
            for (int32_t Depth = 0; Owner && Depth < 3 && !Cast<APawn>(reinterpret_cast<UObject*>(Owner)); ++Depth)
            {
                Owner = Owner->GetOwner();
            }
            APawn* Killer = Owner ? Cast<APawn>(reinterpret_cast<UObject*>(Owner)) : nullptr;
            if (Killer && Killer != reinterpret_cast<APawn*>(Pawn))
            {
                return Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Killer->PlayerState)));
            }
        }
        return nullptr;
    }
}

AFortAthenaAIBotController::AFortAthenaAIBotController(UFortServerBotManagerAthena* InManager, AAIController* InController, int32_t InBotIndex, uint8_t InTeam)
    : Manager(InManager)
    , Controller(InController)
    , BotIndex(InBotIndex)
    , Team(InTeam)
{
}

AAIController* AFortAthenaAIBotController::GetController() const
{
    return Controller.Get();
}

AFortPlayerPawnAthena* AFortAthenaAIBotController::GetPawn() const
{
    return PlayerBotPawn.Get();
}

AFortPlayerStateAthena* AFortAthenaAIBotController::GetPlayerState() const
{
    AAIController* AIController = Controller.Get();
    return AIController ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(AIController->PlayerState))) : nullptr;
}

void AFortAthenaAIBotController::AssignDisplayName(const std::wstring& Name)
{
    // 21.40's AFortAthenaAIBotController::AssignDisplayName. The player state is the game's own: it's a bot
    // (APlayerState::bIsABot) on its team, and its name shows in the kill feed and above its head.
    BotName = Name;
    AFortPlayerStateAthena* State = GetPlayerState();
    if (!State)
    {
        return;
    }
    State->PlayerName = FString(Name.c_str());
    if (APlayerState::HasPlayerNamePrivate())
    {
        State->PlayerNamePrivate = FString(Name.c_str());
    }
    State->bIsABot = true;
    State->TeamIndex = EFortTeam{ Team };
    State->ForceNetUpdate();
}

void AFortAthenaAIBotController::InitializeSkills(int32_t Level)
{
    // 21.40's AFortAthenaAIBotController::InitializeSkills.
    AIBotSkills = UFortServerBotManagerAthena::SkillsForLevel(Level);
}

bool AFortAthenaAIBotController::SpawnPawnAt(const FVector& Location, float Yaw, bool bSkydive)
{
#if WITH_SERVER_CODE
    AAIController* AIController = Controller.Get();
    AFortGameModeAthena* GameMode = Manager->GetGameMode();
    UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
    UClass* PawnClass = AIController && World ? BotPawnClass(GameMode, AIController) : nullptr;
    if (!PawnClass)
    {
        Log::Error(L"Battle Royale bots: no pawn class for {}", BotName);
        return false;
    }
    FTransform SpawnTransform;
    const float HalfYaw = Yaw * (3.1415926535897932f / 360.0f);
    SpawnTransform.Rotation.Z = std::sin(HalfYaw);
    SpawnTransform.Rotation.W = std::cos(HalfYaw);
    SpawnTransform.SetLocation(Location);
    FActorSpawnParameters SpawnInfo;
    SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
    SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AFortPlayerPawnAthena* Pawn = Cast<AFortPlayerPawnAthena>(World->SpawnActor(PawnClass, &SpawnTransform, SpawnInfo));
    if (!Pawn)
    {
        Log::Error(L"Battle Royale bots: couldn't spawn {}'s {}", BotName, PawnClass->GetName().ToStdString());
        return false;
    }

    // The bot's look (8.30: its customization loadout): a default outfit, glider and pickaxe, on the pawn (its loadout
    // replicates: OnRep_CustomizationLoadout) and the player state (the outfit's hero gives it its character parts).
    if (Pawn->HasCustomizationLoadout())
    {
        FFortAthenaLoadout& Loadout = Pawn->CustomizationLoadout;
        UAthenaCharacterItemDefinition* Character = RandomCharacter();
        Loadout.Character = Character;
        if (FFortAthenaLoadout::HasGlider())
        {
            Loadout.Glider = DefaultGlider();
        }
        Loadout.Pickaxe = DefaultPickaxe();
        AFortPlayerStateAthena* State = GetPlayerState();
        UFortHeroType* HeroType = Character ? static_cast<UFortHeroType*>(Character->HeroDefinition) : nullptr;
        if (State && HeroType && AFortPlayerState::CanSetHeroType())
        {
            if (UFortHero* Hero = UFortCheatManager::CreateLookHero(HeroType))
            {
                State->SetHeroType(Hero);
            }
        }
    }

    AIController->Possess(Pawn);
    PlayerBotPawn = Pawn;
    // A player pawn takes its ability system (its player state's) in OnRep_PlayerState (AFortPlayerPawn's, 3.0
    // 0xDF4E10), which on the server only AFortPlayerController's possession runs: an AI controller's doesn't, and a
    // weapon equipped without one crashes giving its abilities.
    if (!static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent))
    {
        Pawn->OnRep_PlayerState();
    }
    if (!static_cast<UAbilitySystemComponent*>(Pawn->AbilitySystemComponent))
    {
        Log::Error(L"Battle Royale bots: {}'s pawn has no ability system, it can't hold weapons", BotName);
        AIController->UnPossess();
        Pawn->K2_DestroyActor();
        PlayerBotPawn = nullptr;
        return false;
    }
    ApplyStartingHealth();
    Pawn->ForceNetUpdate();

    // The harvesting tool first (Core-held, 8.30's weapon list keeps the bot's weapons on its controller).
    bPickaxeEquipped = false;
    EquippedIndex = -1;
    EquippedWeapon = nullptr;
    UAthenaPickaxeItemDefinition* Pickaxe = DefaultPickaxe();
    if (UFortWeaponItemDefinition* PickaxeWeapon = Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr)
    {
        EquippedWeapon = Pawn->EquipWeaponDefinition(PickaxeWeapon, FGuid::NewGuid());
        bPickaxeEquipped = EquippedWeapon.Get() != nullptr;
    }
    if (!WeaponList.empty())
    {
        EquipWeapon(BestWeaponIndex());
    }

    if (bSkydive && Natives::AFortPlayerPawnAthena_BeginSkydiving)
    {
        Natives::AFortPlayerPawnAthena_BeginSkydiving(Pawn, true);
    }
    return true;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::ApplyStartingHealth()
{
    // The admin panel's starting health and shield (8.30 has no bot health field: its bot pawn class's default).
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FFortAthenaBotSettings& Settings = Manager->GetSettings();
    if (!Pawn)
    {
        return;
    }
    Pawn->SetMaxHealth(100.0f);
    Pawn->ForceMaxHealth(100.0f);
    Pawn->SetHealth(Settings.StartingHealth);
    Pawn->ForceMaxShield(100.0f);
    Pawn->SetShield(Settings.StartingShield);
}

void AFortAthenaAIBotController::EnterAircraft(float InJumpTime, float InLatestJumpTime)
{
    // The bus has left: like a player's, the warmup pawn goes (StartAircraftPhase destroys theirs) and the bot rides it.
    StopFiring();
    if (AFortPlayerPawnAthena* Pawn = GetPawn())
    {
        if (AAIController* AIController = Controller.Get())
        {
            AIController->UnPossess();
        }
        Pawn->K2_DestroyActor();
    }
    PlayerBotPawn = nullptr;
    // The warmup's pickups don't come along (ResetToStartingItems takes a player's).
    WeaponList.clear();
    Ammo.clear();
    State = EFortAthenaAIBotState::InAircraft;
    JumpTime = InJumpTime;
    LatestJumpTime = InLatestJumpTime;
}

void AFortAthenaAIBotController::Destroy()
{
    StopFiring();
    if (AFortPlayerPawnAthena* Pawn = GetPawn())
    {
        Pawn->K2_DestroyActor();
    }
    if (AAIController* AIController = Controller.Get())
    {
        AIController->K2_DestroyActor();
    }
    PlayerBotPawn = nullptr;
    State = EFortAthenaAIBotState::Dead;
}

void AFortAthenaAIBotController::AddKill()
{
    ++Kills;
    if (AFortPlayerStateAthena* PlayerState = GetPlayerState())
    {
        PlayerState->SetKillScore(Kills);
    }
}

void AFortAthenaAIBotController::Tick(float Now, float DeltaSeconds)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    switch (State)
    {
    case EFortAthenaAIBotState::Dead:
        return;
    case EFortAthenaAIBotState::InAircraft:
        JumpOffBus(Now);
        return;
    case EFortAthenaAIBotState::Warmup:
    {
        // A bot killed on the warmup island comes back, as players do.
        const bool bGone = !Pawn || Pawn->IsPendingKill() || Pawn->bIsDying;
        if (bGone)
        {
            if (WarmupRespawnTime < 0.0f)
            {
                WarmupRespawnTime = Now + 3.0f;
            }
            else if (Now >= WarmupRespawnTime)
            {
                WarmupRespawnTime = -1.0f;
                FVector Spot;
                if (Manager->FindGroundNear(Pawn ? LocationOf(Pawn) : LastKnownPosition, 2000.0f, Spot))
                {
                    SpawnPawnAt(FVector(Spot.X, Spot.Y, Spot.Z + 100.0f), RandomRange(0.0f, 360.0f), false);
                }
            }
            return;
        }
        LastKnownPosition = LocationOf(Pawn);
        Wander(Now);
        return;
    }
    default:
        break;
    }

    // Skydiving or on the ground: the pawn's death takes the bot out of the match.
    bool bDead = !Pawn || Pawn->IsPendingKill() || Pawn->bIsDying;
    if (!bDead && Now >= NextDeathCheck)
    {
        NextDeathCheck = Now + 0.5f;
        bDead = Pawn->IsDead();
    }
    if (bDead)
    {
        AFortPlayerStateAthena* BotKiller = Pawn ? Manager->LastBotAttackerOf(Pawn) : nullptr;
        AFortPlayerStateAthena* Killer = Pawn ? KillerOf(Pawn) : nullptr;
        Manager->RemoveFromAliveBots(this, Killer ? Killer : BotKiller);
        return;
    }
    if (State == EFortAthenaAIBotState::Skydiving)
    {
        FreeFalling(Now);
        return;
    }

    LastKnownPosition = LocationOf(Pawn);
    ManageWeapon(Now);
    if (Now >= NextPerceptionTime)
    {
        NextPerceptionTime = Now + 0.25f;
        UpdateCurrentTarget(Now);
    }
    const wchar_t* Intent = L"attack";
    if (!RangeAttack(Now))
    {
        StopFiring();
        Intent = Storm(Now) ? L"storm" : Loot(Now) ? L"loot" : L"wander";
        if (Intent[0] == L'w')
        {
            Wander(Now);
        }
    }
    TryToUnstuck(Now);
    TakeStormDamage(Now);

    if (Now >= NextLogTime)
    {
        NextLogTime = Now + 20.0f;
        AFortPawn* Target = CurrentTarget.Get();
        UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
        const FVector Velocity = Movement ? static_cast<FVector>(Movement->Velocity) : FVector();
        Log::Info(L"Battle Royale bots: {} at ({:.0f}, {:.0f}, {:.0f}) {}s, speed {:.0f}, mode {}, max walk {:.0f}, style {}: {:.0f} health, {:.0f} shield, {} weapons, holding {} ({} loaded), target {}{}, {} shots", BotName,
            LastKnownPosition.X, LastKnownPosition.Y, LastKnownPosition.Z, Intent, Velocity.Size(), Movement ? static_cast<int32_t>(Movement->MovementMode) : -1, Movement ? static_cast<float>(Movement->MaxWalkSpeed) : -1.0f, static_cast<int32_t>(Pawn->CurrentMovementStyle), Pawn->GetHealth(), Pawn->GetShield(),
            WeaponList.size(), EquippedWeapon.Get() ? NameOf(static_cast<UFortWeaponItemDefinition*>(EquippedWeapon.Get()->WeaponData)) : std::wstring(L"nothing"),
            EquippedWeapon.Get() ? static_cast<int32_t>(EquippedWeapon.Get()->AmmoCount) : 0, Target ? NameOf(Target) : std::wstring(L"none"), bHasVisualLineOfSightOnTarget ? L" (in sight)" : L"", ShotsFired);
    }
#endif
}

void AFortAthenaAIBotController::JumpOffBus(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_JumpOffBus: at the bot's moment along the path (EnterAircraft picked it), or
    // when the bus is about to leave the drop zone, the bot leaves the aircraft where it is, skydiving from the bus as a
    // player does (ExitAircraft's AFortPlayerPawnAthena::BeginSkydiving).
    if (Now < JumpTime && Now < LatestJumpTime)
    {
        return;
    }
    AActor* Aircraft = Manager->GetAircraft();
    const FVector BusLocation = Aircraft ? LocationOf(Aircraft) : LastKnownPosition + FVector(0.0f, 0.0f, 15000.0f);
    const FVector Start(BusLocation.X + RandomRange(-300.0f, 300.0f), BusLocation.Y + RandomRange(-300.0f, 300.0f), BusLocation.Z - 500.0f);

    // Where to land: loot near the bus's path, or a spot on the ground below it (21.40's _FreeFalling picks a POI).
    const std::vector<FFortAthenaBotLoot>& LootList = Manager->GetLoot();
    std::vector<FVector> Near;
    for (const FFortAthenaBotLoot& Entry : LootList)
    {
        if (Entry.bWeapon && Dist2D(Entry.Location, Start) < 20000.0f)
        {
            Near.push_back(Entry.Location);
        }
    }
    if (!Near.empty())
    {
        DropTarget = Near[static_cast<size_t>(FMath::Rand()) % Near.size()];
    }
    else if (!Manager->FindGroundNear(Start, 15000.0f, DropTarget))
    {
        DropTarget = FVector(Start.X, Start.Y, 0.0f);
    }
    if (!SpawnPawnAt(Start, RandomRange(0.0f, 360.0f), true))
    {
        JumpTime = Now + 1.0f;
        LatestJumpTime = std::max(LatestJumpTime, Now + 1.0f);
        return;
    }
    State = EFortAthenaAIBotState::Skydiving;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    Log::Info(L"Battle Royale bots: {} jumps from the bus at ({:.0f}, {:.0f}, {:.0f}) towards ({:.0f}, {:.0f}) {:.0f} m away (skydiving {})", BotName, Start.X, Start.Y, Start.Z,
        DropTarget.X, DropTarget.Y, Dist2D(Start, DropTarget) / 100.0f, Pawn && Pawn->bIsSkydiving ? L"yes" : L"no");
    NextLogTime = Now + 3.0f;
    LandedTime = 0.0f;
#endif
}

void AFortAthenaAIBotController::FreeFalling(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_FreeFalling: steer towards the landing spot while skydiving and under the
    // parachute, until the pawn walks.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    LastKnownPosition = Location;
    const FVector Direction = Direction2D(Location, DropTarget);
    const float Distance = Dist2D(Location, DropTarget);
    if (Distance > 300.0f)
    {
        Pawn->AddMovementInput(Direction, 1.0f, true);
    }
    if (AAIController* AIController = Controller.Get(); AIController && Now >= NextShotTime)
    {
        NextShotTime = Now + 1.0f;
        AIController->SetFocalPoint(FVector(DropTarget.X, DropTarget.Y, Location.Z));
    }

    UCharacterMovementComponent* Movement = static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->CharacterMovement;
    const bool bWalking = Movement && Movement->MovementMode == MoveWalking;
    if (Now >= NextLogTime)
    {
        NextLogTime = Now + 5.0f;
        Log::Info(L"Battle Royale bots: {} falls at ({:.0f}, {:.0f}, {:.0f}), {:.0f} m from its spot (skydiving {}, parachute {}, movement {})", BotName, Location.X, Location.Y, Location.Z,
            Distance / 100.0f, Pawn->bIsSkydiving ? L"yes" : L"no", Pawn->bIsParachuteOpen ? L"open" : L"closed", Movement ? static_cast<int32_t>(Movement->MovementMode) : -1);
    }
    if (bWalking && !Pawn->bIsSkydiving && !Pawn->bIsParachuteOpen)
    {
        if (LandedTime <= 0.0f)
        {
            LandedTime = Now;
        }
        if (Now - LandedTime > 0.5f)
        {
            State = EFortAthenaAIBotState::Landed;
            NextLogTime = Now + 5.0f;
            Log::Info(L"Battle Royale bots: {} lands at ({:.0f}, {:.0f}, {:.0f}), {:.0f} m from its spot", BotName, Location.X, Location.Y, Location.Z, Distance / 100.0f);
        }
    }
    else
    {
        LandedTime = 0.0f;
    }
#endif
}

void AFortAthenaAIBotController::UpdateCurrentTarget(float Now)
{
#if WITH_SERVER_CODE
    // 8.30's perception (OnPerceptionSensed, an EAlertLevel state machine over sight, hearing and damage), cheaply: the
    // nearest enemies in sight range, the nearest one in line of sight first; one that hurt the bot is its target too.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    LastKnownPosition = Location;
    struct FCandidate
    {
        AFortPawn* Pawn;
        float Distance;
    };
    std::vector<FCandidate> Candidates;
    for (const TWeakObjectPtr<AFortPawn>& Weak : Manager->GetEnemyPawns())
    {
        AFortPawn* Other = Weak.Get();
        if (!Other || reinterpret_cast<AFortPlayerPawnAthena*>(Other) == Pawn || Other->bIsDying || Other->IsPendingKill() || Manager->TeamOfPawn(Other) == Team)
        {
            continue;
        }
        const float Distance = (LocationOf(Other) - Location).Size();
        if (Distance <= AIBotSkills.SightRange)
        {
            Candidates.push_back({ Other, Distance });
        }
    }
    std::sort(Candidates.begin(), Candidates.end(), [](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });

    // Damage: the newest damager is an enemy pawn within twice the sight range.
    AFortPlayerStateAthena* Attacker = KillerOf(Pawn);
    AFortPawn* AttackerPawn = nullptr;
    for (const TWeakObjectPtr<AFortPawn>& Weak : Manager->GetEnemyPawns())
    {
        AFortPawn* Other = Weak.Get();
        if (Attacker && Other && reinterpret_cast<UObject*>(static_cast<APlayerState*>(Other->PlayerState)) == reinterpret_cast<UObject*>(Attacker) && Manager->TeamOfPawn(Other) != Team)
        {
            AttackerPawn = Other;
        }
    }

    AFortPawn* NewTarget = nullptr;
    bool bInSight = false;
    const FVector Eye(Location.X, Location.Y, Location.Z + 60.0f);
    for (size_t Index = 0; Index < Candidates.size() && Index < 3 && !NewTarget; ++Index)
    {
        const FVector TargetLocation = LocationOf(Candidates[Index].Pawn);
        FVector Hit;
        UObject* HitActor = nullptr;
        UObject* HitComponent = nullptr;
        const bool bBlocked = UKismetSystemLibrary::LineTraceSingle(Pawn, Eye, FVector(TargetLocation.X, TargetLocation.Y, TargetLocation.Z + 40.0f), 0, false, true, Hit, HitActor, HitComponent);
        if (!bBlocked || HitActor == reinterpret_cast<UObject*>(Candidates[Index].Pawn))
        {
            NewTarget = Candidates[Index].Pawn;
            bInSight = true;
        }
    }
    if (!NewTarget && AttackerPawn && (LocationOf(AttackerPawn) - Location).Size() <= AIBotSkills.SightRange * 2.0f)
    {
        NewTarget = AttackerPawn;
        bInSight = true;
    }

    if (NewTarget != CurrentTarget.Get())
    {
        CurrentTarget = NewTarget;
        TargetAcquiredTime = Now;
        if (NewTarget)
        {
            Log::Info(L"Battle Royale bots: {} spots {} {:.0f} m away", BotName, NameOf(reinterpret_cast<AFortPlayerStateAthena*>(static_cast<APlayerState*>(NewTarget->PlayerState)) ?
                reinterpret_cast<UObject*>(static_cast<APlayerState*>(NewTarget->PlayerState)) : reinterpret_cast<UObject*>(NewTarget)), (LocationOf(NewTarget) - Location).Size() / 100.0f);
        }
    }
    bHasVisualLineOfSightOnTarget = bInSight;
    CurrentAlertLevel = NewTarget ? EAlertLevel::Threatened : Attacker ? EAlertLevel::Alerted : EAlertLevel::Unaware;
#endif
}

bool AFortAthenaAIBotController::HasUsableWeapon() const
{
    for (const FFortAthenaAIBotWeapon& Weapon : WeaponList)
    {
        const auto Found = Ammo.find(Weapon.AmmoDefinition);
        if (Weapon.LoadedAmmo > 0 || (Found != Ammo.end() && Found->second > 0))
        {
            return true;
        }
    }
    return false;
}

int32_t AFortAthenaAIBotController::BestWeaponIndex() const
{
    // The newest weapon with ammo (later weapons are usually the better finds), else the first.
    for (int32_t Index = static_cast<int32_t>(WeaponList.size()) - 1; Index >= 0; --Index)
    {
        const FFortAthenaAIBotWeapon& Weapon = WeaponList[static_cast<size_t>(Index)];
        const auto Found = Ammo.find(Weapon.AmmoDefinition);
        if (Weapon.LoadedAmmo > 0 || (Found != Ammo.end() && Found->second > 0))
        {
            return Index;
        }
    }
    return WeaponList.empty() ? -1 : 0;
}

void AFortAthenaAIBotController::EquipWeapon(int32_t Index)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Index < 0 || Index >= static_cast<int32_t>(WeaponList.size()))
    {
        return;
    }
    StopFiring();
    FFortAthenaAIBotWeapon& Entry = WeaponList[static_cast<size_t>(Index)];
    AFortWeapon* Weapon = Pawn->EquipWeaponDefinition(Entry.WeaponDefinition, Entry.ItemGuid);
    if (!Weapon)
    {
        Log::Warning(L"Battle Royale bots: {} couldn't equip {}", BotName, NameOf(Entry.WeaponDefinition));
        return;
    }
    Entry.ClipSize = std::max(BulletsPerClip(Weapon), 1);
    Weapon->AmmoCount = Entry.LoadedAmmo;
    EquippedWeapon = Weapon;
    EquippedIndex = Index;
    bPickaxeEquipped = false;
#endif
}

void AFortAthenaAIBotController::ManageWeapon(float Now)
{
#if WITH_SERVER_CODE
    // 8.30's _ManageWeapon: fire while the target is in sight, reload an empty weapon (from the bot's own ammo: no
    // player controller's inventory to reload from), and change to a weapon with ammo.
    AFortWeapon* Weapon = EquippedWeapon.Get();
    if (bPickaxeEquipped || EquippedIndex < 0 || EquippedIndex >= static_cast<int32_t>(WeaponList.size()))
    {
        if (HasUsableWeapon())
        {
            EquipWeapon(BestWeaponIndex());
        }
        return;
    }
    FFortAthenaAIBotWeapon& Entry = WeaponList[static_cast<size_t>(EquippedIndex)];
    if (!Weapon)
    {
        EquipWeapon(EquippedIndex);
        return;
    }
    Entry.LoadedAmmo = Weapon->AmmoCount;
    if (Entry.LoadedAmmo > 0)
    {
        return;
    }
    if (ReloadUntil <= 0.0f)
    {
        StopFiring();
        ReloadUntil = Now + 1.6f;
        return;
    }
    if (Now < ReloadUntil)
    {
        return;
    }
    ReloadUntil = 0.0f;
    int32_t& Reserve = Ammo[Entry.AmmoDefinition];
    const int32_t Loaded = std::min(Reserve, Entry.ClipSize);
    if (Loaded > 0)
    {
        Reserve -= Loaded;
        Entry.LoadedAmmo = Loaded;
        Weapon->AmmoCount = Loaded;
        Weapon->ForceNetUpdate();
    }
    else if (HasUsableWeapon())
    {
        EquipWeapon(BestWeaponIndex());
    }
#endif
}

void AFortAthenaAIBotController::FireShot(AFortPawn* Target, float Distance)
{
#if WITH_SERVER_CODE
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortWeapon* Weapon = EquippedWeapon.Get();
    if (!Pawn || !Target)
    {
        return;
    }
    Pawn->PawnStartFire(0);
    const FFortAthenaAIBotWeapon* Held = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;
    if (Weapon && Held && Weapon->AmmoCount > 0)
    {
        Weapon->AmmoCount = Weapon->AmmoCount - 1;
    }

    // The hit: the shot's line, from the bot's eyes through its aim point (the target plus FAIBotSkills' error), passes
    // within the target's capsule, and nothing stands between them.
    const FVector Location = LocationOf(Pawn);
    const FVector Eye(Location.X, Location.Y, Location.Z + 60.0f);
    const FVector TargetLocation = LocationOf(Target);
    const FVector Center(TargetLocation.X, TargetLocation.Y, TargetLocation.Z + 20.0f);
    const FVector Aim = Center + AimError;
    const FVector Ray = Aim - Eye;
    const float RayLength = Ray.Size();
    bool bHit = false;
    if (RayLength > 1.0f)
    {
        const FVector Direction = Ray * (1.0f / RayLength);
        const FVector ToTarget = Center - Eye;
        const float Along = ToTarget | Direction;
        const FVector Closest = Eye + Direction * Along;
        const float Horizontal = Dist2D(Closest, Center);
        bHit = Along > 0.0f && Horizontal < 45.0f && std::fabs(Closest.Z - Center.Z) < 90.0f;
    }
    if (bHit)
    {
        FVector HitLocation;
        UObject* HitActor = nullptr;
        UObject* HitComponent = nullptr;
        const bool bBlocked = UKismetSystemLibrary::LineTraceSingle(Pawn, Eye, Center, 0, false, true, HitLocation, HitActor, HitComponent);
        bHit = !bBlocked || HitActor == reinterpret_cast<UObject*>(Target);
    }
    if (!bHit)
    {
        return;
    }
    float Damage = bPickaxeEquipped ? 20.0f : Held ? Held->Damage : 20.0f;
    if (Held && Held->Pellets > 1)
    {
        // A shotgun's pellets: all of them close up, fewer with the distance 🟡.
        Damage *= static_cast<float>(Held->Pellets) * std::clamp(1.0f - (Distance - 400.0f) / 2000.0f, 0.25f, 1.0f);
    }
    Manager->ApplyBotDamage(this, Target, Damage, Held ? Held->WeaponDefinition : nullptr);
#endif
}

void AFortAthenaAIBotController::StopFiring()
{
    if (!bFiring)
    {
        return;
    }
    bFiring = false;
    if (AFortPlayerPawnAthena* Pawn = GetPawn())
    {
        Pawn->PawnStopFire(0);
    }
}

bool AFortAthenaAIBotController::RangeAttack(float Now)
{
#if WITH_SERVER_CODE
    // 8.30's _RangeAttack (a ranged weapon at a target in sight; the destination is the target) and 21.40's
    // _MeleeAttack (the harvesting tool, close up). Aim: FAIBotSkills' reaction time before the first shot, an error that
    // grows with the distance, and shots spaced by MaxTimeBetweenShots +- DeviationTimeBetweenShots.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortPawn* Target = CurrentTarget.Get();
    AAIController* AIController = Controller.Get();
    if (!Target || !AIController || Target->bIsDying || !bHasVisualLineOfSightOnTarget)
    {
        return false;
    }
    const FVector Location = LocationOf(Pawn);
    const FVector TargetLocation = LocationOf(Target);
    const float Distance = (TargetLocation - Location).Size();
    const bool bArmed = HasUsableWeapon();
    if (!bArmed && Distance > 400.0f)
    {
        return false;
    }

    // The aim, refreshed every shot.
    const float ErrorScale = AIBotSkills.AimErrorAt10m * std::max(Distance, 300.0f) / 1000.0f;
    if (Now >= NextShotTime || AimError.SizeSquared() == 0.0f)
    {
        AimError = FVector(RandomRange(-1.0f, 1.0f), RandomRange(-1.0f, 1.0f), RandomRange(-0.6f, 0.6f)) * ErrorScale;
    }
    AIController->SetFocalPoint(FVector(TargetLocation.X, TargetLocation.Y, TargetLocation.Z + 20.0f) + AimError);

    // Keep a fighting distance: close in from afar, back off when too close (a shotgun or the pickaxe wants it close).
    if (!bArmed)
    {
        if (!bPickaxeEquipped)
        {
            UAthenaPickaxeItemDefinition* Pickaxe = DefaultPickaxe();
            if (UFortWeaponItemDefinition* PickaxeWeapon = Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr)
            {
                StopFiring();
                EquippedWeapon = Pawn->EquipWeaponDefinition(PickaxeWeapon, FGuid::NewGuid());
                bPickaxeEquipped = true;
                EquippedIndex = -1;
            }
        }
        MoveTowards(TargetLocation, Now, 80.0f);
    }
    else if (Distance > 2500.0f)
    {
        MoveTowards(TargetLocation, Now, 1500.0f);
    }
    else
    {
        StopMoving();
        if (Distance < 600.0f)
        {
            Pawn->AddMovementInput(Direction2D(TargetLocation, Location), 0.6f, false);
        }
    }

    if (bFiring && Now >= FireUntil)
    {
        StopFiring();
    }
    const bool bReady = Now >= TargetAcquiredTime + AIBotSkills.MaxTrackingReactionTime && Now >= NextShotTime && ReloadUntil <= 0.0f;
    const bool bLoaded = bPickaxeEquipped ? Distance < 250.0f : (EquippedWeapon.Get() && EquippedWeapon.Get()->AmmoCount > 0);
    if (bReady && bLoaded && !bFiring)
    {
        FireShot(Target, Distance);
        bFiring = true;
        if (ShotsFired++ == 0)
        {
            Log::Info(L"Battle Royale bots: {} opens fire on {} {:.0f} m away with {} ({} loaded)", BotName, NameOf(Target), Distance / 100.0f,
                EquippedWeapon.Get() ? NameOf(static_cast<UFortWeaponItemDefinition*>(EquippedWeapon.Get()->WeaponData)) : std::wstring(L"nothing"), EquippedWeapon.Get() ? static_cast<int32_t>(EquippedWeapon.Get()->AmmoCount) : 0);
        }
        FireUntil = Now + 0.12f;
        const FFortAthenaAIBotWeapon* Held = !bPickaxeEquipped && EquippedIndex >= 0 && EquippedIndex < static_cast<int32_t>(WeaponList.size()) ? &WeaponList[static_cast<size_t>(EquippedIndex)] : nullptr;
        const float WeaponInterval = bPickaxeEquipped ? 0.8f : Held && Held->FiringRate > 0.0f ? 1.0f / Held->FiringRate : 0.5f;
        NextShotTime = Now + std::max(WeaponInterval, AIBotSkills.MaxTimeBetweenShots + RandomRange(-AIBotSkills.DeviationTimeBetweenShots, AIBotSkills.DeviationTimeBetweenShots));
    }
    return true;
#else
    return false;
#endif
}

bool AFortAthenaAIBotController::Loot(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_Loot (GatherNearestLoot / ComputeLootScore), for weapons and their ammo on the
    // floor: up to two weapons, then ammo for them, nearest first.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    AFortPickup* Target = LootTarget.Get();
    if (Target && (Target->bPickedUp || Target->IsPendingKill()))
    {
        Target = nullptr;
        LootTarget = nullptr;
    }
    if (!Target && Now >= NextLootSearch)
    {
        NextLootSearch = Now + 1.0f;
        const bool bWantWeapon = WeaponList.size() < 2;
        float Best = 7000.0f;
        for (const FFortAthenaBotLoot& Entry : Manager->GetLoot())
        {
            AFortPickup* Pickup = Entry.Pickup.Get();
            if (!Pickup || Pickup->bPickedUp)
            {
                continue;
            }
            bool bWanted = Entry.bWeapon ? bWantWeapon : false;
            if (!Entry.bWeapon && !WeaponList.empty())
            {
                FFortItemEntry& PickupEntry = Pickup->PrimaryPickupItemEntry;
                UFortItemDefinition* Definition = PickupEntry.ItemDefinition;
                bWanted = std::any_of(WeaponList.begin(), WeaponList.end(), [Definition](const FFortAthenaAIBotWeapon& Weapon) { return Weapon.AmmoDefinition == Definition; })
                    && Ammo[Definition] < 120;
            }
            const float Distance = (Entry.Location - Location).Size();
            if (bWanted && Distance < Best && std::fabs(Entry.Location.Z - Location.Z) < 800.0f)
            {
                Best = Distance;
                Target = Pickup;
            }
        }
        LootTarget = Target;
    }
    if (!Target)
    {
        return false;
    }
    const FVector PickupLocation = LocationOf(Target);
    if (Dist2D(PickupLocation, Location) < 200.0f && std::fabs(PickupLocation.Z - Location.Z) < 250.0f)
    {
        TakePickup(Target);
        LootTarget = nullptr;
        return true;
    }
    MoveTowards(PickupLocation, Now, 50.0f);
    return true;
#else
    return false;
#endif
}

void AFortAthenaAIBotController::TakePickup(AFortPickup* Pickup)
{
#if WITH_SERVER_CODE
    // Into the bot's own inventory (8.30 keeps it in the controller's AFortInventory; Core's brain holds it), and the
    // pickup is gone as when a player takes it.
    FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
    UFortItemDefinition* Definition = Entry.ItemDefinition;
    const int32_t Count = std::max(static_cast<int32_t>(Entry.Count), 1);
    if (UFortWeaponItemDefinition* WeaponDefinition = Cast<UFortWeaponItemDefinition>(Definition))
    {
        FFortAthenaAIBotWeapon Weapon;
        Weapon.WeaponDefinition = WeaponDefinition;
        Weapon.AmmoDefinition = static_cast<TAssetPtr<UFortWorldItemDefinition>&>(WeaponDefinition->AmmoData).LoadSynchronous();
        Weapon.ItemGuid = FGuid::NewGuid();
        Weapon.LoadedAmmo = std::max(static_cast<int32_t>(Entry.LoadedAmmo), 0);
        ReadWeaponStats(Weapon);
        // A weapon found empty with no ammo to go with it gets a magazine 🟡 (bots don't search chests yet: phase 2).
        if (Weapon.LoadedAmmo == 0 && Ammo[Weapon.AmmoDefinition] == 0)
        {
            Ammo[Weapon.AmmoDefinition] += 30;
        }
        if (WeaponList.size() >= 2)
        {
            WeaponList.erase(WeaponList.begin());
        }
        WeaponList.push_back(Weapon);
        EquipWeapon(static_cast<int32_t>(WeaponList.size()) - 1);
        Log::Info(L"Battle Royale bots: {} picks up {} ({} loaded, {} ammo held)", BotName, NameOf(WeaponDefinition), Weapon.LoadedAmmo, Ammo[Weapon.AmmoDefinition]);
    }
    else
    {
        Ammo[Definition] += Count;
        Log::Info(L"Battle Royale bots: {} picks up {} {}", BotName, Count, NameOf(Definition));
    }
    Pickup->bPickedUp = true;
    Manager->ForgetLoot(Pickup);
    Pickup->K2_DestroyActor();
#endif
}

bool AFortAthenaAIBotController::Storm(float Now)
{
    // 8.30's _Storm: the safe zone's phase and state feed the behaviour tree, which heads for the next circle.
    FVector Center;
    float Radius = 0.0f;
    if (!Manager->GetSafeZone(Center, Radius))
    {
        return false;
    }
    const FVector Location = LocationOf(GetPawn());
    if (Dist2D(Location, Center) <= Radius * 0.8f)
    {
        return false;
    }
    // A point well inside the circle on the bot's side of it.
    const FVector Inward = Direction2D(Center, Location);
    const FVector Goal(Center.X + Inward.X * Radius * 0.4f, Center.Y + Inward.Y * Radius * 0.4f, Location.Z);
    MoveTowards(Goal, Now, 300.0f);
    return true;
}

void AFortAthenaAIBotController::Wander(float Now)
{
    AFortPlayerPawnAthena* Pawn = GetPawn();
    const FVector Location = LocationOf(Pawn);
    if (!bHasWanderGoal || Dist2D(Location, WanderGoal) < 200.0f || Now >= NextWanderTime)
    {
        const float Range = State == EFortAthenaAIBotState::Warmup ? 800.0f : 2500.0f;
        FVector Center;
        float Radius = 0.0f;
        FVector Around = Location;
        if (State != EFortAthenaAIBotState::Warmup && Manager->GetSafeZone(Center, Radius) && Dist2D(Location, Center) > Radius * 0.5f)
        {
            Around = FVector(Location.X + (Center.X - Location.X) * 0.3f, Location.Y + (Center.Y - Location.Y) * 0.3f, Location.Z);
        }
        const float Angle = RandomRange(0.0f, 6.2831853f);
        const float Distance = RandomRange(Range * 0.3f, Range);
        WanderGoal = FVector(Around.X + std::cos(Angle) * Distance, Around.Y + std::sin(Angle) * Distance, Location.Z);
        bHasWanderGoal = true;
        NextWanderTime = Now + RandomRange(5.0f, 9.0f);
    }
    MoveTowards(WanderGoal, Now, 150.0f);
}

void AFortAthenaAIBotController::MoveTowards(const FVector& Destination, float Now, float AcceptanceRadius)
{
#if WITH_SERVER_CODE
    // Phase 1 has no navmesh (3.0's Athena_Terrain has none cooked): the engine's direct move (MoveToLocation without
    // pathfinding) where it takes it, else the pawn's own movement input, every frame towards the goal.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AAIController* AIController = Controller.Get();
    if (!Pawn || !AIController)
    {
        return;
    }
    const FVector Location = LocationOf(Pawn);
    if (Dist2D(Location, Destination) <= AcceptanceRadius)
    {
        StopMoving();
        return;
    }
    if (!bHasMoveGoal || Dist2D(MoveGoal, Destination) > 150.0f)
    {
        MoveGoal = Destination;
        bHasMoveGoal = true;
        if (!bFiring && !CurrentTarget.Get())
        {
            AIController->SetFocalPoint(FVector(Destination.X, Destination.Y, Location.Z + 60.0f));
        }
    }
    // Facing the way it goes: Battle Royale's pawn moves slower sideways and backwards (its directional speed curve).
    if (!bFiring && !CurrentTarget.Get() && Now >= NextFaceTime)
    {
        NextFaceTime = Now + 0.25f;
        const FVector Direction = Direction2D(Location, Destination);
        AIController->SetFocalPoint(FVector(Location.X + Direction.X * 1000.0f, Location.Y + Direction.Y * 1000.0f, Location.Z + 60.0f));
    }
    Pawn->AddMovementInput(Direction2D(Location, Destination), 1.0f, false);
#endif
}

void AFortAthenaAIBotController::StopMoving()
{
    bHasMoveGoal = false;
}

void AFortAthenaAIBotController::TryToUnstuck(float Now)
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_Movement::TryToUnstuck: a bot that wants to move and hasn't, jumps; still stuck,
    // it gives up on its goal and goes somewhere else.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    if (!Pawn || Now < NextStuckCheck)
    {
        return;
    }
    NextStuckCheck = Now + 1.5f;
    const FVector Location = LocationOf(Pawn);
    const bool bMoving = bHasMoveGoal;
    if (bMoving && Dist2D(Location, LastStuckCheckLocation) < 60.0f)
    {
        ++StuckChecks;
        static_cast<ACharacter*>(reinterpret_cast<APawn*>(Pawn))->Jump();
        if (StuckChecks >= 3)
        {
            StuckChecks = 0;
            LootTarget = nullptr;
            bHasWanderGoal = true;
            const float Angle = RandomRange(0.0f, 6.2831853f);
            WanderGoal = FVector(Location.X + std::cos(Angle) * 700.0f, Location.Y + std::sin(Angle) * 700.0f, Location.Z);
            NextWanderTime = Now + 3.0f;
        }
    }
    else
    {
        StuckChecks = 0;
    }
    LastStuckCheckLocation = Location;
#endif
}

void AFortAthenaAIBotController::TakeStormDamage(float Now)
{
#if WITH_SERVER_CODE
    if (Now < NextStormTick)
    {
        return;
    }
    NextStormTick = Now + 1.0f;
    AFortPlayerPawnAthena* Pawn = GetPawn();
    FVector Center;
    float Radius = 0.0f;
    if (!Pawn || !Manager->GetCurrentSafeZone(Center, Radius))
    {
        LastStormHealth = -1.0f;
        return;
    }
    const float Health = Pawn->GetHealth();
    if (Dist2D(LocationOf(Pawn), Center) <= Radius)
    {
        LastStormHealth = -1.0f;
        return;
    }
    // Outside: the game's own storm took some since the last second, or Core takes it.
    const bool bGameHurts = LastStormHealth >= 0.0f && Health < LastStormHealth - 0.1f;
    if (!bGameHurts)
    {
        const float NewHealth = Health - Manager->GetStormDamagePerSecond();
        if (NewHealth <= 0.5f)
        {
            Log::Info(L"Battle Royale bots: the storm takes {}", BotName);
            Pawn->SetHealth(1.0f);
            Pawn->ForceKill(FGameplayTag{ FName(L"Gameplay.Damage.OutsideSafeZone", FNAME_Find) }, nullptr, nullptr);
            return;
        }
        Pawn->SetHealth(NewHealth);
    }
    LastStormHealth = Pawn->GetHealth();
#endif
}

void AFortAthenaAIBotController::DropInventory()
{
#if WITH_SERVER_CODE
    // 8.30's bots drop their loot as players do: each weapon (with its magazine) and the ammo, where the bot fell.
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortGameModeAthena* GameMode = Manager->GetGameMode();
    UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
    if (!Pawn || !World)
    {
        return;
    }
    const FVector Location = LocationOf(Pawn);
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    const auto Drop = [&](UFortItemDefinition* Definition, int32_t Count, int32_t LoadedAmmo)
    {
        if (!Definition || Count <= 0 || !EntryStruct)
        {
            return;
        }
        std::vector<uint8_t> Storage(EntryStruct->GetStructureSize(), 0);
        FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(Storage.data());
        FFortItemEntry::Construct(Entry, Definition, Count, 0);
        Entry->LoadedAmmo = LoadedAmmo;
        const FVector Spot(Location.X + RandomRange(-80.0f, 80.0f), Location.Y + RandomRange(-80.0f, 80.0f), Location.Z);
        AFortPickup::CreatePickup(World, *Entry, &Spot);
        EntryStruct->DestroyStruct(Entry);
    };
    for (const FFortAthenaAIBotWeapon& Weapon : WeaponList)
    {
        Drop(Weapon.WeaponDefinition, 1, Weapon.LoadedAmmo);
    }
    for (const auto& [Definition, Count] : Ammo)
    {
        Drop(Definition, Count, 0);
    }
    WeaponList.clear();
    Ammo.clear();
#endif
}

void AFortAthenaAIBotController::OnPossesedPawnDied(AFortPlayerStateAthena* KillerState)
{
#if WITH_SERVER_CODE
    // 21.40's AFortAthenaAIBotController::OnPossesedPawnDied. The bot's player state takes its death report as a
    // player's does (AFortPlayerControllerAthena's handler): who eliminated it (else itself) and how; it replicates
    // (OnRep_DeathInfo), which writes the clients' kill feed.
    StopFiring();
    AFortPlayerPawnAthena* Pawn = GetPawn();
    AFortPlayerStateAthena* PlayerState = GetPlayerState();
    if (PlayerState && AFortPlayerStateAthena::HasEliminator())
    {
        FDeathInfo& DeathInfo = PlayerState->DeathInfo;
        DeathInfo.FinisherOrDowner = KillerState ? KillerState : PlayerState;
        DeathInfo.bDBNO = false;
        if (Pawn && Natives::AFortPlayerPawn_GetDeathCause)
        {
            DeathInfo.DeathCause = Natives::AFortPlayerPawn_GetDeathCause(Pawn, Pawn->DeathTags, false);
        }
        PlayerState->ForceNetUpdate();
    }
    DropInventory();
    State = EFortAthenaAIBotState::Dead;
    CurrentTarget = nullptr;
    LootTarget = nullptr;
    Log::Info(L"Battle Royale bots: {} died at ({:.0f}, {:.0f}, {:.0f}){}", BotName, LastKnownPosition.X, LastKnownPosition.Y, LastKnownPosition.Z,
        KillerState ? std::format(L", eliminated by {}", KillerState->GetPlayerName().ToStdString()) : std::wstring(L" (nobody's elimination)"));
#endif
}
