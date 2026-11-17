#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_Barrier.h"
#include "Athena/AthenaBarrierFlag.h"
#include "Athena/AthenaBarrierObjective.h"
#include "Athena/AthenaBigBaseWall.h"
#include "Athena/FortAthenaMapInfo.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Engine/World.h"
#include "FortKismetLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"

#include <cmath>

namespace
{
    constexpr float RadiansToDegrees = 180.0f / 3.1415926535897932f;
    constexpr float DegreesToRadians = 3.1415926535897932f / 180.0f;
    // The constructor's WinningTeam (0xD22B42: mov dword ptr [rbx+384h], 67h): no team has won.
    constexpr int32_t NoWinningTeam = 103;
    // How often a team's CheckHealthThreshold looks at its mascot. The build keeps the timer's handle in the team state
    // (CheckHealthThreshold clears it), not the rate it was set with; once a second is Core's.
    constexpr float HealthCheckRate = 1.0f;

    AFortGameStateAthena* GameStateOf(AFortGameModeAthena* GameMode)
    {
        UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
        return World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    }

    FVector FlightEnd(const FAircraftFlightInfo& Flight)
    {
        const FRotator Rotation = Flight.FlightStartRotation;
        const float Distance = Flight.FlightSpeed * Flight.TimeTillFlightEnd;
        const FVector Start = Flight.FlightStartLocation;
        return FVector(Start.X + std::cos(Rotation.Yaw * DegreesToRadians) * Distance, Start.Y + std::sin(Rotation.Yaw * DegreesToRadians) * Distance, Start.Z);
    }

    // The island's ground under Location: the landscape the first blocking hits down from high over it reach. Hits on
    // anything else (6.30's floating island over Loot Lake, a roof, a tree) are traced through. False when there is no
    // landscape under it (open water).
    bool GroundUnder(UObject* WorldContext, const FVector& Location, FVector& OutGround)
    {
        float StartZ = 30000.0f;
        for (int32_t Attempt = 0; Attempt < 8; ++Attempt)
        {
            FVector Hit;
            UObject* HitActor = nullptr;
            UObject* HitComponent = nullptr;
            if (!UKismetSystemLibrary::LineTraceSingle(WorldContext, FVector(Location.X, Location.Y, StartZ), FVector(Location.X, Location.Y, -5000.0f), 0, false, true, Hit, HitActor, HitComponent))
            {
                return false;
            }
            if (HitActor && HitActor->GetClass()->GetName().ToStdString().find(L"Landscape") != std::wstring::npos)
            {
                OutGround = Hit;
                return true;
            }
            StartZ = Hit.Z - 10.0f;
        }
        return false;
    }

    FVector ClosestPointOnLine(const FVector& Point, const FVector& LineStart, const FVector& LineEnd)
    {
        const FVector Line = LineEnd - LineStart;
        const float LengthSquared = Line.SizeSquared();
        const float Along = LengthSquared > 0.0f ? ((Point - LineStart) | Line) / LengthSquared : 0.0f;
        return LineStart + Line * Along;
    }

    // The last of the match's storm circles that still has a radius (the map info's SafeZoneDefinitions, as the
    // playlist's game data scales them): Food Fight's first circle shrinks the map to the play area and the ones after
    // hold it, then the radii go to 0. -1 without the definitions.
    int32_t LastSafeZoneWithRadius(AFortGameStateAthena* GameState)
    {
        AFortAthenaMapInfo* MapInfo = GameState ? static_cast<AFortAthenaMapInfo*>(GameState->MapInfo) : nullptr;
        if (!MapInfo || !AFortAthenaMapInfo::HasSafeZoneDefinitions())
        {
            return -1;
        }
        TStructArray<FFortSafeZoneDefinition>& Definitions = MapInfo->SafeZoneDefinitions;
        int32_t Last = -1;
        for (int32_t Index = 0; Index < Definitions.Num(); ++Index)
        {
            if (static_cast<FScalableFloat&>(Definitions[Index].Radius).GetValueAtLevel(0.0f) > 0.0f)
            {
                Last = Index;
            }
        }
        return Last;
    }
}

