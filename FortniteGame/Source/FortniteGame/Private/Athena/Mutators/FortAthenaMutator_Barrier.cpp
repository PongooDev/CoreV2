#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_Barrier.h"
#include "Athena/AthenaBarrierFlag.h"
#include "Athena/AthenaBarrierObjective.h"
#include "Athena/AthenaBigBaseWall.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Engine/World.h"
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

    // The ground under Location (the first blocking hit from high over it down to under the island), or Location's own
    // height when nothing is hit.
    FVector GroundUnder(UObject* WorldContext, const FVector& Location)
    {
        FVector Hit;
        if (UKismetSystemLibrary::LineTraceSingle(WorldContext, FVector(Location.X, Location.Y, 30000.0f), FVector(Location.X, Location.Y, -5000.0f), 0, false, true, Hit))
        {
            return Hit;
        }
        return Location;
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
    if (!GameState || AFortGameStateAthena::GameplayMutatorsOffset() < 0)
    {
        return;
    }
    const TArray<UObject*>& Mutators = GameState->GameplayMutators;
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
    if (!GameMode || !StaticClass() || AFortGameModeAthena::GameplayMutatorsOffset() < 0)
    {
        return;
    }
    const TArray<UObject*>& Mutators = GameMode->GameplayMutators;
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
    if (GamePhaseStep == EAthenaGamePhaseStep::GetBusLocked() && !BigBaseWall)
    {
        SpawnBigBaseWall(GameMode);
        SpawnObjectives(GameMode);
    }
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
    const FVector Ground = GroundUnder(this, (Start + End) * 0.5f);
    AAthenaBigBaseWall* Wall = AAthenaBigBaseWall::CreateWall(World, WallClass, Start, End, Ground.Z);
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
    Log::Info(L"Food Fight: {} from ({:.0f}, {:.0f}) to ({:.0f}, {:.0f}) at Z {:.0f}; its fall starts {:.0f} s into the storm circle before circle {:.0f}; the mutator hears it start to fall {} and come down {}",
        Wall->GetName().ToStdString(), Start.X, Start.Y, End.X, End.Y, Ground.Z, static_cast<FScalableFloat&>(TimeToBringDownWall).GetValueAtLevel(0.0f),
        static_cast<FScalableFloat&>(SafeZonePhaseWhenToBringDownWall).GetValueAtLevel(0.0f), bComingDownBound ? L"yes" : L"no", bDownBound ? L"yes" : L"no");
#endif
}

void AFortAthenaMutator_Barrier::SpawnObjectives(AFortGameModeAthena* GameMode)
{
#if WITH_SERVER_CODE
    AFortGameStateAthena* GameState = GameStateOf(GameMode);
    AAthenaBigBaseWall* Wall = BigBaseWall;
    if (!GameState || !Wall)
    {
        return;
    }
    const FAircraftFlightInfo& MidLine = GameState->FlightPathMidLine;
    FVector Center = (FVector(MidLine.FlightStartLocation) + FlightEnd(MidLine)) * 0.5f;
    Center.Z = 0.0f;
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
        FVector Location = GroundUnder(this, Center + Right * (Distance * Side));
        Location.Z += ZOffset;
        // Facing the wall.
        const float FacingYaw = std::atan2(-Right.Y * Side, -Right.X * Side) * RadiansToDegrees;
        SpawnObjective(*States[Index], Location, FacingYaw);
    }
    ForceNetUpdate();
    Log::Info(L"Food Fight: the restaurants stand {:.0f} off the barrier's middle ({:.0f}, {:.0f}), {} team paths",
        Distance, Center.X, Center.Y, TeamPaths.Num());
#endif
}

void AFortAthenaMutator_Barrier::SpawnObjective(FBarrierTeamState& TeamState, const FVector& Location, float Yaw)
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
    FTransform SpawnTransform;
    SpawnTransform.Rotation = FRotator(0.0f, Yaw, 0.0f).Quaternion();
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
        Objective->HeadRotationYaw = Yaw;
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
    if (UWorld* TimerWorld = GetWorld())
    {
        TimerWorld->GetTimerManager().SetTimer(TeamState.GetHealthCheckTimerHandle(),
            FTimerDelegate::CreateUObject(this, &AFortAthenaMutator_Barrier::CheckHealthThreshold, static_cast<uint8_t>(TeamState.TeamNum)), HealthCheckRate, true);
    }
    Log::Info(L"Food Fight: team {}'s restaurant {} at ({:.0f}, {:.0f}, {:.0f}), facing {:.0f}; its mascot {} ({} of {} health)", TeamState.TeamNum.ToString(),
        Flag->GetName().ToStdString(), Location.X, Location.Y, Location.Z, Yaw, Objective ? Objective->GetName().ToStdString() : std::wstring(L"none"),
        Objective ? Objective->GetHealth() : 0.0f, Objective ? Objective->GetMaxHealth() : 0.0f);
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

