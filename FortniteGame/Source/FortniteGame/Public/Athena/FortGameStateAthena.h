#pragma once

#include "Athena/FortPlaylistAthena.h"
#include "FortGameState.h"
#include "UObject/ScriptDelegates.h"

class AFortAthenaAircraft;
class AFortAthenaMapInfo;
class AFortSafeZoneIndicator;
class AFortSpecialActorReplicationInfo;

class EAthenaGamePhase
{
    DefineUnrealEnum(EAthenaGamePhase, FortniteGame)
    DefineUEnumValue(None)
    DefineUEnumValue(Setup)
    DefineUEnumValue(Warmup)
    DefineUEnumValue(Aircraft)
    DefineUEnumValue(SafeZones)
    DefineUEnumValue(EndGame)
};

// What the timer under the minimap counts down to (from 3.0): AFortGameStateAthena::GamePhaseStep.
class EAthenaGamePhaseStep
{
    DefineUnrealEnum(EAthenaGamePhaseStep, FortniteGame)
    DefineUEnumValue(None)
    DefineUEnumValue(Setup)
    DefineUEnumValue(Warmup)
    DefineUEnumValue(GetReady)
    DefineUEnumValue(BusLocked)
    DefineUEnumValue(BusFlying)
    DefineUEnumValue(StormForming)
    DefineUEnumValue(StormHolding)
    DefineUEnumValue(StormShrinking)
    DefineUEnumValue(Countdown)
    DefineUEnumValue(FinalCountdown)
    DefineUEnumValue(EndGame)
};