FTimerHandle& FBarrierTeamState::GetHealthCheckTimerHandle()
{
    UScriptStruct* Struct = StaticStruct();
    const int32_t Size = Struct ? Struct->GetStructureSize() : 0x28;
    return *reinterpret_cast<FTimerHandle*>(reinterpret_cast<uint8_t*>(this) + Size - sizeof(FTimerHandle));
}

void AFortAthenaMutator_Barrier::FortGameStateAthena_GatherCustomCharacterPartOverrides(uint8_t TeamNum, std::vector<UCustomCharacterPart*>& OutParts) const
{
    // As the build's own (0xD36460): the team's food (Team_0_State's for its TeamNum, else Team_1_State's), nothing for
    // MAX, else that food's FoodHeadData[FoodTeam].PartsToSwapIn, which take the place of the parts of their type.
    const FBarrierTeamState& Team0 = Team_0_State;
    const FBarrierTeamState& Team1 = Team_1_State;
    const uint8_t Food = TeamNum == static_cast<uint8_t>(Team0.TeamNum) ? static_cast<uint8_t>(Team0.FoodTeam) : static_cast<uint8_t>(Team1.FoodTeam);
    UScriptStruct* HeadDataStruct = FBarrierHeadData::StaticStruct();
    if (Food >= EBarrierFoodTeam::GetMAX() || !HeadDataStruct || !Member_FoodHeadData.IsAvailable())
    {
        return;
    }
    const FBarrierHeadData* HeadData = reinterpret_cast<const FBarrierHeadData*>(reinterpret_cast<const uint8_t*>(&static_cast<FBarrierHeadData&>(FoodHeadData)) + Food * HeadDataStruct->GetStructureSize());
    const TArray<UCustomCharacterPart*>& Parts = HeadData->PartsToSwapIn;
    for (int32_t Index = 0; Index < Parts.Num(); ++Index)
    {
        if (Parts[Index])
        {
            OutParts.push_back(Parts[Index]);
        }
    }
}

void AFortAthenaMutator_Barrier::GatherCustomCharacterPartOverrides(UWorld* World, uint8_t TeamNum, std::vector<UCustomCharacterPart*>& OutParts)
{
    AFortGameStateAthena* GameState = World && StaticClass() ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const TArray<UObject*>* MutatorList = GameState ? GameState->GetGameplayMutatorList() : nullptr;
    if (!MutatorList)
    {
        return;
    }
    const TArray<UObject*>& Mutators = *MutatorList;
    for (int32_t Index = 0; Index < Mutators.Num(); ++Index)
    {
        if (const AFortAthenaMutator_Barrier* Barrier = Cast<AFortAthenaMutator_Barrier>(Mutators[Index]))
        {
            Barrier->FortGameStateAthena_GatherCustomCharacterPartOverrides(TeamNum, OutParts);
        }
    }
}

void AFortAthenaMutator_Barrier::ForEachBarrierMutator(AFortGameModeAthena* GameMode, void (*Visit)(AFortAthenaMutator_Barrier*, AFortGameModeAthena*, uint8_t), uint8_t GamePhaseStep)
{
    const TArray<UObject*>* MutatorList = GameMode && StaticClass() ? GameMode->GetGameplayMutatorList() : nullptr;
    if (!MutatorList)
    {
        return;
    }
    const TArray<UObject*>& Mutators = *MutatorList;
    for (int32_t Index = 0; Index < Mutators.Num(); ++Index)
    {
        if (AFortAthenaMutator_Barrier* Barrier = Cast<AFortAthenaMutator_Barrier>(Mutators[Index]))
        {
            Visit(Barrier, GameMode, GamePhaseStep);
        }
    }
}

