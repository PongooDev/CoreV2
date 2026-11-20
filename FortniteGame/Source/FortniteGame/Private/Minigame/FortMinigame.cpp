#include "pch.h"
#include "Minigame/FortMinigame.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "UObject/UnrealType.h"
#include "Pawns/FortPlayerPawn.h"
#include "Player/FortPlayerState.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <format>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    TWeakObjectPtr<UWorld> InventoryResetWorld;

    // The build's shared empty exec: the one exec many UFunctions have is the empty native the build gives the
    // server-only UFunctions it strips (7.00 0x1470620, 7.10 0x14BB380 for those without parameters and 0x14D99A0 for
    // those with one object, which only steps over it).
    bool HasStrippedExec(UFunction* Function)
    {
        const FNativeFuncPtr Exec = Function ? Function->GetNativeFunc() : nullptr;
        if (!Exec)
        {
            return false;
        }
        int32_t Sharing = 0;
        for (TObjectIterator<UFunction> It; It; ++It)
        {
            if ((*It)->GetNativeFunc() == Exec && ++Sharing > 16)
            {
                return true;
            }
        }
        return false;
    }
}

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
    // 7.20 (CL-4727874) takes the player's state and whether to claim the start it picks (PlayerState, bShouldClaim) in
    // place of their controller. The game's other caller (0x1190EF0) claims it; so does the start of the game here.
    bool bSet = false;
    if (Params.Has(L"PlayerState"))
    {
        APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(PlayerController->PlayerState) : nullptr;
        // 7.40 (CL-5046157) takes only the player state: no bShouldClaim.
        bSet = PlayerState && Params.Set(L"PlayerState", reinterpret_cast<UObject*>(PlayerState)) && (!Params.Has(L"bShouldClaim") || Params.Set(L"bShouldClaim", true));
    }
    else
    {
        bSet = Params.Set(L"PlayerController", PlayerController);
    }
    if (!bSet || !Params.Call(this))
    {
        return false;
    }
    const FVector* Location = Params.Get<FVector>(L"OutLocation");
    const FRotator* Rotation = Params.Get<FRotator>(L"OutRotation");
    const bool* Skydive = Params.Get<bool>(L"bShouldSkydive");
    // 7.40 (CL-5046157) answers whether the player respawns on the ground (bOutRespawningOnGround) instead.
    const bool* OnGround = Skydive ? nullptr : Params.Get<bool>(L"bOutRespawningOnGround");
    const bool* ReturnValue = Params.Get<bool>(L"ReturnValue");
    if (!Location || !Rotation || !ReturnValue || !*ReturnValue)
    {
        return false;
    }
    OutLocation = *Location;
    OutRotation = *Rotation;
    bShouldSkydive = Skydive ? *Skydive : OnGround && !*OnGround;
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

void AFortMinigame::PutPlayerIntoStasis(APawn* PlayerPawn)
{
    // Intact on 7.10 and 7.20 (7.10 0x10B9920, 7.20 0x1192B60): in Warmup only, the pawn's SetStasisMode
    // NoMovementOrFalling, kept in the game's list of pawns in stasis, and bound to the controller's next pawn too. The
    // warmup's end (7.10 0x10A4270, StartMinigame's warmup timer) takes every pawn of that list out of stasis
    // (RemovePlayersFromStasis, 7.10 0x10BAFA0), whatever the warmup's type.
    Reflection::FParams Params(FindStaticFunction_PutPlayerIntoStasis(), true);
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
    int32_t Skydiving = 0;
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
            // SpawnLocationSetting's Sky (#485): DetermineSpawnLocation gives a point in the air (GetAirSpawnLocation,
            // 7.00 0x1075A20) and bShouldSkydive, and the player starts the game in a skydive from there, as from the bus
            // (AFortPlayerPawnAthena::BeginSkydiving, bFromBus false). Without it they fell from that height as a walking
            // pawn, to their death with fall damage on.
            AFortPlayerPawnAthena* AthenaPawn = Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Pawn));
            if (bShouldSkydive && AthenaPawn && AFortPlayerPawnAthena::HasIsSkydiving()
                && Finder::ResolveBotSkydiveLive(AFortPlayerPawnAthena::IsSkydivingOffset()))
            {
                AthenaPawn->BeginSkydiving(false);
                ++Skydiving;
            }
        }
        FinishPlayerTeleportedForMinigameStart(Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(Pawn)));
    }
    Log::Info(L"Minigame: {} takes {} of its {} players to their start{}", GetName().ToStdString(), Moved, Players.Num(),
        Skydiving ? std::format(L", {} of them skydiving from the air", Skydiving) : std::wstring());
#endif
}

DEFINE_FUNCTION(AFortMinigame::execTeleportPlayersToStartLocations)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMinigame*>(Context)->TeleportPlayersToStartLocations();
    P_NATIVE_END;
}

void AFortMinigame::ClearPlayerInventories()
{
#if WITH_SERVER_CODE
    // Who plays: the game's own list (GetParticipatingPlayers), as at the start's teleport.
    if (Role != ROLE_Authority)
    {
        return;
    }
    int32_t Cleared = 0;
    const TArray<AFortPlayerState*> Players = GetParticipatingPlayers();
    for (AFortPlayerState* PlayerState : Players)
    {
        AActor* Owner = PlayerState ? reinterpret_cast<AActor*>(PlayerState)->GetOwner() : nullptr;
        AFortPlayerControllerAthena* Controller = Owner ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Owner)) : nullptr;
        if (Controller)
        {
            ClearPlayerInventory(Controller);
            ++Cleared;
        }
    }
    Log::Info(L"Minigame: {} resets the inventories of {} of its {} players", GetName().ToStdString(), Cleared, Players.Num());