// Battle Royale's game state (from 1.7.2; Athena_GameState_C).
class AFortGameStateAthena : public AFortGameStateZone
{
    DefineUnrealClass(AFortGameStateAthena, FortniteGame)

public:
    // Set by AFortGameModeAthena's phase changes. After Warmup a dead player is out of the match: the pawn-death handler
    // (1.7.2 0x437C30) only takes a player out of the alive players from Aircraft on. GamePhaseStep follows on its own,
    // on the server too: the game state's Tick (5.40 0xC061E0) works it out every frame from the phase, the warmup
    // countdown, the aircraft and the safe zone indicator's times (0xC06E10) and broadcasts GamePhaseStepChanged when it
    // changes (AFortGameModeAthena's and the mutators' OnGamePhaseStepChanged). Its gate, CanUpdateGamePhaseStep
    // (0xBE0640, 5.30 0xBD98A0), is false only while UFortGameInstance's loading screen manager (the TWeakPtr that
    // UFortGlobalUIContext::IsCurrentlyShowingLoadingScreen reads) shows one, so it is true on Core's servers.
    DefineUProperty(EAthenaGamePhase, GamePhase)
    // What OnRep_GamePhase broadcasts (from 1.7.2; one EAthenaGamePhase parameter), on the server too: the game mode's
    // phase changes set GamePhase and call OnRep_GamePhase themselves (1.8.1's StartWarmupPhase 0x439A20; 5.40's
    // SetGamePhase 0xBFF510, whose OnRep_GamePhase 0xBEDB40 broadcasts it at its end).
    DefineUProperty(FMulticastScriptDelegate, GamePhaseChanged)
    static bool HasGamePhaseChanged() { return Member_GamePhaseChanged.IsAvailable(); }
    // From 3.0: what the timer under the minimap shows (UAthenaGamePhaseWidgetBase: the step's icon, and
    // GamePhaseStepTimeRemaining as m:ss). Neither replicates: each client's game state works them out in its own Tick,
    // as the server's does, from what does replicate (6.00 0xC3C470):
    //   Warmup: WarmupCountdownStartTime and WarmupCountdownEndTime (Setup, Warmup, GetReady in the last 10 s);
    //   Aircraft: each of Aircrafts' DropStartTime (BusLocked until then) and DropEndTime (BusFlying until then);
    //   SafeZones: bIsInFinalCountdown and bIsInCountdown (PlaylistTimeRemaining), else SafeZoneIndicator's
    //     SafeZoneStartShrinkTime (StormHolding until then) and SafeZoneFinishShrinkTime (StormShrinking until then),
    //     and with no indicator yet SafeZonesStartTime (StormForming until then);
    //   EndGame: EndGameKickPlayerTime.
    // Every time is the server's world time (GetServerWorldTimeSeconds). A phase whose times have all passed is step
    // None with 0 left, and the widget (AthenaGamePhaseWidget's OnGamePhaseStepChanged) keeps the icon it had.
    DefineUProperty(EAthenaGamePhaseStep, GamePhaseStep)
    DefineUProperty(float, GamePhaseStepTimeRemaining)
    // What the game state's Tick broadcasts when its step changes (one EAthenaGamePhaseStep parameter), on the server
    // too.
    DefineUProperty(FMulticastScriptDelegate, GamePhaseStepChanged)
    static bool HasGamePhaseStep() { return Member_GamePhaseStep.IsAvailable() && Member_GamePhaseStepTimeRemaining.IsAvailable() && Member_GamePhaseStepChanged.IsAvailable(); }
    // The storm circle, replicated: the game mode's own, which it gives the game state as it spawns it (6.00 0xC364A0).
    DefineUProperty(AFortSafeZoneIndicator*, SafeZoneIndicator)
    static bool HasSafeZoneIndicator() { return Member_SafeZoneIndicator.IsAvailable(); }
    // When the storm circle comes (the server's world time, replicated): the game mode spawns it once this has passed.
    DefineUProperty(float, SafeZonesStartTime)
    static bool HasSafeZonesStartTime() { return Member_SafeZonesStartTime.IsAvailable(); }
    // The playlist's own countdowns (replicated), which take the timer over while they run.
    DefineUProperty(bool, bIsInCountdown)
    DefineUProperty(bool, bIsInFinalCountdown)
    static bool HasCountdowns() { return Member_bIsInCountdown.IsAvailable() && Member_bIsInFinalCountdown.IsAvailable(); }
    // From 4.4: what OnPlaylistDataLoadCompleted broadcasts once the match's playlist has loaded (PlaylistName and its
    // gameplay tags), after the game mode's OnPlaylistDataLoaded applied its tables (5.40 0xBF32A0).
    DefineUProperty(FMulticastScriptDelegate, OnCurrentPlaylistLoaded)
    static bool HasCurrentPlaylistLoaded() { return Member_OnCurrentPlaylistLoaded.IsAvailable(); }
    DefineUProperty(int32_t, PlayersLeft)
    // The most players the match had: AddToAlivePlayers (1.10 0x42A4A0) raises it to the alive players' count.
    DefineUProperty(int32_t, TotalPlayers)
    static bool HasTotalPlayers() { return Member_TotalPlayers.IsAvailable(); }
    DefineUProperty(int32_t, TeamsLeft)
    // A large team game's players left on each team (index: the team less HumanPvP_Team1), which the HUD's
    // "<Team>{YourTeamAlive}</> : <EnemyTeam>{EnemyTeamAlive}</>" (3.2 0x2746410, redrawn when PlayersLeft changes) and
    // the map screen's team counts (0x2747150) show. AFortGameModeAthena's AddToAlivePlayers and RemoveFromAlivePlayers
    // (3.2 0x9AE8D0, 0x9CA730) count each player into it, on a playlist with bIsLargeTeamGame (Teams of 20) or 50v50's;
    // other playlists leave it empty.
    DefineUProperty(TArray<int32_t>, TeamXPlayersLeft)
    static bool HasTeamXPlayersLeft() { return Member_TeamXPlayersLeft.IsAvailable(); }
    // The warmup's countdown in the world's time, replicated for the clients' timer: set when the warmup starts
    // (UpdateWarmupPhase, 1.9 0x43D9A0, the end -1 until then), and the bus leaves at the end.
    DefineUProperty(float, WarmupCountdownStartTime)
    DefineUProperty(float, WarmupCountdownEndTime)

    // The map's AFortAthenaMapInfo, set by AFortGameModeAthena's own FinishWorldInitialization (1.7.2 0x3CF860).
    DefineUProperty(AFortAthenaMapInfo*, MapInfo)