void AFortAthenaMutator_Barrier::OnGamePhaseStepChanged(AFortGameModeAthena* GameMode, uint8_t GamePhaseStep)
{
#if WITH_SERVER_CODE
    // The barrier stands from the warmup on: its line is the game state's FlightPathMidLine, which the game mode's
    // OnPlaylistDataLoaded (6.30 0xCAC3F0, slot 0x1A4) works out with the buses' paths (0xC4C780) before the warmup
    // starts. The restaurants come at BusLocked, once the storm has its circles.
    if (!BigBaseWall && GamePhaseStep >= EAthenaGamePhaseStep::GetWarmup())
    {
        SpawnBigBaseWall(GameMode);
    }
    if (GamePhaseStep == EAthenaGamePhaseStep::GetBusLocked())
    {
        SpawnModeObjects(GameMode);
    }
#endif
}

void AFortAthenaMutator_Barrier::OnSafeZoneUpdated(AFortGameModeAthena* GameMode)
{
#if WITH_SERVER_CODE
    SpawnModeObjects(GameMode);
#endif
}

void AFortAthenaMutator_Barrier::SpawnModeObjects(AFortGameModeAthena* GameMode)
{
#if WITH_SERVER_CODE
    // The restaurants, once the storm has its circles (the game mode picks every circle's centre as the storm starts,
    // at BusLocked): they stand around the play area's centre (GetSafeZoneLocation).
    AFortGameStateAthena* GameState = GameStateOf(GameMode);
    const FBarrierTeamState& Team0 = Team_0_State;
    FVector SafeZoneCenter;
    const int32_t SafeZoneIndex = LastSafeZoneWithRadius(GameState);
    if (Team0.ObjectiveFlag || !GameState || GameState->GamePhase < EAthenaGamePhase::GetAircraft() || SafeZoneIndex < 0
        || !UFortKismetLibrary::GetSafeZoneLocation(GameMode, SafeZoneIndex, SafeZoneCenter))
    {
        return;
    }
    if (!BigBaseWall)
    {
        SpawnBigBaseWall(GameMode);
    }
    SpawnObjectives(GameMode, SafeZoneCenter, SafeZoneIndex);
#endif
}

void AFortAthenaMutator_Barrier::SpawnBigBaseWall(AFortGameModeAthena* GameMode)
{
#if WITH_SERVER_CODE
    AFortGameStateAthena* GameState = GameStateOf(GameMode);
    UWorld* World = GetWorld();
    UClass* WallClass = BigBaseWallClass;
    if (!GameState || !World || !WallClass || !AFortGameStateAthena::HasTeamFlightPaths())
    {
        Log::Warning(L"Food Fight: no barrier ({})", !WallClass ? L"the mutator has no BigBaseWallClass" : L"no flight paths in this build");
        return;
    }
    const FAircraftFlightInfo& MidLine = GameState->FlightPathMidLine;
    FVector Start = MidLine.FlightStartLocation;
    FVector End = FlightEnd(MidLine);
    Start.Z = 0.0f;
    End.Z = 0.0f;
    if ((End - Start).SizeSquared() < 1.0f)
    {
        Log::Warning(L"Food Fight: no barrier, the game state's FlightPathMidLine is empty");
        return;
    }
    // Its foot on the lowest of the island's ground along its line, so nothing passes under it anywhere (SM_Barrier stands
    // up from its origin: 0 to 84000 at Barrier_C's scale).
    float ZLevel = 0.0f;
    bool bFoundGround = false;
    constexpr int32_t GroundSamples = 128;
    for (int32_t Sample = 0; Sample <= GroundSamples; ++Sample)
    {
        FVector Ground;
        if (GroundUnder(this, Start + (End - Start) * (static_cast<float>(Sample) / GroundSamples), Ground) && (!bFoundGround || Ground.Z < ZLevel))
        {
            ZLevel = Ground.Z;
            bFoundGround = true;
        }
    }
    const FVector Ground(0.0f, 0.0f, ZLevel);
    AAthenaBigBaseWall* Wall = AAthenaBigBaseWall::CreateWall(World, WallClass, Start, End, ZLevel);
    BigBaseWall = Wall;
    if (!Wall)
    {
        Log::Error(L"Food Fight: {} couldn't be spawned", WallClass->GetName().ToStdString());
        return;
    }
    ForceNetUpdate();
    TMulticastDelegate<>* StartingToComeDown = Wall->GetOnWallStartingToComeDown();
    TMulticastDelegate<>* WallDown = Wall->GetOnWallDown();
    const bool bComingDownBound = StartingToComeDown && StartingToComeDown->AddUObject(this, &AFortAthenaMutator_Barrier::OnWallStartingToComeDown).IsValid();
    const bool bDownBound = WallDown && WallDown->AddUObject(this, &AFortAthenaMutator_Barrier::OnWallIsDown).IsValid();
    Log::Info(L"Food Fight: {} from ({:.0f}, {:.0f}) to ({:.0f}, {:.0f}) at Z {:.0f} (down under Z {:.0f}); its fall starts {:.0f} s into the storm circle before circle {:.0f}; the mutator hears it start to fall {} and come down {}",
        Wall->GetName().ToStdString(), Start.X, Start.Y, End.X, End.Y, Ground.Z, static_cast<FScalableFloat&>(Wall->ZKillLevel).GetValueAtLevel(0.0f), static_cast<FScalableFloat&>(TimeToBringDownWall).GetValueAtLevel(0.0f),
        static_cast<FScalableFloat&>(SafeZonePhaseWhenToBringDownWall).GetValueAtLevel(0.0f), bComingDownBound ? L"yes" : L"no", bDownBound ? L"yes" : L"no");
#endif
}

