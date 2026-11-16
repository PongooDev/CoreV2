#include "pch.h"
#include "Minigame/FortMinigame.h"
#include "Finder.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPlayerPawn.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <unordered_map>

TArray<AFortPlayerState*> AFortMinigame::GetParticipatingPlayers()
{
    TArray<AFortPlayerState*> Players;
    Reflection::FParams Params(FindStaticFunction_GetParticipatingPlayers(), true);
    if (Params.Call(this))
    {
        if (TArray<AFortPlayerState*>* OutPlayers = Params.Get<TArray<AFortPlayerState*>>(L"OutPlayers"))
        {
            Players = *OutPlayers;
        }
    }
    return Players;
}

bool AFortMinigame::DetermineSpawnLocation(AController* PlayerController, FVector& OutLocation, FRotator& OutRotation, bool& bShouldSkydive)
{
    Reflection::FParams Params(FindStaticFunction_DetermineSpawnLocation(), true);
    if (!Params.Set(L"PlayerController", PlayerController) || !Params.Call(this))
    {
        return false;
    }
    const FVector* Location = Params.Get<FVector>(L"OutLocation");
    const FRotator* Rotation = Params.Get<FRotator>(L"OutRotation");
    const bool* Skydive = Params.Get<bool>(L"bShouldSkydive");
    const bool* ReturnValue = Params.Get<bool>(L"ReturnValue");
    if (!Location || !Rotation || !ReturnValue || !*ReturnValue)
    {
        return false;
    }
    OutLocation = *Location;
    OutRotation = *Rotation;
    bShouldSkydive = Skydive && *Skydive;
    return true;
}

void AFortMinigame::FinishPlayerTeleportedForMinigameStart(AFortPlayerPawn* PlayerPawn)
{
    Reflection::FParams Params(FindStaticFunction_FinishPlayerTeleportedForMinigameStart(), true);
    if (Params.Set(L"PlayerPawn", PlayerPawn))
    {
        Params.Call(this);
    }
}

void AFortMinigame::TeleportPlayersToStartLocations()
{
#if WITH_SERVER_CODE
    // The game's own pieces, all intact on 7.00: who plays (GetParticipatingPlayers), where each starts
    // (DetermineSpawnLocation: the game's player start components for the player's team, the air, or where they stand,
    // as SpawnLocationSetting says), and the game's step once a player is there (FinishPlayerTeleportedForMinigameStart,
    // native 0x15C0120, which tells the game's Blueprint through OnPlayerTeleportedForMinigameStart).
    if (Role != ROLE_Authority)
    {
        return;
    }
    int32_t Moved = 0;
    const TArray<AFortPlayerState*> Players = GetParticipatingPlayers();
    for (AFortPlayerState* PlayerState : Players)
    {
        AController* Controller = PlayerState ? Cast<AController>(reinterpret_cast<UObject*>(reinterpret_cast<AActor*>(PlayerState)->GetOwner())) : nullptr;
        APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
        FVector Location;
        FRotator Rotation;
        bool bShouldSkydive = false;
        if (!Pawn || !DetermineSpawnLocation(Controller, Location, Rotation, bShouldSkydive))
        {
            continue;
        }
        if (reinterpret_cast<AActor*>(Pawn)->TeleportTo(Location, Rotation))
        {
            ++Moved;
        }
        FinishPlayerTeleportedForMinigameStart(Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(Pawn)));
    }
    Log::Info(L"Minigame: {} takes {} of its {} players to their start", GetName().ToStdString(), Moved, Players.Num());
#endif
}

DEFINE_FUNCTION(AFortMinigame::execTeleportPlayersToStartLocations)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMinigame*>(Context)->TeleportPlayersToStartLocations();
    P_NATIVE_END;
}

void AFortMinigame::HookNatives()
{
#if WITH_SERVER_CODE
    // 7.00's exec is the empty native the build gives the server-only UFunctions it strips (0x1470620, shared by 277 of
    // them): many UFunctions with the same Func is that exec.
    UFunction* Function = StaticClass() ? FindStaticFunction_TeleportPlayersToStartLocations() : nullptr;
    const FNativeFuncPtr Exec = Function ? Function->GetNativeFunc() : nullptr;
    if (!Exec)
    {
        return;
    }
    int32_t Sharing = 0;
    for (TObjectIterator<UFunction> It; It; ++It)
    {
        if ((*It)->GetNativeFunc() == Exec && ++Sharing > 16)
        {
            break;
        }
    }
    if (Sharing <= 16)
    {
        Log::Info(L"Finder: AFortMinigame::TeleportPlayersToStartLocations is native in this build");
        return;
    }
    Function->SetNativeFunc(&execTeleportPlayersToStartLocations);
    Log::Info(L"Hook AFortMinigame::TeleportPlayersToStartLocations: native");
#endif
}