#endif
}

void AFortMinigame::ClearPlayerInventory(AFortPlayerControllerAthena* PlayerController)
{
#if WITH_SERVER_CODE
    // The player's own reset (#499): AFortPlayerControllerAthena::ClearDroppableItems, 7.10's and 7.20's native that
    // goes with this one (both stripped), which the trip back to the hub runs too.
    if (!PlayerController || Role != ROLE_Authority)
    {
        return;
    }
    const int32_t Removed = PlayerController->ClearDroppableItems();
    if (Removed > 0)
    {
        Log::Info(L"Minigame: {} loses {} items as {}'s inventory is reset", PlayerController->GetName().ToStdString(), Removed, GetName().ToStdString());
    }
    // The game's start and end move each player's same pawn (Core's RespawnMinigamePlayers, the post-game place), where
    // Epic's stripped respawn gave a new one: a Movement Modulator's speed and flight don't come along, and the damage
    // taken in the game stays on the pawn at the hub and into the next game (#518). The new pawn came back at full
    // health, so restore it here with the inventory and movement resets.
    PlayerController->ResetCreativeMovementState();
    if (AFortPlayerPawn* FortPawn = Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(PlayerController->GetPawn())))
    {
        const float MaxHealth = FortPawn->GetMaxHealth();
        if (MaxHealth > 0.0f && FortPawn->GetHealth() < MaxHealth)
        {
            FortPawn->SetHealth(MaxHealth);
            Log::Info(L"Minigame: {} restores {} to full health as its game's inventory is reset", GetName().ToStdString(),
                FortPawn->GetName().ToStdString());
        }
    }
#endif
}

DEFINE_FUNCTION(AFortMinigame::execClearPlayerInventories)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMinigame*>(Context)->ClearPlayerInventories();
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortMinigame::execClearPlayerInventory)
{
    P_GET_OBJECT(AFortPlayerControllerAthena, Z_Param_PlayerController);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMinigame*>(Context)->ClearPlayerInventory(Z_Param_PlayerController);
    P_NATIVE_END;
}

bool AFortMinigame::OverridePlayerSpawnLocation(APawn* PlayerPawn, AFortPlayerStateAthena* PlayerState, FVector& OutLocation, FRotator* OutRotation, bool* bOutRespawningOnGround)
{
    // The island game's answer in the game's respawn path (the volume manager's respawn place, 7.10 0xDBAD80, asks it for
    // a death in the island): _Implementation (7.10 slot 201, 0x10B7620), intact. In Warmup and InProgress it is the
    // SpawnLocationSetting's: ChoosePlayerStart (the player's highest checkpoint, else a spawn pad for their team), else
    // the air (GetAirSpawnLocation). 7.40's (slot 200, 0x11E2DE0) is DetermineSpawnLocation (0x11D1310): the start it
    // picks is claimed there, its rotation comes back too, and bOutRespawningOnGround is false for the air.
    Reflection::FParams Params(FindStaticFunction_OverridePlayerSpawnLocation(), true);
    if (!Params.Set(L"PlayerPawn", PlayerPawn) || !Params.Set(L"PlayerState", reinterpret_cast<UObject*>(PlayerState)) || !Params.Call(this))
    {
        return false;
    }
    const FVector* Location = Params.Get<FVector>(L"OutLocation");
    const bool* ReturnValue = Params.Get<bool>(L"ReturnValue");
    if (!Location || !ReturnValue || !*ReturnValue)
    {
        return false;
    }
    OutLocation = *Location;
    const FRotator* Rotation = OutRotation && Params.Has(L"OutRotation") ? Params.Get<FRotator>(L"OutRotation") : nullptr;
    const bool* OnGround = bOutRespawningOnGround && Params.Has(L"bOutRespawningOnGround") ? Params.Get<bool>(L"bOutRespawningOnGround") : nullptr;
    if (Rotation)
    {
        *OutRotation = *Rotation;
    }
    if (OnGround)
    {
        *bOutRespawningOnGround = *OnGround;
    }
    return true;
}