void AFortAthenaMutator_Barrier::SpawnObjectives(AFortGameModeAthena* GameMode, const FVector& SafeZoneCenter, int32_t SafeZoneIndex)
{
#if WITH_SERVER_CODE
    AFortGameStateAthena* GameState = GameStateOf(GameMode);
    AAthenaBigBaseWall* Wall = BigBaseWall;
    if (!GameState || !Wall)
    {
        return;
    }
    const FAircraftFlightInfo& MidLine = GameState->FlightPathMidLine;
    FVector LineStart = MidLine.FlightStartLocation;
    FVector LineEnd = FlightEnd(MidLine);
    LineStart.Z = 0.0f;
    LineEnd.Z = 0.0f;
    // The play area's centre on the wall's line, so the two restaurants are on either side of it.
    FVector FlatCenter = SafeZoneCenter;
    FlatCenter.Z = 0.0f;
    const FVector Center = ClosestPointOnLine(FlatCenter, LineStart, LineEnd);
    const float Yaw = static_cast<FRotator>(MidLine.FlightStartRotation).Yaw * DegreesToRadians;
    // The wall's right: the two sides of the map.
    const FVector Right(-std::sin(Yaw), std::cos(Yaw), 0.0f);
    const float Distance = static_cast<FScalableFloat&>(ObjectiveDistanceFromWall).GetValueAtLevel(0.0f);
    const float ZOffset = static_cast<FScalableFloat&>(ObjectiveZOffset).GetValueAtLevel(0.0f);

    // A team's side is the one its own bus flies on (TeamFlightPaths is in the teams' order).
    TStructArray<FAircraftFlightInfo>& TeamPaths = GameState->TeamFlightPaths;
    float Team0Side = 1.0f;
    if (TeamPaths.Num() >= 1)
    {
        const FVector TeamMiddle = (FVector(TeamPaths[0].FlightStartLocation) + FlightEnd(TeamPaths[0])) * 0.5f;
        Team0Side = ((TeamMiddle - Center) | Right) >= 0.0f ? 1.0f : -1.0f;
    }
    FBarrierTeamState* States[2] = { &static_cast<FBarrierTeamState&>(Team_0_State), &static_cast<FBarrierTeamState&>(Team_1_State) };
    for (int32_t Index = 0; Index < 2; ++Index)
    {
        const float Side = Index == 0 ? Team0Side : -Team0Side;
        const FVector Wanted = Center + Right * (Distance * Side);
        // On the player building grid, unturned, so its pieces are cells players build onto and trap: TeamObjective_Food1_C's
        // floors sit at (512, -256), (512, 256) and the like from its origin, and a grid floor's origin is the middle of its
        // cell's -Y edge (the floor mesh spans X -256..256, Y 0..512; Core's bot floors, from the build tool, the same).
        // So the restaurant's origin is a cell's middle.
        const FVector Spot(std::floor(Wanted.X / BuildingCellSize) * BuildingCellSize + BuildingCellSize * 0.5f,
            std::floor(Wanted.Y / BuildingCellSize) * BuildingCellSize + BuildingCellSize * 0.5f, 0.0f);
        // Its floor (the pieces' tops are at its origin) on the first building level at or over the highest of the ground
        // under it (its corners, RestaurantHalfExtent each way, and its middle) and ObjectiveZOffset, so no part of it is
        // buried on a slope.
        float FloorZ = 0.0f;
        bool bFoundGround = false;
        for (const FVector& Offset : { FVector(0.0f, 0.0f, 0.0f), FVector(RestaurantHalfExtent, RestaurantHalfExtent, 0.0f), FVector(RestaurantHalfExtent, -RestaurantHalfExtent, 0.0f),
                 FVector(-RestaurantHalfExtent, RestaurantHalfExtent, 0.0f), FVector(-RestaurantHalfExtent, -RestaurantHalfExtent, 0.0f) })
        {
            FVector Ground;
            if (GroundUnder(this, Spot + Offset, Ground) && (!bFoundGround || Ground.Z > FloorZ))
            {
                FloorZ = Ground.Z;
                bFoundGround = true;
            }
        }
        const FVector Location(Spot.X, Spot.Y, std::ceil((FloorZ + ZOffset) / BuildingLevelHeight) * BuildingLevelHeight);
        // The mascot faces the barrier (and the other team's mascot across it).
        const float FacingYaw = std::atan2(-Right.Y * Side, -Right.X * Side) * RadiansToDegrees;
        SpawnObjective(*States[Index], Location, FacingYaw, Index);
    }
    ForceNetUpdate();
    Log::Info(L"Food Fight: the restaurants stand {:.0f} either side of ({:.0f}, {:.0f}), the barrier's line nearest storm circle {}'s centre ({:.0f}, {:.0f}, {:.0f} off the line), {} team paths",
        Distance, Center.X, Center.Y, SafeZoneIndex, SafeZoneCenter.X, SafeZoneCenter.Y, std::sqrt(FVector::DistSquared(FlatCenter, Center)), TeamPaths.Num());
#endif
}

