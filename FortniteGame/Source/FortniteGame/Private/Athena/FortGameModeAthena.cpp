#include "pch.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortAthenaAircraft.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "BackendRequest.h"
#include "Configuration.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameThread.h"
#include "Math/Rotator.h"
#include "Finder.h"
#include "FortZoneTheme.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <string>

FString AFortGameModeAthena::DefaultMatchURL()
{
    // BP_ZT_Athena: the zone theme of Battle Royale's map (ZoneToUse /Game/Athena/Maps/Athena_Terrain, ZoneGameMode
    // Athena_GameMode_C, PlaylistId 2 solo; Core-Docs Athena-Boot-1.7.2.md §1 D5). The match's own playlist comes with
    // its matchmaking assignment. Builds whose Battle Royale map is named elsewhere (Chapter 2's) need their own source.
    // The engine asks for its default map many times while it starts.
    static FString Cached;
    if (Cached.Len() > 0)
    {
        return Cached;
    }

    constexpr const wchar_t* ThemeClassPath = L"/Game/World/ZoneThemes/PvP/BP_ZT_Athena.BP_ZT_Athena_C";
    UClass* ThemeClass = LoadObject<UClass>(nullptr, ThemeClassPath);
    UFortZoneTheme* Theme = ThemeClass ? Cast<UFortZoneTheme>(ThemeClass->GetDefaultObject()) : nullptr;
    const TAssetPtr<UWorld>* Zone = Theme ? &static_cast<TAssetPtr<UWorld>&>(Theme->ZoneToUse) : nullptr;
    const FString Map = Zone ? Zone->ToStringReference().GetLongPackageName() : FString();
    if (Map.Len() == 0)
    {
        Log::Error(L"Battle Royale: no map from {}, the server starts on its default map", ThemeClassPath);
        return FString();
    }

    const std::wstring URL = std::format(L"{}?playlistId={}", Map.ToStdString(), static_cast<int32_t>(Theme->PlaylistId));
    Log::Info(L"Battle Royale: the server starts on {}", URL);
    Cached = FString(URL.c_str());
    return Cached;
}

AFortGameModeAthena* AFortGameModeAthena::GetCurrent(const UObject* WorldContextObject)
{
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    return World ? Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
}

FName AFortGameModeAthena::RedirectLootTierGroup(FName TierGroup)
{
    // The override (1.7.2 0x3FA190) is intact, but its caller went with the stripped container and building code, so
    // Battle Royale's chests and ammo boxes rolled Save the World's Loot_Treasure and Loot_Ammo.
    // FName* RedirectLootTierGroup(FName* OutResult, FName InGroup), the name passed by value in r8.
    if (!Finder::ResolveLootTierRedirectLive())
    {
        return TierGroup;
    }
    FName Redirected;
    CallVirtual<FName*>(this, Natives::AFortGameMode_RedirectLootTierGroupIndex, &Redirected, TierGroup);
    return Redirected;
}