void AFortMinigame::RespawnMinigamePlayers()
{
#if WITH_SERVER_CODE
    // Core's reconstruction: no build has this body (7.10 and 7.20 strip it, its exec is the shared empty one; 7.00 has
    // no such function). Its place is StartMinigame's Respawn warmup (7.10 0x10BFC70: SetMinigameState(Warmup), then
    // PutPlayersIntoStasis for a Stasis warmup, and for a Respawn one the call that went with the stripped body), and
    // its players are put where the game's respawn path puts a player of the game: the island game's own
    // OverridePlayerSpawnLocation, which the game asks in Warmup. 7.10's KillPlayer, which a death-and-respawn would go
    // through, is stripped too, so nobody is killed: each player's pawn goes to that place, skydiving for a game that
    // spawns in the air, and the game hears they arrived (FinishPlayerTeleportedForMinigameStart: the island
    // Blueprint's OnPlayerTeleportedForMinigameStart, its effects). Each player is then held where they arrived for the
    // rest of the warmup (#485): the game's own PutPlayerIntoStasis, which works only in Warmup, and whose pawns the
    // warmup's end releases for either warmup type (7.10 0x10A4270's RemovePlayersFromStasis). A Stasis warmup puts its
    // players into stasis where they stand (PutPlayersIntoStasis, 7.10 0x10B9AC0); a Respawn one does it here, just
    // before the move (#499), since nothing else in StartMinigame holds them. A skydiver is held too (#502): the game's
    // PutPlayerIntoStasis has no exception for one (7.20 0x1192B60), so a game that spawns in the air holds its players
    // in the sky, in the skydive, until the countdown ends. Their hold comes after the skydive starts (below).
    if (Role != ROLE_Authority)
    {
        return;
    }
    const bool bAir = SpawnLocationSetting == EFortMinigamePlayerSpawnLocationSetting::GetAir();
    const bool bHoldInStasis = FindStaticFunction_PutPlayerIntoStasis() != nullptr;
    // 7.40 (CL-5046157): the move goes through the travel screen and the game's client-acknowledged teleport
    // (StartTeleportForMinigameStart); the hold and the skydive's choice stay as below.
    const bool bClientAcknowledged = UsesClientAcknowledgedTeleport();
    UFunction* const OverrideFunction = FindStaticFunction_OverridePlayerSpawnLocation();
    const bool bRotationFromGame = OverrideFunction && OverrideFunction->FindPropertyByName(FName(L"OutRotation", FNAME_Find)) != nullptr;
    // 7.20 and 7.30's DetermineSpawnLocation takes the player state and answers bShouldSkydive, what the Override does for
    // the island's respawn; 7.10's takes the controller and keeps the Override + Air path. A game on start pads that finds
    // no pad for a player still gets the air and bShouldSkydive from it (#513).
    const UFunction* const DetermineFunction = FindStaticFunction_DetermineSpawnLocation();
    const bool bDetermineForRespawn = !bClientAcknowledged && DetermineFunction && DetermineFunction->FindPropertyByName(FName(L"PlayerState", FNAME_Find)) != nullptr;
    int32_t Moved = 0;
    int32_t Skydiving = 0;
    int32_t Held = 0;
    int32_t Travelling = 0;
    int32_t Healed = 0;
    const TArray<AFortPlayerState*> Players = GetParticipatingPlayers();
    for (AFortPlayerState* PlayerState : Players)
    {
        AActor* Owner = PlayerState ? reinterpret_cast<AActor*>(PlayerState)->GetOwner() : nullptr;
        AController* Controller = Owner ? Cast<AController>(reinterpret_cast<UObject*>(Owner)) : nullptr;
        APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        // The same pawn comes back from the hub or goes into the next game, where Epic's stripped respawn gave a new
        // one at full health: damage from the last game would otherwise stay on it (#518). Heal it here, at the start's
        // warmup and at the trip back (the game's BP calls this for the post-game place too).
        if (AFortPlayerPawn* FortPawn = Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(Pawn)))
        {
            const float MaxHealth = FortPawn->GetMaxHealth();
            if (MaxHealth > 0.0f && FortPawn->GetHealth() < MaxHealth)
            {
                FortPawn->SetHealth(MaxHealth);
                ++Healed;
            }
        }
        if (!Pawn || !AthenaState)
        {
            continue;
        }
        AFortPlayerPawnAthena* AthenaPawn = Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Pawn));
        FVector Location;
        // 7.40 answers the start's rotation and whether it is on the ground (false for the air); 7.10 keeps the pawn's
        // rotation and SpawnLocationSetting's Air; 7.20 and 7.30's DetermineSpawnLocation answers both.
        FRotator Rotation = reinterpret_cast<AActor*>(Pawn)->GetActorRotation();
        bool bSkydive = false;
        bool bRotationFromSpawn = bRotationFromGame;
        if (bDetermineForRespawn)
        {
            if (!DetermineSpawnLocation(Controller, Location, Rotation, bSkydive))
            {
                continue;
            }
            bSkydive = bSkydive && AthenaPawn && AFortPlayerPawnAthena::HasIsSkydiving() && Finder::ResolveBotSkydiveLive(AFortPlayerPawnAthena::IsSkydivingOffset());
            bRotationFromSpawn = true;
        }
        else
        {
            bool bOnGround = !bAir;
            if (!OverridePlayerSpawnLocation(Pawn, AthenaState, Location, &Rotation, &bOnGround))
            {
                continue;
            }
            bSkydive = !bOnGround && AthenaPawn && AFortPlayerPawnAthena::HasIsSkydiving() && Finder::ResolveBotSkydiveLive(AFortPlayerPawnAthena::IsSkydivingOffset());
        }
        // The hold comes before the move (#499). Epic's respawn put a new pawn into stasis as it was possessed (the
        // controller's pawn binding PutPlayerIntoStasis adds), before it ever moved. Moved first, the pawn was teleported
        // a little above its spawner: the teleport (UCharacterMovementComponent::OnTeleported) sets Falling, and
        // AFortPlayerPawn::OnMovementModeChanged (7.20 0x1380EB0) tags it Gameplay.Action.Player.Fall from Walking. Its
        // ability system blocks InteractSearch while that tag is on (AbilityTagRelationshipMapping). The stasis then
        // went Falling to None and its release None to Walking, and the tag only goes on a landing (from Falling).
        // The player couldn't open a chest until they jumped: the server refused every GA_DefaultPlayer_InteractSearch.
        // Held first, the pawn goes Walking to None to Falling, and nothing tags it. It settles onto the spawner
        // (Falling to Walking) with its input still ignored until the warmup ends.
        // A skydiver (#502) is held after the move and the skydive's start instead. BeginSkydiving (7.20 0x136F620) sets
        // bIsSkydiving and the movement's custom Skydiving mode (SetMovementMode(MOVE_Custom, 4)), and the teleport sets
        // Falling, either of which would take a pawn held first out of the stasis's MOVE_None, and it fell through the
        // countdown. Held last, the stasis (AFortPlayerPawn::SetStasisMode 0x1389830, NoMovementOrFalling: the
        // controller's move input ignored, StopMovementImmediately, DisableMovement) stops it where it is, and its
        // bIsSkydiving keeps the skydive pose. HandleStateChangedForWarmup puts it back into the skydive's movement as
        // the warmup's end releases it.
        const bool bHold = bHoldInStasis;
        if (bHold && !bSkydive)
        {
            PutPlayerIntoStasis(Pawn);
        }
        // 7.40: held where they stand through the travel screen (a skydiver is held as it arrives, DelayedTeleportPlayer).
        if (bClientAcknowledged && StartTeleportForMinigameStart(Controller, Pawn, Location, Rotation, !bSkydive))
        {
            ++Travelling;
            Held += bHold && !bSkydive ? 1 : 0;
            continue;
        }
        AActor* PawnActor = reinterpret_cast<AActor*>(Pawn);
        if (!PawnActor->TeleportTo(Location, Rotation))
        {
            Held += bHold && !bSkydive ? 1 : 0;
            continue;
        }
        ++Moved;
        // The game's rotation is the respawn's (7.30's volume manager hands it to the respawn with the location), and a
        // respawned player looks that way: AGameModeBase::FinishRestartPlayer sets the controller's rotation (without
        // roll) and sends the client ClientSetRotation(rotation, true). A player pawn turns with its controller's
        // rotation, so the teleport's alone would last until the client's next move.
        if (bRotationFromSpawn)
        {
            Controller->SetControlRotation(FRotator(Rotation.Pitch, Rotation.Yaw, 0.0f));
            Controller->ClientSetRotation(Rotation, true);
        }
        if (bSkydive)
        {
            AthenaPawn->BeginSkydiving(false);
            ++Skydiving;
            if (bHold)
            {
                PutPlayerIntoStasis(Pawn);
            }
        }
        Held += bHold ? 1 : 0;
        FinishPlayerTeleportedForMinigameStart(Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(Pawn)));
    }
    if (Travelling > 0)
    {
        Log::Info(L"Minigame: {} sends {} of its {} players to their respawn place for its warmup through the travel screen (MinigameStart){}, {} held where they stand meanwhile",
            GetName().ToStdString(), Travelling, Players.Num(), Moved ? std::format(L"; {} moved at once", Moved) : std::wstring(), Held);
        return;
    }
    Log::Info(L"Minigame: {} respawns {} of its {} players for its warmup at their respawn place{}, {} held there until it ends", GetName().ToStdString(), Moved, Players.Num(),
        Skydiving ? std::format(L", {} of them skydiving", Skydiving) : std::wstring(), Held);
    if (Healed > 0)
    {
        Log::Info(L"Minigame: {} restores {} of its {} players to full health for the start or the trip back", GetName().ToStdString(), Healed, Players.Num());
    }