void AFortAthenaMutator_Barrier::SpawnObjective(FBarrierTeamState& TeamState, const FVector& Location, float HeadYaw, int32_t ObjectId)
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    UClass* FlagClass = ObjectiveFlag;
    if (!World || !FlagClass)
    {
        return;
    }
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.Owner = this;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    // The restaurant itself isn't turned (the grid's axes); only its mascot's head is.
    FTransform SpawnTransform;
    SpawnTransform.Rotation = FRotator(0.0f, 0.0f, 0.0f).Quaternion();
    SpawnTransform.SetLocation(Location);
    AAthenaBarrierFlag* Flag = Cast<AAthenaBarrierFlag>(World->SpawnActor(FlagClass, &SpawnTransform, SpawnParameters));
    if (!Flag)
    {
        Log::Error(L"Food Fight: team {}'s restaurant ({}) couldn't be spawned", TeamState.TeamNum.ToString(), FlagClass->GetName().ToStdString());
        return;
    }
    Flag->Team = TeamState.TeamNum;
    Flag->FoodTeam = TeamState.FoodTeam;
    Flag->ForceNetUpdate();
    AAthenaBarrierObjective* Objective = Flag->GetObjectiveActor();
    if (Objective)
    {
        Objective->Team = TeamState.TeamNum;
        Objective->FoodTeam = TeamState.FoodTeam;
        // TeamGenerator_C's OnHeadRotationSet turns the head to HeadRotationYaw + 90 (its mesh faces the side): so it
        // looks along Yaw, at the other team's mascot.
        Objective->HeadRotationYaw = HeadYaw - 90.0f;
        Objective->SetAllowDamage(false);
        Objective->ForceNetUpdate();
        TMulticastDelegate<AAthenaBarrierObjective*>* Destroyed = Objective->GetOnObjectiveDestroyed();
        if (!Destroyed || !Destroyed->AddUObject(this, &AFortAthenaMutator_Barrier::OnObjectiveDestroyed).IsValid())
        {
            Log::Error(L"Food Fight: team {}'s mascot can't tell the mutator it is destroyed", TeamState.TeamNum.ToString());
        }
    }
    TeamState.ObjectiveFlag = Flag;
    TeamState.ObjectiveObject = Objective;
    TeamState.bRespawnEnabled = true;
    // The HUD's objective bars under the minimap (BarrierObjectiveWidget, UBarrierWidgetBase's update 0x2EC2970) read the
    // restaurants from the game state's MutatorObjectDataArray: each AAthenaBarrierFlag there, the friendly or the enemy
    // one by its team, and its mascot's health. A mutator's objects go in as the build's own add does it (0xD45DD0:
    // AddDefaulted, ObjectId, then MarkItemDirty).
    AFortGameStateAthena* GameState = GetWorld() ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(GetWorld()->GetGameState())) : nullptr;
    if (GameState && AFortGameStateAthena::HasMutatorObjectDataArray())
    {
        FGameplayMutatorObjectDataArray& Objects = GameState->MutatorObjectDataArray;
        FGameplayMutatorObjectData& Data = static_cast<TStructArray<FGameplayMutatorObjectData>&>(Objects.ObjectDataList).AddDefaulted();
        Data.TheObject = reinterpret_cast<UObject*>(Flag);
        Data.ObjectId = ObjectId;
        Objects.MarkItemDirty(Data);
        GameState->ForceNetUpdate();
    }
    if (UWorld* TimerWorld = GetWorld())
    {
        TimerWorld->GetTimerManager().SetTimer(TeamState.GetHealthCheckTimerHandle(),
            FTimerDelegate::CreateUObject(this, &AFortAthenaMutator_Barrier::CheckHealthThreshold, static_cast<uint8_t>(TeamState.TeamNum)), HealthCheckRate, true);
    }
    Log::Info(L"Food Fight: team {}'s restaurant {} at ({:.0f}, {:.0f}, {:.0f}), facing {:.0f}; its mascot {} ({} of {} health); {} mutator objects for the HUD", TeamState.TeamNum.ToString(),
        Flag->GetName().ToStdString(), Location.X, Location.Y, Location.Z, HeadYaw, Objective ? Objective->GetName().ToStdString() : std::wstring(L"none"),
        Objective ? Objective->GetHealth() : 0.0f, Objective ? Objective->GetMaxHealth() : 0.0f,
        GameState && AFortGameStateAthena::HasMutatorObjectDataArray() ? static_cast<TStructArray<FGameplayMutatorObjectData>&>(static_cast<FGameplayMutatorObjectDataArray&>(GameState->MutatorObjectDataArray).ObjectDataList).Num() : -1);