APawn* AFortGameModeAthena::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
#if WITH_SERVER_CODE
    // The aircraft is the start spot of everyone in it (ChoosePlayerStart, 0x3CA260), and at the end of its path everyone
    // still inside leaves it in the same frame (OnAircraftExitedDropZone, 0x3F35B0: RestartPlayer for each). AFortGameMode's
    // spawn (0x592920) won't put a pawn within 6 units of another player's: it asks ChoosePlayerStart for another spot, the
    // aircraft again, five times, then fails. So only the first of them left the bus and the rest stayed inside (#80). The
    // aircraft spawns the pawn where it is, whatever is there, as the engine's default spawn does (the spot's location and
    // yaw); the warmup island's spots keep AFortGameMode's spawn.
    AFortAthenaAircraft* Aircraft = Cast<AFortAthenaAircraft>(StartSpot);
    UClass* PawnClass = Aircraft && NewPlayer ? GetDefaultPawnClassForController(NewPlayer) : nullptr;
    UWorld* World = PawnClass ? GetWorld() : nullptr;
    if (World)
    {
        const FTransform SpotTransform = Aircraft->GetTransform();
        const float HalfYaw = SpotTransform.Rotation.Rotator().Yaw * (3.1415926535897932f / 360.0f);
        FTransform SpawnTransform;
        SpawnTransform.Rotation.Z = std::sin(HalfYaw);
        SpawnTransform.Rotation.W = std::cos(HalfYaw);
        SpawnTransform.SetLocation(SpotTransform.GetLocation());

        FActorSpawnParameters SpawnInfo;
        SpawnInfo.ObjectFlags = SpawnInfo.ObjectFlags | RF_Transient;
        SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        APawn* Pawn = Cast<APawn>(World->SpawnActor(PawnClass, &SpawnTransform, SpawnInfo));
        if (Pawn)
        {
            Log::Info(L"Battle Royale: {} leaves the aircraft as {}", NewPlayer->GetName().ToStdString(), Pawn->GetName().ToStdString());
        }
        else
        {
            Log::Error(L"Battle Royale: couldn't spawn {}'s {} at the aircraft", NewPlayer->GetName().ToStdString(), PawnClass->GetName().ToStdString());
        }
        return Pawn;
    }
#endif
    return AFortGameModeZone::SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}

namespace
{
    // A player of the match, kept after they leave it: a team's place comes when its last player is out, by which time
    // the others may have gone back to the lobby.
    struct FMatchPlayer
    {
        uint8_t Team = 0;
        int32_t Kills = 0;
        int32_t SecondsAlive = 0;
        int32_t Place = 0;
        // Who eliminated the player (their account id), on a build whose server doesn't count kills (1.8): empty while
        // the player is alive or was out by the storm or a fall.
        std::wstring EliminatedBy;
        bool bReported = false;
    };

    struct FMatchResults
    {
        std::map<std::wstring, FMatchPlayer> Players;
        std::chrono::steady_clock::time_point NextCheck;
        // Whether the backend should count a place 1 as the win: the build doesn't report its winners itself.
        bool bCountWins = false;
    };

    // An MCP account id is 32 hex digits (an invalid id's ToString is "INVALID").
    bool IsAccountId(const std::wstring& Id)
    {
        return !Id.empty() && Id.size() <= 64 && std::all_of(Id.begin(), Id.end(), [](wchar_t Character) { return std::iswxdigit(Character) != 0; });
    }

    // Brings the players up to date from their player states and reports those whose team has a place.
    void CheckMatchResults(AFortGameModeAthena* GameMode, FMatchResults& Results)
    {
        UWorld* World = GameMode->GetWorld();
        AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
        if (!GameState)
        {
            return;
        }

        const TArray<APlayerState*>& PlayerArray = GameState->PlayerArray;
        for (APlayerState* PlayerState : PlayerArray)
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            const std::wstring AccountId = AthenaState ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
            if (!IsAccountId(AccountId))
            {
                continue;
            }
            FMatchPlayer& Player = Results.Players[AccountId];
            if (Player.bReported)
            {
                continue;
            }
            Player.Team = AthenaState->TeamIndex;
            Player.SecondsAlive = AthenaState->SecondsAlive;
            Player.Place = std::max<int32_t>(AthenaState->Place, 0);
            if (!AFortPlayerStateAthena::CountsKills() && Player.EliminatedBy.empty())
            {
                AFortPlayerStateAthena* Eliminator = AthenaState->GetEliminator();
                const std::wstring EliminatorId = Eliminator && Eliminator != AthenaState ? Eliminator->UniqueId.ToString().ToStdString() : std::wstring();
                if (IsAccountId(EliminatorId))
                {
                    Player.EliminatedBy = EliminatorId;
                }
            }
        }

