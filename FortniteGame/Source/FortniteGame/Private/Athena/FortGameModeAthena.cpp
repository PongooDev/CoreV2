#include "pch.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortAthenaAircraft.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "BackendRequest.h"
#include "Building/BuildingSMActor.h"
#include "Configuration.h"
#include "Engine/EngineTypes.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameThread.h"
#include "Math/Rotator.h"
#include "Misc/Guid.h"
#include "Finder.h"
#include "FortZoneTheme.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Online/FortGameSession.h"
#include "Online/FortPartyBeaconHost.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

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

bool AFortGameModeAthena::AddPlaylistContextTag(FGameplayTagContainer& Tags)
{
    // The game mode's GameContextTags (1.8 +0x5A0) are the context of every quest event (AFortPlayerController's
    // GetSourceAndContextTags, 0x87DC20, through the game mode's 0x3F2D80), and Battle Royale's objectives ask for the
    // playlist there (interact_athena_treasurechest, daily_athena_loot_chest's: Athena.Playlist). Only Save the World's
    // world record fills them (OnInitWorldRecordComplete, 0x591340: theater, zone theme and tile tags; BP_ZT_Athena has
    // no ZoneTags), and no code names the Athena.Playlist tags (PvPTags rows 21-23), so a Battle Royale event had no
    // context and matched none of them. The playlist ids are MCP's, as the match's CurrentPlaylistId has them.
    const int32_t PlaylistId = CurrentPlaylistId;
    const wchar_t* const Name = PlaylistId == 2 ? L"Athena.Playlist.Solo" : PlaylistId == 10 ? L"Athena.Playlist.Duo" : PlaylistId == 9 ? L"Athena.Playlist.Squad" : L"Athena.Playlist";
    const FGameplayTag Tag{ FName(Name) };
    if (Tags.GameplayTags.Contains(Tag))
    {
        return false;
    }
    Tags.GameplayTags.Add(Tag);
    for (const wchar_t* const ParentName : { L"Athena.Playlist", L"Athena" })
    {
        const FGameplayTag Parent{ FName(ParentName) };
        if (!(Parent == Tag) && !Tags.ParentTags.Contains(Parent))
        {
            Tags.ParentTags.Add(Parent);
        }
    }
    return true;
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
        // The player's client has its end-of-match stats and rewards (SendMatchReport).
        bool bSentReport = false;
    };

    struct FMatchResults
    {
        std::map<std::wstring, FMatchPlayer> Players;
        // The most players the match had left at once: its players, for the end-of-match screen.
        int32_t MostPlayersLeft = 0;
        std::chrono::steady_clock::time_point NextCheck;
        // Whether the backend should count a place 1 as the win: the build doesn't report its winners itself.
        bool bCountWins = false;
        // The match's own id, new for each match, so the backend pays its V-Bucks rewards once (Core-Backend #100).
        std::string MatchId;
    };

    // An MCP account id is 32 hex digits (an invalid id's ToString is "INVALID").
    bool IsAccountId(const std::wstring& Id)
    {
        return !Id.empty() && Id.size() <= 64 && std::all_of(Id.begin(), Id.end(), [](wchar_t Character) { return std::iswxdigit(Character) != 0; });
    }

    // The end-of-match screen (AthenaMatchStatsPartial) shows the stats of the controller's match report, and waits for
    // its rewards with "Team Still Alive" until they come: the server sends both, with ClientSendMatchStatsForPlayer and
    // ClientSendEndBattleRoyaleMatchForPlayer. 1.9, 1.9.1 and 1.10 have no caller of either (no code names them), so the
    // screen showed zeros and kept waiting after the player's team was out (#162). Core sends them once the player has a
    // place, with what the server knows: the place, the match's players, the time alive and the kills. The screen's other
    // stats (accuracy, hits, distance, damage, materials) aren't kept by anything on the server and stay 0, and the
    // rewards it sends are empty: they end the wait, and the screen plays no XP.
    void SendMatchReport(AFortPlayerStateAthena* AthenaState, const FMatchPlayer& Player, int32_t TotalPlayers)
    {
        AFortPlayerControllerAthena* Controller = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(AthenaState->GetOwner()));
        UFunction* SendStats = AFortPlayerControllerAthena::FindStaticFunction_ClientSendMatchStatsForPlayer();
        UFunction* SendEnd = AFortPlayerControllerAthena::FindStaticFunction_ClientSendEndBattleRoyaleMatchForPlayer();
        if (!Controller || !SendStats || !SendEnd)
        {
            return;
        }

        Reflection::FParams Stats(SendStats, true);
        UProperty* StatsProperty = SendStats->FindPropertyByName(FName(L"Stats", FNAME_Find));
        UScriptStruct* StatsStruct = StatsProperty && StatsProperty->GetClass()->HasAnyCastFlag(CASTCLASS_UStructProperty) ? static_cast<UStructProperty*>(StatsProperty)->Struct : nullptr;
        uint8_t* StatsData = static_cast<uint8_t*>(Stats.GetAddress(L"Stats"));
        if (!StatsStruct || !StatsData)
        {
            return;
        }
        const auto SetStat = [StatsStruct, StatsData](const wchar_t* Name, int32_t Value)
        {
            UProperty* Field = StatsStruct->FindPropertyByName(FName(Name, FNAME_Find));
            if (Field && Field->ElementSize == sizeof(Value))
            {
                std::memcpy(StatsData + Field->GetOffset_ForInternal(), &Value, sizeof(Value));
            }
        };
        SetStat(L"Place", Player.Place);
        SetStat(L"TotalPlayers", std::max(TotalPlayers, Player.Place));
        SetStat(L"SecondsAlive", std::max(Player.SecondsAlive, 0));
        SetStat(L"Kills", std::max(Player.Kills, 0));
        Stats.Call(Controller);

        Reflection::FParams End(SendEnd, true);
        End.Set(L"bSuccess", true);
        End.Call(Controller);
        Log::Info(L"Battle Royale: sent {} the end-of-match screen (place {} of {}, {} kills, {} s alive)", Controller->GetName().ToStdString(), Player.Place,
            std::max(TotalPlayers, Player.Place), std::max(Player.Kills, 0), std::max(Player.SecondsAlive, 0));
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
        if (AFortGameStateAthena* AthenaGameState = Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(GameState)))
        {
            Results.MostPlayersLeft = std::max<int32_t>(Results.MostPlayersLeft, AthenaGameState->PlayersLeft);
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

        // The end-of-match screen of each player still here whose place is final.
        const int32_t TotalPlayers = std::max(Results.MostPlayersLeft, static_cast<int32_t>(Results.Players.size()));
        for (APlayerState* PlayerState : PlayerArray)
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            const std::wstring AccountId = AthenaState ? AthenaState->UniqueId.ToString().ToStdString() : std::wstring();
            const auto Found = IsAccountId(AccountId) ? Results.Players.find(AccountId) : Results.Players.end();
            if (Found != Results.Players.end() && Found->second.bReported && !Found->second.bSentReport)
            {
                Found->second.bSentReport = true;
                SendMatchReport(AthenaState, Found->second, TotalPlayers);
            }
        }
        if (Count > 0)
        {
            const int32_t PlaylistId = GameMode->CurrentPlaylistId;
            BackendRequest::PostJson(std::format(L"Battle Royale results of {} players", Count), L"/fortnite/api/game/v2/battleroyale/recordresults",
                std::format("{{\"playlistId\":{},\"results\":[{}],\"countWins\":{},\"matchId\":\"{}\"}}", PlaylistId, Body,
                    Results.bCountWins ? "true" : "false", Results.MatchId),
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
    const FGuid MatchGuid = FGuid::NewGuid();
    Results->MatchId = std::format("{:08X}{:08X}{:08X}{:08X}", MatchGuid.A, MatchGuid.B, MatchGuid.C, MatchGuid.D);
    WatchResults(FWeakObjectPtr(this), Results);
    Log::Info(L"Battle Royale: reporting the match's results to {} ({}{})", Configuration::Get().Backend,
        Results->bCountWins ? L"with its wins" : L"its wins through the build's recordwin",
        AFortPlayerStateAthena::CountsKills() ? L"" : AFortPlayerStateAthena::HasEliminator() ? L", kills counted by Core" : L", no kills: no DeathInfo");
#endif
}

namespace
{
    // How often the server asks for the admin panel's settings before its warmup, and reports during it.
    constexpr auto WarmupAskInterval = std::chrono::seconds(30);
    constexpr auto WarmupReportInterval = std::chrono::seconds(1);
    constexpr auto ReserveInterval = std::chrono::milliseconds(100);

    struct FWarmupWatch
    {
        std::chrono::steady_clock::time_point NextAsk{};
        std::chrono::steady_clock::time_point NextReserve{};
        bool bAsking = false;
        // The backend has no such route (one from before Core-Backend #122): it isn't asked again.
        bool bUnsupported = false;
        // The bus has left and the backend has heard so.
        bool bReportedEnd = false;
        // The phase the last check saw: a new one is reported at once.
        uint8_t LastPhase = 0xFF;

        // The backend's answer: the warmup's length (0: the game's own), whether it answered and has late joining on,
        // whether the matchmaking service sends parties here, the match's room, and the late parties it sent (their
        // account ids).
        int32_t WarmupSeconds = 0;
        bool bAnswered = false;
        bool bLateJoin = false;
        bool bOpen = false;
        int32_t MaxPlayers = 0;
        std::vector<std::vector<std::wstring>> Parties;

        // The game's own warmup lengths, from before Core first changed them (-1 until then), and the length the game
        // mode has now (0: the game's).
        float GameDuration = -1.0f;
        float GameEarly = -1.0f;
        int32_t AppliedSeconds = 0;
        // The game's warmup keeps its shorter countdown for a nearly full match: the match can't grow (no late joining)
        // or is of full size (MatchCapacity).
        bool bEarlyCountdown = false;
        bool bLoggedLength = false;
        // The running countdown was set to AppliedSeconds.
        bool bCountdownSet = false;
        bool bWarnedNoWarmupLength = false;

        // Late joining: the match was opened (MaxPlayerCount raised), the players who connected during the warmup
        // without a reservation, and those known to have their team (a reservation, and a player controller: PickTeam
        // has run).
        bool bOpened = false;
        bool bWarnedNoReservations = false;
        bool bWarnedNoTeam = false;
        std::set<std::wstring> LateIds;
        std::set<std::wstring> Settled;
    };

    std::string NarrowId(const std::wstring& Id)
    {
        std::string Result;
        std::transform(Id.begin(), Id.end(), std::back_inserter(Result), [](wchar_t Character) { return static_cast<char>(Character); });
        return Result;
    }

    std::wstring WidenId(const std::string& Id)
    {
        return std::wstring(Id.begin(), Id.end());
    }

    // The answer's "parties": [["<accountId>", ...], ...].
    std::vector<std::vector<std::wstring>> JsonParties(const std::string& Json)
    {
        std::vector<std::vector<std::wstring>> Parties;
        const size_t Key = Json.find("\"parties\"");
        size_t Index = Key == std::string::npos ? std::string::npos : Json.find('[', Key);
        int32_t Depth = 0;
        for (; Index != std::string::npos && Index < Json.size(); ++Index)
        {
            const char Character = Json[Index];
            if (Character == '[')
            {
                if (++Depth == 2)
                {
                    Parties.emplace_back();
                }
            }
            else if (Character == ']')
            {
                if (--Depth == 0)
                {
                    break;
                }
            }
            else if (Character == '"' && Depth == 2)
            {
                const size_t End = Json.find('"', Index + 1);
                if (End == std::string::npos)
                {
                    break;
                }
                Parties.back().push_back(WidenId(Json.substr(Index + 1, End - Index - 1)));
                Index = End;
            }
        }
        return Parties;
    }

    AFortGameStateAthena* AthenaGameStateOf(AFortGameModeAthena* GameMode)
    {
        UWorld* World = GameMode->GetWorld();
        return World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    }

    // The reservation beacon's state (AFortGameSession's ReservationBeaconHost), or null.
    UPartyBeaconState* BeaconStateOf(AFortGameModeAthena* GameMode)
    {
        AFortGameSession* Session = AFortGameSession::HasReservationBeacon() ? Cast<AFortGameSession>(reinterpret_cast<UObject*>(static_cast<AGameSession*>(GameMode->GameSession))) : nullptr;
        APartyBeaconHost* Beacon = Session && APartyBeaconHost::IsReflected() ? static_cast<APartyBeaconHost*>(Session->ReservationBeaconHost) : nullptr;
        return Beacon ? static_cast<UPartyBeaconState*>(Beacon->State) : nullptr;
    }

    // The most players a match on this server can have: its game session's own MaxPlayers, from its config (the class
    // default: OnGivenMatchAssignment, 1.9 0x42D2B0, overwrites the session's with the match's reservations), capped by
    // the playlist's teams (the beacon's NumTeams x NumPlayersPerTeam, once the match is assigned). 0 when unknown.
    int32_t MatchCapacity(AFortGameModeAthena* GameMode)
    {
        AGameSession* Session = static_cast<AGameSession*>(GameMode->GameSession);
        const UObject* Default = Session && AGameSession::HasMaxPlayers() ? Session->GetClass()->GetDefaultObject(false) : nullptr;
        int32_t Capacity = Default ? static_cast<const AGameSession*>(Default)->MaxPlayers : 0;
        if (UPartyBeaconState* State = BeaconStateOf(GameMode))
        {
            const int32_t Teams = State->NumTeams * State->NumPlayersPerTeam;
            if (Teams > 0)
            {
                Capacity = Capacity > 0 ? std::min(Capacity, Teams) : Teams;
            }
        }
        return Capacity;
    }

    // The account ids of the players connected (from their login on, so those still loading the map count).
    std::vector<std::wstring> ConnectedAccounts(AFortGameModeAthena* GameMode)
    {
        std::vector<std::wstring> Ids;
        UWorld* World = GameMode->GetWorld();
        UNetDriver* Driver = World && UNetConnection::HasPlayerID() ? World->GetNetDriver() : nullptr;
        if (!Driver)
        {
            return Ids;
        }
        for (UNetConnection* Connection : static_cast<TArray<UNetConnection*>&>(Driver->ClientConnections))
        {
            const std::wstring Id = Connection && Connection->PlayerID.IsValid() ? Connection->PlayerID.ToString().ToStdString() : std::wstring();
            if (IsAccountId(Id))
            {
                Ids.push_back(Id);
            }
        }
        return Ids;
    }

    void ReportWarmup(const std::shared_ptr<FWarmupWatch>& Watch, const std::wstring& Phase, const std::vector<std::wstring>& AccountIds, float SecondsLeft)
    {
        std::string Ids;
        for (const std::wstring& Id : AccountIds)
        {
            Ids += std::format("{}\"{}\"", Ids.empty() ? "" : ",", NarrowId(Id));
        }
        Watch->bAsking = true;
        BackendRequest::PostJson(L"Battle Royale warmup report", L"/fortnite/api/game/v2/matchmakingservice/warmup",
            std::format("{{\"phase\":\"{}\",\"accountIds\":[{}],\"secondsLeft\":{:.1f}}}", NarrowId(Phase), Ids, std::max(SecondsLeft, 0.0f)),
            FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret, [Watch](uint32_t Status, const std::string& Body)
        {
            // {warmupSeconds: a number or null, lateJoin, sessionId, open, maxPlayers, parties}. Anything else (no
            // answer) keeps what the server has.
            const std::string Seconds = Status == 200 ? BackendRequest::JsonField(Body, "warmupSeconds") : std::string();
            const bool bAnswered = Seconds == "null" || (!Seconds.empty() && std::isdigit(static_cast<unsigned char>(Seconds[0])));
            const int32_t WarmupSeconds = bAnswered && Seconds != "null" ? std::clamp(std::atoi(Seconds.c_str()), 0, 3600) : 0;
            const bool bLateJoin = bAnswered && BackendRequest::JsonField(Body, "lateJoin") == "true";
            const bool bOpen = bAnswered && BackendRequest::JsonField(Body, "open") == "true";
            const int32_t MaxPlayers = bAnswered ? std::clamp(std::atoi(BackendRequest::JsonField(Body, "maxPlayers").c_str()), 0, 1000) : 0;
            auto Parties = bAnswered ? JsonParties(Body) : std::vector<std::vector<std::wstring>>();
            GameThread::Enqueue([Watch, Status, bAnswered, WarmupSeconds, bLateJoin, bOpen, MaxPlayers, Parties = std::move(Parties)]() mutable
            {
                Watch->bAsking = false;
                if (Status == 404 && !Watch->bUnsupported)
                {
                    Watch->bUnsupported = true;
                    Log::Warning(L"Battle Royale: the backend has no warmup settings (Core-Backend #122), so the match keeps the game's warmup and takes no late players");
                }
                if (!bAnswered)
                {
                    return;
                }
                Watch->WarmupSeconds = WarmupSeconds;
                Watch->bAnswered = true;
                Watch->bLateJoin = bLateJoin;
                Watch->bOpen = bOpen;
                Watch->MaxPlayers = MaxPlayers;
                Watch->Parties = std::move(Parties);
            });
        });
    }

    // The game mode's warmup lengths: the backend's or the game's own, and a countdown already running moved to the
    // backend's. Either way the countdown still shortens to the game's early countdown once the match's players are all
    // in, as it did before the panel's setting (the backend's length only replaces the full one). The game shortens its warmup to
    // WarmupEarlyCountdownDuration (15 s) once enough of MaxPlayerCount is in (UpdateWarmupPhase, 1.9 0x43D9A0:
    // NumPlayers >= MaxPlayerCount times a percent at +0xABC, which isn't reflected), and OnGivenMatchAssignment
    // (0x42D2B0) sets MaxPlayerCount to the players the match was assigned. So a match taking late players had its bus
    // leave 15 s into the game's 90 s warmup, before the backend's answer opened it (Core-Backend #122). With the
    // game's length the shorter countdown is the full one while the match may still grow: until the backend says late
    // joining is off (or has no warmup settings), when the assigned players are all the match gets and the countdown
    // shortens once they are in (#153), or until MaxPlayerCount is the most a match here can have (MatchCapacity), as
    // when late joining opened it to them; the game's percent then counts a match of full size, as on Epic's servers.
    void ApplyWarmupLength(AFortGameModeAthena* GameMode, AFortGameStateAthena* GameState, FWarmupWatch& Watch, float Now)
    {
        if (!AFortGameModeAthena::HasWarmupLength())
        {
            if (Watch.WarmupSeconds > 0 && !Watch.bWarnedNoWarmupLength)
            {
                Watch.bWarnedNoWarmupLength = true;
                Log::Warning(L"Battle Royale: this build reflects no WarmupCountdownDuration, so the warmup keeps the game's length");
            }
            return;
        }

        if (Watch.GameDuration < 0.0f)
        {
            Watch.GameDuration = GameMode->WarmupCountdownDuration;
            Watch.GameEarly = GameMode->WarmupEarlyCountdownDuration;
        }
        const bool bGame = Watch.WarmupSeconds <= 0;
        const float Duration = bGame ? Watch.GameDuration : static_cast<float>(Watch.WarmupSeconds);
        const int32_t Capacity = MatchCapacity(GameMode);
        const bool bClosed = Watch.bUnsupported || (Watch.bAnswered && !Watch.bLateJoin);
        const bool bEarly = bClosed || (AFortGameModeAthena::HasMaxPlayerCount() && Capacity > 0 && GameMode->MaxPlayerCount >= Capacity);
        GameMode->WarmupCountdownDuration = Duration;
        GameMode->WarmupEarlyCountdownDuration = bEarly ? std::min(Watch.GameEarly, Duration) : Duration;

        if (!Watch.bLoggedLength || Watch.WarmupSeconds != Watch.AppliedSeconds || bEarly != Watch.bEarlyCountdown)
        {
            if (Watch.WarmupSeconds != Watch.AppliedSeconds)
            {
                Watch.bCountdownSet = bGame;
            }
            Watch.bLoggedLength = true;
            Watch.AppliedSeconds = Watch.WarmupSeconds;
            Watch.bEarlyCountdown = bEarly;
            if (!bGame)
            {
                Log::Info(L"Battle Royale: the warmup lasts {} s (the backend's), {}", Watch.WarmupSeconds,
                    bEarly ? std::format(L"{:.0f} s once the match's players are all in", std::min(Watch.GameEarly, Duration)) : std::wstring(L"in full while late players can still join"));
            }
            else if (bEarly)
            {
                Log::Info(L"Battle Royale: the warmup is the game's: {:.0f} s, {:.0f} s once enough of the match's {} players are in{}", Watch.GameDuration, Watch.GameEarly,
                    AFortGameModeAthena::HasMaxPlayerCount() ? GameMode->MaxPlayerCount : 0, bClosed ? L" (no late joining)" : L"");
            }
            else
            {
                Log::Info(L"Battle Royale: the warmup is the game's {:.0f} s in full (its {:.0f} s countdown once enough players are in is for a match of {} players)", Watch.GameDuration, Watch.GameEarly, Capacity);
            }
        }

        // UpdateWarmupPhase (1.9 0x43D9A0) sets the countdown once, when the warmup starts: one that started before the
        // backend answered takes the backend's length from its start.
        if (!Watch.bCountdownSet && Watch.AppliedSeconds > 0 && GameState->GamePhase == EAthenaGamePhase::GetWarmup() && AFortGameStateAthena::HasWarmupCountdown() && GameState->WarmupCountdownEndTime >= 0.0f)
        {
            Watch.bCountdownSet = true;
            const float End = std::max(GameState->WarmupCountdownStartTime + static_cast<float>(Watch.AppliedSeconds), Now + 1.0f);
            Log::Info(L"Battle Royale: the warmup's countdown ends in {:.0f} s (was {:.0f} s)", End - Now, GameState->WarmupCountdownEndTime - Now);
            GameState->WarmupCountdownEndTime = End;
        }
    }

    // Late joining: the match lets in as many players as the backend gives it room for. The game mode lets in only the
    // assigned players (OnGivenMatchAssignment, 1.9 0x42D2B0, sets MaxPlayerCount and the game session's MaxPlayers
    // from the beacon's reservations), PreLogin turning anyone else away ("Match is full", 0x4319F0) until the bus
    // leaves ("Match has started": StartAircraftPhase sets the flag, 0x4388CB). Raised during the warmup only: the
    // match has started then (HandleMatchHasStarted, 0x427D90, clears the failed assignment timer, which kicks everyone
    // when it fires, 0x42D100). The playlist's teams cap it.
    void OpenMatch(AFortGameModeAthena* GameMode, FWarmupWatch& Watch)
    {
        UPartyBeaconState* State = BeaconStateOf(GameMode);
        AGameSession* Session = static_cast<AGameSession*>(GameMode->GameSession);
        if (!State || !Session || !AFortGameModeAthena::HasMaxPlayerCount() || !AGameSession::HasMaxPlayers() || !UNetConnection::HasPlayerID())
        {
            if (!Watch.bWarnedNoReservations)
            {
                Watch.bWarnedNoReservations = true;
                Log::Warning(L"Battle Royale: no reservation beacon, MaxPlayerCount or connection player id on this build, so the match takes no late players");
            }
            return;
        }
        const int32_t Room = std::min(Watch.MaxPlayers, State->NumTeams * State->NumPlayersPerTeam);
        if (Room <= 0)
        {
            return;
        }
        if (Room > GameMode->MaxPlayerCount || Room > Session->MaxPlayers)
        {
            Log::Info(L"Battle Royale: the match takes late players during its warmup, up to {} (MaxPlayerCount was {}, the session's MaxPlayers {})", Room, GameMode->MaxPlayerCount, Session->MaxPlayers);
            GameMode->MaxPlayerCount = std::max(GameMode->MaxPlayerCount, Room);
            Session->MaxPlayers = std::max(Session->MaxPlayers, Room);
        }
        Watch.bOpened = true;
    }

    // Gives each player who joined during the warmup a team. Battle Royale's clients never talk to the beacon, so only
    // the assigned players have a reservation, and PickTeam (1.9 0x430A50) gives the beacon's team to anyone on a
    // dedicated session's match: for a player without one that is Spectator (0x6FF3D0 answers 103), which has no
    // place in the match. So each such player gets a reservation of their own between their login and their joining
    // the game (map load, then Login and PickTeam): on their party's reservation when the backend sent their party
    // and a member already holds one, else on the first team no reservation has. The beacon's reservations are its
    // state's reflected array (UPartyBeaconState::Reservations), the ids copied from the connection with the game's
    // own struct copy, and a player is looked at again until they have a player controller, in case the beacon dropped
    // one that took too long to load (APartyBeaconHost's session timeout).
    void ReserveLatePlayers(AFortGameModeAthena* GameMode, FWarmupWatch& Watch, bool bWarmup)
    {
        UPartyBeaconState* State = BeaconStateOf(GameMode);
        UWorld* World = GameMode->GetWorld();
        UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
        static UScriptStruct* const IdStruct = FindObject<UScriptStruct>(nullptr, L"/Script/Engine.UniqueNetIdRepl");
        if (!State || !Driver || !IdStruct)
        {
            return;
        }

        std::vector<std::pair<std::wstring, UNetConnection*>> Unsettled;
        for (UNetConnection* Connection : static_cast<TArray<UNetConnection*>&>(Driver->ClientConnections))
        {
            const std::wstring Id = Connection && Connection->PlayerID.IsValid() ? Connection->PlayerID.ToString().ToStdString() : std::wstring();
            // After the bus has left a new player is turned away ("Match has started"): only those who came before.
            if (IsAccountId(Id) && !Watch.Settled.contains(Id) && (bWarmup || Watch.LateIds.contains(Id)))
            {
                Unsettled.emplace_back(Id, Connection);
            }
        }
        if (Unsettled.empty())
        {
            return;
        }

        TStructArray<FPartyReservation>& Reservations = State->GetMember_Reservations();
        std::map<std::wstring, int32_t> ReservationOf;
        std::set<int32_t> Teams;
        for (int32_t Index = 0; Index < Reservations.Num(); ++Index)
        {
            FPartyReservation& Reservation = Reservations[Index];
            Teams.insert(Reservation.TeamNum);
            TStructArray<FPlayerReservation>& Members = Reservation.GetMember_PartyMembers();
            for (int32_t Member = 0; Member < Members.Num(); ++Member)
            {
                ReservationOf[Members[Member].UniqueId.ToString().ToStdString()] = Index;
            }
        }

        for (const auto& [Id, Connection] : Unsettled)
        {
            const bool bJoined = static_cast<APlayerController*>(Connection->PlayerController) != nullptr;
            if (ReservationOf.contains(Id))
            {
                if (bJoined)
                {
                    Watch.Settled.insert(Id);
                }
                continue;
            }
            if (bJoined)
            {
                // In the game before Core saw them: PickTeam has run.
                Watch.Settled.insert(Id);
                Log::Warning(L"Battle Royale: {} joined without a reservation, so PickTeam made them a Spectator", Id);
                continue;
            }
            Watch.LateIds.insert(Id);

            int32_t Index = -1;
            const auto Party = std::find_if(Watch.Parties.begin(), Watch.Parties.end(), [&](const std::vector<std::wstring>& Members) { return std::find(Members.begin(), Members.end(), Id) != Members.end(); });
            for (const std::wstring& Member : Party != Watch.Parties.end() ? *Party : std::vector<std::wstring>())
            {
                const auto Found = ReservationOf.find(Member);
                if (Found != ReservationOf.end() && Reservations[Found->second].GetMember_PartyMembers().Num() < State->NumPlayersPerTeam)
                {
                    Index = Found->second;
                    break;
                }
            }
            if (Index < 0)
            {
                int32_t Team = 0;
                while (Team < State->NumTeams && Teams.contains(Team))
                {
                    ++Team;
                }
                if (Team >= State->NumTeams)
                {
                    if (!Watch.bWarnedNoTeam)
                    {
                        Watch.bWarnedNoTeam = true;
                        Log::Warning(L"Battle Royale: all {} teams are taken, so {} has no team", State->NumTeams, Id);
                    }
                    continue;
                }
                Index = Reservations.Num();
                FPartyReservation& Reservation = Reservations.AddDefaulted();
                Reservation.TeamNum = Team;
                IdStruct->CopyScriptStruct(&Reservation.GetMember_PartyLeader(), &Connection->GetMember_PlayerID());
                Teams.insert(Team);
            }
            FPartyReservation& Reservation = Reservations[Index];
            FPlayerReservation& Member = Reservation.GetMember_PartyMembers().AddDefaulted();
            IdStruct->CopyScriptStruct(&Member.GetMember_UniqueId(), &Connection->GetMember_PlayerID());
            ReservationOf[Id] = Index;
            Log::Info(L"Battle Royale: {} joins in the warmup on HumanPvP_Team{} (beacon team {}, {} of its party there)", Id, Reservation.TeamNum + 1, Reservation.TeamNum,
                Reservation.GetMember_PartyMembers().Num());
        }
    }

    void CheckWarmup(AFortGameModeAthena* GameMode, const std::shared_ptr<FWarmupWatch>& Watch, std::chrono::steady_clock::time_point Now)
    {
        AFortGameStateAthena* GameState = AthenaGameStateOf(GameMode);
        UWorld* World = GameMode->GetWorld();
        if (!GameState || !World)
        {
            return;
        }
        const uint8_t Phase = GameState->GamePhase;
        const bool bWarmup = Phase == EAthenaGamePhase::GetWarmup();
        const float WorldNow = World->GetTimeSeconds();
        if (Phase != Watch->LastPhase)
        {
            Watch->LastPhase = Phase;
            Watch->NextAsk = Now;
        }

        if (Phase <= EAthenaGamePhase::GetWarmup())
        {
            ApplyWarmupLength(GameMode, GameState, *Watch, WorldNow);
        }
        if (bWarmup && Watch->bOpen)
        {
            OpenMatch(GameMode, *Watch);
        }
        if (Watch->bOpened && Now >= Watch->NextReserve)
        {
            Watch->NextReserve = Now + ReserveInterval;
            ReserveLatePlayers(GameMode, *Watch, bWarmup);
        }

        // Every 30 seconds before the warmup (the settings), every second during it (the match's players and time left),
        // once after.
        if (Watch->bAsking || Watch->bUnsupported || Watch->bReportedEnd || Now < Watch->NextAsk)
        {
            return;
        }
        const bool bAfter = Phase > EAthenaGamePhase::GetWarmup();
        Watch->NextAsk = Now + (bWarmup ? WarmupReportInterval : WarmupAskInterval);
        Watch->bReportedEnd = bAfter;
        float SecondsLeft = 0.0f;
        if (bWarmup && AFortGameStateAthena::HasWarmupCountdown())
        {
            SecondsLeft = GameState->WarmupCountdownEndTime >= 0.0f ? GameState->WarmupCountdownEndTime - WorldNow
                : AFortGameModeAthena::HasWarmupLength() ? GameMode->WarmupCountdownDuration : 0.0f;
        }
        if (bAfter)
        {
            Watch->bOpen = false;
        }
        ReportWarmup(Watch, GameState->GamePhase.ToString(), ConnectedAccounts(GameMode), SecondsLeft);
    }

    void WatchWarmupOf(FWeakObjectPtr GameMode, std::shared_ptr<FWarmupWatch> Watch)
    {
        GameThread::Enqueue([GameMode, Watch]()
        {
            AFortGameModeAthena* Athena = static_cast<AFortGameModeAthena*>(GameMode.Get());
            AFortGameStateAthena* GameState = Athena ? AthenaGameStateOf(Athena) : nullptr;
            // Ends with the match, or once the storm is up: everyone who came during the warmup is in the game by then.
            if (!Athena || (GameState && GameState->GamePhase > EAthenaGamePhase::GetAircraft() && Watch->bReportedEnd))
            {
                return;
            }
            CheckWarmup(Athena, Watch, std::chrono::steady_clock::now());
            WatchWarmupOf(GameMode, Watch);
        });
    }
}

void AFortGameModeAthena::WatchWarmup()
{
#if WITH_SERVER_CODE
    // Core-Backend #122: the admin panel's warmup length and late joining. The warmup lasts the game mode's
    // WarmupCountdownDuration, a config value no match option sets (1.9's OnGivenMatchAssignment, 0x42D2B0, reads only
    // match.option.warmupRequiredPlayerCount; 1.9.1's, 0x42CF40, adds customKey, isMatchStatsDisabled and
    // isProgressionDisabled). And the matchmaking service can't add players to a match: the server's MMS client knows
    // only Registered, AssignMatch and UnassignMatch (0x3BF2730), takes an AssignMatch only while Registered, and
    // leaves the service once its match is assigned (0x709370). So the server asks the backend (POST
    // matchmakingservice/warmup) from its world's start and sets the warmup's length; with late joining on it opens
    // its match during the warmup, reports its players and time left every second so the backend sends it the parties
    // that queue, and gives each of them a team.
    WatchWarmupOf(FWeakObjectPtr(this), std::make_shared<FWarmupWatch>());
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

void AFortGameModeAthena::HookBuildingVTables()
{
    UClass* AthenaClass = StaticClass();
    UObject* AthenaDefault = AthenaClass && Finder::ResolveScoreBuildingConstructionLive() ? AthenaClass->GetDefaultObject(false) : nullptr;
    void** AthenaVTable = AthenaDefault ? *reinterpret_cast<void***>(AthenaDefault) : nullptr;
    if (!AthenaVTable)
    {
        return;
    }

    // Each Battle Royale game mode class whose slot is the Athena game mode's own (AFortGameMode's, 1.7.2 0x589F50).
    const int32_t Index = Natives::AFortGameMode_ScoreBuildingConstructionIndex;
    void* const Score = AthenaVTable[Index];
    ScoreBuildingConstructionOG = reinterpret_cast<decltype(ScoreBuildingConstructionOG)>(Score);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(AthenaClass) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Score)
        {
            Hooks::SwapVTable(L"AFortGameModeAthena::ScoreBuildingConstruction", VTable, Index, reinterpret_cast<void*>(&ScoreBuildingConstructionHK));
        }
    }
}