    // 3.5 to 6.00: the match's playlist, set by SetCurrentPlaylistId (3.5.1 0x9FA020) from the game mode's playlist
    // list. ApplyPlaylistDataTables (0x9DC890) copies its tables over the default ones.
    DefineUProperty(UFortPlaylistAthena*, CurrentPlaylistData)
    // From 6.01 (CL-4424678) the playlist is in CurrentPlaylistInfo instead (+0x1820; BasePlaylist +0x18E8,
    // OverridePlaylist +0x18F0), which replicates it with the match's overrides of its properties.
    DefineUStructProperty(FPlaylistPropertyArray, CurrentPlaylistInfo)
    static bool HasCurrentPlaylistInfo() { return Member_CurrentPlaylistInfo.IsAvailable() && FPlaylistPropertyArray::IsAvailable(); }
    // Whether the game state has the match's playlist as an object (from 3.5), in either of the two.
    static bool HasCurrentPlaylist() { return Member_CurrentPlaylistData.IsAvailable() || HasCurrentPlaylistInfo(); }
    // The match's playlist: CurrentPlaylistData, or CurrentPlaylistInfo's (its OverridePlaylist, else its BasePlaylist,
    // as the game reads it everywhere). Null while the game state has none, and on a build before 3.5.
    UFortPlaylistAthena* GetCurrentPlaylist() const;
    // Makes Playlist the game state's, where the build keeps it. 3.5 to 6.00: CurrentPlaylistData, which replicates as
    // the pointer it is. From 6.01: CurrentPlaylistInfo's BasePlaylist, which FPlaylistPropertyArray::NetDeltaSerialize
    // (6.01 0xEF6450) sends to a connection whose last state has another PlaylistReplicationKey (every new connection
    // has none), so the key moves on for the players already in; no OverridePlaylist, as the match overrides none of
    // the playlist's own properties; and the match's overrides of its mutators' (FPlaylistPropertyArray::
    // SetPropertyOverrides: the party's custom match options, from MatchOptions). Epic's own store is stripped with
    // SetCurrentPlaylistId (6.01 has no writer of the three left but the game state's constructor, 0xBF8331, and the
    // client's read), so this is what the serializer and the readers need. False on a build before 3.5.
    bool SetCurrentPlaylist(UFortPlaylistAthena* Playlist);
    // Up to 5.21 the playlist's id is the game state's too (5.30 to 6.00 have none; 6.01's has it again, +0x115C).
    DefineUProperty(int32_t, CurrentPlaylistId)
    static bool HasCurrentPlaylistId() { return Member_CurrentPlaylistId.IsAvailable(); }
    // Whether the playlist's assets have loaded (LoadCurrentPlaylistData, then OnPlaylistDataLoadCompleted), or are
    // loading (from 4.2).
    DefineUProperty(bool, bPlaylistDataIsLoaded)
    DefineUProperty(bool, bPlaylistDataIsActivelyLoading)
    static bool HasPlaylistDataLoadState() { return Member_bPlaylistDataIsLoaded.IsAvailable() && Member_bPlaylistDataIsActivelyLoading.IsAvailable(); }
    // 5.30 0xBEC6C0: what a client does with a playlist that has replicated, and what the stripped server side did
    // after setting it (5.21's AFortGameModeAthena::OnGivenMatchAssignment, 0xBD59E0): the game state's values from it
    // (0xBD2710), then LoadCurrentPlaylistData (0xBD3AD0). 4.5's (0xA77500) and 5.0's (0xBC4050) do the same.
    DefineUFunction(OnRep_CurrentPlaylistData)
    // 6.01's, for CurrentPlaylistInfo (0xC1DFF0): on the server TeamCount is the playlist's MaxTeamCount, then
    // TotalFinalCountdownTime its FinalWinConditionTime and LoadCurrentPlaylistData (0xC185D0).
    DefineUFunction(OnRep_CurrentPlaylistInfo)
    // From 3.5; SetCurrentPlaylistId clears it (4.4 0xA922C0, 5.21 0xBD5CC7).
    DefineUProperty(bool, bStormReachedFinalPosition)
    static bool HasStormReachedFinalPosition() { return Member_bStormReachedFinalPosition.IsAvailable(); }

    // Gives the game state the playlist manager's playlist with that id (the world's game mode's PlaylistManager) and
    // loads it: CurrentPlaylistData, its values and LoadCurrentPlaylistData, whose end has the game mode's
    // OnPlaylistDataLoaded build the aircraft paths and apply the playlist's tables, then the playlist's additional
    // levels. Nothing when the game state has that playlist already, loaded or loading. The game's own on 3.5 to 4.5
    // (4.2 0xA592E0, 4.5 0xA7FF10); 5.0 to 5.21 inline it into AFortGameModeAthena::OnGivenMatchAssignment (5.21
    // 0xBD5CBA), and 5.30 strips it with that, so Core runs its steps there. From 6.01 the playlist goes into
    // CurrentPlaylistInfo: its BasePlaylist, with a new PlaylistReplicationKey so the connected clients get it too.
    void SetCurrentPlaylistId(int32_t PlaylistId);
    // The same with the playlist's name, which OnGivenMatchAssignment uses when the game mode has it (4.5 0xA800A0, a
    // function of its own that logs as the id's does; 5.21 0xBD5B68, inlined). Core's steps on every build.
    void SetCurrentPlaylistId(FName PlaylistName);
    // The playlist's additional levels stream in, on the server (21.40's name; 5.21 0xBE7080, which 5.30 strips):
    // AFortGameState::StartStreamingAdditionalPlaylistLevel with each one's package name.
    void StreamAdditionalPlaylistLevels();
    // Whether every one of the game state's playlist's AdditionalLevels is in AdditionalPlaylistLevelsStreamed (true
    // with none, or no playlist; Core's name). 6.20's Deimos_Gameplay brings the island's navmesh with it.
    bool HasStreamedAdditionalPlaylistLevels() const;
    // 3.5: the playlist's bIsLargeTeamGame, on the game state (the game mode's before 3.5, with bFiftyFiftySquadPlay,
    // which 3.5 dropped).
    DefineUProperty(bool, bIsLargeTeamGame)
    static bool HasLargeTeamGame() { return Member_bIsLargeTeamGame.IsAvailable(); }