        // The kills, final by the time the player's team has a place. On 1.8 they are the other teams' players the
        // player eliminated, as 1.7.2's death handler counts them, kept in KillScore for the game's own use 🟡 (the HUD
        // and scoreboard weren't looked at).
        for (APlayerState* PlayerState : PlayerArray)
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            const std::wstring AccountId = AthenaState ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
            const auto Found = IsAccountId(AccountId) ? Results.Players.find(AccountId) : Results.Players.end();
            if (Found == Results.Players.end() || Found->second.bReported)
            {
                continue;
            }
            FMatchPlayer& Player = Found->second;
            if (!AFortPlayerStateAthena::CountsKills())
            {
                const int32_t Kills = static_cast<int32_t>(std::count_if(Results.Players.begin(), Results.Players.end(), [&](const auto& Other)
                {
                    return Other.second.EliminatedBy == AccountId && Other.second.Team != Player.Team;
                }));
                AthenaState->SetKills(Kills);
            }
            Player.Kills = AthenaState->GetKills();
        }

        // The team's place, for its players who left before it was out.
        for (auto& [AccountId, Player] : Results.Players)
        {
            for (const auto& [OtherId, Other] : Results.Players)
            {
                if (Player.Place == 0 && Other.Place > 0 && Other.Team == Player.Team)
                {
                    Player.Place = Other.Place;
                }
            }
        }

        std::string Body;
        int32_t Count = 0;
        for (auto& [AccountId, Player] : Results.Players)
        {
            if (Player.bReported || Player.Place == 0)
            {
                continue;
            }
            Player.bReported = true;
            std::string Id;
            std::transform(AccountId.begin(), AccountId.end(), std::back_inserter(Id), [](wchar_t Digit) { return static_cast<char>(Digit); });
            Body += std::format("{}{{\"accountId\":\"{}\",\"place\":{},\"kills\":{},\"secondsAlive\":{}}}", Count++ > 0 ? "," : "",
                Id, Player.Place, std::max(Player.Kills, 0), std::max(Player.SecondsAlive, 0));
            Log::Info(L"Battle Royale: {} placed {} with {} kills, {} s alive", AccountId, Player.Place, Player.Kills, Player.SecondsAlive);
        }
        if (Count > 0)
        {
            const int32_t PlaylistId = GameMode->CurrentPlaylistId;
            BackendRequest::PostJson(std::format(L"Battle Royale results of {} players", Count), L"/fortnite/api/game/v2/battleroyale/recordresults",
                std::format("{{\"playlistId\":{},\"results\":[{}],\"countWins\":{}}}", PlaylistId, Body, Results.bCountWins ? "true" : "false"),
                FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret);
        }
    }

    void WatchResults(FWeakObjectPtr GameMode, std::shared_ptr<FMatchResults> Results)
    {
        GameThread::Enqueue([GameMode, Results]()
        {
            AFortGameModeAthena* Athena = static_cast<AFortGameModeAthena*>(GameMode.Get());
            if (!Athena)
            {
                return;
            }
            const auto Now = std::chrono::steady_clock::now();
            if (Now >= Results->NextCheck)
            {
                Results->NextCheck = Now + std::chrono::seconds(1);
                CheckMatchResults(Athena, *Results);
            }
            WatchResults(GameMode, Results);
        });
    }
}

void AFortGameModeAthena::WatchMatchResults()
{
#if WITH_SERVER_CODE
    // Epic's stats service kept Battle Royale's stats (Balance/DataTables/LeaderboardStats: br_placetop<n>, br_kills,
    // br_matchesplayed, br_minutesplayed per playlist) from what its servers reported, but the 1.7.2 server reports only
    // its winners (battleroyale/recordwin, 0x72E130): UFortMcpUtils::RecordUserStatsUrl (/api/stats/bulk?ownertype=1)
    // has no caller left, so wins were the only stat. The rest comes from the player states: Place is set for every
    // player of a team when the team is out (TeamsLeft then, 0x44A870, from the death handler 0x437C30) or has won (1,
    // 0x400490 and 0x4008F0), with SecondsAlive and the player's Kills final by then. They are read once a second and
    // each player is reported once, with the DedicatedServer client's token. Core-Backend counts the win from recordwin,
    // which 1.8 doesn't have (Core-Backend #81): from a build without it, the report says to count a place 1 as the win.
    auto Results = std::make_shared<FMatchResults>();
    Results->bCountWins = Finder::StringRefs(L"/api/game/v2/battleroyale/recordwin").empty();
    WatchResults(FWeakObjectPtr(this), Results);
    Log::Info(L"Battle Royale: reporting the match's results to {} ({}{})", Configuration::Get().Backend,
        Results->bCountWins ? L"with its wins" : L"its wins through the build's recordwin",
        AFortPlayerStateAthena::CountsKills() ? L"" : AFortPlayerStateAthena::HasEliminator() ? L", kills counted by Core" : L", no kills: no DeathInfo");
#endif
}