#endif
}

bool AFortMinigame::UsesClientAcknowledgedTeleport()
{
    // 7.40's shape: no RespawnMinigamePlayers (7.10 to 7.30 have it, a stripped UFunction, and keep the move at once), and
    // the two ends of the client-acknowledged teleport. Checked against 7.40 (CL-5046157) only: a later build's warmup has
    // to be read again at its bring-up.
    return StaticClass() && !FindStaticFunction_RespawnMinigamePlayers() && FindStaticFunction_OnClientFinishTeleportingForMinigame() && FindStaticFunction_ClearTeleportForMinigame();
}

FMinigamePlayer* AFortMinigame::FindMinigamePlayer(const AFortPlayerState* PlayerState)
{
    if (!PlayerState || !Member_PlayerArray.IsAvailable() || !FMinigamePlayer::HasTeleportingOrRespawning() || !FMinigamePlayer::StaticStruct())
    {
        return nullptr;
    }
    FMinigamePlayers& Array = PlayerArray;
    TStructArray<FMinigamePlayer>& Entries = Array.Players;
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        if (static_cast<AFortPlayerState*>(Entries[Index].PlayerState) == PlayerState)
        {
            return &Entries[Index];
        }
    }
    return nullptr;
}

void AFortMinigame::OnClientFinishTeleportingForMinigame(AFortPlayerPawn* InPawn)
{
    Reflection::FParams Params(FindStaticFunction_OnClientFinishTeleportingForMinigame(), true);
    if (Params.Set(L"InPawn", InPawn))
    {
        Params.Call(this);
    }
}

void AFortMinigame::ToggleAutoRestartMinigame()
{
    Reflection::FParams Params(FindStaticFunction_ToggleAutoRestartMinigame(), true);
    Params.Call(this);
}

void AFortMinigame::ClearTeleportForMinigame(AFortPlayerPawn* InPawn)
{
    Reflection::FParams Params(FindStaticFunction_ClearTeleportForMinigame(), true);
    if (Params.Set(L"InPawn", InPawn))
    {
        Params.Call(this);
    }
}