    // 3.5: EAirCraftBehavior (1: OpposingAirCraftForEachTeam, 50v50's) and the aircrafts StartAircraftPhase spawns, one
    // per team in that behaviour. GetAircraftIndex (3.5.1 0x9E5750) maps a player state to its aircraft from them.
    DefineUProperty(uint8_t, AirCraftBehavior)
    DefineUProperty(TArray<AFortAthenaAircraft*>, Aircrafts)
    static bool HasAircrafts() { return Member_AirCraftBehavior.IsAvailable() && Member_Aircrafts.IsAvailable(); }
    static bool HasAirCraftBehavior() { return Member_AirCraftBehavior.IsAvailable(); }

    static bool HasWarmupCountdown() { return Member_WarmupCountdownStartTime.IsAvailable() && Member_WarmupCountdownEndTime.IsAvailable(); }

    // From 5.40: the list of the actors every map and compass shows (The Getaway's jewels, carriers, van spawners and
    // vans). Replicated; the actors register themselves with it on the server (AddActorToReplicationList).
    DefineUProperty(AFortSpecialActorReplicationInfo*, SpecialActorData)
    static bool HasSpecialActorData() { return Member_SpecialActorData.IsAvailable(); }
    static ptrdiff_t SpecialActorDataOffset() { return HasSpecialActorData() ? Member_SpecialActorData.GetOffsetChecked() : -1; }

    // The mutators that change the game state's answers (AFortAthenaMutator's bMutatesGameState), spawned by the
    // playlist's modifiers. The game state hears of each as its mutator observer (6.01 0xC19B80): on the authority
    // CurrentPlaylistInfo first gives the mutator the match's property overrides of its scope, then it joins this list,
    // which the game's own code walks for each answer (a new pawn's health and shield, 0xC0A540, its gravity, 0xC00510,
    // the fall damage, 0xC00470, the time of day at the match's start, 0xC27290, the items a death drops, 0xC26970).
    DefineUProperty(TArray<UObject*>, GameplayMutators)
    static ptrdiff_t GameplayMutatorsOffset() { return Member_GameplayMutators.IsAvailable() ? Member_GameplayMutators.GetOffsetChecked() : -1; }
    // Whether the match brings its players back after they die. Reflected (and replicated) from 6.01; an unreflected
    // flag of the game state before (from 4.2). The game mode's OnPlaylistDataLoaded sets it when the loaded playlist's
    // RespawnType isn't None (6.02 0xBD3A03), and a mutator that respawns players by itself sets and clears it
    // (AFortAthenaMutator_Disco: set at BeginPlay, 0xC70B7D, then at each game phase step, 0xC92753). The game mode's
    // IsRespawningAllowed reads it.
    DefineBitfieldUProperty(bAllowRespawning)
    static bool HasAllowRespawning() { return Member_bAllowRespawning.IsAvailable(); }
    // The flag's byte in the game state and its bit in that byte; -1 and 0 on a build that doesn't reflect it.
    static ptrdiff_t AllowRespawningOffset() { return HasAllowRespawning() ? Member_bAllowRespawning.GetOffsetChecked() : -1; }
    static uint8_t AllowRespawningMask() { return HasAllowRespawning() ? Member_bAllowRespawning.GetByteMask() : 0; }
    // Core's log of what became of the match's property overrides, once the playlist's modifiers have registered (the
    // game mode's HandleCurrentPlaylistLoaded): for each, the mutator of its scope and the value its property has now,
    // or that no mutator of that scope spawned. Nothing to say on a match without overrides.
    void ReportPropertyOverrides() const;
};