void AFortGameModeAthena::HookVTables()
{
    UClass* AthenaClass = StaticClass();
    UObject* ZoneDefault = AthenaClass ? AFortGameModeZone::StaticClass()->GetDefaultObject(false) : nullptr;
    void** ZoneVTable = ZoneDefault ? *reinterpret_cast<void***>(ZoneDefault) : nullptr;
    if (!ZoneVTable || !Natives::AFortGameModeZone_SpawnDefaultPawnFor)
    {
        return;
    }

    // AFortGameModeAthena::SpawnDefaultPawnFor_Implementation (1.7.2 0x3FEE60) keeps its four "SpawnDefaultPawnFor:
    // Failed due to no ..." checks and returns null where it spawned the pawn, so nothing spawns: not on the warmup island,
    // not on a warmup respawn, not when jumping from the aircraft or at its forced drop (all RestartPlayer). Its slot is the
    // Zone mode's; the Zone mode's restoration (the checks, then AFortGameMode's spawn at the start spot) is the same, apart
    // from the aircraft (SpawnDefaultPawnFor_Implementation).
    // The first slot holding it: vtables sit back to back (no RTTI), so the next class's repeats it, which
    // Finder::VTableIndex counts as ambiguous.
    int32_t SpawnIndex = -1;
    for (int32_t Index = 0; Index < 0x200 && SpawnIndex < 0; ++Index)
    {
        SpawnIndex = ZoneVTable[Index] == reinterpret_cast<void*>(Natives::AFortGameModeZone_SpawnDefaultPawnFor) ? Index : -1;
    }
    if (SpawnIndex < 0)
    {
        Log::Error(L"Finder: AFortGameModeZone::SpawnDefaultPawnFor_Implementation's slot not found, Battle Royale players won't spawn");
        return;
    }
    for (TObjectIterator<UClass> It; SpawnIndex >= 0 && It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[SpawnIndex] != reinterpret_cast<void*>(&SpawnDefaultPawnFor_ImplementationHK))
        {
            Hooks::SwapVTable(L"AFortGameModeAthena::SpawnDefaultPawnFor_Implementation", VTable, SpawnIndex, reinterpret_cast<void*>(&SpawnDefaultPawnFor_ImplementationHK));
        }
    }
}

APawn* AFortGameModeAthena::SpawnDefaultPawnFor_ImplementationHK(AFortGameModeAthena* This, AController* NewPlayer, AActor* StartSpot)
{
    return This->SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}

void AFortGameModeAthena::HookCallSites()
{
    // StartAircraftPhase's per-player cleanup after the warmup pawn is destroyed is stripped (a call to the shared empty
    // function), so the warmup island's loot went onto the aircraft (#86).
    if (uint8_t* Call = Finder::AircraftPhasePlayerCleanupCall())
    {
        Hooks::RedirectCall(L"AFortGameModeAthena::StartAircraftPhase player cleanup", Call, reinterpret_cast<void*>(&AircraftPhasePlayerCleanupCall));
    }
}

void AFortGameModeAthena::AircraftPhasePlayerCleanupCall(AController* Controller)
{
    if (AFortPlayerControllerAthena* PlayerController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller)))
    {
        PlayerController->ResetToStartingItems();
    }
}