bool AFortMinigame::StartTeleportForMinigameStart(AController* Controller, APawn* Pawn, const FVector& Location, const FRotator& Rotation, bool bRespawningOnGround)
{
#if WITH_SERVER_CODE
    // 7.40's warmup (0x11EE110) for a Respawn game keeps only `FName(L"MinigameStart")` of its per-player step
    // (0x11EE17F-0x11EE193: the FName, then straight to the warmup's timer); a player joining a game in progress gets the
    // same name with ClientMinigameStartedText (AddMinigamePlayer, 0x11C8C58), then the game's DelayedTeleportPlayer timer.
    // MinigameStart is RespawnCameraData's row for it (FadeOutTime 0.001, WaitTime 1.75, SafetyTime 5), the respawn
    // preparation's InRespawnCameraBehavior: the server stores the destination in RespawnData and sends
    // ClientStartRespawnPreparation with the row and the text; the client blacks out, streams the place in and answers
    // ServerClientIsReadyToRespawn (bClientIsReady, 0xDA6F40); DelayedTeleportPlayer sets bServerIsReady and waits for the
    // client (0x11D0790). Core's StartDelayedTeleport and FinishDelayedTeleportWhenReady are that preparation and that wait
    // (as #497's rift travel has them). The player is marked teleporting meanwhile (FMinigamePlayer::
    // bIsTeleportingOrRespawningForGameplay, which the game's own teleport ends clear).
    AFortPlayerControllerAthena* AthenaController = Controller ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller)) : nullptr;
    if (!AthenaController || !Pawn || !AFortPlayerControllerAthena::HasDelayedTeleport())
    {
        return false;
    }
    APlayerState* PlayerState = static_cast<APlayerState*>(Controller->PlayerState);
    // A trip already under way (a rift's) is left to finish, as the trip back to the hub leaves one.
    if (AthenaController->IsDelayedTeleporting())
    {
        Log::Info(L"Minigame: {}'s player {} is travelling already; their trip is left to finish", GetName().ToStdString(),
            PlayerState ? PlayerState->GetPlayerName().ToStdString() : std::wstring(L"?"));
        return true;
    }
    FMinigamePlayer* Entry = FindMinigamePlayer(reinterpret_cast<AFortPlayerState*>(PlayerState));
    if (Entry)
    {
        Entry->bIsTeleportingOrRespawningForGameplay = true;
    }
    const FText* Text = Member_ClientMinigameStartedText.IsAvailable() ? &GetMember_ClientMinigameStartedText() : nullptr;
    if (!AthenaController->StartDelayedTeleport(Location, Rotation, L"MinigameStart", Text))
    {
        if (Entry)
        {
            Entry->bIsTeleportingOrRespawningForGameplay = false;
        }
        return false;
    }
    const TWeakObjectPtr<AFortMinigame> WeakThis(this);
    AthenaController->FinishDelayedTeleportWhenReady([WeakThis, bRespawningOnGround](AFortPlayerControllerAthena* ReadyController, const FVector& ReadyLocation, const FRotator& ReadyRotation)
    {
        if (AFortMinigame* Minigame = WeakThis.Get())
        {
            Minigame->DelayedTeleportPlayer(ReadyController, ReadyLocation, ReadyRotation, bRespawningOnGround);
        }
    });
    return true;
#else
    return false;
#endif
}

void AFortMinigame::DelayedTeleportPlayer(AFortPlayerControllerAthena* PlayerController, const FVector& Location, const FRotator& Rotation, bool bRespawningOnGround)
{
#if WITH_SERVER_CODE
    // Both sides are ready (7.40 0x11D0790 returns at 0x11D0974 with nothing more: the move and the end of the teleport are
    // stripped). Which of the game's two ends goes with which arrival isn't in the image (their binders are stripped);
    // Core's reading: OnClientFinishTeleportingForMinigame starts a skydive unconditionally (0x11E1B10, BeginSkydiving
    // 0x13C1DD0 with bFromBus false), so it is the air's, and ClearTeleportForMinigame, which only clears the player's
    // teleport, the ground's.
    APawn* Pawn = PlayerController && Role == ROLE_Authority ? PlayerController->GetPawn() : nullptr;
    AFortPlayerPawn* PlayerPawn = Pawn ? Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(Pawn)) : nullptr;
    if (!PlayerPawn)
    {
        APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(reinterpret_cast<AController*>(PlayerController)->PlayerState) : nullptr;
        if (FMinigamePlayer* Entry = FindMinigamePlayer(reinterpret_cast<AFortPlayerState*>(PlayerState)))
        {
            Entry->bIsTeleportingOrRespawningForGameplay = false;
        }
        return;
    }
    const bool bMoved = reinterpret_cast<AActor*>(Pawn)->TeleportTo(Location, Rotation);
    const bool bSkydive = bMoved && !bRespawningOnGround;
    if (bSkydive)
    {
        OnClientFinishTeleportingForMinigame(PlayerPawn);
    }
    else
    {
        ClearTeleportForMinigame(PlayerPawn);
    }
    // A skydiver is held in the sky for the rest of the warmup (#502), after its skydive starts. 7.40's
    // PutPlayerIntoStasis (0x11E4610) has no Warmup check of its own (7.20's had one), and the warmup's end releases only
    // the pawns already held (RemovePlayersFromStasis, 0x11E6460), so an arrival after the warmup isn't held.
    const bool bWarmup = Member_CurrentState.IsAvailable() && CurrentState == EFortMinigameState::GetWarmup();
    const bool bHeld = bSkydive && bWarmup && FindStaticFunction_PutPlayerIntoStasis();
    if (bHeld)
    {
        PutPlayerIntoStasis(Pawn);
    }
    APlayerState* PlayerState = static_cast<APlayerState*>(reinterpret_cast<AController*>(PlayerController)->PlayerState);
    Log::Info(L"Minigame: {}'s player {} {} their start ({:.0f}, {:.0f}, {:.0f}){}", GetName().ToStdString(), PlayerState ? PlayerState->GetPlayerName().ToStdString() : std::wstring(L"?"),
        bMoved ? L"arrives at" : L"couldn't be moved to", Location.X, Location.Y, Location.Z,
        bSkydive ? (bHeld ? L", skydiving, held there until the warmup ends" : L", skydiving") : L"");