#endif
}

void AFortAthenaMutator_Barrier::CheckHealthThreshold(uint8_t TeamNum)
{
#if WITH_SERVER_CODE
    UFunction* Function = FindStaticFunction_CheckHealthThreshold();
    if (!Function)
    {
        return;
    }
    Reflection::FParams Params(Function);
    Params.Set(L"TeamNum", TeamNum);
    Params.Call(this);
#endif
}

int32_t* AFortAthenaMutator_Barrier::GetWinningTeam() const
{
    // Between ObjectiveFlag (+0x378) and FoodHeadData (+0x390): bMutatorControlledWin (+0x380, BeginPlay sets it for a
    // MutatorControlled playlist), WinningTeam (+0x384) and the win's done flag (+0x388).
    if (!Member_ObjectiveFlag.IsAvailable())
    {
        return nullptr;
    }
    int32_t* WinningTeam = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(const_cast<AFortAthenaMutator_Barrier*>(this)) + Member_ObjectiveFlag.GetOffsetChecked() + 0xC);
    return *WinningTeam == NoWinningTeam || (*WinningTeam >= 0 && *WinningTeam < EFortTeam::GetMAX()) ? WinningTeam : nullptr;
}

void AFortAthenaMutator_Barrier::SendMutatorEvent(int32_t EventId, int32_t EventParam1)
{
#if WITH_SERVER_CODE
    AFortGameStateAthena* GameState = GetWorld() ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(GetWorld()->GetGameState())) : nullptr;
    if (!GameState || !AFortGameStateAthena::HasMutatorEventData())
    {
        return;
    }
    FGameplayMutatorEventData& Event = GameState->MutatorEventData;
    Event.EventId = EventId;
    Event.EventParam1 = EventParam1;
    Event.EventParam2 = 0;
    Event.EventParam3 = 0;
    GameState->ForceNetUpdate();