void AFortGameModeAthena::ScoreBuildingConstructionHK(AFortGameModeAthena* This, AController* Builder, ABuildingSMActor* Building)
{
#if WITH_SERVER_CODE
    // A built piece takes its builder's team through the builder's WorldPlayerId: ServerCreateBuildingActor (1.7.2
    // 0x825810, 1.8 0x845D70) initializes the piece with it (0x80DAD0, slot 0xF7), and a piece still on its constructor's
    // Spectator takes the team of the player state with that id (0x4333B0, 1.8 0x47B730: WorldPlayerId +0x540, then
    // SetTeam, slot 0x117). The id stays -1 for every Battle Royale player (Core gives it in its Zone Login hook, which
    // Athena's own Login, 0x3F2450, never reaches, and Battle Royale has no world record to give it from), so every piece
    // stayed Spectator, and CanBePlayerEdited (0x461EF0, 1.8 0x4646F0) only compares the editor's team with a piece's
    // that isn't: anyone could edit anyone's walls (#120). The game mode scores the piece right after (0x825E72, 1.8
    // 0x8463D2) with its builder, whose team is the one CanBePlayerEdited reads for the editor (the controller's team is
    // its player state's, 0x80CE10, Athena's is TeamIndex, 0x42FE70). An edit's new piece copies the old one's team
    // (ReplaceBuildingActor, 0x47D5D0). Save the World's game modes keep their slot.
    AFortPlayerStateAthena* PlayerState = Builder ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Builder->PlayerState))) : nullptr;
    if (Building && Building->Role == ROLE_Authority && Building->Team == EFortTeam::GetSpectator())
    {
        if (PlayerState)
        {
            Building->Team = PlayerState->TeamIndex;
        }
        else
        {
            Log::Warning(L"Battle Royale: {} built by {} has no team, anyone can edit it", Building->GetName().ToStdString(), Builder ? Builder->GetName().ToStdString() : L"nobody");
        }
    }
#endif
    ScoreBuildingConstructionOG(This, Builder, Building);
}