#endif
}

void AFortMinigame::HandleStateChangedForWarmup(uint8_t NewState)
{
#if WITH_SERVER_CODE
    // SetMinigameState broadcasts the new state as it sets it, before StartMinigame's warmup step.
    if (Role != ROLE_Authority || WarmupType != EFortMinigameWarmupType::GetRespawn())
    {
        return;
    }
    if (NewState == EFortMinigameState::GetWarmup())
    {
        // 7.40: the warmup's Respawn step comes after its SetMinigameState returns (0x11EE153, then 0x11EE158), and that
        // runs the island Blueprint's HandleMinigameWarmup after this broadcast (OnRep_CurrentState 0x11E27C0: the
        // broadcast at 0x11E2953, HandleMinigameWarmup at 0x11E299D), whose AssignTeams sets each player's team that
        // DetermineSpawnLocation's start pads are chosen by. The engine's next tick is after both.
        UWorld* World = UsesClientAcknowledgedTeleport() ? GetWorld() : nullptr;
        if (World)
        {
            const TWeakObjectPtr<AFortMinigame> WeakThis(this);
            World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis]()
            {
                AFortMinigame* Minigame = WeakThis.Get();
                if (Minigame && (!Member_CurrentState.IsAvailable() || Minigame->CurrentState == EFortMinigameState::GetWarmup()))
                {
                    Minigame->RespawnMinigamePlayers();
                }
            }));
            return;
        }
        RespawnMinigamePlayers();
    }
    // 7.20 and 7.30's DetermineSpawnLocation and 7.40's also send a game on start pads to the air for a player it finds
    // no pad for, so every Respawn game is checked; ResumeSkydivingAfterWarmup touches only a skydiver the release left
    // walking.
    else if (NewState == EFortMinigameState::GetInProgress())
    {
        ResumeSkydivingAfterWarmup();
    }
#endif
}

void AFortMinigame::ResumeSkydivingAfterWarmup()
{
#if WITH_SERVER_CODE
    // The warmup's end (7.20 0x1181770) takes its pawns out of stasis (RemovePlayersFromStasis 0x11947E0:
    // SetStasisMode(None), whose way out of NoMovementOrFalling is the movement's SetMovementMode(MOVE_Walking), vtable
    // +0x568, the slot execSetMovementMode calls) and then runs StartMinigame, whose SetMinigameState(InProgress)
    // broadcasts here in the same frame. A player held in the sky (#502) still has bIsSkydiving but now walks on air,
    // and would drop out of the skydive. Epic's place for a player's skydive at a game's start is the island
    // Blueprint's OnPlayerTeleportedForMinigameStart (Minigame_PlayerCreated: BeginSkydiving(false) when
    // bTeleportedToSky), which FinishPlayerTeleportedForMinigameStart (7.20 0x1182B40) fires with the player's
    // FMinigamePlayer::bWantsToSkydive; its caller went with the stripped RespawnMinigamePlayers. So each such pawn starts
    // its skydive again here (BeginSkydiving: the custom Skydiving movement), and falls from where it was held.
    if (Role != ROLE_Authority || !AFortPlayerPawnAthena::HasIsSkydiving() || !Finder::ResolveBotSkydiveLive(AFortPlayerPawnAthena::IsSkydivingOffset()))
    {
        return;
    }
    constexpr uint8_t MOVE_Custom = 6;
    int32_t Resumed = 0;
    const TArray<AFortPlayerState*> Players = GetParticipatingPlayers();
    for (AFortPlayerState* PlayerState : Players)
    {
        AActor* Owner = PlayerState ? reinterpret_cast<AActor*>(PlayerState)->GetOwner() : nullptr;
        AController* Controller = Owner ? Cast<AController>(reinterpret_cast<UObject*>(Owner)) : nullptr;
        APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
        AFortPlayerPawnAthena* AthenaPawn = Pawn ? Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Pawn)) : nullptr;
        UCharacterMovementComponent* Movement = AthenaPawn ? static_cast<ACharacter*>(Pawn)->CharacterMovement : nullptr;
        if (AthenaPawn && AthenaPawn->bIsSkydiving && Movement && Movement->MovementMode != MOVE_Custom)
        {
            AthenaPawn->BeginSkydiving(false);
            ++Resumed;
        }
    }
    if (Resumed > 0)
    {
        Log::Info(L"Minigame: {} starts, and {} of its players held in the sky skydive from there", GetName().ToStdString(), Resumed);
    }
#endif
}

