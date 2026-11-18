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
        bSet = PlayerState && Params.Set(L"PlayerState", reinterpret_cast<UObject*>(PlayerState)) && Params.Set(L"bShouldClaim", true);
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

bool AFortMinigame::OverridePlayerSpawnLocation(APawn* PlayerPawn, AFortPlayerStateAthena* PlayerState, FVector& OutLocation)
{
    // The island game's answer in the game's respawn path (the volume manager's respawn place, 7.10 0xDBAD80, asks it for
    // a death in the island): _Implementation (7.10 slot 201, 0x10B7620), intact. In Warmup and InProgress it is the
    // SpawnLocationSetting's: ChoosePlayerStart (the player's highest checkpoint, else a spawn pad for their team), else
    // the air (GetAirSpawnLocation).
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
    int32_t Moved = 0;
    int32_t Skydiving = 0;
    int32_t Held = 0;
    const TArray<AFortPlayerState*> Players = GetParticipatingPlayers();
    for (AFortPlayerState* PlayerState : Players)
    {
        AActor* Owner = PlayerState ? reinterpret_cast<AActor*>(PlayerState)->GetOwner() : nullptr;
        AController* Controller = Owner ? Cast<AController>(reinterpret_cast<UObject*>(Owner)) : nullptr;
        APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        FVector Location;
        if (!Pawn || !AthenaState || !OverridePlayerSpawnLocation(Pawn, AthenaState, Location))
        {
            continue;
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
        AFortPlayerPawnAthena* AthenaPawn = Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Pawn));
        // A skydiver (#502) is held after the move and the skydive's start instead. BeginSkydiving (7.20 0x136F620) sets
        // bIsSkydiving and the movement's custom Skydiving mode (SetMovementMode(MOVE_Custom, 4)), and the teleport sets
        // Falling, either of which would take a pawn held first out of the stasis's MOVE_None, and it fell through the
        // countdown. Held last, the stasis (AFortPlayerPawn::SetStasisMode 0x1389830, NoMovementOrFalling: the
        // controller's move input ignored, StopMovementImmediately, DisableMovement) stops it where it is, and its
        // bIsSkydiving keeps the skydive pose. HandleStateChangedForWarmup puts it back into the skydive's movement as
        // the warmup's end releases it.
        const bool bSkydive = bAir && AthenaPawn && AFortPlayerPawnAthena::HasIsSkydiving() && Finder::ResolveBotSkydiveLive(AFortPlayerPawnAthena::IsSkydivingOffset());
        const bool bHold = bHoldInStasis;
        if (bHold && !bSkydive)
        {
            PutPlayerIntoStasis(Pawn);
        }
        AActor* PawnActor = reinterpret_cast<AActor*>(Pawn);
        if (!PawnActor->TeleportTo(Location, PawnActor->GetActorRotation()))
        {
            Held += bHold && !bSkydive ? 1 : 0;
            continue;
        }
        ++Moved;
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
    Log::Info(L"Minigame: {} respawns {} of its {} players for its warmup at their respawn place{}, {} held there until it ends", GetName().ToStdString(), Moved, Players.Num(),
        Skydiving ? std::format(L", {} of them skydiving", Skydiving) : std::wstring(), Held);
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
        RespawnMinigamePlayers();
    }
    else if (NewState == EFortMinigameState::GetInProgress() && SpawnLocationSetting == EFortMinigamePlayerSpawnLocationSetting::GetAir())
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
    if (Respawn && !HasStrippedExec(Respawn))
    {
        Log::Info(L"Finder: AFortMinigame::RespawnMinigamePlayers is native in this build");
    }
    else if (Respawn)
    {
        const UProperty* Property = Finder::ResolveClassFunctionMap() ? StaticClass()->FindPropertyByName(FName(L"StateChangedDelegate")) : nullptr;
        const UFunction* Signature = Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
            ? static_cast<const UFunction*>(static_cast<const UMulticastDelegateProperty*>(Property)->SignatureFunction) : nullptr;
        const bool bReflected = FindStaticFunction_OverridePlayerSpawnLocation() && EFortMinigameState::StaticEnum() && EFortMinigameWarmupType::StaticEnum();
        HandleStateChangedForWarmupFunction = Signature && bReflected ? StaticClass()->AddNativeFunction(L"HandleStateChangedForWarmup", &execHandleStateChangedForWarmup, Signature) : nullptr;
        if (HandleStateChangedForWarmupFunction)
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