#endif
}

void AFortAthenaMutator_Barrier::OnObjectiveDestroyed(AAthenaBarrierObjective* Objective)
{
#if WITH_SERVER_CODE
    FBarrierTeamState& Team0 = Team_0_State;
    FBarrierTeamState& Team1 = Team_1_State;
    FBarrierTeamState* Loser = Objective && Team0.ObjectiveObject == Objective ? &Team0 : Objective && Team1.ObjectiveObject == Objective ? &Team1 : nullptr;
    if (!Loser)
    {
        return;
    }
    FBarrierTeamState& Winner = Loser == &Team0 ? Team1 : Team0;
    Loser->bRespawnEnabled = false;
    // Its restaurant's icon goes red: the flag's minimap and compass brushes are FoodDisplayData's Enabled ones while its
    // CurrentState is FlagUp and the Disabled ones (tinted red) otherwise (AAthenaBarrierFlag 0xC48D40, which the
    // mutator's FortGameStateAthena_OnDrawSpecialActorIconsOnMap draws, 0xD2A6B0). Replicated; the clients'
    // OnRep_CurrentState plays the flag's OnStateChange.
    if (AAthenaBarrierFlag* LoserFlag = Loser->ObjectiveFlag)
    {
        LoserFlag->CurrentState = EBarrierFlagState::GetFlagDown();
        LoserFlag->ForceNetUpdate();
    }
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(Team0.GetHealthCheckTimerHandle());
        World->GetTimerManager().ClearTimer(Team1.GetHealthCheckTimerHandle());
    }
    int32_t* WinningTeam = GetWinningTeam();
    if (WinningTeam && *WinningTeam == NoWinningTeam)
    {
        *WinningTeam = static_cast<uint8_t>(Winner.TeamNum);
    }
    ForceNetUpdate();
    SendMutatorEvent(3, static_cast<uint8_t>(Loser->TeamNum));
    Log::Info(L"Food Fight: team {}'s mascot is destroyed, team {} wins{}", Loser->TeamNum.ToString(), Winner.TeamNum.ToString(),
        WinningTeam ? L"" : L" (WinningTeam wasn't found: the match won't end)");
#endif
}

void AFortAthenaMutator_Barrier::OnWallStartingToComeDown()
{
#if WITH_SERVER_CODE
    SendMutatorEvent(1, 0);
    Log::Info(L"Food Fight: the barrier starts to come down");
#endif
}

void AFortAthenaMutator_Barrier::OnWallIsDown()
{
#if WITH_SERVER_CODE
    SendMutatorEvent(2, 0);
    for (FBarrierTeamState* State : { &static_cast<FBarrierTeamState&>(Team_0_State), &static_cast<FBarrierTeamState&>(Team_1_State) })
    {
        if (AAthenaBarrierObjective* Objective = State->ObjectiveObject)
        {
            Objective->SetAllowDamage(true);
        }
    }
    Log::Info(L"Food Fight: the barrier is down, the mascots can be damaged");
#endif
}