DEFINE_FUNCTION(AFortMinigame::execRespawnMinigamePlayers)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMinigame*>(Context)->RespawnMinigamePlayers();
    P_NATIVE_END;
}

DEFINE_FUNCTION(AFortMinigame::execHandleStateChangedForWarmup)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_NewState);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMinigame*>(Context)->HandleStateChangedForWarmup(Z_Param_NewState);
    P_NATIVE_END;
}

void AFortMinigame::BindGameEvents(AFortMinigame* Minigame)
{
    // The minigame's own broadcasts, once each (AddUniqueDynamic): its ClearPlayerInventories as the game goes in
    // progress and as it stops being in progress, and Core's warmup handler with each new state.
    if (!Minigame || Minigame->Role != ROLE_Authority)
    {
        return;
    }
    if (ClearPlayerInventoriesFunction)
    {
        const FName Name = ClearPlayerInventoriesFunction->GetFName();
        FMulticastScriptDelegate& Started = Minigame->MinigameStartedDelegate;
        FMulticastScriptDelegate& Ended = Minigame->MinigameEndedDelegate;
        Started.AddUniqueDynamic(Minigame, Name);
        Ended.AddUniqueDynamic(Minigame, Name);
    }
    if (HandleStateChangedForWarmupFunction)
    {
        FMulticastScriptDelegate& StateChanged = Minigame->StateChangedDelegate;
        StateChanged.AddUniqueDynamic(Minigame, HandleStateChangedForWarmupFunction->GetFName());
    }
}

void AFortMinigame::BindWorld(UWorld* World)
{
#if WITH_SERVER_CODE
    // Nothing in 7.10 calls ClearPlayerInventories any more: no Blueprint references it (none of the paks has the name),
    // there is no FName or dynamic binding of it in the image, and its native callers went with the stripped server code,
    // so there is no call left to restore. 7.01's patch notes put the reset "when a game is started and also after it
    // finishes"; the game's own broadcasts at those two moments are the minigame's MinigameStartedDelegate
    // (HandleMinigameStarted_Implementation, as SetMinigameState goes InProgress) and MinigameEndedDelegate (as it
    // leaves InProgress: EndMinigame's PostGameEnd or PostGameAbandon). Each island volume's UFortMinigameVolumeComponent
    // spawns its minigame (MinigameClass), so the world's spawns from now on are bound, and those already up.
    // RespawnMinigamePlayers likewise (its only caller was StartMinigame's Respawn warmup): the game's broadcast at that
    // moment is the minigame's StateChangedDelegate with Warmup.
    if (!World || (!ClearPlayerInventoriesFunction && !HandleStateChangedForWarmupFunction) || InventoryResetWorld.Get() == World)
    {
        return;
    }
    InventoryResetWorld = World;
    const FDelegateHandle Handle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* Actor)
    {
        BindGameEvents(Actor ? Cast<AFortMinigame>(reinterpret_cast<UObject*>(Actor)) : nullptr);
    }));
    int32_t Existing = 0;
    for (TObjectIterator<AFortMinigame> It; It; ++It)
    {
        AFortMinigame* Minigame = *It;
        if (Minigame && !Minigame->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) && Minigame->GetWorld() == World)
        {
            BindGameEvents(Minigame);
            ++Existing;
        }
    }
    const std::wstring What = std::format(L"{}{}{}", ClearPlayerInventoriesFunction ? L"reset their players' inventories as their games start and end (MinigameStartedDelegate, MinigameEndedDelegate)" : L"",
        ClearPlayerInventoriesFunction && HandleStateChangedForWarmupFunction ? L" and " : L"",
        HandleStateChangedForWarmupFunction ? L"respawn their players for a Respawn warmup (StateChangedDelegate)" : L"");
    Log::Info(L"Minigame: the minigames {} ({} already up)", Handle.IsValid() ? What + L", bound as they spawn (UWorld::OnActorSpawned)"
        : std::wstring(L"can't be bound as they spawn: the world's OnActorSpawned wasn't found"), Existing);
#endif
}

void AFortMinigame::HookNatives()
{
#if WITH_SERVER_CODE
    // 7.00's exec is the empty native the build gives the server-only UFunctions it strips (0x1470620, shared by 277 of
    // them): many UFunctions with the same Func is that exec.
    UFunction* Function = StaticClass() ? FindStaticFunction_TeleportPlayersToStartLocations() : nullptr;
    if (Function && !HasStrippedExec(Function))
    {
        Log::Info(L"Finder: AFortMinigame::TeleportPlayersToStartLocations is native in this build");
    }
    else if (Function)
    {
        Function->SetNativeFunc(&execTeleportPlayersToStartLocations);
        Log::Info(L"Hook AFortMinigame::TeleportPlayersToStartLocations: native");
    }

    // 7.10 and 7.20 (CL-4667333, CL-4727874): the inventory reset's two natives are stripped (7.10's execs 0x14BB380 and
    // 0x14D99A0). 7.00 has neither UFunction.
    UFunction* ClearInventories = StaticClass() ? FindStaticFunction_ClearPlayerInventories() : nullptr;
    UFunction* ClearInventory = ClearInventories ? FindStaticFunction_ClearPlayerInventory() : nullptr;
    if (ClearInventories && ClearInventory && !HasStrippedExec(ClearInventories))
    {
        Log::Info(L"Finder: AFortMinigame::ClearPlayerInventories is native in this build");
    }
    else if (ClearInventories && ClearInventory && !HasStartedAndEndedDelegates())
    {
        Log::Error(L"Hook AFortMinigame::ClearPlayerInventories: not restored (the minigame's MinigameStartedDelegate and MinigameEndedDelegate not found), so a Creative game doesn't reset its players' inventories");
    }
    else if (ClearInventories && ClearInventory)
    {
        ClearInventories->SetNativeFunc(&execClearPlayerInventories);
        ClearPlayerInventoriesFunction = ClearInventories;
        Log::Info(L"Hook AFortMinigame::ClearPlayerInventories: native");
        if (HasStrippedExec(ClearInventory))
        {
            ClearInventory->SetNativeFunc(&execClearPlayerInventory);
            Log::Info(L"Hook AFortMinigame::ClearPlayerInventory: native");
        }
    }

    // 7.10 and 7.20: the Respawn warmup's RespawnMinigamePlayers is stripped (7.10's exec 0x14BB380, 7.20's 0x83BA30).
    // 7.00 has no such UFunction (nor WarmupType's Respawn). Core's native for the minigame's StateChangedDelegate (its
    // OnStateChanged signature, the new EFortMinigameState) goes in the class's FuncMap.
    UFunction* Respawn = StaticClass() ? FindStaticFunction_RespawnMinigamePlayers() : nullptr;
    const auto AddWarmupHandler = [](const UFunction*& Signature, bool& bReflected)
    {
        const UProperty* Property = Finder::ResolveClassFunctionMap() ? StaticClass()->FindPropertyByName(FName(L"StateChangedDelegate")) : nullptr;
        Signature = Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
            ? static_cast<const UFunction*>(static_cast<const UMulticastDelegateProperty*>(Property)->SignatureFunction) : nullptr;
        bReflected = FindStaticFunction_OverridePlayerSpawnLocation() && EFortMinigameState::StaticEnum() && EFortMinigameWarmupType::StaticEnum();
        HandleStateChangedForWarmupFunction = Signature && bReflected ? StaticClass()->AddNativeFunction(L"HandleStateChangedForWarmup", &execHandleStateChangedForWarmup, Signature) : nullptr;
        return HandleStateChangedForWarmupFunction != nullptr;
    };
    if (Respawn && !HasStrippedExec(Respawn))
    {
        Log::Info(L"Finder: AFortMinigame::RespawnMinigamePlayers is native in this build");
    }
    else if (!Respawn && UsesClientAcknowledgedTeleport())
    {
        // 7.40 (CL-5046157): no RespawnMinigamePlayers UFunction. The Respawn branch of the warmup (0x11EE110, StartMinigame
        // 0x11ED9D0's call for a game with a WarmupDuration) is stripped down to its FName "MinigameStart" (0x11EE17F) and
        // no call; the Stasis branch beside it is intact (PutPlayerIntoStasis for each player). The same broadcast as on
        // 7.10 to 7.30 (StateChangedDelegate with Warmup: the warmup's SetMinigameState, 0x11EE14C) runs Core's
        // RespawnMinigamePlayers, which moves the players through the travel screen and the game's client-acknowledged
        // teleport.
        const UFunction* Signature = nullptr;
        bool bReflected = false;
        if (AddWarmupHandler(Signature, bReflected))
        {
            Log::Info(L"Finder: AFortMinigame's Respawn warmup is stripped in this build (no RespawnMinigamePlayers; the warmup keeps only its FName MinigameStart): restored "
                L"(Core's RespawnMinigamePlayers from the minigames' StateChangedDelegate, through the travel screen; the delayed teleport {}, PlayerArray's teleport flag {}, "
                L"ClientMinigameStartedText {})", AFortPlayerControllerAthena::HasDelayedTeleport() ? L"found" : L"not found (moved at once)",
                Member_PlayerArray.IsAvailable() && FMinigamePlayer::HasTeleportingOrRespawning() ? L"found" : L"not found", Member_ClientMinigameStartedText.IsAvailable() ? L"found" : L"not found");
        }
        else
        {
            Log::Error(L"Hook AFortMinigame Respawn warmup: not restored (StateChangedDelegate's signature {}, OverridePlayerSpawnLocation and the minigame enums {}), so a Respawn warmup moves nobody to the spawn pads",
                Signature ? L"found" : L"not found", bReflected ? L"found" : L"not found");
        }
    }
    else if (Respawn)
    {
        const UFunction* Signature = nullptr;
        bool bReflected = false;
        if (AddWarmupHandler(Signature, bReflected))
        {
            Respawn->SetNativeFunc(&execRespawnMinigamePlayers);
            Log::Info(L"Hook AFortMinigame::RespawnMinigamePlayers: native (Core's reconstruction), from the minigames' StateChangedDelegate");
        }
        else
        {
            Log::Error(L"Hook AFortMinigame::RespawnMinigamePlayers: not restored (StateChangedDelegate's signature {}, OverridePlayerSpawnLocation and the minigame enums {}), so a Respawn warmup moves nobody to the spawn pads",
                Signature ? L"found" : L"not found", bReflected ? L"found" : L"not found");
        }
    }

    // The game mode binds its world's minigames as it binds the match's events (AFortGameModeAthena::BindMatchEvents);
    // where that came first, its world is bound here.
    for (TObjectIterator<AFortGameModeAthena> It; It; ++It)
    {
        if (!It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) && It->GetWorld())
        {
            BindWorld(It->GetWorld());
        }
    }
#endif
}
